#include "input/gestures.h"

#include <initializer_list>

namespace reader {
namespace {

constexpr uint32_t kChordWindowMs = 120; // second press must follow this fast
constexpr uint32_t kShortMaxMs = 500;    // short click
constexpr uint32_t kLongChordMinMs = 800; // held A+B: "open from the beginning"
constexpr uint32_t kRepeatDelayMs = 500; // hold before autorepeat
constexpr uint32_t kRepeatPeriodMs = 160;

bool g_repeat_enabled = true;

} // namespace

void set_gesture_repeat(bool enabled) { g_repeat_enabled = enabled; }

void GestureEngine::begin(uint32_t now_ms) {
    (void)now_ms;
    _a = Button{};
    _b = Button{};
    _chord = false;
    _chord_started_ms = 0;
}

void GestureEngine::update(bool a_down, bool b_down, uint32_t now_ms, GestureSink& sink) {
    if (a_down && !_a.down) {
        _a.down = true;
        _a.pressed_ms = now_ms;
        _a.consumed = false;
        _a.repeat_started = false;
        _a.modifier = false;
        on_press(_a, _b, now_ms, sink);
    } else if (!a_down && _a.down) {
        _a.down = false;
        on_release(_a, now_ms, sink);
    }

    if (b_down && !_b.down) {
        _b.down = true;
        _b.pressed_ms = now_ms;
        _b.consumed = false;
        _b.repeat_started = false;
        _b.modifier = false;
        on_press(_b, _a, now_ms, sink);
    } else if (!b_down && _b.down) {
        _b.down = false;
        on_release(_b, now_ms, sink);
    }

    // Autorepeat for a single held button.
    for (Button* button : {&_a, &_b}) {
        if (!g_repeat_enabled) break;
        if (!button->down || button->consumed || _chord) continue;
        if (now_ms - button->pressed_ms < kRepeatDelayMs) continue;
        if (!button->repeat_started) {
            button->repeat_started = true;
            button->last_repeat_ms = now_ms;
            emit(button == &_a ? Gesture::ClickA : Gesture::ClickB, sink);
        } else if (now_ms - button->last_repeat_ms >= kRepeatPeriodMs) {
            button->last_repeat_ms = now_ms;
            emit(button == &_a ? Gesture::ClickA : Gesture::ClickB, sink);
        }
    }
}

void GestureEngine::on_press(Button& button, Button& other, uint32_t now, GestureSink& sink) {
    if (!other.down) return;

    // Repeated taps while the modifier is held keep changing the font size.
    if (other.modifier) {
        button.consumed = true;
        emit(&button == &_a ? Gesture::FontUp : Gesture::FontDown, sink);
        return;
    }

    const bool simultaneous = now - other.pressed_ms <= kChordWindowMs;
    if (simultaneous && !other.repeat_started && !other.consumed) {
        _chord = true;
        _chord_started_ms = other.pressed_ms;
        button.consumed = true;
        other.consumed = true;
        return;
    }

    // A held button plus a tap is a modifier combo. This also works after the
    // held button started autorepeating: the repeats have already moved the
    // cursor, and the tap then jumps/pages (file manager) or changes the font.
    other.modifier = true;
    other.consumed = true;
    button.consumed = true;
    emit(&button == &_a ? Gesture::FontUp : Gesture::FontDown, sink);
}

void GestureEngine::on_release(Button& button, uint32_t now, GestureSink& sink) {
    if (_chord) {
        if (!_a.down && !_b.down) {
            const uint32_t duration = now - _chord_started_ms;
            _chord = false;
            _a.consumed = true;
            _b.consumed = true;
            if (duration <= kShortMaxMs) {
                emit(Gesture::Chord, sink);
            } else if (duration >= kLongChordMinMs) {
                emit(Gesture::ChordLong, sink);
            }
        }
        return;
    }

    if (button.consumed || button.repeat_started) return;
    if (now - button.pressed_ms <= kShortMaxMs) {
        emit(&button == &_a ? Gesture::ClickA : Gesture::ClickB, sink);
    }
}

} // namespace reader
