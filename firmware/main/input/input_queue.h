// Decouples button input from E-Ink rendering.
//
// A dedicated task polls the buttons every few milliseconds and pushes
// recognised gestures into a queue. The UI task drains the queue and renders
// once, so fast tapping never waits for a waveform and intermediate frames are
// skipped instead of queued.
#pragma once

#include "input/gestures.h"

namespace reader {

// Creates the queue and starts the polling task (pinned to the other core).
bool start_input_task();

// Safe from any task (UI, console). Returns false when the queue is full.
bool post_gesture(Gesture gesture);

// Non-blocking pop used by the UI loop.
bool pop_gesture(Gesture& gesture);

} // namespace reader
