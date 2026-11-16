/*
 * E-Reader for M5PaperDIY (K159 / K159-B).
 *
 * Two screens only: a file manager over the SD card and a paginated reader.
 * All navigation uses buttons A and B; the PWR button stays with the system
 * power controller (M5PM1) and is never intercepted here.
 */
#include <M5Unified.h>
#include <driver/gpio.h>
#include <esp_log.h>

#include "board/board.h"
#include "debug/console.h"
#include "input/input_queue.h"
#include "store/progress.h"
#include "ui/app.h"

namespace {

constexpr char kTag[] = "ereader";
constexpr char kVersion[] = "1.0";

} // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "E-Reader %s for M5PaperDIY", kVersion);

    if (!reader::board().init()) {
        ESP_LOGE(kTag, "board init failed");
        while (true) {
            reader::board().delay_ms(1000);
        }
    }

    reader::start_input_task();
    reader::start_debug_console();

    reader::App& app = reader::app();
    app.begin();
    app.run();
}
