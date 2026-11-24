#include "rf_hunt.h"
#include "rf_profiles.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <freertos/semphr.h>
#include <climits>
#include <esp_heap_caps.h>

// Strong (>= 20 dB SNR) trains that no decoder accepted, per profile index (signalDecoder.cpp)
extern volatile uint32_t linkraStrongUndecoded[32];

static SemaphoreHandle_t huntMutex = nullptr;

void* SpiRamJsonAllocator::allocate(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void SpiRamJsonAllocator::deallocate(void* p) { heap_caps_free(p); }
void* SpiRamJsonAllocator::reallocate(void* p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
SpiRamJsonAllocator* SpiRamJsonAllocator::instance() { static SpiRamJsonAllocator a; return &a; }
static const char* HUNT_FILE = "/hunt.json";
static const int HUNT_LOG_MAX = 150;
static const int HUNT_DEV_MAX = 80;

// 868 plan, repeated: Bresser FSK 8.2k gets most time, 868 OOK (Fine Offset WH1080
// EU, FS20) second; the home FSK profile stays in so LaCrosse/Ecowitt keep reporting.
struct HuntStep { const char* id; uint32_t ms; };
static const HuntStep HUNT_PLAN[] = {
    {"fsk868-8k", 15 * 60000UL},
    {"fsk868-17k", 5 * 60000UL},
    {"ook868", 10 * 60000UL},
    {"fsk868-17k", 5 * 60000UL},
};
static const int HUNT_STEPS = sizeof(HUNT_PLAN) / sizeof(HUNT_PLAN[0]);

static int profileIndex(const char* id) {
    for (int i = 0; i < RX_PROFILE_COUNT; i++)
        if (strcmp(RX_PROFILES[i].id, id) == 0) return i;
    return -1;
}

RfHunt& RfHunt::get() {
    static RfHunt h;
    return h;
}

void RfHunt::begin() {
    if (!huntMutex) huntMutex = xSemaphoreCreateMutex();
    Preferences p;
    p.begin("rfhunt", true);
    until = (time_t)p.getLong64("until", 0);
    started = (time_t)p.getLong64("started", 0);
    p.end();
    // Only an active hunt is held in memory; a finished one is served from flash.
    if (!until) return;
    File f = LittleFS.open(HUNT_FILE, "r");
    if (f) {
        JsonDocument d(SpiRamJsonAllocator::instance());
        if (!deserializeJson(d, f)) {
            devices = d["devices"];
            log = d["log"];
            JsonArrayConst dw = d["dwellMs"].as<JsonArrayConst>();
            int i = 0;
            for (JsonVariantConst v : dw) { if (i < 32) dwellMs[i++] = v.as<uint32_t>(); }
            JsonArrayConst su = d["strongUndecoded"].as<JsonArrayConst>();
            i = 0;
            for (JsonVariantConst v : su) { if (i < 32) strongSaved[i++] = v.as<uint32_t>(); }
        }
        f.close();
    }
    Serial.printf("[HUNT] Wetterstations-Suche geladen, Ende %ld, %u Geräte\n",
                  (long)until, (unsigned)devices.as<JsonObject>().size());
}

bool RfHunt::start(time_t untilEpoch) {
    if (!clockSynced() || untilEpoch <= time(nullptr)) return false;
    xSemaphoreTake(huntMutex, portMAX_DELAY);
    until = untilEpoch;
    started = time(nullptr);
    devices.clear(); log.clear();
    memset(dwellMs, 0, sizeof(dwellMs));
    for (int i = 0; i < 32; i++) { strongBase[i] = linkraStrongUndecoded[i]; strongSaved[i] = 0; }
    Preferences p;
    p.begin("rfhunt", false);
    p.putLong64("until", (int64_t)until);
    p.putLong64("started", (int64_t)started);
    p.end();
    dirty = true;
    xSemaphoreGive(huntMutex);
    save(true);
    Serial.printf("[HUNT] Wetterstations-Suche gestartet bis %ld\n", (long)until);
    return true;
}

void RfHunt::stop() {
    xSemaphoreTake(huntMutex, portMAX_DELAY);
    if (!until) { xSemaphoreGive(huntMutex); return; }   // already stopped (two tasks raced)
    for (int i = 0; i < 32; i++) strongCount(i);         // freeze the live counts
    until = 0;
    Preferences p;
    p.begin("rfhunt", false);
    p.putLong64("until", 0);
    p.end();
    xSemaphoreGive(huntMutex);
    save(true);   // keep the results for reading (served from flash)
    xSemaphoreTake(huntMutex, portMAX_DELAY);
    devices.clear(); log.clear();
    devices.shrinkToFit(); log.shrinkToFit();
    xSemaphoreGive(huntMutex);
}

bool RfHunt::active() {
    if (!until) return false;
    // Before NTP sync after a reboot the end time cannot be checked: keep hunting.
    if (clockSynced() && time(nullptr) >= until) {
        stop();
        Serial.println("[HUNT] Wetterstations-Suche beendet (Endzeit erreicht)");
        return false;
    }
    return true;
}

int RfHunt::profile868(uint32_t nowMs) {
    if (!active()) return -1;
    uint32_t cycle = 0;
    for (int i = 0; i < HUNT_STEPS; i++) cycle += HUNT_PLAN[i].ms;
    uint32_t t = nowMs % cycle;
    for (int i = 0; i < HUNT_STEPS; i++) {
        if (t < HUNT_PLAN[i].ms) return profileIndex(HUNT_PLAN[i].id);
        t -= HUNT_PLAN[i].ms;
    }
    return profileIndex(HUNT_PLAN[0].id);
}

void RfHunt::noteDwell(int idx, uint32_t ms) {
    if (idx < 0 || idx >= 32 || !until) return;
    dwellMs[idx] += ms;
}

uint32_t RfHunt::strongCount(int i) {
    // counted only while the hunt runs; strongSaved carries the count across reboots
    if (until) { liveStrong[i] = linkraStrongUndecoded[i] - strongBase[i]; }
    return strongSaved[i] + liveStrong[i];
}

void RfHunt::record(JsonDocument& doc, bool is868, const char* profileId, int rssi, int snr) {
    if (!until || !huntMutex) return;
    const char* model = doc["model"] | "";
    if (!*model) return;
    String key = model;
    if (doc["id"].is<const char*>()) key += "_" + doc["id"].as<String>();
    else if (!doc["id"].isNull()) key += "_" + String(doc["id"].as<long long>());
    if (!doc["channel"].isNull()) key += "_c" + doc["channel"].as<String>();
    const long long now = clockSynced() ? (long long)time(nullptr) : 0;
    String data;
    serializeJson(doc, data);
    if (data.length() > 600) data = data.substring(0, 600);

    xSemaphoreTake(huntMutex, portMAX_DELAY);
    JsonObject devs = devices.is<JsonObject>() ? devices.as<JsonObject>() : devices.to<JsonObject>();
    JsonObject d = devs[key];
    if (d.isNull() && devs.size() >= HUNT_DEV_MAX) {
        // Full: drop the oldest one-off sighting (mostly noise phantoms) to make room
        const char* victim = nullptr; long long oldest = LLONG_MAX;
        for (JsonPair kv : devs) {
            if ((kv.value()["count"] | 0) == 1 && (kv.value()["last"] | 0LL) < oldest) {
                oldest = kv.value()["last"] | 0LL; victim = kv.key().c_str();
            }
        }
        if (victim) devs.remove(String(victim));
    }
    if (d.isNull()) {
        if (devs.size() < HUNT_DEV_MAX) {
            d = devs[key].to<JsonObject>();
            d["model"] = model; d["band"] = is868 ? "868" : "433";
            d["first"] = now; d["count"] = 0; d["rssiMax"] = -128; d["snrMax"] = -99;
        }
    }
    if (!d.isNull()) {
        d["count"] = (d["count"] | 0) + 1;
        d["last"] = now;
        d["rssiLast"] = rssi;
        if (rssi > (d["rssiMax"] | -128)) d["rssiMax"] = rssi;
        if (snr > (d["snrMax"] | -99)) d["snrMax"] = snr;
        d["profile"] = profileId ? profileId : "";
        d["data"] = data;
    }
    JsonArray l = log.is<JsonArray>() ? log.as<JsonArray>() : log.to<JsonArray>();
    JsonObject e = l.add<JsonObject>();
    e["t"] = now; e["key"] = key; e["rssi"] = rssi; e["snr"] = snr;
    e["profile"] = profileId ? profileId : "";
    while (l.size() > HUNT_LOG_MAX) l.remove(0);
    dirty = true;
    xSemaphoreGive(huntMutex);
    save(false);
}

void RfHunt::save(bool force) {
    if (!huntMutex) return;
    if (!force && (!dirty || millis() - lastSaveMs < 10 * 60000UL)) return;
    xSemaphoreTake(huntMutex, portMAX_DELAY);
    File f = LittleFS.open(HUNT_FILE, "w");
    if (f) {
        JsonDocument d(SpiRamJsonAllocator::instance());
        d["until"] = (long long)until;
        d["started"] = (long long)started;
        d["devices"] = devices;
        d["log"] = log;
        JsonArray dw = d["dwellMs"].to<JsonArray>();
        for (int i = 0; i < RX_PROFILE_COUNT && i < 32; i++) dw.add(dwellMs[i]);
        JsonArray su = d["strongUndecoded"].to<JsonArray>();
        for (int i = 0; i < RX_PROFILE_COUNT && i < 32; i++) su.add(strongCount(i));
        serializeJson(d, f);
        f.close();
    }
    lastSaveMs = millis();
    dirty = false;
    xSemaphoreGive(huntMutex);
}

bool RfHunt::hasStoredResult() { return !until && LittleFS.exists(HUNT_FILE); }

String RfHunt::statusJson() {
    const bool on = active();
    JsonDocument d(SpiRamJsonAllocator::instance());
    if (huntMutex) xSemaphoreTake(huntMutex, portMAX_DELAY);
    d["active"] = on;
    d["until"] = (long long)until;
    d["started"] = (long long)started;
    d["now"] = clockSynced() ? (long long)time(nullptr) : 0;
    d["devices"] = devices;
    JsonArray prof = d["profiles"].to<JsonArray>();
    for (int i = 0; i < RX_PROFILE_COUNT && i < 32; i++) {
        if (!dwellMs[i] && !strongCount(i)) continue;
        JsonObject p = prof.add<JsonObject>();
        p["id"] = RX_PROFILES[i].id;
        p["minutes"] = dwellMs[i] / 60000;
        p["strongUndecoded"] = strongCount(i);
    }
    // newest first
    JsonArray out = d["log"].to<JsonArray>();
    JsonArrayConst l = log.as<JsonArrayConst>();
    for (int i = (int)l.size() - 1; i >= 0; i--) out.add(l[i]);
    if (huntMutex) xSemaphoreGive(huntMutex);
    String s;
    serializeJson(d, s);
    return s;
}
