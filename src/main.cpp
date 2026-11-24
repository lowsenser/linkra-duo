#include <esp_heap_caps.h>
/*
 * Linkra Duo A0.1 - RF Gateway
 * E07-400MBL-01 (CC1101) via SPI
 * Using rtl_433_ESP for protocol decoding (200+ device types)
 */

#include <Arduino.h>
#include "core/system_init.h"
#include "core/crash_log.h"

void setup() {
    // Larger malloc()s go to PSRAM (8 MB) so the ~320 KB internal RAM stays
    // available for WiFi, lwIP and task stacks.
    heap_caps_malloc_extmem_enable(512);

    Serial.begin(115200);
    CrashLog::install();
    delay(1000);
    
    Serial.println("\n\n");
    Serial.println("====================================");
    Serial.println("  Linkra Duo A0.1 Booting...");
    Serial.println("  CC1101 via rtl_433_ESP");
    Serial.println("====================================");
    
    SystemInit::begin();
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
