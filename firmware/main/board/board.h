// Board support: display, SD card, battery, power.
#pragma once

#include <M5Unified.h>
#include <cstdint>
#include <string>

namespace reader {

enum class EpdMode : uint8_t {
    Quality, // GC16, 15 LUT frames: 16 gray levels, clears ghosting
    Text,    // GL16, 12 frames: grayscale partial update
    Fast,    // DU, 8 frames: 2 levels, ~3x faster than Text
    Fastest, // DU4, 5 frames: 2 levels, quickest
};

// Axis-aligned dirty region used for partial E-Ink updates.
struct Region {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool empty() const { return w <= 0 || h <= 0; }
};

class Board {
  public:
    bool init();

    M5GFX& display() { return M5.Display; }
    int width() const { return M5.Display.width(); }
    int height() const { return M5.Display.height(); }

    void delay_ms(uint32_t ms);
    uint32_t millis();

    int battery_millivolts();
    int battery_percent();

    // `force` tries to mount even when the card-detect switch says "no card"`r
    // (some units have a flaky switch).
    bool mount_sd(bool force = false);
    bool sd_mounted() const { return _sd_mounted; }
    void unmount_sd();

    /* ------------------------------ E-Ink updates ----------------------------- */
    // Frames are collected as dirty regions and flushed together. Only the
    // touched areas reach the panel, which keeps ghosting and refresh time low.
    void begin_frame();
    void touch(int x, int y, int w, int h);
    void touch(const Region& region) { touch(region.x, region.y, region.w, region.h); }

    // When true the next commit repaints the whole panel with GC16.
    void request_full_refresh();
    bool full_refresh_pending() const { return _force_full; }

    // Flushes the collected regions. Returns true when a full refresh happened.
    bool commit_frame(EpdMode mode);

    struct Stats {
        uint32_t full_flushes = 0;
        uint32_t partial_flushes = 0;
        uint32_t last_regions = 0;
        int32_t last_x = 0;
        int32_t last_y = 0;
        int32_t last_w = 0;
        int32_t last_h = 0;
        uint32_t last_ms = 0;
        int32_t last_detect_level = -1;
    };
    const Stats& stats() const { return _stats; }

    void wait_display();

    void power_off();

  private:
    void merge_regions();
    void flush_rect(const Region& region, EpdMode mode);
    void flush_full(EpdMode mode);

    bool _sd_mounted = false;
    bool _display_ok = false;
    bool _force_full = true;
    bool _frame_open = false;
    Region _regions[8];
    int _region_count = 0;
    int _partial_updates = 0;
    Stats _stats;
};

Board& board();

} // namespace reader
