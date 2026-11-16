// Button gesture recognition: short clicks, A+B chord, long-press repeat and
// the font-size modifier gestures (hold B + tap A / hold A + tap B).
#pragma once

#include <cstdint>

namespace reader {

enum class Gesture : uint8_t {
    None,
    ClickA,
    ClickB,
    Chord,     // short A+B: open / leave the reader
    ChordLong, // long A+B: open a book from the beginning
    FontUp,    // hold B, tap A
    FontDown,  // hold A, tap B
};

// Autorepeat can be switched off (reference mode needs "hold A" as a modifier,
// not as a stream of clicks).
void set_gesture_repeat(bool enabled);

struct GestureSink {
    virtual ~GestureSink() = default;
    virtual void on_gesture(Gesture gesture) = 0;
};

class GestureEngine {
  public:
    void begin(uint32_t now_ms);
    // Call every few milliseconds with debounced button states.
    void update(bool a_down, bool b_down, uint32_t now_ms, GestureSink& sink);

    bool chord_active() const { return _chord; }

  private:
    struct Button {
        bool down = false;
        bool consumed = false;
        bool repeat_started = false;
        bool modifier = false;
        uint32_t pressed_ms = 0;
        uint32_t last_repeat_ms = 0;
    };

    void on_press(Button& button, Button& other, uint32_t now, GestureSink& sink);
    void on_release(Button& button, uint32_t now, GestureSink& sink);
    void emit(Gesture gesture, GestureSink& sink) {
        if (gesture != Gesture::None) sink.on_gesture(gesture);
    }

    Button _a;
    Button _b;
    bool _chord = false;
    uint32_t _chord_started_ms = 0;
};

} // namespace reader
