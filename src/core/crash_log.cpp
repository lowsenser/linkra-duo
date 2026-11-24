#include "crash_log.h"
#include <esp32-hal.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace {
constexpr uint32_t kMagic = 0x4C4E4B43;  // "LNKC"
constexpr int kMaxBt = 32;
struct Record {
    uint32_t magic;
    uint32_t bootsSince;
    int core;
    uint32_t pc;
    uint32_t btLen;
    bool btCorrupt;
    char reason[96];
    uint32_t bt[kMaxBt];
};
RTC_NOINIT_ATTR Record rec;

// Runs in panic context: plain memory writes only.
void onPanic(arduino_panic_info_t* info, void*) {
    rec.magic = kMagic;
    rec.bootsSince = 0;
    rec.core = info->core;
    rec.pc = (uint32_t)info->pc;
    rec.btCorrupt = info->backtrace_corrupt;
    rec.btLen = info->backtrace_len < (unsigned)kMaxBt ? info->backtrace_len : kMaxBt;
    for (uint32_t i = 0; i < rec.btLen; i++) rec.bt[i] = info->backtrace[i];
    const char* r = info->reason ? info->reason : "";
    size_t i = 0;
    for (; i + 1 < sizeof(rec.reason) && r[i]; i++) rec.reason[i] = r[i];
    rec.reason[i] = '\0';
}
}  // namespace

namespace CrashLog {
void install() {
    if (rec.magic == kMagic) {
        rec.bootsSince++;
        Serial.printf("[Crash] Letzter Absturz: %s PC=0x%08lx\n", rec.reason, (unsigned long)rec.pc);
    }
    set_arduino_panic_handler(onPanic, nullptr);
}
bool present() { return rec.magic == kMagic; }
void clear() { rec.magic = 0; }
String json() {
    String s = "{\"present\":";
    if (!present()) return s + "false,\"reset_reason\":" + String((int)esp_reset_reason()) + "}";
    s += "true,\"reset_reason\":" + String((int)esp_reset_reason());
    s += ",\"boots_since\":" + String(rec.bootsSince) + ",\"core\":" + String(rec.core);
    char buf[16];
    snprintf(buf, sizeof buf, "0x%08lx", (unsigned long)rec.pc);
    s += ",\"pc\":\"" + String(buf) + "\",\"reason\":\"";
    for (const char* p = rec.reason; *p; p++) if (*p != '"' && *p != '\\' && (uint8_t)*p >= 0x20) s += *p;
    s += "\",\"backtrace_corrupt\":" + String(rec.btCorrupt ? "true" : "false") + ",\"backtrace\":[";
    for (uint32_t i = 0; i < rec.btLen && i < (uint32_t)kMaxBt; i++) {
        snprintf(buf, sizeof buf, "0x%08lx", (unsigned long)rec.bt[i]);
        s += (i ? ",\"" : "\"") + String(buf) + "\"";
    }
    return s + "]}";
}
}  // namespace CrashLog
