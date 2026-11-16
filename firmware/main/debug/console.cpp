#include "debug/console.h"
#include <esp_app_desc.h>

#ifndef BUILD_STAMP
#define BUILD_STAMP "unknown"
#endif
#include "ui/draw.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <driver/usb_serial_jtag.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "board/board.h"
#include "input/input_queue.h"
#include "ui/app.h"

namespace reader {
namespace {

constexpr char kTag[] = "rdr-console";
constexpr size_t kMaxLine = 4096;
constexpr size_t kMaxPayload = 3 * 1024 * 1024;

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::vector<uint8_t> base64_decode(const char* text, size_t length) {
    std::vector<uint8_t> out;
    out.reserve(length * 3 / 4 + 4);
    int accumulator = 0;
    int bits = 0;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (c == '=' || c == '\r' || c == '\n') continue;
        const int value = base64_value(c);
        if (value < 0) continue;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    return out;
}

void write_str(const char* text) {
    usb_serial_jtag_write_bytes(text, std::strlen(text), pdMS_TO_TICKS(500));
}

void write_bytes(const void* data, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    // usb_serial_jtag_write_bytes queues into a FreeRTOS ring buffer, which
    // rejects items larger than its capacity: send bounded chunks.
    constexpr size_t kChunk = 2048;
    size_t sent = 0;
    int stalls = 0;
    while (sent < length) {
        const size_t chunk = std::min(kChunk, length - sent);
        const int written = usb_serial_jtag_write_bytes(bytes + sent, chunk, pdMS_TO_TICKS(1000));
        if (written > 0) {
            sent += static_cast<size_t>(written);
            stalls = 0;
        } else if (++stalls > 50) {
            break;
        }
    }
}

void send_screenshot(int x, int y, int w, int h) {
    auto& gfx = board().display();
    const int screen_w = board().width();
    const int screen_h = board().height();
    if (w <= 0 || h <= 0) {
        x = 0;
        y = 0;
        w = screen_w;
        h = screen_h;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    const size_t pixels = static_cast<size_t>(w) * h;

    // Read the panel in bands: a full-frame RGB buffer (1.5 MB) does not fit while a
    // big book and its notes sit in PSRAM.
    constexpr int kBandRows = 48;
    const size_t band_pixels = static_cast<size_t>(w) * kBandRows;
    auto* rgb = static_cast<uint8_t*>(heap_caps_malloc(band_pixels * 3, MALLOC_CAP_SPIRAM));
    auto* gray = static_cast<uint8_t*>(heap_caps_malloc(band_pixels, MALLOC_CAP_SPIRAM));
    if (rgb == nullptr || gray == nullptr) {
        heap_caps_free(rgb);
        heap_caps_free(gray);
        write_str("rdr err no-memory\n");
        return;
    }

    char header[80];
    std::snprintf(header, sizeof(header), "rdr shot %d %d %d %d %u\n", x, y, w, h,
                  static_cast<unsigned>(pixels));
    write_str(header);
    for (int row = 0; row < h; row += kBandRows) {
        const int rows = (h - row) < kBandRows ? (h - row) : kBandRows;
        const size_t count = static_cast<size_t>(w) * rows;
        gfx.readRectRGB(x, y + row, w, rows, rgb);
        for (size_t i = 0; i < count; ++i) {
            // BGR888: the panel is grayscale, so a luminance mix is enough.
            const int b = rgb[i * 3 + 0];
            const int g = rgb[i * 3 + 1];
            const int r = rgb[i * 3 + 2];
            gray[i] = static_cast<uint8_t>((r * 3 + g * 6 + b) / 10);
        }
        write_bytes(gray, count);
    }
    write_str("rdr end\n");

    heap_caps_free(rgb);
    heap_caps_free(gray);
}

// Writes one base64 chunk to a file; `append` keeps the existing bytes. Uploading a
// 760 KB book is impossible in one console line (the line buffer is 4 KB), so the host
// tool sends it in pieces and only the first piece truncates the file.
void handle_put(const char* args, bool append) {
    while (*args == ' ') ++args;
    const char* space = std::strchr(args, ' ');
    if (space == nullptr || space == args) {
        write_str("rdr err usage\n");
        return;
    }
    std::string path(args, space - args);
    if (!path.empty() && path[0] == '/') path.erase(0, 1);
    const char* payload = space + 1;
    const size_t payload_len = std::strlen(payload);
    if (payload_len > kMaxPayload) {
        write_str("rdr err too-large\n");
        return;
    }
    std::vector<uint8_t> data = base64_decode(payload, payload_len);

    std::string full = std::string("/sdcard/") + path;
    FILE* file = std::fopen(full.c_str(), append ? "ab" : "wb");
    if (file == nullptr) {
        write_str("rdr err open-failed\n");
        return;
    }
    const size_t written = std::fwrite(data.data(), 1, data.size(), file);
    std::fflush(file);
    std::fclose(file);

    char reply[64];
    std::snprintf(reply, sizeof(reply), "rdr ok %u\n", static_cast<unsigned>(written));
    write_str(reply);
}

void handle_echo(const char* text) {
    std::string hex;
    for (const char* p = text; *p != '\0'; ++p) {
        char byte[4];
        std::snprintf(byte, sizeof(byte), "%02X ", static_cast<unsigned char>(*p));
        hex += byte;
    }
    write_str(("rdr echo " + hex + "\n").c_str());
}

void handle_probe(const char* encoded) {
    std::vector<uint8_t> name_bytes = base64_decode(encoded, std::strlen(encoded));
    std::string name(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size());
    std::string full = "/sdcard/" + name;

    struct stat info = {};
    const int stat_result = stat(full.c_str(), &info);
    const int stat_errno = errno;

    errno = 0;
    FILE* file = std::fopen(full.c_str(), "rb");
    const int open_errno = errno;

    errno = 0;
    const int fd = ::open(full.c_str(), O_RDONLY);
    const int posix_errno = errno;

    char reply[256];
    std::snprintf(reply, sizeof(reply), "rdr probe len=%u stat=%d/%d fopen=%s/%d posix=%d/%d\n",
                  static_cast<unsigned>(full.size()), stat_result, stat_errno, file ? "ok" : "null",
                  open_errno, fd, posix_errno);
    write_str(reply);
    if (file != nullptr) std::fclose(file);
    if (fd >= 0) ::close(fd);
}

void handle_bench() {
    auto& display = board().display();
    // Each EPD mode maps to a LUT with a fixed number of frames, so the panel
    // time is what dominates. Measure it instead of guessing.
    struct ModeInfo {
        const char* name;
        m5gfx::epd_mode_t mode;
    };
    const ModeInfo modes[] = {
        {"quality", m5gfx::epd_mode_t::epd_quality},
        {"text", m5gfx::epd_mode_t::epd_text},
        {"fast", m5gfx::epd_mode_t::epd_fast},
        {"fastest", m5gfx::epd_mode_t::epd_fastest},
    };
    struct AreaInfo {
        const char* name;
        int x, y, w, h;
    };
    const AreaInfo areas[] = {
        {"row", 0, 200, 540, 54},
        {"half", 0, 300, 540, 480},
        {"full", 0, 0, 540, 960},
    };

    for (const AreaInfo& area : areas) {
        for (const ModeInfo& info : modes) {
            // Change the pixels first so the waveform has work to do.
            display.fillRect(area.x + 4, area.y + 4, area.w - 8, area.h - 8, TFT_BLACK);
            display.fillRect(area.x + 8, area.y + 8, area.w - 16, area.h - 16, TFT_WHITE);
            display.waitDisplay();

            const uint32_t start = board().millis();
            display.setEpdMode(info.mode);
            display.display(area.x, area.y, area.w, area.h);
            display.waitDisplay();
            const uint32_t elapsed = board().millis() - start;

            char line[96];
            std::snprintf(line, sizeof(line), "rdr bench %-7s %-6s %u ms\n", area.name, info.name,
                          static_cast<unsigned>(elapsed));
            write_str(line);
        }
    }
    write_str("rdr bench end\n");
}

void handle_get(const char* arguments, bool base64) {
    std::string path(arguments);
    if (!path.empty() && path[0] == '/') path.erase(0, 1);
    const std::string full = "/sdcard/" + path;
    FILE* file = std::fopen(full.c_str(), "rb");
    if (file == nullptr) {
        write_str("rdr err open\n");
        return;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(file);
        write_str("rdr err size\n");
        return;
    }

    char header[64];
    if (base64) {
        write_str("rdr b64\n");
    } else {
        std::snprintf(header, sizeof(header), "rdr raw %ld\n", size);
        write_str(header);
    }

    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    constexpr size_t kChunk = 768; // multiple of 3 -> clean base64 blocks
    std::vector<uint8_t> buffer(kChunk);
    char encoded[1100];
    size_t got = 0;
    while ((got = std::fread(buffer.data(), 1, kChunk, file)) > 0) {
        if (!base64) {
            write_bytes(buffer.data(), got);
            continue;
        }
        size_t out = 0;
        for (size_t i = 0; i + 2 < got; i += 3) {
            const uint32_t triple = (buffer[i] << 16) | (buffer[i + 1] << 8) | buffer[i + 2];
            encoded[out++] = kAlphabet[(triple >> 18) & 0x3F];
            encoded[out++] = kAlphabet[(triple >> 12) & 0x3F];
            encoded[out++] = kAlphabet[(triple >> 6) & 0x3F];
            encoded[out++] = kAlphabet[triple & 0x3F];
        }
        const size_t remainder = got % 3;
        if (remainder == 1) {
            const uint32_t triple = buffer[got - 1] << 16;
            encoded[out++] = kAlphabet[(triple >> 18) & 0x3F];
            encoded[out++] = kAlphabet[(triple >> 12) & 0x3F];
            encoded[out++] = '=';
            encoded[out++] = '=';
        } else if (remainder == 2) {
            const uint32_t triple = (buffer[got - 2] << 16) | (buffer[got - 1] << 8);
            encoded[out++] = kAlphabet[(triple >> 18) & 0x3F];
            encoded[out++] = kAlphabet[(triple >> 12) & 0x3F];
            encoded[out++] = kAlphabet[(triple >> 6) & 0x3F];
            encoded[out++] = '=';
        }
        encoded[out++] = '\n';
        write_bytes(encoded, out);
    }
    std::fclose(file);
    write_str("rdr end\n");
}

void handle_command(char* line) {
    if (std::strncmp(line, "rdr ", 4) != 0) return;
    char* command = line + 4;
    while (*command == ' ') ++command;

    // Split the verb from its arguments so prefixes never collide
    // ("open" vs "openidx", "put" vs "probe").
    char* arguments = command;
    while (*arguments != '\0' && *arguments != ' ') ++arguments;
    if (*arguments == ' ') {
        *arguments++ = '\0';
        while (*arguments == ' ') ++arguments;
    }
    const char* verb = command;

    // State-changing commands must not race App::begin(): wait until boot is done.
    auto needs_ready = [](const char* name) {
        static const char* const kBlocking[] = {"open",   "openb64", "openidx", "put",  "rm",
                                                "heal",   "key",     "limit",   "clean", "bench",
                                                "reset",  "sburst",  "burst"};
        for (const char* candidate : kBlocking) {
            if (std::strcmp(name, candidate) == 0) return true;
        }
        return false;
    };
    if (needs_ready(verb)) {
        const uint32_t start = xTaskGetTickCount();
        while (!app().booted() && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(20000)) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!app().booted()) {
            write_str("rdr err busy\\n");
            return;
        }
    }

    if (std::strcmp(verb, "ping") == 0) {
        char reply[96];
        std::snprintf(reply, sizeof(reply), "rdr pong ereader %s\n", esp_app_get_description()->version);
        write_str(reply);
        return;
    }
    if (std::strcmp(verb, "version") == 0) {
        // Release identification: version, plus the build date ESP-IDF records.
        const esp_app_desc_t* desc = esp_app_get_description();
        char reply[192];
        std::snprintf(reply, sizeof(reply), "rdr version %s built %s\n", desc->version,
                      BUILD_STAMP);
        write_str(reply);
        return;
    }
    if (std::strcmp(verb, "note") == 0) {
        // Reports the reference-mode geometry: what the note panel decided to show.
        char reply[160];
        std::snprintf(reply, sizeof(reply), "rdr note mode=%d focus=%d links=%d lines=%d visible=%d max=%d panel_h=%d shift=%d\n",
                      app().link_mode() ? 1 : 0, app().link_focus(), app().link_count(),
                      app().link_line_count(), app().link_visible(), app().link_max_scroll(),
                      app().link_panel_height(), app().page_shift());
        write_str(reply);
        return;
    }
    if (std::strcmp(verb, "fill") == 0) {
        // Paints a 120x120 square with one panel level: checks fill_level() itself.
        const int level = arguments[0] ? std::atoi(arguments) : 12;
        board().begin_frame();
        // Black frame marks the spot; the interior tests fill_level() itself.
        board().display().drawRect(98, 298, 124, 124, TFT_BLACK);
        ui::fill_level(board().display(), 100, 300, 120, 120, static_cast<uint8_t>(level));
        // Second test patch: a sparse dot band, the same helper the note shade uses.
        board().display().drawRect(96, 448, 348, 34, TFT_BLACK);
        ui::fill_dither(board().display(), 100, 452, 340, 26, 5);
        board().touch(96, 296, 128, 128);
        board().touch(94, 446, 352, 38);
        board().commit_frame(EpdMode::Fast);
        write_str("rdr ok\n");
        return;
    }
    if (std::strcmp(verb, "shot") == 0) {
        int x = 0, y = 0, w = 0, h = 0;
        if (std::sscanf(arguments, "%d %d %d %d", &x, &y, &w, &h) != 4) {
            x = y = 0;
            w = board().width();
            h = board().height();
        }
        send_screenshot(x, y, w, h);
        return;
    }
    if (std::strcmp(verb, "put") == 0) {
        handle_put(arguments, false);
        return;
    }
    if (std::strcmp(verb, "append") == 0) {
        handle_put(arguments, true);
        return;
    }
    if (std::strcmp(verb, "echo") == 0) {
        handle_echo(arguments);
        return;
    }
    if (std::strcmp(verb, "ls") == 0) {
        // Lists a directory: "rdr ls books/Dasha" -> name<TAB>size per line.
        char path[256];
        std::snprintf(path, sizeof(path), "/sdcard/%s", arguments[0] == '/' ? arguments + 1 : arguments);
        DIR* dir = opendir(path);
        if (dir == nullptr) {
            write_str("rdr err ls\n");
            return;
        }
        while (dirent* entry = readdir(dir)) {
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
            char full[512];
            std::snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
            struct stat info {};
            const long long size = stat(full, &info) == 0 ? static_cast<long long>(info.st_size) : -1;
            char line[512];
            std::snprintf(line, sizeof(line), "%s\t%lld\t%s\n", entry->d_name, size,
                          S_ISDIR(info.st_mode) ? "dir" : "file");
            write_str(line);
        }
        closedir(dir);
        write_str("rdr ok\n");
        return;
    }
    if (std::strcmp(verb, "probe") == 0) {
        handle_probe(arguments);
        return;
    }
    if (std::strcmp(verb, "key") == 0) {
        Gesture gesture = Gesture::None;
        if (std::strcmp(arguments, "A") == 0) gesture = Gesture::ClickA;
        else if (std::strcmp(arguments, "B") == 0) gesture = Gesture::ClickB;
        else if (std::strcmp(arguments, "AB") == 0) gesture = Gesture::Chord;
        else if (std::strcmp(arguments, "ABL") == 0) gesture = Gesture::ChordLong;
        else if (std::strcmp(arguments, "UP") == 0) gesture = Gesture::FontUp;
        else if (std::strcmp(arguments, "DN") == 0) gesture = Gesture::FontDown;
        if (gesture == Gesture::None) {
            write_str("rdr err key\n");
        } else {
            app().inject(gesture);
            write_str("rdr ok\n");
        }
        return;
    }
    if (std::strcmp(verb, "open") == 0) {
        std::string relative(arguments);
        if (!relative.empty() && relative[0] == '/') relative.erase(0, 1);
        if (app().open_path(relative)) {
            write_str("rdr ok\n");
        } else {
            write_str("rdr err open\n");
        }
        return;
    }
    if (std::strcmp(verb, "openb64") == 0) {
        std::vector<uint8_t> name_bytes = base64_decode(arguments, std::strlen(arguments));
        std::string relative(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size());
        if (!relative.empty() && relative[0] == '/') relative.erase(0, 1);
        if (app().open_path(relative)) {
            write_str("rdr ok\n");
        } else {
            write_str("rdr err open\n");
        }
        return;
    }
    if (std::strcmp(verb, "openidx") == 0) {
        if (app().open_index(std::atoi(arguments))) {
            write_str("rdr ok\n");
        } else {
            write_str("rdr err open\n");
        }
        return;
    }
    if (std::strcmp(verb, "dump") == 0) {
        // Wait for the UI to finish its frame so the reported state is coherent.
        for (int i = 0; i < 40 && app().busy(); ++i) vTaskDelay(pdMS_TO_TICKS(50));
        std::string text = app().dump_text();
        char header[48];
        std::snprintf(header, sizeof(header), "rdr dump %u\n", static_cast<unsigned>(text.size()));
        write_str(header);
        write_bytes(text.data(), text.size());
        write_str("rdr end\n");
        return;
    }
    if (std::strcmp(verb, "get") == 0) {
        handle_get(arguments, false);
        return;
    }
    if (std::strcmp(verb, "getb64") == 0) {
        handle_get(arguments, true);
        return;
    }
    if (std::strcmp(verb, "sdmount") == 0) {
        const bool forced = arguments[0] == '1' || arguments[0] == 'f';
        const bool mounted = board().mount_sd(forced);
        char reply[96];
        std::snprintf(reply, sizeof(reply), "rdr sdmount %s (detect=%d, forced=%d)\n",
                      mounted ? "ok" : "failed", static_cast<int>(board().stats().last_detect_level),
                      forced ? 1 : 0);
        write_str(reply);
        return;
    }
    if (std::strcmp(verb, "rm") == 0) {
        char path[256];
        std::snprintf(path, sizeof(path), "/sdcard/%s", arguments[0] == '/' ? arguments + 1 : arguments);
        write_str(std::remove(path) == 0 ? "rdr ok\n" : "rdr err rm\n");
        return;
    }
    if (std::strcmp(verb, "heal") == 0) {
        char path[256];
        std::snprintf(path, sizeof(path), "%s", arguments[0] == '/' ? arguments + 1 : arguments);
        write_str(app().force_repair(path) ? "rdr ok\n" : "rdr err heal\n");
        return;
    }
    if (std::strcmp(verb, "limit") == 0) {
        app().set_open_limit(static_cast<uint64_t>(std::strtoull(arguments, nullptr, 10)));
        write_str("rdr ok\n");
        return;
    }
    if (std::strcmp(verb, "clean") == 0) {
        // Force one full GC16 repaint of the current screen (clears ghosting).
        app().request_clean();
        write_str("rdr ok\n");
        return;
    }
    if (std::strcmp(verb, "bench") == 0) {
        handle_bench();
        return;
    }
    if (std::strcmp(verb, "burst") == 0) {
        // rdr burst <A|B|AB|UP|DN> <count>: injects N gestures back to back so the
        // UI task has to coalesce them into a single refresh.
        char name[8] = {0};
        int count = 0;
        if (std::sscanf(arguments, "%7s %d", name, &count) != 2 || count <= 0) {
            write_str("rdr err usage\n");
            return;
        }
        Gesture gesture = Gesture::None;
        if (std::strcmp(name, "A") == 0) gesture = Gesture::ClickA;
        else if (std::strcmp(name, "B") == 0) gesture = Gesture::ClickB;
        else if (std::strcmp(name, "AB") == 0) gesture = Gesture::Chord;
        else if (std::strcmp(name, "UP") == 0) gesture = Gesture::FontUp;
        else if (std::strcmp(name, "DN") == 0) gesture = Gesture::FontDown;
        if (gesture == Gesture::None) {
            write_str("rdr err key\n");
            return;
        }
        for (int i = 0; i < count; ++i) post_gesture(gesture);
        write_str("rdr ok\n");
        return;
    }
    if (std::strcmp(verb, "pek") == 0) {
        // Raw PMIC power-key state: 0=none / 2=short click (latched, cleared on read).
        char line[64];
        std::snprintf(line, sizeof(line), "rdr pek %u mv=%d\n", M5.Power.getKeyState(),
                      board().battery_millivolts());
        write_str(line);
        return;
    }
    if (std::strcmp(verb, "bat") == 0) {
        char line[96];
        std::snprintf(line, sizeof(line), "rdr bat mv=%d percent=%d\n", board().battery_millivolts(),
                      board().battery_percent());
        write_str(line);
        return;
    }
    if (std::strcmp(verb, "stats") == 0) {
        const auto& stats = board().stats();
        char reply[192];
        std::snprintf(reply, sizeof(reply),
                      "rdr stats full=%u partial=%u last_regions=%u last=%d,%d %dx%d %ums detect=%d\n",
                      static_cast<unsigned>(stats.full_flushes), static_cast<unsigned>(stats.partial_flushes),
                      static_cast<unsigned>(stats.last_regions), static_cast<int>(stats.last_x),
                      static_cast<int>(stats.last_y), static_cast<int>(stats.last_w),
                      static_cast<int>(stats.last_h), static_cast<unsigned>(stats.last_ms),
                      static_cast<int>(stats.last_detect_level));
        write_str(reply);
        return;
    }
    if (std::strcmp(verb, "reboot") == 0) {
        // Real power cycle: keeps the progress (the device must reopen the book).
        write_str("rdr ok\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    if (std::strcmp(verb, "reset") == 0) {
        app().reset_progress();
        write_str("rdr ok\n");
        return;
    }
    write_str("rdr err unknown\n");
}

void console_task(void*) {
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.rx_buffer_size = 8192;
    config.tx_buffer_size = 16384;
    if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
        ESP_LOGE(kTag, "driver install failed");
        vTaskDelete(nullptr);
        return;
    }

    auto* line = static_cast<char*>(std::malloc(kMaxLine));
    if (line == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    size_t length = 0;
    uint8_t chunk[512];

    while (true) {
        const int read = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        for (int i = 0; i < read; ++i) {
            const char c = static_cast<char>(chunk[i]);
            if (c == '\n' || c == '\r') {
                line[length] = '\0';
                if (length > 0) handle_command(line);
                length = 0;
            } else if (length + 1 < kMaxLine) {
                line[length++] = c;
            } else {
                length = 0;
            }
        }
    }
}

} // namespace

void start_debug_console() {
    xTaskCreate(console_task, "rdr-console", 16384, nullptr, 3, nullptr);
}

} // namespace reader

void write_str(const char* text) {
    usb_serial_jtag_write_bytes(text, std::strlen(text), pdMS_TO_TICKS(500));
}
