#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include <rtl_433_ESP.h>
#include <ArduinoJson.h>
#include <functional>
#include <vector>
#include "rf_profiles.h"

// pulse_data_t is defined in the rtl_433 C core
extern "C" {
#include "pulse_data.h"
}

// JSON message buffer size for rtl_433_ESP callback
#define RTL433_JSON_BUFFER 1024

/**
 * Decoded RF Signal from rtl_433_ESP
 * Contains parsed JSON fields from the decoder pipeline
 */
struct RFSignal {
    uint32_t timestamp;
    float frequency;
    int rssi;
    String protocol;    // "model" from rtl_433 JSON (e.g. "Acurite-Tower")
    String data;        // Full JSON string from rtl_433
    
    // Decoded fields (from rtl_433 JSON)
    String model;       // Device model name
    int id;             // Device ID
    float temperature;  // Temperature in C (NAN if not present)
    float humidity;     // Humidity % (NAN if not present)
    int battery_ok;     // Battery status (-1 if not present)
    int channel;        // Channel (-1 if not present)
    String rawMessage;  // Original JSON from rtl_433
};

/**
 * Thread-safe cache for RF status data (per module)
 * Updated by RF task, read by Web API (no SPI blocking!)
 */
struct RFModuleStatus {
    bool moduleOk = false;
    bool receiving = false;
    float currentFreq = 433.92;
    int rssi = -128;
    uint32_t totalSignals = 0;
    uint32_t decodedSignals = 0;
    uint32_t unparsedSignals = 0;
    uint32_t lastUpdate = 0;
    int sensitivityLevel = 1;  // 0-3
    bool is868 = false;        // false=433MHz/OOK, true=868MHz/FSK
};

struct RFStatusCache {
    RFModuleStatus module433;
    RFModuleStatus module868;
    uint32_t lastUpdate = 0;
};

// ===== SCAN MODES =====

enum class ScanMode : uint8_t {
    FIXED,
    CONTINUOUS_SWEEP,
    DUAL_SCAN,          // Both modules sweep their bands simultaneously
    DUAL_MODULATION,
};

struct DualModConfig {
    uint32_t ookDwellMs     = 24000;
    uint32_t fskDwellMs     = 6000;
    float fskDeviationKHz   = 47.607f;
    uint8_t bwMDMCFG4       = 0x87;
    uint32_t cycleMs() const { return ookDwellMs + fskDwellMs; }
    int ookPercent() const { return (int)(100.0f * ookDwellMs / cycleMs()); }
};

struct SweepConfig {
    float startFreqMHz  = 433.0;
    float endFreqMHz    = 435.0;
    float stepMHz       = 0;
    uint8_t bwMDMCFG4   = 0xc7;
    uint32_t dwellMs    = 30000;
    uint32_t durationMs = 300000;

    float bwKHz() const {
        switch (bwMDMCFG4) {
            case 0xf7: return 58.0f;
            case 0xc7: return 102.0f;
            case 0x87: return 203.0f;
            case 0x57: return 325.0f;
            default:   return 102.0f;
        }
    }

    float effectiveStepMHz() const {
        if (stepMHz > 0) return stepMHz;
        return (bwKHz() / 2.0f) / 1000.0f;
    }
};

struct FreqHeatmapEntry {
    float freqMHz;
    int rssiPeak;
    int rssiAvgCurrent;
    int32_t rssiSum;
    uint32_t rssiSamples;
    uint32_t signalsDecoded;
    uint32_t signalsUnparsed;
    uint32_t sweepCycles;
};

#define SWEEP_MAX_FREQS 50

// Per-module sweep state for dual parallel scan
struct ModuleSweepState {
    SweepConfig cfg;
    FreqHeatmapEntry* heatmap = nullptr;
    int freqCount = 0;
    int currentIdx = 0;
    uint32_t stepStartMs = 0;
    uint32_t cycleCount = 0;
    bool smartDwellActive = false;   // true = signal detected, waiting for it to end
    uint32_t smartDwellStartMs = 0;  // when signal was first detected
    int rssiPeak = -140;
    int rssiSum = 0;
    int rssiSamples = 0;
    float currentFreqMHz = 0;
    bool complete = false;           // true when all cycles done
    uint32_t signalsThisStep = 0;    // signals decoded on current freq step
};

struct DeviceProfile {
    char deviceKey[48];
    float rxFreqMHz;
    uint32_t lastSeenMs;
    uint32_t avgIntervalMs;
    uint32_t intervals[5];
    uint8_t intervalIdx;
    uint8_t intervalCount;
    bool tagged;
};

#define MAX_DEVICE_PROFILES 64
#define SIGNAL_HISTORY_MAX 15

struct RFModuleConfig {
    uint8_t csPin;
    uint8_t gdo0Pin;
    uint8_t sckPin;
    uint8_t misoPin;
    uint8_t mosiPin;
    float frequency;
    bool is868;
    const char* name;
};

// ===== 868 MHz FSK Receiver Constants =====
#define RX868_RECEIVER_BUFFER_SIZE  2
#define RX868_MINIMUM_PULSE_LENGTH  30      // 17.24kbps: 30µs optimal for LaCrosse/AmbientWeather (58µs bits)
#define RX868_MINIMUM_SIGNAL_LENGTH 500     // FSK signals can be short
#define RX868_RSSI_SAMPLES          1000

/**
 * RF Manager - Dual CC1101 (433 OOK + 868 FSK)
 * 
 * Architecture:
 *   Module 433 (OOK): Full rtl_433_ESP library (receiver + decoder pipeline)
 *   Module 868 (FSK): Own interrupt-driven receiver → feeds same decoder queue
 *   
 *   Both modules share ONE decoder task running both OOK and FSK demods.
 *   Source band is identified via pulse_data_t.freq1_hz tag.
 */
class RFManager {
public:
    static RFManager& getInstance();
    
    void begin();
    void startReceiving();
    void stopReceiving();
    bool isModuleOk(bool module868 = false);
    
    typedef std::function<void(RFSignal&, bool is868)> SignalCallback;
    void onSignal(SignalCallback cb);
    
    std::vector<RFSignal> getLastSignals(int count = 10);
    void clearSignals();
    
    RFStatusCache getStatusCache();
    RFModuleStatus getModuleStatus(bool module868);
    int getCurrentRSSI(bool module868 = false);
    float getCurrentFrequency(bool module868 = false);
    
    // Diagnostics for 868 module
    String getDiag868JSON();
    
    void setScanMode(ScanMode mode, const SweepConfig& cfg = SweepConfig());
    void startDualScan(const SweepConfig& cfg433, const SweepConfig& cfg868);
    ScanMode getScanMode();
    
    void setRSSISensitivity(int level, bool module868 = false);
    int getRSSISensitivity(bool module868 = false);
    
    String getSweepStatusJSON();
    String getHeatmapJSON();
    String getDualScanStatusJSON();
    String getDualScanHeatmapJSON();
    
    // Legacy API (hardware dual replaces time-division)
    void setDualModulation(bool enable, const DualModConfig& cfg = DualModConfig()) { /* no-op */ }
    bool isDualModActive() { return true; }
    bool isCurrentModeOOK() { return true; }
    String getDualModStatusJSON();
    
    void tagDevice(const char* deviceKey, bool tagged);
    String getDeviceProfilesJSON();

    // ===== RECEIVE PROFILES (time-division over modulation/bitrate) =====
    String getRxProfilesJSON();
    // Tuning: temporary parameter set for slot tune433/tune868, pinned for durationMs
    bool setTune(const RxProfile& params, uint32_t durationMs);
    void setProfileRotation(bool enabled);
    bool pinProfile(const char* id, uint32_t durationMs);  // hold one profile for a test
    void applyHomeProfiles();
    
private:
    RFManager() {}
    static RFManager* instance;
    
    // ===== MODULE 1: 433 MHz OOK (rtl_433_ESP library) =====
    struct Module433 {
        bool moduleOk = false;
        bool receiving = false;
        float currentFreq = 433.92;
        uint32_t totalSignals = 0;
        uint32_t decodedSignals = 0;
        uint32_t unparsedSignals = 0;
        int rssiSensitivityLevel = 0;  // 0=most sensitive (delta 3)
        TaskHandle_t loopTaskHandle = nullptr;
    } mod433;
    
    // ===== MODULE 2: 868 MHz FSK (own receiver → shared decoder) =====
    struct Module868 {
        SPIClass* spi = nullptr;
        CC1101* radio = nullptr;
        Module* radioModule = nullptr;
        bool moduleOk = false;
        bool receiving = false;
        float currentFreq = 868.35;
        uint32_t totalSignals = 0;
        uint32_t decodedSignals = 0;
        uint32_t unparsedSignals = 0;
        int rssiSensitivityLevel = 0;  // 0=most sensitive (delta 3)
        TaskHandle_t receiverTaskHandle = nullptr;
    } mod868;
    
    std::vector<RFSignal> signalHistory;
    SignalCallback signalCallback;
    SemaphoreHandle_t historyMutex = nullptr;
    
    RFStatusCache statusCache;
    SemaphoreHandle_t cacheMutex = nullptr;
    void updateStatusCache();
    
    // Unified callback (reads currentSignalIs868 from signalDecoder)
    static void rtl433Callback(char* message);
    
    void initModule433();
    void initModule868();
    void processSignal(char* message, bool is868, int forcedProfile = -1);
    
    // ===== 433 LOOP TASK (bridges ISR pulse trains → decoder queue) =====
    static void rf433LoopTask(void* pvParameters);
    
    // ===== 868 RECEIVER (interrupt-driven, mirrors rtl_433_ESP pattern) =====
    static void IRAM_ATTR rx868InterruptHandler();
    static void rx868ReceiverTask(void* pvParameters);
    
    // Static state for ISR + receiver task access
    static volatile bool rx868_receiveMode;
    static volatile unsigned long rx868_signalStart;
    static volatile unsigned long rx868_signalEnd;
    static volatile unsigned long rx868_lastChange;
    static volatile int16_t rx868_nrpulses;
    static pulse_data_t* rx868_pulseTrains;
    static volatile uint8_t rx868_actualPulseTrain;
    static uint8_t rx868_availPulseTrain;
    static bool rx868_enabledReceiver;
    static int rx868_currentRssi;
    static int rx868_signalRssi;
    static int rx868_rssiThreshold;
    static int rx868_averageRssi;
    static int rx868_totalSignals;
    static int rx868_ignoredSignals;
    static volatile uint32_t rx868_isrEdgeCount;  // GDO0 edge counter for diagnostics
    static volatile uint32_t rx868_taskLoopCount;  // Receiver task loop counter
    
    static int rx868_getRSSI();
    void rx868_resetReceiver();
    void rx868_enableReceiver();
    void rx868_disableReceiver();
    int rx868_receivePulseTrain();
    
    // ===== SCAN MODE STATE =====
    volatile ScanMode scanMode = ScanMode::FIXED;
    SweepConfig sweepCfg;
    
    FreqHeatmapEntry* heatmap = nullptr;
    int sweepFreqCount = 0;
    int sweepCurrentIdx = 0;
    uint32_t sweepStepStartMs = 0;
    uint32_t sweepStartMs = 0;
    uint32_t sweepCycleCount = 0;
    
    // Dual scan state (per-module)
    ModuleSweepState dualSweep433;
    ModuleSweepState dualSweep868;
    uint32_t dualScanStartMs = 0;
    uint32_t dualScanDurationMs = 0;  // 0 = use per-module cycle limits
    StackType_t* dualScanStack = nullptr;  // PSRAM-allocated task stack
    StaticTask_t dualScanTaskBuf;          // Static task control block
    
    void setFrequencyFast(float freqMHz, bool module868);
    void setBandwidthReg(uint8_t mdmcfg4Val, bool module868);
    void ensureAGCFix(bool module868);
    static void sweepTaskFunc(void* pvParameters);
    static void dualScanTaskFunc(void* pvParameters);
    
    // Dual scan helpers
    void dualScanInitModule(ModuleSweepState& state, bool is868);
    bool dualScanStepModule(ModuleSweepState& state, bool is868);
    void dualScanRestoreModule(bool is868);
    
    struct RxProfileStats {
        uint32_t visits = 0;
        uint32_t decodes = 0;
        uint32_t unparsed = 0;
        uint32_t lastDecodeMs = 0;
        uint32_t activeMs = 0;
    };
    RxProfileStats rxStats[RX_PROFILE_COUNT];
    int activeProfile[2] = {-1, -1};        // [0]=433, [1]=868
    uint32_t profileStartMs[2] = {0, 0};
    int lastAlternative[2] = {-1, -1};
    bool profileRotation = true;
    int pinnedProfile = -1;
    uint32_t pinnedUntilMs = 0;
    SemaphoreHandle_t radioMutex = nullptr;
    TaskHandle_t profileTaskHandle = nullptr;
    static volatile uint32_t rx868_pulseTag;
    static volatile float rx868_tagFreqHz;
    static volatile uint16_t rx868_minPulseUs;
    static volatile bool rx868_ook;
    static volatile int8_t rx868_freqEst;   // last FREQEST during a signal (1 LSB = 1.587 kHz)
    // Wireless M-Bus packet mode on the 868 CC1101
    volatile bool wmbusActive = false;
    bool wmbusRegsSaved = false;
    uint8_t wmbusSavedRegs[0x2F];
    struct { uint32_t syncs = 0, ok = 0, bad = 0, overflow = 0; char lastError[48] = ""; uint8_t version = 0, marc = 0, rxbytes = 0, pktctrl0 = 0, mdmcfg2 = 0, iocfg0 = 0; uint32_t isr = 0, wakes = 0, maxFill = 0, isrUs = 0, latencyUs = 0, wakeUs = 0, spiUs = 0; uint8_t firstRx = 0, mdmcfg4 = 0, mdmcfg3 = 0, deviatn = 0, freq2 = 0, freq1 = 0, freq0 = 0; } wmbusStats;
    void wmbusEnter(const RxProfile& p);
    void wmbusLeave();
    bool wmbusPoll(int profileIdx);  // true while a frame is being received
    static void IRAM_ATTR wmbusFifoIsr();
    static void wmbusTaskFunc(void* pv);
    TaskHandle_t wmbusTaskHandle = nullptr;
    uint8_t cc868Status(uint8_t reg);
    void cc868Strobe(uint8_t cmd);
    void cc868ReadFifo(uint8_t* buf, uint8_t n);
    int homeProfile(bool is868) const;
    int nextAlternative(bool is868);
    uint32_t effectiveDwell(int idx) const;
    int16_t applyProfile(int idx);  // caller holds radioMutex
    RxProfile tuneParams[2];        // overrides for tune433 / tune868
    bool tuneSet[2] = {false, false};
    RxProfile effectiveProfile(int idx) const;  // RX_PROFILES[idx] with tune overrides applied
    // Register watchdog: detects writes by anyone else (e.g. the EBYTE kit MCU) or a module power loss
    uint8_t regSnap[2][9];
    bool regSnapValid[2] = {false, false};
    uint32_t regErrors[2] = {0, 0};
    char regErrorLast[2][64] = {"", ""};
    static void rssiFrameSample(bool wmbus);   // 5 Hz live level frames for the web UI
    uint32_t implausibleDrops = 0;
    uint32_t unconfirmedDrops = 0;       // first sightings of new devices awaiting confirmation      // decodes rejected by the plausibility filter
    char implausibleLast[64] = "";
    uint32_t lastRegCheck[2] = {0, 0};
    void snapshotRegs(int m);
    bool checkRegs(int m);   // true = registers as expected
    static void profileTaskFunc(void* pvParameters);

    DeviceProfile* deviceProfiles = nullptr;
    int deviceProfileCount = 0;
    DeviceProfile* findOrCreateProfile(const char* deviceKey);
    void updateDeviceProfile(const char* deviceKey, float freqMHz, bool is868);
};
