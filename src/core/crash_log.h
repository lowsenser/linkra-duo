#pragma once
// Records the last panic (reason, PC, backtrace) in RTC memory that survives a
// software reset, so a crash can be analysed without a serial console.
// Decode: xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/<env>/firmware.elf <addresses>
#include <Arduino.h>

namespace CrashLog {
void install();          // call early in setup()
bool present();
String json();           // {"present":..,"reason":..,"pc":"0x..","core":n,"backtrace":["0x..",..],"boots":n}
void clear();
}
