#include "supervisor.h"
#include "../storage/device_database.h"
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_ota_ops.h>
#include <WiFi.h>

namespace {
// Tasks whose stack reserve is worth watching. Names as FreeRTOS stores them:
// max 15 characters (longer names are truncated, and xTaskGetHandle asserts on them).
const char* const kTasks[] = {"systemInit", "rfProfiles", "wmbus", "rfPipe", "rx433rmt", "rx868Receiver",
                              "rtl_433_Decoder", "rtl_433_Receive", "mqttPub", "async_tcp", "loopTask", "authBtn"};
constexpr uint32_t kHeapCriticalBytes = 24 * 1024;
constexpr uint32_t kCriticalForMs = 60000;
uint32_t lowSinceMs = 0;
uint32_t lastLogMs = 0;

uint32_t freeInternal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
}  // namespace

namespace Supervisor {

void fillJson(JsonObject o) {
    o["heap_internal"] = freeInternal();
    o["heap_internal_min"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    o["heap_internal_largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    o["psram_free"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        o["app_partition"] = running->label;
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(running, &state) == ESP_OK)
            o["app_state"] = state == ESP_OTA_IMG_PENDING_VERIFY ? "pending_verify" : state == ESP_OTA_IMG_VALID ? "valid" : "other";
    }
    JsonObject st = o["stack_free"].to<JsonObject>();
    for (const char* name : kTasks) {
        TaskHandle_t h = xTaskGetHandle(name);
        if (h) st[name] = uxTaskGetStackHighWaterMark(h);   // bytes on ESP-IDF
    }
}

// Confirm a freshly installed OTA image once it ran healthy for a while.
static void confirmOtaImage(uint32_t now) {
    static bool done = false;
    if (done) return;
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        done = true;
        return;
    }
    if (now < 90000 || WiFi.status() != WL_CONNECTED) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        Serial.printf("[OTA] Neue Firmware in %s bestätigt (kein Rollback)\n", running->label);
    done = true;
}

void tick() {
    const uint32_t now = millis();
#ifdef LINKRA_ROLLBACK_TEST
    // Test build only (-DLINKRA_ROLLBACK_TEST): crash 30 s after boot, before the
    // 90-s confirmation, to prove the bootloader returns to the previous image.
    if (now > 30000) { Serial.println("[OTA] ROLLBACK-TEST: absichtlicher Absturz"); delay(50); abort(); }
#endif
    confirmOtaImage(now);
    const uint32_t heap = freeInternal();
    if (heap < kHeapCriticalBytes) {
        if (!lowSinceMs) lowSinceMs = now ? now : 1;
        else if (now - lowSinceMs > kCriticalForMs) {
            Serial.printf("[Supervisor] Interner Heap seit %lus unter %lu B (%lu B) – Neustart\n",
                          (unsigned long)(kCriticalForMs / 1000), (unsigned long)kHeapCriticalBytes, (unsigned long)heap);
            DeviceDatabase::getInstance().close();
            delay(100);
            ESP.restart();
        }
    } else {
        lowSinceMs = 0;
    }
    if (now - lastLogMs >= 60000) {
        lastLogMs = now;
        String line = "[Supervisor] heap " + String(heap) + " min " +
                      String(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) + " | stack";
        for (const char* name : kTasks) {
            TaskHandle_t h = xTaskGetHandle(name);
            if (h) line += String(" ") + name + "=" + String(uxTaskGetStackHighWaterMark(h));
        }
        Serial.println(line);
    }
}

}  // namespace Supervisor
