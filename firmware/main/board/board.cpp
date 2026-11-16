#include "board/board.h"

#include <driver/gpio.h>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>

namespace reader {
namespace {

constexpr char kTag[] = "Board";
constexpr char kMountPoint[] = "/sdcard";

constexpr gpio_num_t kPinMiso = GPIO_NUM_40;
constexpr gpio_num_t kPinMosi = GPIO_NUM_38;
constexpr gpio_num_t kPinSclk = GPIO_NUM_39;
constexpr gpio_num_t kPinCs = GPIO_NUM_47;
constexpr gpio_num_t kPinDet = GPIO_NUM_48;

// A GC16 pass after this many GL16 partial updates removes accumulated
// ghosting without making the UI feel sluggish.
constexpr int kPartialLimit = 20;

m5gfx::epd_mode_t to_epd_mode(EpdMode mode) {
    switch (mode) {
    case EpdMode::Quality:
        return m5gfx::epd_mode_t::epd_quality;
    case EpdMode::Text:
        return m5gfx::epd_mode_t::epd_text;
    case EpdMode::Fast:
        return m5gfx::epd_mode_t::epd_fast;
    case EpdMode::Fastest:
        return m5gfx::epd_mode_t::epd_fastest;
    }
    return m5gfx::epd_mode_t::epd_quality;
}

sdmmc_card_t* g_card = nullptr;
bool g_spi_ready = false;

} // namespace

Board& board() {
    static Board instance;
    return instance;
}

bool Board::init() {
    auto config = M5.config();
    config.clear_display = false;
    config.internal_imu = false;
    config.internal_mic = false;
    config.internal_spk = false;
    config.output_power = true;
    M5.begin(config);

    if (M5.getDisplayCount() == 0) {
        ESP_LOGE(kTag, "no display");
        return false;
    }

    M5.Display.setRotation(0);
    M5.Display.setAutoDisplay(false);
    M5.Display.setEpdMode(to_epd_mode(EpdMode::Quality));
    M5.Display.fillScreen(TFT_WHITE);
    _display_ok = true;

    ESP_LOGI(kTag, "display %dx%d", M5.Display.width(), M5.Display.height());
    return true;
}

void Board::delay_ms(uint32_t ms) {
    m5gfx::delay(ms);
}

uint32_t Board::millis() {
    return m5gfx::millis();
}

int Board::battery_millivolts() {
    return M5.Power.M5pm1.getBatteryVoltage();
}

int Board::battery_percent() {
    const int mv = battery_millivolts();
    if (mv <= 0) return -1;
    static const struct {
        int mv;
        int percent;
    } kCurve[] = {
        {4200, 100}, {4100, 95}, {4000, 85}, {3950, 78}, {3900, 70}, {3850, 62},
        {3800, 55},  {3750, 47}, {3700, 40}, {3650, 33}, {3600, 26}, {3550, 19},
        {3500, 12},  {3450, 7},  {3400, 3},  {3300, 0},
    };
    if (mv >= kCurve[0].mv) return 100;
    for (size_t i = 1; i < sizeof(kCurve) / sizeof(kCurve[0]); ++i) {
        if (mv >= kCurve[i].mv) {
            const int hi_mv = kCurve[i - 1].mv;
            const int lo_mv = kCurve[i].mv;
            const int hi_p = kCurve[i - 1].percent;
            const int lo_p = kCurve[i].percent;
            return lo_p + (mv - lo_mv) * (hi_p - lo_p) / (hi_mv - lo_mv);
        }
    }
    return 0;
}

bool Board::mount_sd(bool force) {
    if (_sd_mounted) return true;

    gpio_config_t detect = {};
    detect.pin_bit_mask = 1ULL << kPinDet;
    detect.mode = GPIO_MODE_INPUT;
    detect.pull_up_en = GPIO_PULLUP_ENABLE;
    detect.pull_down_en = GPIO_PULLDOWN_DISABLE;
    detect.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&detect);

    const bool detect_pin_says_inserted = gpio_get_level(kPinDet) == 0;
    _stats.last_detect_level = gpio_get_level(kPinDet);
    if (!detect_pin_says_inserted && !force) {
        ESP_LOGW(kTag, "no SD card detected (detect pin high)");
        return false;
    }
    if (!detect_pin_says_inserted) {
        ESP_LOGW(kTag, "detect pin says no card, trying to mount anyway");
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    if (!g_spi_ready) {
        spi_bus_config_t bus = {};
        bus.mosi_io_num = kPinMosi;
        bus.miso_io_num = kPinMiso;
        bus.sclk_io_num = kPinSclk;
        bus.quadwp_io_num = GPIO_NUM_NC;
        bus.quadhd_io_num = GPIO_NUM_NC;
        bus.max_transfer_sz = 16 * 1024;
        const esp_err_t err = spi_bus_initialize(static_cast<spi_host_device_t>(host.slot), &bus, SDSPI_DEFAULT_DMA);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "spi init: %s", esp_err_to_name(err));
            return false;
        }
        g_spi_ready = true;
    }

    gpio_set_pull_mode(kPinMiso, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(kPinMosi, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(kPinCs, GPIO_PULLUP_ONLY);

    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = 6;
    mount.allocation_unit_size = 16 * 1024;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = kPinCs;
    slot.host_id = static_cast<spi_host_device_t>(host.slot);

    const esp_err_t err = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot, &mount, &g_card);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "sd mount: %s", esp_err_to_name(err));
        g_card = nullptr;
        return false;
    }
    _sd_mounted = true;
    ESP_LOGI(kTag, "SD mounted");
    return true;
}

void Board::unmount_sd() {
    if (!_sd_mounted || g_card == nullptr) return;
    esp_vfs_fat_sdcard_unmount(kMountPoint, g_card);
    g_card = nullptr;
    _sd_mounted = false;
}

void Board::begin_frame() {
    _region_count = 0;
    _frame_open = true;
}

void Board::touch(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    const int screen_w = width();
    const int screen_h = height();
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;

    if (!_frame_open) begin_frame();

    if (_region_count >= static_cast<int>(sizeof(_regions) / sizeof(_regions[0]))) {
        // Too many small edits: just repaint everything.
        _regions[0] = Region{0, 0, screen_w, screen_h};
        _region_count = 1;
        return;
    }
    _regions[_region_count++] = Region{x, y, w, h};
}

void Board::request_full_refresh() {
    _force_full = true;
}

void Board::merge_regions() {
    const int screen_w = width();
    bool merged = true;
    while (merged && _region_count > 1) {
        merged = false;
        for (int i = 0; i < _region_count && !merged; ++i) {
            for (int j = i + 1; j < _region_count; ++j) {
                const Region& a = _regions[i];
                const Region& b = _regions[j];
                // Merge overlapping or vertically adjacent (same band) regions;
                // the EPD LUT needs aligned, contiguous areas to stay efficient.
                const bool overlap = a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
                const bool same_row = a.y < b.y + b.h && b.y < a.y + a.h &&
                                      (a.x + a.w + 2 >= b.x) && (b.x + b.w + 2 >= a.x);
                if (!overlap && !same_row) continue;

                const int left = a.x < b.x ? a.x : b.x;
                const int top = a.y < b.y ? a.y : b.y;
                const int right = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
                const int bottom = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
                _regions[i] = Region{left, top, right - left, bottom - top};
                _regions[j] = _regions[_region_count - 1];
                --_region_count;
                merged = true;
                break;
            }
        }
    }
    (void)screen_w;
}


bool Board::commit_frame(EpdMode mode) {
    const uint32_t started_ms = millis();
    _frame_open = false;
    auto& display = M5.Display;

    bool full = _force_full || _region_count == 0;
    // Regions often overlap (a panel inside the content area, an icon inside a row).
    // Merge them first: summing raw areas made small repeated edits look like 80% of
    // the screen and forced a full repaint.
    merge_regions();
    int covered = 0;
    for (int i = 0; i < _region_count; ++i) {
        covered += _regions[i].w * _regions[i].h;
    }
    const int screen_area = width() * height();
    // Past this share a full GC16 pass is cheaper and cleaner than several
    // partial waveforms (and it wipes accumulated ghosting).
    if (covered * 100 >= screen_area * 68) full = true;
    if (_partial_updates >= kPartialLimit) full = true;

    display.waitDisplay();

    if (full) {
        _stats.full_flushes++;
        _stats.last_regions = static_cast<uint32_t>(_region_count);
        _stats.last_x = 0;
        _stats.last_y = 0;
        _stats.last_w = width();
        _stats.last_h = height();
        flush_full(mode);
        _force_full = false;
        _partial_updates = 0;
        _region_count = 0;
        display.waitDisplay();
        _stats.last_ms = millis() - started_ms;
        return true;
    }

    merge_regions();
    for (int i = 0; i < _region_count; ++i) {
        flush_rect(_regions[i], mode);
        display.waitDisplay();
    }
    _stats.last_ms = millis() - started_ms;
    _stats.partial_flushes += static_cast<uint32_t>(_region_count);
    _stats.last_regions = static_cast<uint32_t>(_region_count);
    _stats.last_x = _regions[0].x;
    _stats.last_y = _regions[0].y;
    _stats.last_w = _regions[0].w;
    _stats.last_h = _regions[0].h;
    ++_partial_updates;
    _region_count = 0;
    _force_full = false;
    return false;
}

void Board::flush_rect(const Region& region, EpdMode mode) {
    auto& display = M5.Display;
    display.setEpdMode(to_epd_mode(mode));
    display.display(region.x, region.y, region.w, region.h);
}

void Board::flush_full(EpdMode mode) {
    auto& display = M5.Display;
    display.setEpdMode(to_epd_mode(mode));
    display.display(0, 0, display.width(), display.height());
}

void Board::wait_display() {
    M5.Display.waitDisplay();
}

void Board::power_off() {
    M5.Power.M5pm1.powerOff();
}

} // namespace reader
