#include <esp_heap_caps.h>
/*
 * Linkra Duo A0.1 - RF Gateway
 * E07-400MBL-01 (CC1101) via SPI
 * Using rtl_433_ESP for protocol decoding (200+ device types)
 */

#include <Arduino.h>
#include "core/system_init.h"
#include "core/crash_log.h"
#include "core/supervisor.h"

// OTA rollback: a freshly installed image boots in "pending verify". Arduino would
// mark it valid at once; we defer that until the Supervisor has seen it run
// healthy (WiFi up, 90 s without a crash). A crash before that makes the
// bootloader fall back to the previous image on the next reset.
// extern "C" is required: the weak default lives in esp32-hal-misc.c (C linkage);
// a plain C++ definition gets a mangled name and silently never overrides it.
extern "C" bool verifyRollbackLater() { return true; }

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
    Supervisor::tick();
    vTaskDelay(pdMS_TO_TICKS(1000));
}
