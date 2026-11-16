#include "input/input_queue.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>

namespace reader {
namespace {

constexpr char kTag[] = "Input";

constexpr gpio_num_t kButtonA = GPIO_NUM_4;
constexpr gpio_num_t kButtonB = GPIO_NUM_3;
constexpr uint32_t kPollMs = 2;
constexpr uint32_t kDebounceMs = 10;
constexpr int kQueueLength = 32;

QueueHandle_t g_queue = nullptr;

// Small sink that forwards gestures from the gesture engine to the queue.
class QueueSink : public GestureSink {
  public:
    void on_gesture(Gesture gesture) override { post_gesture(gesture); }
};

// The power key belongs to the PMIC: a single press power-cycles the board and a
// double press switches it off, so firmware never sees a usable press.
bool read_a() { return gpio_get_level(kButtonA) == 0; }
bool read_b() { return gpio_get_level(kButtonB) == 0; }

void input_task(void*) {
    GestureEngine engine;
    QueueSink sink;
    engine.begin(xTaskGetTickCount() * portTICK_PERIOD_MS);

    bool raw_a = read_a();
    bool raw_b = read_b();
    bool stable_a = raw_a;
    bool stable_b = raw_b;
    TickType_t changed_at = xTaskGetTickCount();

    while (true) {
        const bool a = read_a();
        const bool b = read_b();
        if (a != raw_a || b != raw_b) {
            raw_a = a;
            raw_b = b;
            changed_at = xTaskGetTickCount();
        } else if ((a != stable_a || b != stable_b) &&
                   (xTaskGetTickCount() - changed_at) >= pdMS_TO_TICKS(kDebounceMs)) {
            stable_a = a;
            stable_b = b;
        }
        // One update per tick: it also drives autorepeat and combo detection.
        engine.update(stable_a, stable_b, xTaskGetTickCount() * portTICK_PERIOD_MS, sink);
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
    }
}

} // namespace

bool post_gesture(Gesture gesture) {
    if (g_queue == nullptr || gesture == Gesture::None) return false;
    const uint8_t value = static_cast<uint8_t>(gesture);
    return xQueueSend(g_queue, &value, 0) == pdTRUE;
}

bool pop_gesture(Gesture& gesture) {
    if (g_queue == nullptr) return false;
    uint8_t value = 0;
    if (xQueueReceive(g_queue, &value, 0) != pdTRUE) return false;
    gesture = static_cast<Gesture>(value);
    return true;
}

bool start_input_task() {
    if (g_queue == nullptr) {
        g_queue = xQueueCreate(kQueueLength, sizeof(uint8_t));
        if (g_queue == nullptr) {
            ESP_LOGE(kTag, "queue allocation failed");
            return false;
        }
    }

    gpio_config_t buttons = {};
    buttons.pin_bit_mask = (1ULL << kButtonA) | (1ULL << kButtonB);
    buttons.mode = GPIO_MODE_INPUT;
    buttons.pull_up_en = GPIO_PULLUP_ENABLE;
    buttons.pull_down_en = GPIO_PULLDOWN_DISABLE;
    buttons.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&buttons) != ESP_OK) {
        ESP_LOGE(kTag, "gpio config failed");
        return false;
    }

    // Pin to the other core: the UI task spends its time waiting for the panel.
    const BaseType_t created =
        xTaskCreatePinnedToCore(input_task, "reader_input", 4096, nullptr, tskIDLE_PRIORITY + 4, nullptr, 1);
    if (created != pdPASS) {
        ESP_LOGE(kTag, "task creation failed");
        return false;
    }
    ESP_LOGI(kTag, "button task started");
    return true;
}

} // namespace reader
