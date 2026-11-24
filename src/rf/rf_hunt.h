#pragma once
// Weather-station hunt: for a fixed time (e.g. overnight) the 868 MHz module
// cycles through the profiles of common weather stations that the normal
// rotation only probes briefly (Bresser 5/6/7-in-1 FSK 8.2 kbit/s, 868 OOK
// Fine Offset), and every decode is logged on the device. The end time is kept
// in NVS and the summary in /littlefs/hunt.json, so a reboot does not lose it.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>

// ArduinoJson allocator in PSRAM: the hunt keeps up to ~60 KB of JSON and the
// many small strings would otherwise land in the scarce internal heap
// (2026-11-22: free heap 90 -> 47 KB, WiFi/TCP stalled).
struct SpiRamJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override;
    void deallocate(void* p) override;
    void* reallocate(void* p, size_t n) override;
    static SpiRamJsonAllocator* instance();
};

class RfHunt {
public:
    static RfHunt& get();
    void begin();                                  // load state after boot
    bool start(time_t untilEpoch);                 // false if clock not synced
    void stop();
    bool active();
    // Profile index the 868 module should use now, -1 if the hunt is not active
    int profile868(uint32_t nowMs);
    // Called for every plausible decode (both bands)
    void record(JsonDocument& doc, bool is868, const char* profileId, int rssi, int snr);
    void noteDwell(int profileIdx, uint32_t ms);  // time spent per profile
    String statusJson();
    bool hasStoredResult();                        // finished hunt: serve /hunt.json from flash
    static bool clockSynced() { return time(nullptr) > 1700000000; }

private:
    void save(bool force);
    time_t until = 0, started = 0;
    uint32_t lastSaveMs = 0;
    bool dirty = false;
    JsonDocument devices{SpiRamJsonAllocator::instance()};   // key -> {model, band, count, ...}
    JsonDocument log{SpiRamJsonAllocator::instance()};       // last decodes
    uint32_t dwellMs[32] = {0};
    uint32_t strongBase[32] = {0};
    uint32_t strongSaved[32] = {0};   // from before the last reboot
    uint32_t liveStrong[32] = {0};    // since boot / start
    uint32_t strongCount(int i);
};
