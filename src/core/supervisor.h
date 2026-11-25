#pragma once
// Health supervisor (review P1 #10): watches internal heap and task stack
// reserves, logs them every minute and restarts the device cleanly if the
// internal heap stays critically low (WiFi/HTTP die below ~30 KB).
#include <Arduino.h>
#include <ArduinoJson.h>

namespace Supervisor {
void tick();                          // call about once per second (Arduino loop)
void fillJson(JsonObject o);          // heap + per-task stack high-water marks
}
