#pragma once

#include <Arduino.h>
#include "event_bus.h"

/**
 * Asynchrone System-Initialisierung
 * Verhindert Watchdog-Resets durch langsame Operationen
 */
class SystemInit {
public:
    static void begin();

private:
    static void initSystemTask(void* pvParameters);
    static void initWiFi();
    static void initWebServer();
    static void initHardware();
    static void initRF();
};
