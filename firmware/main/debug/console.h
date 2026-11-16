// Developer console over USB-Serial-JTAG.
//
// Commands are plain text lines prefixed with "rdr ". The channel is used to
// push test books onto the SD card, drive the UI (button injection) and grab a
// grayscale screenshot of the E-Ink frame buffer while developing on hardware.
#pragma once

namespace reader {

void start_debug_console();

} // namespace reader
