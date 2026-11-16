// Reading progress and settings, stored as JSON on the SD card.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "text/types.h"

namespace reader {

struct ProgressEntry {
    std::string path;        // relative to the SD root, no leading slash
    uint64_t token = 0;
    uint32_t char_offset = 0;
    uint32_t permille = 0;
    uint32_t order = 0;      // higher = more recent
    uint64_t file_size = 0;  // size at save time; a changed file invalidates it
    // Page bookkeeping: the index only makes sense for the layout it was taken in,
    // so the layout signature (font size + margins + panel width) is stored too.
    int page = 0;
    uint32_t layout = 0;
};

class ProgressStore {
  public:
    bool load();
    bool save() const;

    // Updates (or inserts) the entry and marks it as the most recent file.
    void update(const std::string& path, PagePos position, uint32_t permille, int page = 0,
                uint32_t layout = 0);
    // Drops the saved position of one file (used by "open from the beginning").
    void forget(const std::string& path);

    // Returns the entry only when it still belongs to the same file content
    // (the recorded size must match the file on the card).
    const ProgressEntry* find_valid(const std::string& path) const;

    const ProgressEntry* find(const std::string& path) const;
    std::string last_path() const;

    int font_size() const { return _font_size; }
    void set_font_size(int size);
    // True when the device was switched off while a book was open: on boot the
    // reader reopens that book at the saved position.
    bool reading() const { return _reading; }
    void set_reading(bool value) { _reading = value; }

  private:
    static uint64_t file_size_of(const std::string& path);

    std::vector<ProgressEntry> _entries;
    std::string _last_path;
    int _font_size = 1;
    bool _reading = false;
    uint32_t _next_order = 1;
    bool _dirty = false;
};

ProgressStore& progress_store();

} // namespace reader
