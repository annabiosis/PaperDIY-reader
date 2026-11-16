#include "ui/app.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "core/encoding.h"
#include "core/utf8.h"
#include "formats/formats.h"
#include "input/gestures.h"
#include "input/input_queue.h"
#include "store/progress.h"
#include "text/font.h"
#include "ui/draw.h"
#include "ui/icon_draw.h"

namespace reader {

namespace {
class PageCounter;

} // namespace

namespace {

constexpr char kMountPoint[] = "/sdcard";
static const char* kTag = "App";
constexpr int kHeaderHeight = 42;
constexpr int kFooterHeight = 42;
// Reader text margins, from the mockup: 48 px on both sides.
constexpr int kReaderMargin = 48;
constexpr int kMarginLeft = 26;
constexpr int kMarginRight = 26;
constexpr size_t kMaxHistory = 200;
// A layout-independent page position is remembered every N pages so the back
// history can be rebuilt cheaply after a font size change.
constexpr int kCheckpointStep = 10;
// Guard for layout walks (page counting / snapping after a font change).
constexpr int kMaxWalkPages = 4000;
// Width of the cursor strip at each side of a file manager row.
constexpr int kCursorStrip = 30;
// DU leaves residue on antialiased pixels: force a full GC16 pass every N pages.
constexpr int kPagesPerClean = 4;
// Reference mode: the note panel can be repainted on its own while scrolling.
constexpr uint8_t kPanelPart = 8;

// File manager grid, taken from the Figma mockup (reader page, Frame 1):
// rows 48 px high with an 8 px gap, icons on a 24 px grid at x=36, names at 24 px.
constexpr int kRowHeight = 48;
constexpr int kRowPitch = 56;
constexpr int kListTopGap = 8;
constexpr int kIconX = 36;
constexpr int kNameX = 68;
// Level 13 of the panel palette (#DDDDDD): the header/footer fill in the mockup.
constexpr uint16_t kHeaderFill = TFT_WHITE;
constexpr uint16_t kFooterFill = TFT_WHITE;
// Locked (too large) files are drawn in this grey, like the mockup row.
constexpr uint16_t kLockedInk = 0x9CF3;
constexpr int kScrollbarWidth = 4;
constexpr uint8_t kScrollbarLevel = 6;

std::string extension_of(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return std::string();
    return name.substr(dot + 1);
}

bool position_before(const PagePos& left, const PagePos& right);

// Counts the pages of the current book in the current layout, in the background,
// so the footer can show a real "N / M". It walks a second Document from the very
// beginning (the format parsers keep only one cursor), one page per idle tick, and
// remembers which page contains the position the reader is looking at.
class PageCounter {
  public:
    bool start(const std::string& path, const LayoutMetrics& metrics, const PagePos& anchor) {
        reset();
        _source = open_file_source(path, nullptr);
        if (!_source) return false;
        _doc = open_document(format_from_name(path), std::move(_source), nullptr);
        if (!_doc) return false;
        _paginator = std::make_unique<Paginator>(*_doc, metrics);
        if (!_paginator->restart()) return false;
        _anchor = anchor;
        return true;
    }

    void reset() {
        _paginator.reset();
        _doc.reset();
        _source.reset();
        _count = 0;
        _anchor_found = false;
        _anchor_index = 0;
        _finished = false;
    }

    bool valid() const { return _paginator != nullptr; }
    bool finished() const { return _finished; }
    int count() const { return _count; }
    bool anchor_found() const { return _anchor_found; }
    int anchor_index() const { return _anchor_index; }
    const PagePos& anchor() const { return _anchor; }

    // Lays out one more page. Returns false when the document ends.
    bool step() {
        if (!valid() || _finished) return false;
        PageView page;
        if (!_paginator->next(page)) {
            _finished = true;
            return false;
        }
        ++_count;
        if (!_anchor_found && position_before(_anchor, page.end)) {
            _anchor_found = true;
            _anchor_index = _count;
        }
        return true;
    }

  private:
    std::unique_ptr<ByteSource> _source;
    std::unique_ptr<Document> _doc;
    std::unique_ptr<Paginator> _paginator;
    PagePos _anchor;
    int _count = 0;
    bool _anchor_found = false;
    int _anchor_index = 0;
    bool _finished = false;
};

// Per-format open limits (see docs/decisions.md): larger files are refused by the
// parsers, so the file manager greys them out with a lock.
uint64_t g_open_limit_override = 0;

PageCounter& page_counter() {
    static PageCounter counter;
    return counter;
}

uint64_t max_openable_size(const std::string& name) {
    if (g_open_limit_override != 0) return g_open_limit_override;
    const Format format = format_from_name(name);
    switch (format) {
    case Format::Pdf:
        return 12ull * 1024 * 1024;
    case Format::Unknown:
        return 0;
    default:
        return 8ull * 1024 * 1024;
    }
}

std::string lower_utf8(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
            ++i;
            continue;
        }
        // Cyrillic uppercase U+0410..U+042F -> lowercase 0xD0B0..0xD0BF.
        if (c == 0xD0 && i + 1 < text.size()) {
            const unsigned char d = static_cast<unsigned char>(text[i + 1]);
            if (d >= 0x90 && d <= 0xAF) {
                out.push_back(text[i]);
                out.push_back(static_cast<char>(d + 0x20));
                i += 2;
                continue;
            }
            if (d == 0x81) { // Yo -> yo (CP1251 upper case)
                out += "\xD1\x91";
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

bool has_supported_extension(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string extension = name.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == "txt" || extension == "md" || extension == "markdown" || extension == "fb2" ||
           extension == "epub" || extension == "pdf";
}

std::string human_size(uint64_t bytes) {
    char buffer[32];
    if (bytes >= 1024ULL * 1024ULL) {
        std::snprintf(buffer, sizeof(buffer), "%.1fM", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lluK", static_cast<unsigned long long>((bytes + 1023) / 1024));
    }
    return buffer;
}

} // namespace

bool App::begin() {
    _booted = false;
    // First thing after power-on: tell the user the device is starting, so the
    // screen does not stay blank while the SD card and the library are prepared.
    _screen = Screen::Booting;
    render();

    if (!board().mount_sd() && !board().mount_sd(true)) {
        _font_size = 1;
        set_message("SD card not found", "Insert a card with books and press A+B", Screen::FileList);
        _message_back = Screen::FileList;
        _last_save_ms = board().millis();
        mark_all();
        _booted = true;
        return true;
    }

    progress_store().load();
    _font_size = progress_store().font_size();

    const std::string last = progress_store().last_path();
    if (!last.empty()) {
        const size_t slash = last.find_last_of('/');
        const std::string dir = slash == std::string::npos ? std::string() : last.substr(0, slash);
        const std::string name = slash == std::string::npos ? last : last.substr(slash + 1);
        load_directory(dir, name);
    } else {
        load_directory("");
    }

    // Switched off while reading: reopen the same book at the same place, but do it
    // from run() — opening inside begin() would race the debug console.
    ESP_LOGI(kTag, "boot: reading=%d last=%s", progress_store().reading() ? 1 : 0, last.c_str());
    if (progress_store().reading() && !last.empty()) _pending_resume = last;

    _battery_percent = board().battery_percent();
    _battery_read_ms = board().millis();
    const uint32_t now = board().millis();
    _last_save_ms = now;
    // Leave the splash screen and queue the first real frame.
    if (_pending_resume.empty()) _screen = Screen::FileList;
    mark_all();
    _booted = true;
    return true;
}

void App::run() {
    while (true) {
        const uint32_t now = board().millis();

        // Deferred boot resume: begin() only records the file, run() opens it, so the
        // debug console never sees a half-initialised reader.
        if (!_pending_resume.empty()) {
            const std::string pending = _pending_resume;
            _pending_resume.clear();
            _restart_book = false;
            open_file(pending);
        }

        // Drain everything the button task queued while the panel was busy:
        // several taps collapse into one state change and one refresh.
        Gesture gesture = Gesture::None;
        while (pop_gesture(gesture)) on_gesture(gesture);

        if (_screen == Screen::Reader && now - _last_save_ms >= 30000) {
            save_progress(true);
        }

        if (now - _battery_read_ms >= 60000) {
            _battery_read_ms = now;
            const int percent = board().battery_percent();
            if (percent != _battery_percent) {
                _battery_percent = percent;
                mark(kHeaderPart);
            }
        }

        if (_dirty_parts == 0) {
            // Idle: advance the background page counter.
            step_counting();
        }
        if (_dirty_parts != 0) render();
        board().delay_ms(10);
    }
}

/* ---------------------------- file manager ------------------------------- */

void App::load_directory(const std::string& relative, const std::string& select_name) {
    _dir = relative;
    _entries.clear();
    _cursor = 0;
    _scroll = 0;

    if (!relative.empty()) {
        FileEntry parent;
        parent.name = "..";
        parent.is_dir = true;
        parent.parent = true;
        _entries.push_back(parent);
    }

    std::string full = kMountPoint;
    if (!relative.empty()) full += "/" + relative;
    DIR* dir = opendir(full.c_str());
    if (dir != nullptr) {
        std::vector<FileEntry> dirs;
        std::vector<FileEntry> files;
        dirent* item = nullptr;
        while ((item = readdir(dir)) != nullptr) {
            const std::string name = item->d_name;
            if (name.empty() || name[0] == '.') continue;
            std::string path = full + "/" + name;
            struct stat info = {};
            if (stat(path.c_str(), &info) != 0) continue;
            FileEntry entry;
            entry.name = name;
            entry.size = static_cast<uint64_t>(info.st_size);
            if (S_ISDIR(info.st_mode)) {
                entry.is_dir = true;
                dirs.push_back(std::move(entry));
            } else if (has_supported_extension(name)) {
                files.push_back(std::move(entry));
            }
        }
        closedir(dir);

        auto by_name = [](const FileEntry& left, const FileEntry& right) {
            return lower_utf8(left.name) < lower_utf8(right.name);
        };
        std::sort(dirs.begin(), dirs.end(), by_name);
        std::sort(files.begin(), files.end(), by_name);
        for (auto& entry : dirs) _entries.push_back(std::move(entry));
        for (auto& entry : files) _entries.push_back(std::move(entry));
    }

    if (!select_name.empty()) {
        for (size_t i = 0; i < _entries.size(); ++i) {
            if (_entries[i].name == select_name) {
                _cursor = static_cast<int>(i);
                break;
            }
        }
    }
    mark_all();
}

void App::activate() {
    if (_entries.empty()) return;
    if (_cursor < 0) _cursor = 0;
    if (_cursor >= static_cast<int>(_entries.size())) _cursor = static_cast<int>(_entries.size()) - 1;
    const FileEntry& entry = _entries[static_cast<size_t>(_cursor)];

    if (entry.parent) {
        const size_t slash = _dir.find_last_of('/');
        const std::string parent = slash == std::string::npos ? std::string() : _dir.substr(0, slash);
        const std::string name = slash == std::string::npos ? _dir : _dir.substr(slash + 1);
        load_directory(parent, name);
        return;
    }
    if (entry.is_dir) {
        load_directory(_dir.empty() ? entry.name : _dir + "/" + entry.name);
        return;
    }
    open_file(_dir.empty() ? entry.name : _dir + "/" + entry.name);
}

/* -------------------------------- reading -------------------------------- */

bool App::open_file(const std::string& relative) {
    struct Guard {
        volatile bool* flag;
        ~Guard() { *flag = false; }
    } guard{&_busy};
    _busy = true;
    const std::string full = std::string(kMountPoint) + "/" + relative;
    std::string error;

    // Preloader only for files that take a visible moment to open; small books
    // appear instantly and do not need the extra frames.
    struct stat probe {};
    const bool slow_open = stat(full.c_str(), &probe) == 0 && probe.st_size > 300 * 1024;
    const std::string short_name = relative.substr(relative.find_last_of('/') + 1);
    if (slow_open) show_loading(short_name, "reading file", 15);

    _source = open_file_source(full, &error);
    if (!_source) {
        std::string hex;
        for (unsigned char c : full) {
            char byte[8];
            std::snprintf(byte, sizeof(byte), "%02X ", c);
            hex += byte;
        }
        ESP_LOGE("App", "open failed '%s' len=%u err=%s hex=%s", full.c_str(),
                 static_cast<unsigned>(full.size()), error.c_str(), hex.c_str());
        set_message("Cannot open", error.empty() ? full : error, Screen::FileList);
        return false;
    }

    const Format format = format_from_name(relative);
    if (slow_open) show_loading(short_name, "parsing", 55);
    _doc = open_document(format, std::move(_source), &error);
    if (!_doc) {
        // Self-healing: a file the parser rejects (usually a lying encoding
        // declaration or broken XML) is rewritten as UTF-8 and offered back.
        if (format == Format::Txt || format == Format::Markdown || format == Format::Fb2) {
            std::string repaired;
            if (repair_file(relative, &repaired)) {
                _question_title = "File repaired";
                _question_body = "The file did not open: its encoding was broken.\n"
                                 "A repaired UTF-8 copy was written.\n"
                                 "The broken file will be kept as\n"
                                 "broken-<name>. Replace the original?";
                _question_file = relative;
                _question_repaired = repaired;
                show_question();
                return false;
            }
        }
        set_message("Format error", error.empty() ? "cannot read the file" : error, Screen::FileList);
        return false;
    }
    if (slow_open) show_loading(short_name, "layout", 85);

    _file = relative;
    _paginator = std::make_unique<Paginator>(*_doc, reader_metrics());
    ESP_LOGD(kTag, "paginator ready");
    _history.clear();
    _checkpoints.clear();
    _checkpoints.push_back(Checkpoint{PagePos{}, 0});
    _page_index = 1;
    _end_of_book = false;
    _pages_since_save = 0;

    const ProgressEntry* saved = _restart_book ? nullptr : progress_store().find_valid(relative);
    ESP_LOGD(kTag, "entry: %p", static_cast<const void*>(saved));
    ESP_LOGI(kTag, "resume %s: saved=%p token=%llu page=%d layout=%u current=%u", relative.c_str(),
             static_cast<const void*>(saved),
             saved ? static_cast<unsigned long long>(saved->token) : 0ull, saved ? saved->page : -1,
             saved ? saved->layout : 0u, layout_signature());
    bool restored_index = false;
    if (saved != nullptr) {
        if (saved->token != 0 || saved->char_offset != 0) {
            _paginator->seek(PagePos{saved->token, saved->char_offset});
        } else {
            _paginator->restart();
        }
        // The saved page number is exact for the layout it was taken in; after a
        // font size change it must be recounted (below).
        if (saved->page > 0 && saved->layout == layout_signature()) {
            _page_index = saved->page;
            restored_index = true;
        }
    } else {
        _paginator->restart();
    }

    ESP_LOGD(kTag, "seek done");
    if (!_paginator->next(_page)) {
        set_message("Empty file", "no text to display", Screen::FileList);
        _paginator.reset();
        _doc.reset();
        return false;
    }

    if (saved != nullptr && !restored_index) {
        // No usable page number (old entry or another layout): walk from the start
        // to the saved position, which also snaps the page onto the layout grid.
        PageView containing;
        const PagePos target{saved->token, saved->char_offset};
        ESP_LOGD(kTag, "walk start");
        if (walk_to_position(target, containing)) {
            _page = std::move(containing);
        }
    }

    ESP_LOGD(kTag, "page ready");
    _screen = Screen::Reader;
    // Remember that a book was open, so a power cycle reopens it in place.
    progress_store().set_reading(true);
    save_progress(true);
    start_counting();
    mark_all();
    return true;
}

void App::close_reader() {
    ESP_LOGD(kTag, "close_reader screen=%d", static_cast<int>(_screen));
    if (_screen == Screen::Reader) {
        save_progress(true);
        progress_store().set_reading(false);
        progress_store().save();
    }
    _paginator.reset();
    _doc.reset();
    _page = PageView();
    _history.clear();
    _checkpoints.clear();

    const size_t slash = _file.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : _file.substr(0, slash);
    const std::string name = slash == std::string::npos ? _file : _file.substr(slash + 1);
    _screen = Screen::FileList;
    load_directory(dir, name);
}

namespace {

bool position_before(const PagePos& left, const PagePos& right) {
    if (left.token != right.token) return left.token < right.token;
    return left.char_offset < right.char_offset;
}

} // namespace

// Walks the document in the current layout and finds the page that contains
// `position`, filling the back-navigation history and the page number on the way.
// A single pass keeps content, numbering and history consistent after a font
// size change (previously the number restarted from a checkpoint and "previous
// page" could land on an overlapping page).
bool App::walk_to_position(const PagePos& position, PageView& containing) {
    if (!_paginator) return false;

    Checkpoint base;
    bool have_base = false;
    for (const Checkpoint& checkpoint : _checkpoints) {
        if (!position_before(position, checkpoint.position)) {
            base = checkpoint;
            have_base = true;
        } else {
            break;
        }
    }

    if (!_paginator->seek(have_base ? base.position : PagePos{})) {
        if (!_paginator->restart()) return false;
    }

    _history.clear();
    int index = have_base ? base.index : 0;
    bool found = false;
    bool first_walked = true;
    for (int guard = 0; guard < kMaxWalkPages; ++guard) {
        if (!_paginator->next(containing)) break;
        ++index;

        // Seed a checkpoint at the first page of the window and then every
        // kCheckpointStep pages, so later walks (and page-up windows) start close.
        if (first_walked || index % kCheckpointStep == 0) {
            add_checkpoint(containing.start);
            first_walked = false;
        }

        if (position_before(position, containing.end)) {
            found = true;
            break;
        }
        _history.push_back(containing.start);
        if (_history.size() > kMaxHistory) _history.erase(_history.begin());
    }

    _page_index = std::max(1, index);
    return found;
}

void App::snap_to_layout(const PagePos& position) {
    // Snap so pages stay on the paginator's grid; otherwise "previous page" would
    // jump to a page that overlaps the current one. Checkpoints are kept: their
    // positions are still valid, so the walk stays bounded.
    PageView containing;
    if (walk_to_position(position, containing)) {
        _page = std::move(containing);
        // The paginator already sits right after this page, as in the normal flow.
    } else if (_paginator) {
        // Fallback: keep the old position, drop the history.
        if (!_paginator->seek(position)) _paginator->restart();
        PageView page;
        if (_paginator->next(page)) _page = std::move(page);
        _history.clear();
    }
}

// Extends the back history one window at a time, so unlimited page-up stays
// cheap even after a font size change invalidated every page start.
//
// Only pages that END at or before the current page are collected: a page that
// merely starts earlier but reaches past the current position overlaps it, and
// "page up" would keep landing on the same content.
bool App::extend_history_backwards() {
    if (!_paginator) return false;

    const PagePos current = _page.start;
    const Checkpoint* base = nullptr;
    for (const Checkpoint& checkpoint : _checkpoints) {
        if (position_before(checkpoint.position, current)) {
            base = &checkpoint;
        } else {
            break;
        }
    }
    if (base == nullptr) return false;

    if (!_paginator->seek(base->position)) return false;

    std::vector<PagePos> collected;
    PageView page;
    for (int guard = 0; guard < kMaxWalkPages; ++guard) {
        if (!_paginator->next(page)) break;
        if (position_before(current, page.end)) break; // reaches past the current page
        collected.push_back(page.start);
    }

    // Restore the invariant: the paginator must sit right after the current page.
    if (_paginator->seek(current)) {
        PageView dummy;
        _paginator->next(dummy);
    } else if (!_paginator->restart()) {
        return false;
    }

    if (collected.empty()) return false;

    // Remember the earliest page of this window: the next extension starts there,
    // so repeated page-up walks stay bounded instead of rescanning the document.
    add_checkpoint(collected.front());
    _history = std::move(collected);
    return true;
}

void App::add_checkpoint(const PagePos& position) {
    size_t index = 0;
    while (index < _checkpoints.size() && position_before(_checkpoints[index].position, position)) ++index;
    if (index < _checkpoints.size() && _checkpoints[index].position == position) return;
    _checkpoints.insert(_checkpoints.begin() + static_cast<std::ptrdiff_t>(index), Checkpoint{position, 0});
    // Keep the oldest entry (document start) plus a bounded recent set.
    while (_checkpoints.size() > 512) {
        _checkpoints.erase(_checkpoints.begin() + 1);
    }
}

void App::turn_page(int delta) {
    if (!_paginator || delta == 0) return;
    PageView page;
    if (delta > 0) {
        if (!_paginator->next(page)) {
            if (!_end_of_book) {
                _end_of_book = true;
                mark(kFooterPart);
            }
            return;
        }
        _history.push_back(_page.start);
        if (_history.size() > kMaxHistory) _history.erase(_history.begin());
        _page = std::move(page);
        ++_page_index;
        ++_pages_since_save;
        _end_of_book = _page.at_end;
        // Document position of this page: valid across font size changes.
        if (_page_index % kCheckpointStep == 0) add_checkpoint(_page.start);
        // Every page turn is persisted: a power cycle must not lose the last pages.
        save_progress(true);
    } else {
        if (_history.empty() && !extend_history_backwards()) return;
        if (_history.empty()) return;
        const PagePos target = _history.back();
        if (!_paginator->seek(target)) return;
        if (!_paginator->next(page)) return;

        // A page whose end is past the current page would overlap it: drop the
        // whole chain and rebuild it from the checkpoints instead.
        if (position_before(_page.start, page.end)) {
            if (!extend_history_backwards()) return;
            if (_history.empty()) return;
            const PagePos previous = _history.back();
            if (!_paginator->seek(previous)) return;
            if (!_paginator->next(page)) return;
        }
        _history.pop_back();
        _page = std::move(page);
        --_page_index;
        if (_page_index < 1) _page_index = 1;
        ++_pages_since_save;
        _end_of_book = false;
        save_progress(true);
    }
    // A page turn repaints the content area and both status bars; the panel
    // gets one GL16 pass instead of a GC16 flash (see render()).
    mark_all();
}

void App::change_font(int delta) {
    const int next = std::max(0, std::min(font_size_count() - 1, _font_size + delta));
    if (next == _font_size) return;
    _font_size = next;
    progress_store().set_font_size(_font_size);
    progress_store().save();

    if (_paginator) {
        const PagePos position = _page.start;
        _paginator->set_metrics(reader_metrics());
        snap_to_layout(position);
        // Page numbers depend on the layout: recount from scratch.
        start_counting();
    }
    mark_all();
}

LayoutMetrics App::reader_metrics() const {
    LayoutMetrics metrics;
    metrics.x = kReaderMargin;
    metrics.width = board().width() - kReaderMargin * 2;
    metrics.y = kHeaderHeight + 8;
    metrics.height = board().height() - kHeaderHeight - kFooterHeight - 12;
    metrics.size_index = _font_size;
    return metrics;
}

uint32_t App::layout_signature() const {
    const LayoutMetrics metrics = reader_metrics();
    uint32_t hash = 2166136261u;
    const int values[5] = {metrics.x, metrics.y, metrics.width, metrics.height, metrics.size_index};
    for (int value : values) {
        hash ^= static_cast<uint32_t>(value);
        hash *= 16777619u;
    }
    return hash;
}

void App::save_progress(bool force) {
    if (_doc == nullptr || _file.empty()) return;
    progress_store().update(_file, _page.start, _page.permille, _page_index, layout_signature());
    const uint32_t now = board().millis();
    if (force || _pages_since_save >= 10 || now - _last_save_ms >= 30000) {
        progress_store().save();
        _pages_since_save = 0;
        _last_save_ms = now;
    }
}

/* -------------------------------- messages ------------------------------- */

void App::set_message(const std::string& title, const std::string& body, Screen back_to) {
    _message_title = title;
    _message_body = body;
    _message_back = back_to;
    _screen = Screen::Message;
    mark_all();
}

void App::handle_file_list(Gesture gesture) {
    switch (gesture) {
    case Gesture::ClickA:
        if (_cursor > 0) {
            --_cursor;
            ensure_cursor_visible();
            _row_refresh = true;
            mark(kContentPart);
        }
        break;
    case Gesture::ClickB:
        if (_cursor + 1 < static_cast<int>(_entries.size())) {
            ++_cursor;
            ensure_cursor_visible();
            _row_refresh = true;
            mark(kContentPart);
        }
        break;
    case Gesture::Chord:
        activate();
        break;
    case Gesture::FontUp:
        // Held B + tap A: jump to the beginning of the list.
        if (_cursor != 0 || _scroll != 0) {
            _cursor = 0;
            _scroll = 0;
            _row_refresh = false;
            mark(kContentPart);
        }
        break;
    case Gesture::FontDown: {
        // Held A + tap B: page down (or to the last entry when the page is short).
        if (_entries.empty()) break;
        const int last = static_cast<int>(_entries.size()) - 1;
        const int target = std::min(last, _cursor + visible_rows());
        if (target != _cursor) {
            _cursor = target;
            ensure_cursor_visible();
            _row_refresh = false;
            mark(kContentPart);
        }
        break;
    }
    case Gesture::ChordLong: {
        // Held A+B on a book: forget its saved position and open from page 1.
        if (_entries.empty()) break;
        if (_cursor < 0 || _cursor >= static_cast<int>(_entries.size())) break;
        const FileEntry& entry = _entries[static_cast<size_t>(_cursor)];
        if (entry.is_dir) {
            activate();
            break;
        }
        const std::string path = _dir.empty() ? entry.name : _dir + "/" + entry.name;
        progress_store().forget(path);
        progress_store().save();
        _restart_book = true;
        open_file(path);
        _restart_book = false;
        break;
    }
    default:
        break;
    }
}

void App::handle_reader(Gesture gesture) {
    if (_link_mode) {
        // Inside the mode the buttons navigate references, not pages.
        switch (gesture) {
        case Gesture::ClickA:
            move_link_focus(-1);
            return;
        case Gesture::ClickB:
            move_link_focus(1);
            return;
        case Gesture::Chord:
            leave_link_mode();
            return;
        case Gesture::FontUp: {
            // Hold B + tap A: scroll the note up.
            const int before_shift = _page_shift;
            const int before_h = _link_panel_h;
            if (_link_scroll > 0) _link_scroll = std::max(0, _link_scroll - 1);
            prepare_link_text();
            const bool moved = _page_shift != before_shift || _link_panel_h != before_h;
            mark(moved ? kContentPart : kPanelPart);
            return;
        }
        case Gesture::FontDown: {
            // Hold A + tap B: scroll the note down.
            const int before_shift = _page_shift;
            const int before_h = _link_panel_h;
            if (_link_scroll < _link_max_scroll) _link_scroll = std::min(_link_max_scroll, _link_scroll + 1);
            prepare_link_text();
            const bool moved = _page_shift != before_shift || _link_panel_h != before_h;
            mark(moved ? kContentPart : kPanelPart);
            return;
        }
        default:
            return;
        }
    }
    switch (gesture) {
    case Gesture::ClickA:
        turn_page(-1);
        break;
    case Gesture::ClickB:
        turn_page(1);
        break;
    case Gesture::Chord:
        close_reader();
        break;
    case Gesture::ChordLong:
        enter_link_mode();
        break;
    case Gesture::FontUp:
        change_font(1);
        break;
    case Gesture::FontDown:
        change_font(-1);
        break;
    default:
        break;
    }
}

std::vector<App::PageLink> App::page_links() const {
    std::vector<PageLink> links;
    for (size_t line = 0; line < _page.lines.size(); ++line) {
        for (const LinkView& link : _page.lines[line].links) {
            PageLink entry;
            entry.line = static_cast<int>(line);
            entry.note = link.note;
            entry.x = link.x;
            entry.width = link.width;
            // The reference label as printed in the book ("[1]"), not our note index.
            if (link.para < _page.paragraphs.size()) {
                const Paragraph& paragraph = _page.paragraphs[link.para];
                if (link.run < paragraph.runs.size()) {
                    const std::string& text = paragraph.runs[link.run].text;
                    if (link.begin < text.size() && link.end <= text.size()) {
                        entry.label = text.substr(link.begin, link.end - link.begin);
                    }
                }
            }
            links.push_back(entry);
        }
    }
    return links;
}

void App::enter_link_mode() {
    if (!_doc || _doc->note_count() == 0) {
        // Nothing to browse: say so instead of entering a dead mode.
        set_message("No references", "no footnotes were found in this book", Screen::Reader);
        _message_back = Screen::Reader;
        return;
    }
    _page_links = page_links();
    if (_page_links.empty()) {
        // The current page may simply have no references: look for the nearest page
        // that has one, first forward, then backward, and come back if there is none.
        const PagePos origin = _page.start;
        constexpr int kScanPages = 25;
        for (int step = 0; step < kScanPages && _page_links.empty(); ++step) {
            const int before = _page_index;
            turn_page(1);
            if (_page_index == before) break;
            _page_links = page_links();
        }
        if (_page_links.empty()) {
            // Back to the origin, then scan backwards.
            snap_to_layout(origin);
            for (int step = 0; step < kScanPages && _page_links.empty(); ++step) {
                const int before = _page_index;
                turn_page(-1);
                if (_page_index == before) break;
                _page_links = page_links();
            }
        }
        if (_page_links.empty()) {
            snap_to_layout(origin);
            set_message("No references", "no pages with footnotes nearby", Screen::Reader);
            _message_back = Screen::Reader;
            return;
        }
    }
    // "Hold A" must act as a modifier in this mode, not as an autorepeat stream.
    set_gesture_repeat(false);
    _link_mode = true;
    _link_focus = 0;
    _link_shown_note = 0;
    prepare_link_text();
    mark(kContentPart);
}

void App::leave_link_mode() {
    set_gesture_repeat(true);
    _link_mode = false;
    _page_shift = 0;
    _link_panel_h = 0;
    _link_focus = -1;
    _link_shown_note = 0;
    _page_links.clear();
    mark(kContentPart);
}

void App::move_link_focus(int delta) {
    if (!_link_mode) return;
    if (_page_links.empty()) {
        // The page was turned while in the mode: look for references again.
        _page_links = page_links();
        if (_page_links.empty()) {
            const int before = static_cast<int>(_page_index);
            turn_page(delta > 0 ? 1 : -1);
            if (static_cast<int>(_page_index) != before) _page_links = page_links();
        }
        _link_focus = delta > 0 ? 0 : static_cast<int>(_page_links.size()) - 1;
        prepare_link_text();
        mark(kContentPart);
        return;
    }
    int next = _link_focus + delta;
    if (next >= 0 && next < static_cast<int>(_page_links.size())) {
        const int previous_shift = _page_shift;
        const int previous_line = _link_line;
        _link_focus = next;
        prepare_link_text();
        const bool same_place = _page_shift == previous_shift && _link_line == previous_line;
        // Several references often sit on one line: then only the panel changes.
        mark(same_place ? kPanelPart : kContentPart);
        return;
    }
    // Walking past the page edge turns it and continues on the neighbouring page.
    const int before = static_cast<int>(_page_index);
    turn_page(delta > 0 ? 1 : -1);
    if (static_cast<int>(_page_index) == before) return; // no next page
    _page_links = page_links();
    if (_page_links.empty()) {
        _link_focus = -1;
        _link_panel_h = 0;
        _page_shift = 0;
        _link_lines.clear();
        _link_lines_note = 0;
        _link_scroll = 0;
    } else {
        _link_focus = delta > 0 ? 0 : static_cast<int>(_page_links.size()) - 1;
    }
    mark_all();
}

int App::focused_link_line() const {
    if (!_link_mode || _link_focus < 0) return -1;
    if (_link_focus >= static_cast<int>(_page_links.size())) return -1;
    return _page_links[static_cast<size_t>(_link_focus)].line;
}

void App::render_link_marks() {
    if (!_link_mode) return;
    auto& gfx = board().display();
    const int line_index = focused_link_line();
    if (line_index < 0 || line_index >= static_cast<int>(_page.lines.size())) return;
    const LineView& raw_line = _page.lines[static_cast<size_t>(line_index)];
    // The same cursor sprite as the file manager, centred on the optical middle of
    // the text line (baseline minus half of ascent..descent), not on the box.
    LineView line = raw_line;
    line.y = static_cast<int16_t>(line.y + _page_shift);
    line.baseline = static_cast<int16_t>(line.baseline + _page_shift);
    // Use the font of the line itself (headings are larger and bold), not the base size.
    const SpanView* first_span = line.spans.empty() ? nullptr : &line.spans.front();
    const auto& font = first_span != nullptr
                           ? find_font(first_span->size_index, first_span->face)
                           : find_font(_font_size, Face::Regular);
    const int optical_center = line.baseline - (font.ascent - font.descent) / 2;
    const int left = std::max(0, line.x - kIconX);
    // +2 px optical nudge: reference markers are brackets that reach below the
    // baseline, so the ink centre sits slightly lower than the font box centre.
    const int icon_y = optical_center - 12 + 2;
    const Region region = {static_cast<int16_t>(left), static_cast<int16_t>(icon_y), 24, 24};
    gfx.fillRect(region.x, region.y, region.w, region.h, TFT_WHITE);
    ui::draw_icon(gfx, "cursor", left, icon_y, TFT_BLACK, TFT_WHITE);
    board().touch(region);
}

void App::prepare_link_text() {
    if (!_link_mode || _link_focus < 0 || _link_focus >= static_cast<int>(_page_links.size())) {
        // No reference in focus (a page without links): drop the panel geometry so the
        // shade and the panel disappear instead of staying from the previous page.
        _link_panel_h = 0;
        _page_shift = 0;
        _link_lines.clear();
        _link_lines_note = 0;
        _link_scroll = 0;
        _link_max_scroll = 0;
        return;
    }
    const int width = board().width();
    const int height = board().height();
    const PageLink& link = _page_links[static_cast<size_t>(_link_focus)];
    const auto& body = find_font(0, Face::Regular);
    const auto& heading = find_font(0, Face::Bold);

    constexpr int kPad = 24;
    constexpr int kPanelMin = 140;
    // A long note gets half the screen: the panel is never taller (the page keeps the
    // other half) and never shorter than that for long notes.
    const int panel_max = std::max(kPanelMin, (height - kFooterHeight) / 2);

    std::string text;
    if (link.note == 0) {
        text = "Footnote text not found.";
    } else {
        text = _doc != nullptr ? _doc->note(link.note - 1) : std::string();
        if (text.empty()) text = "Footnote text is empty.";
    }

    const int text_width = width - 2 * kPad;
    if (_link_lines_note != link.note) {
        _link_lines.clear();
        size_t pos = 0;
        while (pos < text.size()) {
            size_t take = 0;
            while (pos + take < text.size()) {
                const size_t space = text.find(' ', pos + take);
                const size_t candidate = (space == std::string::npos ? text.size() : space + 1) - pos;
                if (measure_text(body, text.substr(pos, candidate)) > text_width) break;
                take = candidate;
                if (space == std::string::npos) break;
            }
            if (take == 0) take = text.size() - pos;
            _link_lines.push_back(text.substr(pos, take));
            pos += take;
            while (pos < text.size() && text[pos] == ' ') ++pos;
        }
        _link_scroll = 0;
        _link_lines_note = link.note;
    }

    // Panel height follows the content for SHORT notes only. A note that needs more
    // than half the screen always gets half the screen: a squeezed panel made long
    // footnotes painful to read and to scroll.
    const int header_h = 4 + 12 + heading.line_height + 10 + 10 + 10;
    const int body_h = static_cast<int>(_link_lines.size()) * body.line_height;
    const int wanted_h = header_h + body_h + 18;
    const int half_screen = (height - kFooterHeight) / 2;
    int panel_h = wanted_h < half_screen ? std::max(kPanelMin, wanted_h) : half_screen;
    if (panel_h > panel_max) panel_h = panel_max;

    // The focused reference must always stay visible. The panel is not shrunk for
    // that: the page is shifted instead, and lines that leave the visible band are
    // skipped when drawing. The paginator is untouched, so leaving the mode restores
    // the normal reading position.
    _page_shift = 0;
    if (link.line >= 0 && link.line < static_cast<int>(_page.lines.size()) && !_page.lines.empty()) {
        const LineView& line = _page.lines[static_cast<size_t>(link.line)];
        const int panel_top = height - kFooterHeight - panel_h;
        // Anchor on the line's BOX, not its baseline: the renderer clips any line whose
        // box touches the panel, and anchoring on the baseline clipped the focused line
        // itself, so the cursor pointed at empty paper.
        const int box_bottom_limit = panel_top - 6;
        if (line.y + line.height > box_bottom_limit) {
            _page_shift = box_bottom_limit - (line.y + line.height);
        }
        const int header_limit = kHeaderHeight + 4;
        if (line.y + _page_shift < header_limit) {
            _page_shift = header_limit - line.y;
        }
    }
    _link_panel_h = panel_h;

    _link_line = link.line;
    const int top = height - kFooterHeight - panel_h;
    const int available = height - kFooterHeight - 8 - (top + header_h);
    _link_visible = std::max(1, (available - body.ascent) / body.line_height);
    _link_max_scroll = std::max(0, static_cast<int>(_link_lines.size()) - _link_visible);
}

void App::render_link_panel() {
    if (!_link_mode) return;
    auto& gfx = board().display();
    const int width = board().width();
    const int height = board().height();
    if (_link_focus < 0 || _link_focus >= static_cast<int>(_page_links.size())) return;
    const PageLink& link = _page_links[static_cast<size_t>(_link_focus)];
    _link_shown_note = link.note;

    // Notes are book text: the smallest reading face (24 px) instead of the 14 px
    // chrome face, which is unreadable on e-ink.
    const auto& body = find_font(0, Face::Regular);
    const auto& heading = find_font(0, Face::Bold);
    const auto& meta = find_ui_font(0);

    std::string text;
    std::string title;
    if (link.note == 0) {
        text = "Footnote text not found.";
    } else {
        title = _doc != nullptr ? _doc->note_title(link.note - 1) : std::string();
        text = _doc != nullptr ? _doc->note(link.note - 1) : std::string();
        if (text.empty()) text = "Footnote text is empty.";
    }

    // Panel paddings are half of the reader margins (48 / 2 = 24).
    constexpr int kPad = 24;
    const int panel_h = _link_panel_h > 0 ? _link_panel_h : std::max(180, height * 2 / 5);
    const int top = height - kFooterHeight - panel_h;
    gfx.fillRect(0, top, width, panel_h, TFT_WHITE);
    // 4 px black border: the panel must read as a separate layer, not as page text.
    constexpr int kBorder = 4;
    gfx.fillRect(0, top, width, kBorder, TFT_BLACK);
    ESP_LOGD(kTag, "panel geom h=%d top=%d lines=%u focus=%d", panel_h, top,
             static_cast<unsigned>(_link_lines.size()), _link_focus);
    // The panel is drawn after the page, so it has to be registered as a dirty
    // region or the driver would not send it to the panel.
    board().touch(Region{0, static_cast<int16_t>(top), width, static_cast<int16_t>(panel_h)});

    char counter[32];
    std::snprintf(counter, sizeof(counter), "%d/%d", _link_focus + 1,
                  static_cast<int>(_page_links.size()));
    const int counter_width = measure_text(meta, counter);

    // Header: the reference label and the note's own heading.
    int y = top + kBorder + 12;
    // Prefer the label as printed in the book: internal indices count every note in
    // the file, so "note_1" may be index 2 and the header would look wrong.
    std::string header = link.label.empty() ? std::string() : link.label;
    if (header.empty()) {
        header = "[";
        header += std::to_string(link.note);
        header += "]";
    }
    if (!title.empty()) header += " " + title;
    const std::string shown_header =
        ui::elide(heading, header, width - 2 * kPad - counter_width - 12);
    ui::draw_text(gfx, kPad, y + heading.ascent, shown_header, heading,
                  ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
    ui::draw_text(gfx, width - 10 - counter_width, y + meta.ascent, counter, meta,
                  ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
    y += heading.line_height + 10;
    gfx.drawFastHLine(kPad, y, width - 2 * kPad, TFT_BLACK);
    y += 10;

    // Wrap the body once per note: scrolling then only picks a window. The cache is
    // keyed by the note number, otherwise a short note would inherit the previous
    // (long) one and show a scrollbar it does not need.
    const int text_width = width - 2 * kPad;
    if (_link_lines_note != link.note) {
        _link_lines.clear();
        size_t pos = 0;
        while (pos < text.size()) {
            size_t take = 0;
            while (pos + take < text.size()) {
                const size_t space = text.find(' ', pos + take);
                const size_t candidate = (space == std::string::npos ? text.size() : space + 1) - pos;
                if (measure_text(body, text.substr(pos, candidate)) > text_width) break;
                take = candidate;
                if (space == std::string::npos) break;
            }
            if (take == 0) take = text.size() - pos;
            _link_lines.push_back(text.substr(pos, take));
            pos += take;
            while (pos < text.size() && text[pos] == ' ') ++pos;
        }
        _link_scroll = 0;
        _link_lines_note = link.note;
    }

    const int available = height - kFooterHeight - 8 - (y + 0);
    _link_visible = std::max(1, (available - body.ascent) / body.line_height);
    _link_max_scroll = std::max(0, static_cast<int>(_link_lines.size()) - _link_visible);
    if (_link_scroll > _link_max_scroll) _link_scroll = _link_max_scroll;
    if (_link_scroll < 0) _link_scroll = 0;

    ESP_LOGD(kTag, "panel file=%s notes=%u focus=%d/%d note=%u lines=%u", _file.c_str(),
             static_cast<unsigned>(_doc ? _doc->note_count() : 0), _link_focus,
             static_cast<int>(_page_links.size()), link.note, static_cast<unsigned>(_link_lines.size()));
    int line_y = y + body.ascent;
    for (int i = 0; i < _link_visible; ++i) {
        const int index = _link_scroll + i;
        if (index >= static_cast<int>(_link_lines.size())) break;
        ui::draw_text(gfx, kPad, line_y, _link_lines[static_cast<size_t>(index)], body,
                      ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
        line_y += body.line_height;
    }

    // A thin bar on the right shows that the note is longer than the panel.
    if (_link_max_scroll > 0) {
        const int bar_x = width - 8;
        const int bar_top = top + 14;
        const int bar_h = panel_h - 22;
        const int thumb = std::max(20, bar_h * _link_visible /
                                             static_cast<int>(_link_lines.size()));
        const int offset = (bar_h - thumb) * _link_scroll / std::max(1, _link_max_scroll);
        gfx.fillRect(bar_x, bar_top + offset, 4, thumb, TFT_BLACK);
    }
}
void App::handle_message(Gesture gesture) {
    if (gesture != Gesture::Chord) return;
    if (_message_back == Screen::Reader && _doc) {
        _screen = Screen::Reader;
        mark_all();
        return;
    }
    if (!board().sd_mounted()) {
        if (!board().mount_sd()) {
            set_message("SD card not found", "Insert a card with books and press A+B", Screen::FileList);
            return;
        }
        progress_store().load();
        _font_size = progress_store().font_size();
    }
    _screen = Screen::FileList;
    load_directory(_dir);
}

void App::on_gesture(Gesture gesture) {
    ESP_LOGD(kTag, "gesture %d on screen %d", static_cast<int>(gesture), static_cast<int>(_screen));
    switch (_screen) {
    case Screen::FileList:
        handle_file_list(gesture);
        break;
    case Screen::Reader:
        handle_reader(gesture);
        break;
    case Screen::Message:
        handle_message(gesture);
        break;
    case Screen::Loading:
        // Opening is synchronous: gestures are queued and handled right after.
        break;
    case Screen::Question:
        handle_question(gesture);
        break;
    case Screen::Booting:
        break;
    }
}

void App::inject(Gesture gesture) {
    post_gesture(gesture);
}

bool App::open_path(const std::string& relative) {
    if (!board().sd_mounted() && !board().mount_sd()) return false;
    return open_file(relative);
}

bool App::open_index(int index) {
    if (index < 0 || index >= static_cast<int>(_entries.size())) return false;
    const FileEntry& entry = _entries[static_cast<size_t>(index)];
    if (entry.is_dir) return false;
    return open_file(_dir.empty() ? entry.name : _dir + "/" + entry.name);
}

void App::set_open_limit(uint64_t bytes) {
    g_open_limit_override = bytes;
    load_directory(_dir);
}

void App::start_counting() {
    if (_file.empty() || !_paginator) return;
    const std::string full = std::string(kMountPoint) + "/" + _file;
    _total_pages = 0;

    // The counter opens a second copy of the document. TXT/MD/FB2 decode the whole
    // file into memory (and CP1251 doubles it into UTF-8), so a second copy can
    // exhaust the heap and abort — for those formats the total is not counted.
    const Format format = format_from_name(_file);
    if (format == Format::Txt || format == Format::Markdown || format == Format::Fb2) {
        _counting = false;
        return;
    }
    struct stat info {};
    const uint64_t size = stat(full.c_str(), &info) == 0 ? static_cast<uint64_t>(info.st_size) : 0;
    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t needed = static_cast<size_t>(size) * 3 + (256u * 1024u);
    if (free_heap < needed) {
        ESP_LOGI(kTag, "page counter skipped: free=%u needed=%u", static_cast<unsigned>(free_heap),
                 static_cast<unsigned>(needed));
        _counting = false;
        return;
    }
    _counting = page_counter().start(full, reader_metrics(), _page.start);
}

void App::step_counting() {
    if (!_counting) return;
    // One page per idle tick: ~30 ms of work, invisible unless the user is
    // hammering the buttons (gestures are queued anyway).
    if (!page_counter().step()) {
        _total_pages = page_counter().count();
        _counting = false;
        // Release the second copy of the book: the counter is not needed anymore.
        page_counter().reset();
        mark(kFooterPart);
        return;
    }
    if (page_counter().anchor_found() && page_counter().anchor() == _page.start) {
        const int index = page_counter().anchor_index();
        if (index > 0 && index != _page_index) {
            _page_index = index;
            mark(kFooterPart);
        }
    }
}

void App::request_clean() {
    mark_all();
    board().request_full_refresh();
    _force_clean = true;
}

void App::reset_progress() {
    progress_store().set_reading(false);
    if (!_file.empty()) progress_store().update(_file, PagePos{}, 0, 1, layout_signature());
    progress_store().save();
    _paginator.reset();
    _doc.reset();
    _history.clear();
    _page = PageView();
    _screen = Screen::FileList;
    load_directory("");
}

App& app() {
    static App instance;
    return instance;
}

std::string App::dump_text() {
    if (_busy) return "rdr busy\n";
    std::string out;
    if (_screen == Screen::FileList) {
        out += "[" + std::string("/") + _dir + "] cursor=" + std::to_string(_cursor) +
               " scroll=" + std::to_string(_scroll) + " rows=" + std::to_string(visible_rows()) + "\n";
        for (size_t i = 0; i < _entries.size(); ++i) {
            out += (static_cast<int>(i) == _cursor) ? "> " : "  ";
            if (_entries[i].is_dir && !_entries[i].parent) {
                out += "[" + _entries[i].name + "]";
            } else {
                out += _entries[i].name;
            }
            out += " [" + human_size(_entries[i].size) + (_entries[i].size > max_openable_size(_entries[i].name) ? " LOCKED" : "") + "]";
            out += "\n";
        }
        return out;
    }
    if (_screen == Screen::Message) {
        return "[" + _message_title + "] " + _message_body + "\n";
    }
    out += "[" + _file + "] page " + std::to_string(_page_index) + " hist=" + std::to_string(_history.size()) + " total=" + std::to_string(_total_pages) + ( _counting ? "+" : "") + " font=" + std::to_string(_font_size) + " pos=" + std::to_string(static_cast<unsigned long long>(_page.start.token)) + "/" + std::to_string(_page.start.char_offset) + " " +
           std::to_string(_page.permille / 10) + "%\n";
    for (const auto& line : _page.lines) {
        for (const auto& span : line.spans) {
            if (span.para >= _page.paragraphs.size()) continue;
            const auto& paragraph = _page.paragraphs[span.para];
            if (span.run >= paragraph.runs.size()) continue;
            const std::string& text = paragraph.runs[span.run].text;
            if (span.begin >= text.size() || span.end > text.size()) continue;
            out.append(text, span.begin, span.end - span.begin);
        }
        if (line.hyphenated) out += "-";
        out += "\n";
    }
    return out;
}

/* -------------------------------- rendering ------------------------------ */

void App::mark(uint8_t parts) {
    _dirty_parts |= parts;
}

void App::mark_all() {
    _dirty_parts = kAllParts;
    _full_repaint = true;
    _row_refresh = false;
}

Region App::header_region() const {
    return Region{0, 0, board().width(), kHeaderHeight};
}

Region App::content_region() const {
    // The file manager has no bottom bar any more, so its list runs to the edge.
    const int footer = _screen == Screen::Reader ? kFooterHeight : 0;
    return Region{0, kHeaderHeight, board().width(), board().height() - kHeaderHeight - footer};
}

Region App::footer_region() const {
    return Region{0, board().height() - kFooterHeight, board().width(), kFooterHeight};
}

int App::list_row_height() const {
    return kRowPitch;
}

int App::visible_rows() const {
    const int top = kHeaderHeight + kListTopGap;
    const int bottom = board().height() - (_screen == Screen::Reader ? kFooterHeight : 0);
    return std::max(1, (bottom - top) / kRowPitch);
}

void App::ensure_cursor_visible() {
    const int rows = visible_rows();
    if (_cursor < _scroll) _scroll = _cursor;
    if (_cursor >= _scroll + rows) _scroll = _cursor - rows + 1;
    if (_scroll < 0) _scroll = 0;
    const int max_scroll = std::max(0, static_cast<int>(_entries.size()) - rows);
    if (_scroll > max_scroll) _scroll = max_scroll;
}

Region App::row_region(int row) const {
    const int top = kHeaderHeight + kListTopGap + (row - _scroll) * kRowPitch;
    return Region{0, top, board().width(), kRowHeight};
}

// The selection cursor is a pair of small triangles at both ends of the row
// ("▶ text.txt 999K ◀"): cheaper for the panel than an inverted row, and only
// two narrow strips need to be refreshed when the cursor moves.
Region App::cursor_region(int row) const {
    const int top = kHeaderHeight + kListTopGap + (row - _scroll) * kRowPitch;
    return Region{0, top, kCursorStrip, kRowHeight};
}

void App::render() {
    struct BusyGuard {
        volatile bool* flag;
        ~BusyGuard() { *flag = false; }
    } busy_guard{&_busy};
    _busy = true;
    auto& gfx = board().display();
    gfx.waitDisplay();
    board().begin_frame();

    if (_screen == Screen::Booting) {
        gfx.fillScreen(TFT_WHITE);
        render_booting();
        board().request_full_refresh();
        board().commit_frame(EpdMode::Quality);
        _dirty_parts = 0;
        _full_repaint = false;
        _row_refresh = false;
        return;
    }
    if (_screen == Screen::Question) {
        gfx.fillScreen(TFT_WHITE);
        render_question();
        board().request_full_refresh();
        board().commit_frame(EpdMode::Quality);
        _dirty_parts = 0;
        _full_repaint = false;
        _row_refresh = false;
        return;
    }
    if (_screen == Screen::Loading) {
        // Discreet steps only: each step is one DU frame (~330 ms), so a book that
        // opens fast never shows a flickering animation.
        gfx.fillScreen(TFT_WHITE);
        render_loading();
        board().request_full_refresh();
        board().commit_frame(EpdMode::Fast);
        _dirty_parts = 0;
        _full_repaint = false;
        _row_refresh = false;
        return;
    }
    if (_screen == Screen::Message) {
        gfx.fillScreen(TFT_WHITE);
        render_message();
        board().request_full_refresh();
        board().commit_frame(EpdMode::Quality);
        _dirty_parts = 0;
        _full_repaint = false;
        _row_refresh = false;
        _prev_cursor = _cursor;
        _prev_scroll = _scroll;
        return;
    }

    const bool full = _full_repaint;


    if (full) {
        gfx.fillScreen(TFT_WHITE);
    }

    if (full || (_dirty_parts & kHeaderPart)) {
        if (!full) {
            const Region region = header_region();
            gfx.fillRect(region.x, region.y, region.w, region.h, TFT_WHITE);
            board().touch(region);
        }
        render_header();
    }

    if (full || (_dirty_parts & kContentPart)) {
        // A cursor move only redraws the two little cursor marks, not the rows:
        // no black fill, minimal ink, minimal waveform time.
        const bool cursor_only = !full && _screen == Screen::FileList && _row_refresh &&
                                 _prev_scroll == _scroll && _prev_cursor >= 0;
        if (cursor_only) {
            const int rows = visible_rows();
            const int previous_row = _prev_cursor - _scroll;
            const int current_row = _cursor - _scroll;
            int first = previous_row < current_row ? previous_row : current_row;
            int last = previous_row < current_row ? current_row : previous_row;
            if (first < 0) first = 0;
            if (last >= rows) last = rows - 1;
            if (last >= first) {
                // Rows are repainted completely: the selected row is drawn in bold and
                // with the filled icon, so the old and the new row both change weight.
                for (int row = first; row <= last; ++row) {
                    const int top = kHeaderHeight + kListTopGap + row * kRowPitch;
                    const Region region = {0, top, board().width(), kRowHeight};
                    gfx.fillRect(region.x, region.y, region.w, region.h, TFT_WHITE);
                    render_content_row(row);
                    board().touch(region);
                }
                if (current_row >= 0 && current_row < rows) render_cursor_marks(current_row);
            }
        } else {
            const Region region = content_region();
            gfx.fillRect(region.x, region.y, region.w, region.h, TFT_WHITE);
            render_content();
            board().touch(region);
        }
    }

    // The bottom bar exists only in the reader: the file manager uses the full height.
    if (!full && _link_mode && (_dirty_parts & kPanelPart) != 0 &&
        (_dirty_parts & kContentPart) == 0) {
        // Scrolling inside a note changes only the panel: repaint just that strip.
        // If the panel geometry changes on the way (a shorter note), the strip alone is
        // not enough — the old border would stay, so fall through to a content repaint.
        const int before_h = _link_panel_h;
        const int before_shift = _page_shift;
        if (_link_panel_h == 0) prepare_link_text();
        if (_link_panel_h != before_h || _page_shift != before_shift) {
            mark(kContentPart);
        } else {
            const int panel_h = _link_panel_h;
            const int top = board().height() - kFooterHeight - panel_h;
            const Region region = {0, static_cast<int16_t>(top), board().width(),
                                   static_cast<int16_t>(panel_h)};
            gfx.fillRect(region.x, region.y, region.w, region.h, TFT_WHITE);
            render_link_panel(); // registers its own dirty region
            board().commit_frame(EpdMode::Fast);
            _dirty_parts = 0;
            _full_repaint = false;
            return;
        }
    }

    if ((full || (_dirty_parts & kFooterPart)) && _screen == Screen::Reader) {
        if (!full) {
            const Region region = footer_region();
            gfx.fillRect(region.x, region.y, region.w, region.h, TFT_WHITE);
            board().touch(region);
        }
        render_footer();
    }

    // Refresh mode policy:
    //   * book pages  -> DU (Fast): ~400 ms. DU leaves residue on pixels that were
    //     mid-gray, so a GC16 pass is forced every kPagesPerClean pages to keep
    //     contrast stable (and on demand via `rdr clean`);
    //   * file manager redraws -> DU (Fast) as well;
    //   * cursor strips and small patches -> DU4 (Fastest).
    EpdMode mode = EpdMode::Fastest;
    if (full) {
        mode = EpdMode::Fast;
        // Reference mode is transient: never flash a full GC16 between link steps.
        if (_screen == Screen::Reader && !_link_mode) {
            if (_force_clean || _pages_since_clean >= kPagesPerClean) {
                _force_clean = false;
                mode = EpdMode::Quality;
                _pages_since_clean = 0;
            } else {
                ++_pages_since_clean;
            }
        }
    } else if ((_dirty_parts & (kHeaderPart | kFooterPart)) != 0) {
        // Text in the bars is replaced, not added: a short DU pulse leaves the
        // previous text visible underneath, so the bars always get a full GC16.
        mode = EpdMode::Quality;
    } else if ((_dirty_parts & kContentPart) != 0) {
        // Rows and icons change shape (fill/stroke icons), give DU its 8 frames.
        mode = EpdMode::Fast;
    }
    ESP_LOGD(kTag, "flush full=%d parts=%u mode=%d", full ? 1 : 0, _dirty_parts, static_cast<int>(mode));
    board().commit_frame(mode);

    _dirty_parts = 0;
    _full_repaint = false;
    _row_refresh = false;
    _prev_cursor = _cursor;
    _prev_scroll = _scroll;
    _busy = false;
}

void App::render_header() {
    auto& gfx = board().display();
    const int width = board().width();
    const auto& small = find_ui_font(1); // 18 px, as in the mockup
    const int baseline = kHeaderHeight - 13;

    // Bars are plain white (no grey fill), only the bottom border separates them.
    gfx.fillRect(0, 0, width, kHeaderHeight, TFT_WHITE);

    if (_screen == Screen::FileList) {
        const auto& icon = find_ui_font(1);
        (void)icon;
        ui::draw_icon(gfx, "sdcard", 8, (kHeaderHeight - 24) / 2, TFT_BLACK, kHeaderFill);
        const std::string path = "/" + _dir;
        const std::string percent_text = battery_text();
        const int percent_width = measure_text(small, percent_text);
        ui::draw_text(gfx, width - 40 - percent_width, baseline, percent_text, small,
                      ui::TextOptions{TFT_BLACK, kHeaderFill, -1});
        ui::draw_text(gfx, 40, baseline, ui::elide(small, path, width - 40 - percent_width - 48), small,
                      ui::TextOptions{TFT_BLACK, kHeaderFill, -1});
        draw_battery_icon(gfx, width - 32, (kHeaderHeight - 24) / 2);
    } else if (_screen == Screen::Reader) {
        const auto& header = find_ui_font(1, true);
        const size_t slash = _file.find_last_of('/');
        const std::string name = slash == std::string::npos ? _file : _file.substr(slash + 1);
        // Right-hand cluster shows the battery charge (the reading progress lives in
        // the footer), same as in the file manager.
        const std::string percent_text = battery_text();
        const int percent_width = measure_text(small, percent_text);
        ui::draw_text(gfx, width - 40 - percent_width, baseline, percent_text, small,
                      ui::TextOptions{TFT_BLACK, kHeaderFill, -1});
        draw_battery_icon(gfx, width - 32, (kHeaderHeight - 24) / 2);
        ui::draw_text(gfx, 12, baseline, ui::elide(header, name, width - percent_width - 60), header,
                      ui::TextOptions{TFT_BLACK, kHeaderFill, -1});
    }
    // Draw the border inside the header band: the content redraw fills from
    // y = kHeaderHeight and would otherwise erase a line drawn at that row.
    gfx.drawFastHLine(0, kHeaderHeight - 1, width, TFT_BLACK);
}

std::string App::battery_text() const {
    if (_battery_percent < 0) return "--";
    char text[8];
    const int percent = _battery_percent > 100 ? 100 : _battery_percent;
    std::snprintf(text, sizeof(text), "%d%%", percent);
    return text;
}

void App::draw_battery_icon(M5GFX& gfx, int x, int y) {
    // 24x24 sprite from the mockup; the level is picked by the charge.
    const char* name = "battery_100";
    if (_battery_percent < 0) {
        name = "battery_50"; // gauge not readable yet
    } else if (_battery_percent <= 15) {
        name = "battery_low";
    } else if (_battery_percent <= 40) {
        name = "battery_25";
    } else if (_battery_percent <= 65) {
        name = "battery_50";
    } else if (_battery_percent <= 85) {
        name = "battery_75";
    }
    ui::draw_icon(gfx, name, x, y, TFT_BLACK, kHeaderFill);
}

int App::row_baseline(int row) const {
    const auto& font = find_ui_font(2); // 24 px names
    const int y = kHeaderHeight + kListTopGap + row * kRowPitch;
    return y + (kRowHeight - font.line_height) / 2 + font.ascent;
}

void App::render_content_row(int row) {
    auto& gfx = board().display();
    const int width = board().width();
    const int index = _scroll + row;
    if (index < 0 || index >= static_cast<int>(_entries.size())) return;

    const FileEntry& entry = _entries[static_cast<size_t>(index)];
    const bool selected = index == _cursor;
    // The selected row uses the bold face: same grid, visibly heavier letters.
    const auto& body = selected ? find_ui_font(2, true) : find_ui_font(2);
    const auto& meta = find_ui_font(1, true);  // 18 px bold, uppercase format
    const auto& small_meta = find_ui_font(0);  // 14 px size
    const int top = kHeaderHeight + kListTopGap + row * kRowPitch;
    const int baseline = row_baseline(row);

    // Files above the open limits are shown greyed with a lock, exactly like the
    // mockup: the reader would refuse them.
    const bool locked = !entry.is_dir && entry.size > max_openable_size(entry.name);

    gfx.fillRect(0, top, width, kRowHeight, TFT_WHITE);
    const uint16_t ink = locked ? kLockedInk : TFT_BLACK;
    render_row_icon(row, ink);

    // Directories are shown as [Folder] without a trailing slash (the path in the
    // header keeps its slashes).
    std::string name = entry.name;
    if (entry.parent) {
        name = "[..]";
    } else if (entry.is_dir) {
        name = "[" + entry.name + "]";
    }
    const int meta_width = entry.is_dir ? 0 : 62;
    ui::draw_text(gfx, kNameX, baseline, ui::elide(body, name, width - kNameX - meta_width - 12), body,
                  ui::TextOptions{ink, TFT_WHITE, -1});

    if (!entry.is_dir) {
        std::string extension = extension_of(entry.name);
        for (char& c : extension) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        std::string size = human_size(entry.size);
        if (locked) size = "( ! ) " + size;
        const int ext_width = measure_text(meta, extension);
        const int size_width = measure_text(small_meta, size);
        const int meta_right = width - kScrollbarWidth - 8;
        ui::draw_text(gfx, meta_right - ext_width, top + 9, extension, meta,
                      ui::TextOptions{ink, TFT_WHITE, -1});
        ui::draw_text(gfx, meta_right - size_width, top + 33, size, small_meta,
                      ui::TextOptions{ink, TFT_WHITE, -1});
    }
    (void)selected;
}

// The icon of the selected row switches to the "fill" variant of the icon set,
// which is how the mockup shows selection without inverting the whole row.
const char* App::icon_name_for(const FileEntry& entry, bool locked, bool selected) const {
    if (entry.parent) return "back"; // no fill variant in the set
    if (entry.is_dir) return selected ? "folder_fill" : "folder";
    if (locked) return selected ? "file_locked_fill" : "file_locked";
    return selected ? "file_fill" : "file";
}

void App::render_row_icon(int row, uint16_t ink) {
    auto& gfx = board().display();
    const int index = _scroll + row;
    if (index < 0 || index >= static_cast<int>(_entries.size())) return;
    const FileEntry& entry = _entries[static_cast<size_t>(index)];
    const bool locked = !entry.is_dir && entry.size > max_openable_size(entry.name);
    const int top = kHeaderHeight + kListTopGap + row * kRowPitch;
    const int icon_y = top + (kRowHeight - 24) / 2;
    gfx.fillRect(kIconX, icon_y, 24, 24, TFT_WHITE);
    ui::draw_icon(gfx, icon_name_for(entry, locked, index == _cursor), kIconX, icon_y, ink, TFT_WHITE);
}

void App::render_cursor_marks(int row) {
    // The mockup moved the cursor to the left gutter only: a 24 px arrow sprite.
    auto& gfx = board().display();
    const int top = kHeaderHeight + kListTopGap + row * kRowPitch;
    ui::draw_icon(gfx, "cursor", 0, top + (kRowHeight - 24) / 2, TFT_BLACK, TFT_WHITE);
}

void App::render_scrollbar(bool content_area) {
    auto& gfx = board().display();
    const int width = board().width();
    const int top = content_area ? kHeaderHeight + kListTopGap : kHeaderHeight;
    const int bottom = board().height() - kFooterHeight;
    const int track = bottom - top;
    if (track <= 8) return;

    int thumb = track;
    int offset = 0;
    if (_screen == Screen::Reader) {
        // Progress through the book.
        const int permille = _page.permille > 1000 ? 1000 : static_cast<int>(_page.permille);
        thumb = std::max(12, track * 100 / std::max(1, static_cast<int>(permille) + 100));
        thumb = std::max(12, track * 1000 / std::max(1000, static_cast<int>(_page.permille) + 1));
        offset = (track - thumb) * permille / 1000;
    } else {
        const int total = static_cast<int>(_entries.size());
        const int rows = visible_rows();
        if (total <= rows) return; // nothing to scroll
        thumb = std::max(12, track * rows / total);
        const int scrollable = total - rows;
        offset = scrollable > 0 ? (track - thumb) * _scroll / scrollable : 0;
    }
    ui::fill_level(gfx, width - kScrollbarWidth, top + offset, kScrollbarWidth, thumb, kScrollbarLevel);
}

void App::render_content() {
    auto& gfx = board().display();
    const int width = board().width();
    const int height = board().height();

    if (_screen == Screen::FileList) {
        const int rows = visible_rows();
        ensure_cursor_visible();
        for (int row = 0; row < rows; ++row) render_content_row(row);
        // A full content redraw also has to restore the cursor triangles; the
        // cursor-only path draws them separately.
        const int cursor_row = _cursor - _scroll;
        if (cursor_row >= 0 && cursor_row < rows) render_cursor_marks(cursor_row);
        render_scrollbar(true);

        if (_entries.empty()) {
            const auto& body = find_ui_font(3); // 28 px, same as dialogs
            ui::draw_text(gfx, 22, kHeaderHeight + 8 + body.ascent + 3, "No books in this folder", body,
                          ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
        }
        return;
    }

    if (_screen != Screen::Reader) return;

    // In reference mode the page is shifted up so the focused link stays visible
    // above the note panel; the layout itself is untouched. The shift is derived from
    // the focused link, so it has to be recomputed before the page is drawn.
    if (_link_mode) prepare_link_text();
    const int shift = _link_mode ? _page_shift : 0;
    const int link_line = _link_mode ? focused_link_line() : -1;

    // "Translucent" shade above the panel: a flat grey strip painted UNDER the text, so
    // the lines that cross it stay readable (black on grey) instead of being covered.
    ESP_LOGD(kTag, "content: mode=%d focus=%d links=%u panel_h=%d", _link_mode ? 1 : 0,
             _link_focus, static_cast<unsigned>(_page_links.size()), _link_panel_h);
    for (const LineView& line : _page.lines) {
        // No copy: LineView owns two vectors, and copying it per line per frame was a
        // real allocation load. The shift is applied at the drawing calls instead.
        const int line_y = line.y + shift;
        const int base = line.baseline + shift;
        // The page is shifted while browsing notes: drop the lines that leave the
        // visible band instead of drawing them over the header or under the panel.
        if (_link_mode && _link_panel_h > 0) {
            const int panel_top = board().height() - kFooterHeight - _link_panel_h;
            const bool focused = link_line >= 0 && &line == &_page.lines[static_cast<size_t>(link_line)];
            if (!focused && (line_y < kHeaderHeight || line_y + line.height > panel_top)) continue;
        }
        if (line.kind == ParaKind::Separator) {
            gfx.drawFastHLine(line.x, base, width - 2 * kMarginLeft, TFT_BLACK);
            continue;
        }
        if (line.code) {
            gfx.fillRect(line.x - 6, line_y + 1, width - kMarginLeft - kMarginRight + 12, line.height - 2, 0xDEFB);
        }
        if (line.quote) {
            gfx.fillRect(line.quote_rule_x, line_y + 2, 2, line.height - 4, TFT_BLACK);
        }
        if (line.show_marker && !line.spans.empty()) {
            const SpanView& first = line.spans.front();
            if (first.para < _page.paragraphs.size()) {
                const Paragraph& paragraph = _page.paragraphs[first.para];
                ui::draw_text(gfx, line.marker_x, base, paragraph.marker,
                              find_font(_font_size, Face::Regular), ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
            }
        }
        for (const SpanView& span : line.spans) {
            if (span.para >= _page.paragraphs.size()) continue;
            const Paragraph& paragraph = _page.paragraphs[span.para];
            if (span.run >= paragraph.runs.size()) continue;
            const std::string& text = paragraph.runs[span.run].text;
            if (span.begin >= text.size() || span.end > text.size() || span.begin >= span.end) continue;
            const std::string part = text.substr(span.begin, span.end - span.begin);
            ui::draw_text(gfx, span.x, base, part, find_font(span.size_index, span.face),
                          ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
        }
        if (line.hyphenated && !line.spans.empty()) {
            // The break is marked by a hyphen that is not part of the source text.
            const SpanView& last = line.spans.back();
            const FontFaceData& font = find_font(last.size_index, last.face);
            ui::draw_text(gfx, last.x + last.width, base, "-", font,
                          ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
        }
    }
    // The mockup puts a thin scrollbar on the right in the reader as well.
    render_scrollbar(false);
    if (_link_mode) {
        render_link_marks();
        render_link_panel();
    }
    (void)height;
}

void App::show_loading(const std::string& name, const char* stage, int percent) {
    ESP_LOGD(kTag, "loading: %s %s %d%%", name.c_str(), stage, percent);
    // E-ink cannot animate smoothly, so the preloader shows three discrete steps
    // (file, parse, layout) with a bar that jumps between them.
    _loading_name = name;
    _loading_stage = stage;
    _loading_percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    _screen = Screen::Loading;
    mark_all();
    render();
    ESP_LOGD(kTag, "loading frame done");
}

void App::render_booting() {
    auto& gfx = board().display();
    const int width = board().width();
    const int height = board().height();
    const auto& title = find_ui_font(4, true); // 32 px bold
    const auto& body = find_ui_font(3);        // 28 px
    const std::string title_text = "STARTING";
    const int stack_h = title.line_height + 18 + body.line_height;
    int y = (height - stack_h) / 2;
    ui::draw_text(gfx, (width - measure_text(title, title_text)) / 2, y + title.ascent, title_text,
                  title, ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
    y += title.line_height + 18;
    const std::string hint = "preparing the library";
    ui::draw_text(gfx, (width - measure_text(body, hint)) / 2, y + body.ascent, hint, body,
                  ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
}

void App::render_loading() {
    auto& gfx = board().display();
    const int width = board().width();
    const int height = board().height();
    // Same ladder as the dialogs: 32 px title, 28 px body, 24 px numbers.
    const auto& title = find_ui_font(4, true);   // 32 px bold
    const auto& body = find_ui_font(3);          // 28 px
    const auto& small = find_ui_font(2);         // 24 px

    // Same vertical rhythm as the dialogs: every block is separated by one gap and
    // the whole stack is centred, so nothing depends on magic offsets.
    constexpr int kGap = 20;
    constexpr int kBarWidth = 360;
    constexpr int kBarHeight = 14;

    const std::string title_text = "OPENING";
    const std::string name = ui::elide(body, _loading_name, width - 80);
    char percent_text[16];
    std::snprintf(percent_text, sizeof(percent_text), "%d%%", _loading_percent);

    const int stack_h = title.line_height + kGap + body.line_height + kGap + kBarHeight + 4 + kGap +
                        small.line_height + kGap + body.line_height;
    int y = (height - stack_h) / 2;

    ui::draw_text(gfx, (width - measure_text(title, title_text)) / 2, y + title.ascent, title_text,
                  title, ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
    y += title.line_height + kGap;

    ui::draw_text(gfx, (width - measure_text(body, name)) / 2, y + body.ascent, name, body,
                  ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
    y += body.line_height + kGap;

    // Bar: 2 px frame around a 14 px body, filled from the left.
    const int bar_x = (width - kBarWidth) / 2;
    gfx.drawRect(bar_x - 2, y - 2, kBarWidth + 4, kBarHeight + 4, TFT_BLACK);
    gfx.drawRect(bar_x - 1, y - 1, kBarWidth + 2, kBarHeight + 2, TFT_BLACK);
    const int filled = kBarWidth * _loading_percent / 100;
    if (filled > 0) gfx.fillRect(bar_x, y, filled, kBarHeight, TFT_BLACK);
    y += kBarHeight + 4 + kGap;

    ui::draw_text(gfx, (width - measure_text(small, percent_text)) / 2, y + small.ascent,
                  percent_text, small, ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
    y += small.line_height + kGap;

    ui::draw_text(gfx, (width - measure_text(body, _loading_stage)) / 2, y + body.ascent,
                  _loading_stage, body, ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
}

void App::show_question() {
    _screen = Screen::Question;
    mark_all();
    render();
}

// One card style for every dialog-like screen (messages, repair question): large
// type, wrapped text, hints at the bottom. 32 px title, 28 px body, 24 px hints.
void App::draw_dialog(const std::string& title, const std::string& body, const char* hint_left,
                      const char* hint_right) {
    auto& gfx = board().display();
    const int width = board().width();
    const int height = board().height();
    const auto& title_font = find_ui_font(4, true); // 32 px bold
    const auto& body_font = find_ui_font(3);        // 28 px
    const auto& hint_font = find_ui_font(2);        // 24 px

    const int card_x = 16;
    const int card_w = width - 2 * card_x;
    constexpr int kPaddingX = 30;
    const int text_width = card_w - 2 * kPaddingX;

    // Greedy word wrap at the body size.
    std::vector<std::string> lines;
    std::string current;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t space = body.find_first_of(" \n", pos);
        if (space == std::string::npos) space = body.size();
        const std::string word = body.substr(pos, space - pos);
        const std::string candidate = current.empty() ? word : current + " " + word;
        if (measure_text(body_font, candidate) > text_width && !current.empty()) {
            lines.push_back(current);
            current = word;
        } else {
            current = candidate;
        }
        pos = space < body.size() ? space + 1 : body.size();
    }
    if (!current.empty()) lines.push_back(current);

    // Block spacing comes from one rhythm: line heights from the atlas plus fixed
    // block gaps, and the whole stack is centred. No magic offsets.
    constexpr int kBlockGap = 22;   // title -> body
    constexpr int kHintGap = 34;    // body  -> hints
    constexpr int kPaddingY = 34;   // card edge -> content

    const int body_h = static_cast<int>(lines.size()) * body_font.line_height;
    const int content_h = title_font.line_height + kBlockGap + body_h + kHintGap + hint_font.line_height;
    int card_h = content_h + 2 * kPaddingY;
    if (card_h > height - 60) card_h = height - 60;
    const int card_y = (height - card_h) / 2;

    gfx.drawRect(card_x, card_y, card_w, card_h, TFT_BLACK);
    gfx.drawRect(card_x + 1, card_y + 1, card_w - 2, card_h - 2, TFT_BLACK);

    // Draw from baselines derived from the atlas metrics, not from fixed pixels.
    int y = card_y + kPaddingY;
    const std::string shown_title = ui::elide(title_font, title, text_width);
    ui::draw_text(gfx, card_x + (card_w - measure_text(title_font, shown_title)) / 2,
                  y + title_font.ascent, shown_title, title_font,
                  ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
    y += title_font.line_height + kBlockGap;

    for (const auto& line : lines) {
        if (y + body_font.line_height > card_y + card_h - kPaddingY - hint_font.line_height) break;
        ui::draw_text(gfx, card_x + (card_w - measure_text(body_font, line)) / 2, y + body_font.ascent,
                      line, body_font, ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
        y += body_font.line_height;
    }

    const int hint_y = card_y + card_h - kPaddingY - hint_font.line_height + hint_font.ascent;
    if (hint_left != nullptr && hint_right != nullptr) {
        const int left_width = measure_text(hint_font, hint_left);
        ui::draw_text(gfx, card_x + card_w / 2 - left_width - 20, hint_y, hint_left, hint_font,
                      ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
        ui::draw_text(gfx, card_x + card_w / 2 + 20, hint_y, hint_right, hint_font,
                      ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
    } else {
        const char* hint = hint_left != nullptr ? hint_left : hint_right;
        if (hint != nullptr) {
            ui::draw_text(gfx, card_x + (card_w - measure_text(hint_font, hint)) / 2, hint_y, hint,
                          hint_font, ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});
        }
    }
}

void App::render_message() {
    const char* hint = _message_back == Screen::Reader ? "A+B - BACK" : "A+B - OK";
    draw_dialog(_message_title, _message_body, nullptr, hint);
}

void App::render_question() { draw_dialog(_question_title, _question_body, "A - YES", "B - NO"); }

void App::handle_question(Gesture gesture) {
    if (gesture == Gesture::ClickA) {
        finish_repair(true);
        return;
    }
    if (gesture == Gesture::ClickB) {
        finish_repair(false);
        return;
    }
    if (gesture == Gesture::Chord || gesture == Gesture::ChordLong) {
        finish_repair(false);
    }
}

// Builds "<dir>/<prefix><file name>" and, if that already exists, "<dir>/<prefix><n>-<name>",
// so repeated repairs never overwrite earlier copies.
std::string unique_sibling(const std::string& relative, const char* prefix) {
    const size_t slash = relative.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : relative.substr(0, slash + 1);
    const std::string name = slash == std::string::npos ? relative : relative.substr(slash + 1);
    auto exists = [](const std::string& candidate) {
        struct stat info {};
        return stat((std::string(kMountPoint) + "/" + candidate).c_str(), &info) == 0;
    };
    std::string candidate = dir + prefix + name;
    for (int attempt = 2; attempt < 100 && exists(candidate); ++attempt) {
        candidate = dir + prefix + std::to_string(attempt) + "-" + name;
    }
    return candidate;
}

void App::finish_repair(bool replace_original) {
    const std::string relative = _question_file;
    const std::string repaired = _question_repaired;
    _question_file.clear();
    _question_repaired.clear();

    if (repaired.empty()) {
        load_directory(_dir);
        _screen = Screen::FileList;
        mark_all();
        return;
    }

    const std::string full_original = std::string(kMountPoint) + "/" + relative;
    const std::string full_repaired = std::string(kMountPoint) + "/" + repaired;
    if (replace_original) {
        // The broken file keeps its provenance in the name: broken-<original>.
        const std::string backup = std::string(kMountPoint) + "/" + unique_sibling(relative, "broken-");
        const int moved_old = std::rename(full_original.c_str(), backup.c_str());
        const int moved_new =
            moved_old == 0 ? std::rename(full_repaired.c_str(), full_original.c_str()) : -1;
        ESP_LOGI(kTag, "repair replace: old->%s=%d new->orig=%d", backup.c_str(), moved_old, moved_new);
        if (moved_new != 0) {
            std::rename(backup.c_str(), full_original.c_str());
            std::remove(full_repaired.c_str());
        }
    } else {
        // Keep both: the original stays untouched, the repaired copy gets fixed-<name>.
        const std::string kept = std::string(kMountPoint) + "/" + unique_sibling(relative, "fixed-");
        ESP_LOGI(kTag, "repair keep: new->%s=%d", kept.c_str(),
                 std::rename(full_repaired.c_str(), kept.c_str()));
    }
    _screen = Screen::FileList;
    load_directory(_dir, relative.substr(relative.find_last_of('/') + 1));
    mark_all();
    if (replace_original) open_file(relative);
}

bool App::force_repair(const std::string& relative) {
    std::string repaired;
    if (!repair_file(relative, &repaired)) return false;
    _question_title = "File repaired";
    _question_body = "A repaired UTF-8 copy was written.\nThe broken file will be kept\nas broken-<name>. Replace the original?";
    _question_file = relative;
    _question_repaired = repaired;
    show_question();
    return true;
}

bool App::repair_file(const std::string& relative, std::string* repaired) {
    const std::string full = std::string(kMountPoint) + "/" + relative;
    std::string error;
    auto source = open_file_source(full, &error);
    if (!source) return false;

    std::string raw;
    if (!read_all(*source, 6u * 1024u * 1024u, &raw, &error)) return false;
    const uint64_t size = source->size();
    source.reset();

    // Decode with the detected encoding; fall back to CP1251, which is what most
    // broken Russian FB2/TXT files actually use when the declaration lies.
    TextEncoding encoding = detect_encoding(reinterpret_cast<const uint8_t*>(raw.data()),
                                            raw.size() < 4096 ? raw.size() : 4096);
    std::string text = to_utf8(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), encoding);
    if (text.empty() || text.find('\xEF') == std::string::npos) {
        if (encoding != TextEncoding::Cp1251) {
            text = cp1251_to_utf8(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
        }
    }
    if (text.empty()) return false;
    (void)size;

    // Sanitise: drop control characters, escape bare ampersands, close the root tag.
    std::string out;
    out.reserve(text.size() + text.size() / 16 + 64);
    const bool xml = format_from_name(relative) == Format::Fb2;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') continue;
        if (xml && c == '&') {
            const size_t end = text.find(';', i);
            const bool entity = end != std::string::npos && end - i <= 10;
            if (!entity) {
                out += "&amp;";
                continue;
            }
        }
        out.push_back(static_cast<char>(c));
    }
    if (xml) {
        // If the body was truncated, close the container so the parser can finish.
        const size_t closing = out.rfind("</FictionBook>");
        if (closing == std::string::npos) out += "\n</FictionBook>\n";
        // A UTF-8 declaration must match the rewritten bytes.
        const size_t decl_end = out.find("?>");
        if (decl_end != std::string::npos && out.compare(0, 5, "<?xml") == 0) {
            out.replace(0, decl_end + 2, "<?xml version=\"1.0\" encoding=\"utf-8\"?>");
        }
    }

    // The repaired copy is written to a temporary name first: it only gets its final
    // name (original, broken-..., fixed-...) after the parser has accepted it.
    const std::string repaired_relative = relative + ".repair.tmp";
    const std::string full_repaired = std::string(kMountPoint) + "/" + repaired_relative;
    FILE* file = std::fopen(full_repaired.c_str(), "wb");
    if (file == nullptr) return false;
    const size_t written = std::fwrite(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (written != out.size()) return false;

    // Verify that the repaired file actually opens before offering it.
    auto check_source = open_file_source(full_repaired, &error);
    if (!check_source) return false;
    auto check_doc = open_document(format_from_name(relative), std::move(check_source), &error);
    if (!check_doc) {
        std::remove(full_repaired.c_str());
        return false;
    }
    *repaired = repaired_relative;
    return true;
}

void App::render_footer() {
    auto& gfx = board().display();
    const int width = board().width();
    const int height = board().height();
    const auto& small = find_ui_font(1); // 18 px
    const int top = height - kFooterHeight;
    const int baseline = height - 12;

    // Only the reading screen keeps a bottom bar, and it holds just the progress:
    // no key hints, no border.
    gfx.fillRect(0, top, width, kFooterHeight, TFT_WHITE);
    if (_screen != Screen::Reader) return;

    char page_text[32];
    if (_total_pages > 0) {
        std::snprintf(page_text, sizeof(page_text), "%d / %d", _page_index, _total_pages);
    } else {
        std::snprintf(page_text, sizeof(page_text), "%d / ...", _page_index);
    }
    ui::draw_text(gfx, 10, baseline, page_text, small, ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});

    char percent_text[16];
    std::snprintf(percent_text, sizeof(percent_text), "%d%%", static_cast<int>(_page.permille / 10));
    const int page_width = measure_text(small, page_text);
    ui::draw_text(gfx, 10 + page_width + 14, baseline, percent_text, small,
                  ui::TextOptions{TFT_BLACK, TFT_WHITE, -1});

    // Thin bar on the right: how far the book has been read.
    const int bar_width = 180;
    const int bar_x = width - bar_width - 12;
    const int bar_y = top + kFooterHeight / 2 - 3;
    gfx.drawRect(bar_x - 1, bar_y - 1, bar_width + 2, 8, TFT_BLACK);
    const int filled = bar_width * static_cast<int>(_page.permille) / 1000;
    if (filled > 0) gfx.fillRect(bar_x, bar_y, filled, 6, TFT_BLACK);
}

} // namespace reader
