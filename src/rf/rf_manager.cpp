#include "rf_manager.h"
#include "wmbus.h"
#include "rf_hunt.h"
#include "rx433_rmt.h"
extern "C" volatile uint32_t linkra_gate_skipped, linkra_gate_run;   // r_api.c width gate
#include <map>
#include <set>
static volatile uint32_t cc868TxnUs = 0, cc868RdyUs = 0;  // wM-Bus SPI timing diagnostics
// Profiles that never produced a decode here are only probed every Nth round.
static uint8_t probeEvery(const char* id) {
    return (strcmp(id, "fsk868-8k") == 0 || strcmp(id, "ook868") == 0) ? 4 : 1;
}

// ---- 868 receive trace (diagnostics): every reception attempt with its outcome ----
struct Trace868 { uint32_t ms; uint32_t durUs; uint16_t pulses; int8_t rssi, thr, noise; char kind; char tag[18]; };
static Trace868* trace868 = nullptr;
static volatile uint32_t trace868Head = 0;   // monotonically increasing write index
static const uint32_t TRACE868_N = 1024;
static void trace868Add(char kind, uint32_t durUs, uint16_t pulses, int rssi, int thr, int noise, const char* tag) {
    if (!trace868) {
        trace868 = (Trace868*)heap_caps_calloc(TRACE868_N, sizeof(Trace868), MALLOC_CAP_SPIRAM);
        if (!trace868) return;
    }
    Trace868& t = trace868[trace868Head % TRACE868_N];
    t.ms = millis(); t.durUs = durUs; t.pulses = pulses; t.rssi = rssi; t.thr = thr; t.noise = noise; t.kind = kind;
    strncpy(t.tag, tag ? tag : "", sizeof t.tag - 1); t.tag[sizeof t.tag - 1] = 0;
    trace868Head++;
}
String getTrace868(uint32_t sinceMs) {
    String out; out.reserve(32768);
    out += "t_ms;kind;dur_us;pulses;rssi;thr;noise;tag\n";
    if (!trace868) return out;
    const uint32_t head = trace868Head, start = head > TRACE868_N ? head - TRACE868_N : 0;
    char line[96];
    for (uint32_t i = start; i < head; i++) {
        const Trace868& t = trace868[i % TRACE868_N];
        if (t.ms < sinceMs) continue;
        snprintf(line, sizeof line, "%lu;%c;%lu;%u;%d;%d;%d;%s\n", (unsigned long)t.ms, t.kind,
                 (unsigned long)t.durUs, t.pulses, t.rssi, t.thr, t.noise, t.tag);
        out += line;
    }
    return out;
}
static inline void clearTrainSlot(pulse_data_t& t, int upto) {   // ISR relies on zeroed arrays
    if (upto > PD_MAX_PULSES) upto = PD_MAX_PULSES;
    for (int x = 0; x < upto; x++) { t.pulse[x] = 0; t.gap[x] = 0; }
    t.num_pulses = 0;
}
#include "../core/event_bus.h"
#include "../storage/device_database.h"
#include "../integration/mqtt_bridge.h"
#include <ArduinoJson.h>

// Include signalDecoder for processSignal() and currentSignalIs868 flag
#include "signalDecoder.h"

// Hardware pin definitions come from platformio.ini -include flag
// (config/hardware_breadboard.h is force-included via build_flags)

// ===== Static member definitions =====
RFManager* RFManager::instance = nullptr;

// 868 receiver static state (accessible from ISR)
volatile bool RFManager::rx868_receiveMode = false;
volatile unsigned long RFManager::rx868_signalStart = 0;
volatile unsigned long RFManager::rx868_signalEnd = 0;
volatile unsigned long RFManager::rx868_lastChange = 0;
volatile int16_t RFManager::rx868_nrpulses = 0;
pulse_data_t* RFManager::rx868_pulseTrains = nullptr;
volatile uint8_t RFManager::rx868_actualPulseTrain = 0;
uint8_t RFManager::rx868_availPulseTrain = 0;
bool RFManager::rx868_enabledReceiver = false;
int RFManager::rx868_currentRssi = -128;
int RFManager::rx868_signalRssi = -128;
int RFManager::rx868_rssiThreshold = MINRSSI;
int RFManager::rx868_averageRssi = -128;
int RFManager::rx868_totalSignals = 0;
int RFManager::rx868_ignoredSignals = 0;
volatile uint32_t RFManager::rx868_isrEdgeCount = 0;
volatile uint32_t RFManager::rx868_taskLoopCount = 0;
volatile uint32_t RFManager::rx868_pulseTag = 0;
volatile float RFManager::rx868_tagFreqHz = 868350000.0f;
volatile uint16_t RFManager::rx868_minPulseUs = RX868_MINIMUM_PULSE_LENGTH;
volatile bool RFManager::rx868_ook = false;
volatile int8_t RFManager::rx868_freqEst = 0;

// Global rtl_433_ESP instance for 433 MHz (module 1)
static rtl_433_ESP rf433;

// Shared callback buffer (decoder task is single-threaded, one at a time)
static char msgBuffer[RTL433_JSON_BUFFER];

RFManager& RFManager::getInstance() {
    static RFManager inst;
    return inst;
}

// ===================================================================
//  BEGIN: Initialize both modules
// ===================================================================
void RFManager::begin() {
    instance = this;
    
    Serial.println("[RF] Initializing dual CC1101 modules...");
    
    cacheMutex = xSemaphoreCreateMutex();
    historyMutex = xSemaphoreCreateMutex();
    radioMutex = xSemaphoreCreateMutex();
    RfHunt::get().begin();
    
    // Allocate 868 pulse train buffer from internal RAM
    rx868_pulseTrains = (pulse_data_t*)heap_caps_calloc(
        RX868_RECEIVER_BUFFER_SIZE, sizeof(pulse_data_t), MALLOC_CAP_INTERNAL);
    if (!rx868_pulseTrains) {
        Serial.println("[RF] FATAL: Cannot allocate 868 pulse train buffer!");
        return;
    }
    
    // Initialize 433 MHz module (OOK) - uses rtl_433_ESP library
    initModule433();
    
    // Initialize 868 MHz module (FSK) - own receiver
    initModule868();
    
    // 433: +6 dB. Sensors arrive at -87..-89 dBm with a -97 dBm floor, so +9 dB sat right at
    // the signal level (5 vs 8 decodes in 5 min). The flood seen earlier at +6 dB is gone since
    // the separate 868 queue, the 800-ms limit and the pre-trigger ring.
    setRSSISensitivity(1, false);
    setRSSISensitivity(1, true);
    // Both modules boot in their home profile; tag pulse trains accordingly.
    const int h433 = homeProfile(false), h868 = homeProfile(true);
    activeProfile[0] = h433; activeProfile[1] = h868;
    profileStartMs[0] = profileStartMs[1] = millis();
    rxStats[h433].visits++; rxStats[h868].visits++;
    rtl_433_ESP::tagFreqHz = RX_PROFILES[h433].freqMHz * 1e6f;
    rtl_433_ESP::pulseTag = rxProfileTag(h433);
    rx868_tagFreqHz = RX_PROFILES[h868].freqMHz * 1e6f;
    rx868_pulseTag = rxProfileTag(h868);
    rx868_minPulseUs = RX_PROFILES[h868].minPulseUs;
    xTaskCreatePinnedToCore(wmbusTaskFunc, "wmbus", 6144, this, 22, &wmbusTaskHandle, 1);
    applyHomeProfiles();  // write home configs (they differ from the init defaults)
    // Priority 3: above the rtl_433 decoder (2) on core 1, which otherwise starved
    // the rotation for minutes under noise load (both radios stuck in one profile).
    xTaskCreatePinnedToCore(profileTaskFunc, "rfProfiles", 4096, this, 3,
                            &profileTaskHandle, 1);
    
    Serial.println("[RF] Both modules initialized");
}

// ===================================================================
//  MODULE 1: 433 MHz OOK (rtl_433_ESP library handles everything)
// ===================================================================
void RFManager::initModule433() {
    Serial.println("[RF] Module 433: Initializing...");
    
    mod433.currentFreq = RF_MODULE_FREQUENCY;
    
    // rtl_433_ESP handles SPI init, CC1101 config, interrupt, receiver task,
    // decoder task (with ALL 275 OOK+FSK decoders), and callback
    // RadioLib setFrequency() expects MHz — RF_MODULE_FREQUENCY is in MHz from platformio.ini
    // Center at 433.67 MHz + 650kHz BW → covers 433.345–433.995 MHz
    // Somfy RTS 433.42: 250kHz from center ✓  Standard 433.92: 250kHz from center ✓
    // 433 pulses are captured by the RMT peripheral (rx433_rmt.cpp); the library
    // only drives the radio and the RSSI / noise-floor task.
    // LINKRA_433_DUAL_CAPTURE: GPIO-ISR and RMT capture listen to the same pin and
    // both feed the decoders; events carry "capture":"isr"|"rmt" (A/B under identical RF).
#ifndef LINKRA_433_DUAL_CAPTURE
#define LINKRA_433_DUAL_CAPTURE 0   // 1 = A/B test (both captures feed the decoders)
#endif
    rtl_433_ESP::externalCapture = !LINKRA_433_DUAL_CAPTURE;
    rf433.initReceiver(CC1101_GDO0_433, RF_MODULE_FREQUENCY);
    rf433.enableReceiver();     // no GPIO interrupt with externalCapture, just _enabledReceiver
    if (!Rx433Rmt::begin(CC1101_GDO0_433)) {
        Serial.println("[RF] 433: RMT capture failed - falling back to the GPIO interrupt");
        rtl_433_ESP::externalCapture = false;
        rf433.enableReceiver();
    }
    
    // Set unified callback — reads currentSignalIs868 flag from signalDecoder
    rf433.setCallback(rtl433Callback, msgBuffer, sizeof(msgBuffer));
    
    // Create a dedicated task to call rf433.loop() — this is the BRIDGE
    // that moves completed pulse trains from the ISR ring buffer to the
    // decoder queue. Without this, 433 signals are captured but never decoded!
    // (The library's ReceiverTask only does RSSI monitoring + signal detection.)
    xTaskCreatePinnedToCore(
        rf433LoopTask,
        "rf433Loop",
        4096,                      // Minimal stack — loop() just copies and enqueues
        nullptr,
        1,                         // Below receiver priority (2)
        &mod433.loopTaskHandle,
        0                          // Core 0 (same as receiver)
    );
    
    mod433.moduleOk = true;
    mod433.receiving = true;
    
    Serial.println("[RF] Module 433: Ready (433.92 MHz OOK)");
}

// ===================================================================
//  MODULE 1: 433 MHz LOOP TASK
//  Calls rf433.loop() which bridges ISR pulse trains → decoder queue.
//  The library expects this to be called continuously from Arduino loop()
//  but we run it as a dedicated FreeRTOS task instead.
// ===================================================================
void RFManager::rf433LoopTask(void* pvParameters) {
    // Wait briefly for receiver to be fully initialized
    vTaskDelay(pdMS_TO_TICKS(100));
    
    static int lastTaskLoops = 0;
    static unsigned long lastWatchdogCheck = 0;
    
    for (;;) {
        rf433.loop();  // Already contains vTaskDelay(1) at the end
        
        // Watchdog: if ReceiverTask stops updating (stuck at -128),
        // log a warning. This helps diagnose intermittent boot failures.
        unsigned long now = millis();
        if (now - lastWatchdogCheck > 10000) { // every 10s
            lastWatchdogCheck = now;
            int loops = rtl_433_ESP::receiverTaskLoops;
            if (loops == lastTaskLoops && loops > 0) {
                Serial.printf("[RF] ⚠️ 433 ReceiverTask stalled! loops=%d, rssi=%d\n",
                    loops, (int)rtl_433_ESP::currentRssi);
            }
            lastTaskLoops = loops;
        }
    }
}

// ===================================================================
//  MODULE 2: 868 MHz FSK (own CC1101 + interrupt-driven receiver)
//  Feeds pulse trains into the SAME decoder queue as module 1
// ===================================================================
void RFManager::initModule868() {
    Serial.println("[RF] Module 868: Initializing...");
    
    // Create SPI3 instance (separate from FSPI/SPI2 used by 433 module)
    mod868.spi = new SPIClass(HSPI);  // SPI3_HOST on ESP32-S3
    mod868.spi->begin(CC1101_SCK_868, CC1101_MISO_868, CC1101_MOSI_868, CC1101_CS_868);
    
    // Create RadioLib Module with custom SPI
    mod868.radioModule = new Module(
        CC1101_CS_868, CC1101_GDO0_868, RADIOLIB_NC, CC1101_GDO0_868,
        *mod868.spi
    );
    
    // Create CC1101 instance
    mod868.radio = new CC1101(mod868.radioModule);
    
    // Initialize radio
    int state = mod868.radio->begin();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: radio.begin() failed, code %d\n", state);
        mod868.moduleOk = false;
        return;
    }
    Serial.println("[RF] Module 868: Radio initialized");
    
    // Set frequency
    state = mod868.radio->setFrequency(868.35);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: setFrequency failed, code %d\n", state);
    }
    
    // Configure for FSK mode
    state = mod868.radio->setOOK(false);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: setOOK(false) failed, code %d\n", state);
    }
    
    state = mod868.radio->setCrcFiltering(false);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: setCrcFiltering failed, code %d\n", state);
    }
    
    // FSK parameters — wideband multi-protocol reception
    // Goal: receive ALL common 868 MHz FSK protocols through one config.
    // The CC1101 bitrate setting controls a digital post-discriminator filter.
    // At 17.24 kbps only LaCrosse/AmbientWeather pass; Bresser (8.8k),
    // Honeywell CM921 (38.4k), M-Bus (32.7k), Somfy io (38.4k) are filtered out.
    // FSK parameters — tuned for LaCrosse/AmbientWeather (17.24 kbps).
    // Tested: 50 kbps decoded NEW device (Marlec-Solar, 20µs bits) but
    // causes crashes every 1-3 min due to ISR noise overload.
    // 100 kbps even worse (sub-bit noise breaks PCM slicers).
    // 17.24 kbps = stable, decodes LaCrosse-TX29, AmbientWeather-WH31E.
    // TODO: investigate time-division multiplexing (switch configs periodically)
    // to support both slow (17k) and fast (50k) protocols.
    state = mod868.radio->setFrequencyDeviation(40.0);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: setFreqDev failed, code %d\n", state);
    }
    
    state = mod868.radio->setBitRate(17.24);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: setBitRate failed, code %d\n", state);
    }
    
    state = mod868.radio->setRxBandwidth(270.0);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: setRxBW failed, code %d\n", state);
    }
    
    state = mod868.radio->disableSyncWordFiltering(false);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: disableSyncWord failed, code %d\n", state);
    }
    
    // CC1101-specific: Set packet length to 0 (infinite mode for async)
    state = mod868.radioModule->SPIsetRegValue(RADIOLIB_CC1101_REG_PKTLEN, 0);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: PKTLEN failed, code %d\n", state);
    }
    
    // ============================================================
    // CC1101 FSK async register tuning (adapted from 433 OOK config)
    // MUST be written BEFORE receiveDirectAsync() — CC1101 config
    // registers require IDLE state for reliable writes!
    // Without these, RadioLib defaults cause:
    //  - FOCCFG carrier-sense gate blocks FOC in async mode (no sync word!)
    //  - AGC settings suboptimal for wideband reception
    //  - Lower LNA/mixer currents reduce sensitivity
    // Result: signals are received but demodulation quality is poor,
    // causing ALL 868 decoders to fail (every signal just "classified").
    // ============================================================
    
    // IF Frequency: 152.3 kHz (vs default 304.7 kHz)
    // Centers signal within channel filter for cleaner demodulation.
    state = mod868.radioModule->SPIsetRegValue(RADIOLIB_CC1101_REG_FSCTRL1, 0x06);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: FSCTRL1 failed, code %d\n", state);
    }
    
    // Frequency Offset Compensation: ENABLED for FSK, gate OFF
    // Default 0x36 has CS gate ON — blocks FOC in async mode (no sync word!).
    // For FSK: enable FOC with ±BW_chan/4 limit (±67.5 kHz for 270kHz BW)
    // to track signals offset from center (e.g., HomeMatic at 868.3 vs our 868.35).
    // Gain = 3K (most aggressive) for fast preamble tracking.
    // OOK module uses 0x18 (FOC limit=0, disabled) — FSK needs active correction!
    state = mod868.radioModule->SPIsetRegValue(RADIOLIB_CC1101_REG_FOCCFG, 0x1A);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: FOCCFG failed, code %d\n", state);
    }
    
    // AGC Control 2: DVGA all gain steps, MAX LNA gain, MAGN_TARGET 42dB
    // For RX filter BW > 100kHz, MAGN_TARGET = 0x07 (42dB). Same as 433 OOK.
    state = mod868.radioModule->SPIsetRegValue(RADIOLIB_CC1101_REG_AGCCTRL2, 0x07);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: AGCCTRL2 failed, code %d\n", state);
    }
    
    // AGC Control 1: No carrier sense threshold — all signals pass through.
    // Default 0x40 has CARRIER_SENSE_ABS_THR which can block weak signals.
    state = mod868.radioModule->SPIsetRegValue(RADIOLIB_CC1101_REG_AGCCTRL1, 0x00);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: AGCCTRL1 failed, code %d\n", state);
    }
    
    // AGC Control 0: Medium hysteresis, 16 samples wait time.
    // OOK boundary bits (2:0) are ignored in FSK mode.
    state = mod868.radioModule->SPIsetRegValue(RADIOLIB_CC1101_REG_AGCCTRL0, 0x91);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: AGCCTRL0 failed, code %d\n", state);
    }
    
    // Frontend RX: Higher LNA and mixer currents for better sensitivity.
    // Chip default 0x56 uses lower currents. 0xB6 = Flipper Zero standard,
    // proven to significantly improve receive sensitivity.
    state = mod868.radioModule->SPIsetRegValue(RADIOLIB_CC1101_REG_FREND1, 0xB6);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: FREND1 failed, code %d\n", state);
    }
    
    Serial.println("[RF] Module 868: CC1101 FSK registers tuned (FOC, AGC, FREND1)");
    
    // Start async direct reception (GDO0 = serial data output)
    // NOTE: Must come AFTER register tuning — SRX strobe enters RX state
    state = mod868.radio->receiveDirectAsync();
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[RF] Module 868: receiveDirectAsync failed, code %d\n", state);
        mod868.moduleOk = false;
        return;
    }
    
    // Reset receiver state
    rx868_resetReceiver();
    
    // Enable interrupt on GDO0 for pulse capture
    rx868_enableReceiver();
    
    // Create receiver task (RSSI monitoring + signal completion)
    Serial.printf("[RF] Module 868: Free heap before task: %d bytes\n", 
        ESP.getFreeHeap());
    BaseType_t taskResult = xTaskCreatePinnedToCore(
        rx868ReceiverTask,
        "rx868Receiver",
        8192,                      // 8KB stack — RSSI polling + pulse copy to PSRAM (was 16KB)
        this,
        2,                         // Same priority as rtl_433 receiver
        &mod868.receiverTaskHandle,
        0                          // Core 0 (same as rtl_433 receiver)
    );
    
    if (taskResult != pdPASS) {
        Serial.printf("[RF] Module 868: ERROR - Task creation FAILED! Result=%d, Free heap=%d\n",
            taskResult, ESP.getFreeHeap());
        mod868.moduleOk = false;
        return;
    }
    Serial.printf("[RF] Module 868: Receiver task created OK (handle=%p)\n", 
        mod868.receiverTaskHandle);
    
    mod868.moduleOk = true;
    mod868.receiving = true;
    mod868.currentFreq = 868.35;
    
    Serial.println("[RF] Module 868: Ready (868.35 MHz FSK)");
}

// ===================================================================
//  868 RECEIVER: Interrupt Handler (IRAM — called on every GDO0 edge)
//  Records pulse/gap timing, same pattern as rtl_433_ESP::interruptHandler
// ===================================================================
// ---- Pre-trigger ring (FSK): edges recorded while NOT receiving ----
// Reception only starts when the 1-ms RSSI poll sees the carrier; by then the
// preamble and sync word of a short telegram (LaCrosse: ~4 ms) were gone and the
// decoder could not lock. The ISR therefore always keeps the last 64 edges; when
// reception starts, the last 2.5 ms are copied to the front of the pulse train.
#define PRE868_N 64
static volatile uint32_t pre868T[PRE868_N];     // edge time (us)
static volatile uint16_t pre868Dur[PRE868_N];   // duration of the level that just ended
static volatile uint8_t  pre868High[PRE868_N];  // 1 = that level was high (pulse)
static volatile uint32_t pre868Idx = 0;
static portMUX_TYPE pre868Mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t pre868LastEdge = 0;

void IRAM_ATTR RFManager::rx868InterruptHandler() {
    rx868_isrEdgeCount++;  // Count ALL edges for diagnostics
    if (!rx868_enabledReceiver) return;
    if (!rx868_receiveMode) {
        if (rx868_ook) return;
        const uint32_t now = micros();
        const uint32_t d = now - pre868LastEdge;
        if (d > (uint32_t)rx868_minPulseUs) {
            portENTER_CRITICAL_ISR(&pre868Mux);
            const uint32_t i = pre868Idx % PRE868_N;
            pre868T[i] = now;
            pre868Dur[i] = d > 65535 ? 65535 : d;
            pre868High[i] = digitalRead(CC1101_GDO0_868) ? 0 : 1;   // now low -> the ended level was high
            pre868Idx++;
            portEXIT_CRITICAL_ISR(&pre868Mux);
            pre868LastEdge = now;
        }
        return;
    }
    
    volatile pulse_data_t& pulseTrain = rx868_pulseTrains[rx868_actualPulseTrain];
    volatile int* pulse = pulseTrain.pulse;
    volatile int* gap = pulseTrain.gap;
    
    const unsigned long now = micros();
    const unsigned int duration = now - rx868_lastChange;
    
    // Filter noise — FSK pulses can be as short as 30µs
    // NOTE: No per-pulse RSSI check here. For FSK, RSSI dips during
    // frequency transitions, and the stale rx868_currentRssi (updated
    // ~1ms by ReceiverTask) would block valid pulses. The receiveMode
    // gate above is sufficient — same fix as the 433 OOK ISR.
    if (duration > (unsigned int)rx868_minPulseUs) {
        
        if (!digitalRead(CC1101_GDO0_868)) {
            // Falling edge — end of pulse
            pulse[rx868_nrpulses] = duration;
        } else {
            // Rising edge — end of gap
            if (pulse[rx868_nrpulses] > 0) {
                gap[rx868_nrpulses] = duration;
                rx868_nrpulses = (rx868_nrpulses + 1) % PD_MAX_PULSES;
            } else if (rx868_nrpulses > 1) {
                // Random positive blip — extend previous gap
                gap[rx868_nrpulses - 1] += duration;
            } else {
                gap[rx868_nrpulses] = duration;
                rx868_nrpulses = (rx868_nrpulses + 1) % PD_MAX_PULSES;
            }
        }
        rx868_lastChange = now;
    }
}

// ===================================================================
//  868 RECEIVER: RSSI reading via direct SPI register read
//  Bypasses CC1101::getRSSI() which may have directModeEnabled=false
//  bug causing it to return the cached rawRSSI (0 → -74 or 148 → -128)
//  instead of the live register 0x34.
// ===================================================================
int RFManager::rx868_getRSSI() {
    if (instance && instance->mod868.radioModule) {
        // Read RSSI status register directly (0x34 needs burst bit 0x40)
        uint8_t rawRssi = instance->mod868.radioModule->SPIreadRegister(
            RADIOLIB_CC1101_REG_RSSI | RADIOLIB_CC1101_CMD_ACCESS_STATUS_REG);
        // CC1101 RSSI formula (DN505): offset = 74 dB
        if (rawRssi >= 128) {
            return (((int)rawRssi - 256) / 2) - 74;
        } else {
            return ((int)rawRssi / 2) - 74;
        }
    }
    return -128;
}

// ===================================================================
//  868 RECEIVER: Reset state
// ===================================================================
void RFManager::rx868_resetReceiver() {
    for (unsigned int i = 0; i < RX868_RECEIVER_BUFFER_SIZE; i++) {
        rx868_pulseTrains[i].num_pulses = 0;
    }
    rx868_availPulseTrain = 0;
    rx868_actualPulseTrain = 0;
    rx868_nrpulses = 0;
    rx868_receiveMode = false;
    rx868_signalStart = micros();
}

// ===================================================================
//  868 RECEIVER: Enable/disable interrupt
// ===================================================================
void RFManager::rx868_enableReceiver() {
    pinMode(CC1101_GDO0_868, INPUT);
    attachInterrupt(digitalPinToInterrupt(CC1101_GDO0_868), rx868InterruptHandler, CHANGE);
    rx868_enabledReceiver = true;
}

void RFManager::rx868_disableReceiver() {
    rx868_enabledReceiver = false;
    detachInterrupt(digitalPinToInterrupt(CC1101_GDO0_868));
}

// ===================================================================
//  868 RECEIVER: Check for completed pulse trains
// ===================================================================
int RFManager::rx868_receivePulseTrain() {
    if (rx868_pulseTrains[rx868_availPulseTrain].num_pulses > 0) {
        uint8_t current = rx868_availPulseTrain;
        rx868_availPulseTrain = (rx868_availPulseTrain + 1) % RX868_RECEIVER_BUFFER_SIZE;
        return current;
    }
    return -1;
}

// ===================================================================
//  Live level frames for the web UI (radar / waterfall): every 200 ms the
//  peak and mean RSSI of both receivers over that window, noise floor,
//  threshold and whether a telegram is being received. Sampled from the 868
//  task loop (~1 kHz); the 433 value is the 433 task's latest 1-ms reading.
// ===================================================================
void RFManager::rssiFrameSample(bool wmbus) {
    static int pk433 = -128, pk868 = -128, n433 = 0, n868 = 0;
    static long s433 = 0, s868 = 0;
    static uint32_t lastFrame = 0;
    const int r433 = rtl_433_ESP::currentRssi;
    if (r433 > -128) { if (r433 > pk433) pk433 = r433; s433 += r433; n433++; }
    if (!wmbus) {
        const int r868 = rx868_currentRssi;
        if (r868 > -128) { if (r868 > pk868) pk868 = r868; s868 += r868; n868++; }
    }
    const uint32_t now = millis();
    if (now - lastFrame < 200) return;
    lastFrame = now;
    char js[200];
    snprintf(js, sizeof(js),
        "{\"t\":%lu,\"p433\":%d,\"m433\":%d,\"p868\":%d,\"m868\":%d,"
        "\"avg433\":%d,\"avg868\":%d,\"th433\":%d,\"th868\":%d,\"rx\":%d,\"wm\":%d}",
        (unsigned long)now, pk433, n433 ? (int)(s433 / n433) : -128, pk868, n868 ? (int)(s868 / n868) : -128,
        (int)rtl_433_ESP::averageRssi, (int)rx868_averageRssi,
        (int)rtl_433_ESP::rssiThreshold, (int)rx868_rssiThreshold,
        (rtl_433_ESP::getReceiveMode() ? 1 : 0) | (rx868_receiveMode ? 2 : 0), wmbus ? 1 : 0);
    pk433 = pk868 = -128; s433 = s868 = 0; n433 = n868 = 0;
    EventBus::getInstance().publish(SystemEvent::RF_RSSI_UPDATE, String(js));
}

// ===================================================================
//  868 RECEIVER TASK: RSSI monitoring + signal completion detection
//  Mirrors rtl_433_ESP::rtl_433_ReceiverTask + loop() combined
// ===================================================================
void RFManager::rx868ReceiverTask(void* pvParameters) {
    RFManager* self = (RFManager*)pvParameters;
    
    int totalRssi868 = 0;
    int rssiCount868 = 0;
    bool rssi868BootstrapDone = false;
    unsigned long gapStart868 = micros();
    int noiseCount868 = 0;
    
    for (;;) {
        rx868_taskLoopCount++;
        if (self->wmbusActive) {   // packet mode is served by wmbusTaskFunc on core 1
            rssiFrameSample(true);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (rx868_enabledReceiver) {
            
            // ---- RSSI monitoring and signal start/end detection ----
            rx868_currentRssi = rx868_getRSSI();
            
            // Only average RSSI during idle (no signal) to get true noise floor.
            // Including signal-energy samples inflates averageRssi and threshold.
            // Bootstrap: first cycle includes ALL samples to escape MINRSSI.
            if (!rx868_receiveMode || !rssi868BootstrapDone) {
                rssiCount868++;
                totalRssi868 += rx868_currentRssi;
            }
            
            // Recalculate RSSI threshold periodically
            if (rssiCount868 > RX868_RSSI_SAMPLES) {
                rx868_averageRssi = totalRssi868 / rssiCount868;
                // Sensitivity deltas: higher = more selective (less noise)
                // With avg≈-105, delta 3→threshold -102, delta 6→-99, etc.
                rx868_rssiThreshold = rx868_averageRssi + 
                    (self->mod868.rssiSensitivityLevel <= 0 ? 3 :
                     self->mod868.rssiSensitivityLevel == 1 ? 6 :
                     self->mod868.rssiSensitivityLevel == 2 ? 9 : 12);
                if (!rssi868BootstrapDone) {
                    rssi868BootstrapDone = true;
                    Serial.printf("[868] RSSI bootstrap done: avg %d, threshold %d\n",
                                  rx868_averageRssi, rx868_rssiThreshold);
                }
                totalRssi868 = 0;
                rssiCount868 = 0;
            }
            
            rssiFrameSample(false);
            
            if (rx868_currentRssi > rx868_rssiThreshold) {
                // Signal present
                if (rx868_receiveMode && rx868_currentRssi > rx868_signalRssi)
                    rx868_signalRssi = rx868_currentRssi;   // peak level of the train
                if (!rx868_receiveMode) {
                    rx868_signalStart = micros();
                    rx868_signalRssi = rx868_currentRssi;
                    rx868_lastChange = micros();
                    noiseCount868 = 0;
                    if (!rx868_ook) {
                        // Copy the last 2.5 ms of edges (pre-trigger) to the start of the train
                        volatile pulse_data_t& tr = rx868_pulseTrains[rx868_actualPulseTrain];
                        portENTER_CRITICAL(&pre868Mux);
                        const uint32_t head = pre868Idx, now = micros();
                        uint32_t first = head > PRE868_N ? head - PRE868_N : 0;
                        while (first < head && now - pre868T[first % PRE868_N] > 2500) first++;
                        int n = 0; bool started = false;
                        for (uint32_t k = first; k < head && n < PD_MAX_PULSES - 2; k++) {
                            const uint32_t i = k % PRE868_N;
                            if (pre868High[i]) { tr.pulse[n] = pre868Dur[i]; started = true; }
                            else if (started) { tr.gap[n] = pre868Dur[i]; n++; }
                        }
                        rx868_nrpulses = n;
                        if (head) rx868_lastChange = pre868T[(head - 1) % PRE868_N];
                        rx868_receiveMode = true;   // inside the lock: the ISR switches to the train
                        portEXIT_CRITICAL(&pre868Mux);
                        rx868_signalStart -= 2500;
                    } else {
                        rx868_receiveMode = true;
                    }
                }
                rx868_signalEnd = micros();
                // Carrier offset estimate of the current signal (FSK + FOC only)
                if (!rx868_ook && (rx868_taskLoopCount & 3) == 0 && self->mod868.radioModule)
                    rx868_freqEst = (int8_t)self->mod868.radioModule->SPIreadRegister(0x32 | 0x40);
                
                // Guard: abort if signal is absurdly long (>250ms) or pulse buffer near full
                // Very long signals are noise/interference and cause decoder crashes
                if ((rx868_signalEnd - rx868_signalStart) > 250000 ||
                    rx868_nrpulses >= (PD_MAX_PULSES - 10)) {
                    rx868_receiveMode = false;
                    rx868_ignoredSignals++;
                    trace868Add('A', rx868_signalEnd - rx868_signalStart, rx868_nrpulses, rx868_signalRssi,
                                rx868_rssiThreshold, rx868_averageRssi, "abort>250ms");
                    clearTrainSlot(rx868_pulseTrains[rx868_actualPulseTrain], rx868_nrpulses + 2);
                    rx868_nrpulses = 0;
                }
            }
            // Keep receiving if minor RSSI dropout (FSK signals can dip briefly)
            // OOK: RSSI drops in every gap, so tolerate long dropouts (like 433 lib).
            // FSK: the carrier is present for the whole telegram, so only end after the
            // RSSI stayed below threshold for 3 ms. Previously one low 1-ms sample ended
            // reception unless the signal was already >30 ms long — every short telegram
            // (LaCrosse ~6 ms) was cut and dropped as "too short".
            else if (rx868_receiveMode &&
                     micros() - rx868_signalEnd < (rx868_ook ? 40000UL : 3000UL)) {
                // Skip over signal dropouts
            }
            else {
                // No signal present
                if (rx868_receiveMode) {
                    // Signal reception complete
                    rx868_receiveMode = false;
                    rx868_totalSignals++;
                    
                    if ((rx868_nrpulses > PD_MIN_PULSES) &&
                        ((rx868_signalEnd - rx868_signalStart) > 
                         (unsigned long)RX868_MINIMUM_SIGNAL_LENGTH)) {
                        
                        // Valid signal — store in pulse train buffer
                        rx868_pulseTrains[rx868_actualPulseTrain].num_pulses = rx868_nrpulses + 1;
                        rx868_pulseTrains[rx868_actualPulseTrain].signalDuration =
                            rx868_signalEnd - rx868_signalStart;
                        rx868_pulseTrains[rx868_actualPulseTrain].signalRssi = rx868_signalRssi;
                        rx868_pulseTrains[rx868_actualPulseTrain].fsk_f1_est = rx868_ook ? 0x7FFF : rx868_freqEst;
                        rx868_pulseTrains[rx868_actualPulseTrain].noise_db = (float)rx868_averageRssi;
                        
                        trace868Add('Q', rx868_signalEnd - rx868_signalStart, rx868_nrpulses, rx868_signalRssi,
                                    rx868_rssiThreshold, rx868_averageRssi, "");
                        gapStart868 = micros();
                        rx868_actualPulseTrain = (rx868_actualPulseTrain + 1) % RX868_RECEIVER_BUFFER_SIZE;
                        rx868_nrpulses = 0;
                    } else {
                        rx868_ignoredSignals++;
                        trace868Add('I', rx868_signalEnd - rx868_signalStart, rx868_nrpulses, rx868_signalRssi,
                                    rx868_rssiThreshold, rx868_averageRssi, "");
                        clearTrainSlot(rx868_pulseTrains[rx868_actualPulseTrain], rx868_nrpulses + 2);
                        rx868_nrpulses = 0;
                    }
                } else {
                    noiseCount868++;
                }
            }
            
            // ---- Check for completed pulse trains and feed to decoder ----
            int trainIdx = self->rx868_receivePulseTrain();
            if (trainIdx >= 0) {
                // Allocate pulse_data copy for the decoder queue — use PSRAM to save internal heap
                pulse_data_t* rtl_pulses = (pulse_data_t*)heap_caps_calloc(
                    1, sizeof(pulse_data_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                
                if (!rtl_pulses) {
                    // OOM — discard signal, free the pulse train slot
                    rx868_pulseTrains[trainIdx].num_pulses = 0;
                    Serial.println("[868] OOM: pulse_data alloc failed, discarding signal");
                } else {
                    memcpy(rtl_pulses, (char*)&rx868_pulseTrains[trainIdx], sizeof(pulse_data_t));
                    
                    // Clear the pulse train buffer for reuse
                    rx868_pulseTrains[trainIdx].num_pulses = 0;
                    for (int x = 0; x < PD_MAX_PULSES; x++) {
                        rx868_pulseTrains[trainIdx].pulse[x] = 0;
                        rx868_pulseTrains[trainIdx].gap[x] = 0;
                    }
                    
                    // TAG: Set freq1_hz so decoder knows this is 868 MHz
                    rtl_pulses->freq1_hz = rx868_tagFreqHz;
                    rtl_pulses->freq2_hz = (float)rx868_pulseTag;
                    
                    if (rtl_pulses->num_pulses > PD_MIN_PULSES) {
                        // Feed into shared decoder queue (same as 433 module uses)
                        // Call the GLOBAL processSignal from signalDecoder, not the RFManager member
                        ::processSignal(rtl_pulses);
                    } else {
                        free(rtl_pulses);
                    }
                }
            }
            
            // CC1101 deaf workaround — restart receiver every hour
            static unsigned long lastDeafCheck = millis();
            if (millis() - lastDeafCheck > 3600000) {
                lastDeafCheck = millis();
                if (self->mod868.radio) {
                    self->mod868.radio->standby();
                    self->mod868.radio->receiveDirectAsync();
                }
            }
        }
        
        vTaskDelay(1);  // 1 tick (~1ms) — same rate as rtl_433_ESP receiver
    }
}

// ===================================================================
//  UNIFIED CALLBACK: Called by decoder task for every decoded signal
//  Reads currentSignalIs868 flag to determine source band
// ===================================================================
void RFManager::rtl433Callback(char* message) {
    if (instance) {
        // currentSignalIs868 is set by decoder task based on freq1_hz
        // (defined in signalDecoder.h, set in signalDecoder.cpp)
        extern volatile bool currentSignalIs868;
        instance->processSignal(message, currentSignalIs868);
    }
}

// ===================================================================
//  SIGNAL PROCESSING (from decoded JSON)
// ===================================================================
void RFManager::processSignal(char* message, bool is868, int forcedProfile) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, message);
    
    if (error) {
        Serial.printf("[RF] JSON parse error: %s\n", error.c_str());
        return;
    }
    
    // Plausibility: a decoder with a short checksum (CRC8) occasionally accepts a
    // noise burst; such phantoms report impossible values (160 °C, 205 %, 4100 hPa).
    {
        const char* bad = nullptr;
        for (JsonPair kv : doc.as<JsonObject>()) {
            const char* k = kv.key().c_str();
            if (!kv.value().is<float>()) continue;
            const float v = kv.value().as<float>();
            const size_t n = strlen(k);
            if (strncmp(k, "temperature", 11) == 0 && n > 2 && strcmp(k + n - 2, "_C") == 0) {
                if (v < -50 || v > 85) bad = k;
            } else if (strncmp(k, "temperature", 11) == 0 && n > 2 && strcmp(k + n - 2, "_F") == 0) {
                if (v < -58 || v > 185) bad = k;
            } else if (strncmp(k, "humidity", 8) == 0) {
                if (v < 0 || v > 100) bad = k;
            } else if (strcmp(k, "pressure_hPa") == 0) {
                if (v < 850 || v > 1100) bad = k;
            } else if (strncmp(k, "wind_", 5) == 0 && strstr(k, "_m_s")) {
                if (v < 0 || v > 75) bad = k;
            } else if (strncmp(k, "wind_", 5) == 0 && strstr(k, "_km_h")) {
                if (v < 0 || v > 270) bad = k;
            }
            if (bad) break;
        }
        if (bad) {
            implausibleDrops++;
            snprintf(implausibleLast, sizeof(implausibleLast), "%s (%s)", (const char*)(doc["model"] | "?"), bad);
            Serial.printf("[RF] Unplausibel verworfen: %s\n", implausibleLast);
            return;
        }
    }

    extern volatile uint32_t currentSignalProfileTag;
    const uint32_t ptag = currentSignalProfileTag;
    const int pidx = forcedProfile >= 0 ? forcedProfile
                   : ptag >= RX_PROFILE_TAG_BASE ? (int)((ptag - RX_PROFILE_TAG_BASE) / 10) : -1;
    const bool havePidx = pidx >= 0 && pidx < RX_PROFILE_COUNT;
    extern volatile int currentSignalFreqEst; extern volatile float currentSignalNoise;
    const int fEst = forcedProfile >= 0 ? 0x7FFF : currentSignalFreqEst;
    const float noiseDbm = forcedProfile >= 0 ? 0.0f : currentSignalNoise;
    
    RFSignal signal;
    signal.timestamp = millis();
    signal.frequency = havePidx ? RX_PROFILES[pidx].freqMHz : (is868 ? 868.35 : 433.92);
    signal.rssi = doc["rssi"] | -128;
    signal.protocol = doc["model"] | "unknown";
    signal.data = message;
    signal.model = doc["model"] | "";
    signal.id = doc["id"] | -1;
    signal.temperature = doc["temperature_C"] | NAN;
    signal.humidity = doc["humidity"] | NAN;
    signal.battery_ok = doc["battery_ok"] | -1;
    signal.channel = doc["channel"] | -1;
    
    // Update counters
    if (is868) {
        mod868.totalSignals++;
        mod868.decodedSignals++;
    } else {
        mod433.totalSignals++;
        mod433.decodedSignals++;
    }
    
    // Detect if this is a real decoded signal vs undecoded noise or classified-undecoded
    bool isUndecoded = signal.model.startsWith("undecoded") || signal.protocol.startsWith("undecoded");
    bool isClassified = (doc["status"].is<const char*>() && strcmp(doc["status"] | "", "classified") == 0);

    // Confirmation: a NEW device is accepted only after its second telegram within
    // 15 min. Noise phantoms (decoders without/with a short checksum: Secplus,
    // Skylink, Acurite-Rain, Elster, Telldus) appear once with a random id and are
    // never confirmed; real sensors repeat every 30-60 s and remotes resend on the
    // next press. Known devices (stored or confirmed since boot) pass at once.
    // wM-Bus telegrams carry a CRC16 per block and are exempt.
    if (!isUndecoded && !isClassified && signal.model.length() > 0 && !signal.model.startsWith("wMBus")) {
        String key = signal.model;
        if (doc["id"].is<const char*>()) key += "_" + doc["id"].as<String>();
        else if (!doc["id"].isNull()) key += "_" + String(doc["id"].as<long long>());
        static std::map<String, uint32_t> pending;    // key -> first sighting (ms)
        static std::set<String> confirmed;
        const uint32_t now = millis();
        bool ok = confirmed.count(key) || DeviceDatabase::getInstance().isDevicePersisted(key.c_str());
        if (!ok) {
            auto it = pending.find(key);
            if (it != pending.end() && now - it->second < 15 * 60000UL) {
                ok = true;
                pending.erase(it);
                if (confirmed.size() < 400) confirmed.insert(key);
            } else {
                if (pending.size() >= 300) {           // drop expired first-sightings
                    for (auto i = pending.begin(); i != pending.end();)
                        i = now - i->second >= 15 * 60000UL ? pending.erase(i) : std::next(i);
                    if (pending.size() >= 300) pending.clear();
                }
                pending[key] = now;
            }
        }
        if (!ok) {
            unconfirmedDrops++;
            isClassified = true;                      // published as raw, not stored
            doc["unconfirmed"] = true;
        }
    }
    
    if (havePidx) {
        if (isClassified || isUndecoded) rxStats[pidx].unparsed++;
        else { rxStats[pidx].decodes++; rxStats[pidx].lastDecodeMs = signal.timestamp; }
    }
    
    // Classified signals are not fully decoded — fix counter
    if (isClassified || isUndecoded) {
        if (is868) mod868.decodedSignals--;
        else mod433.decodedSignals--;
    }
    
    // Only store TRULY DECODED signals in history — classified/undecoded waste ~800 bytes each
    // With PUBLISH_UNPARSED=true, 868 generates dozens of classified signals/sec → SRAM exhaustion
    if (!isUndecoded && !isClassified) {
        xSemaphoreTake(historyMutex, portMAX_DELAY);
        signalHistory.insert(signalHistory.begin(), signal);
        if (signalHistory.size() > SIGNAL_HISTORY_MAX) {
            signalHistory.pop_back();
        }
        xSemaphoreGive(historyMutex);
    }
    
    // Publish event (for SSE) — but check heap first
    uint32_t freeHeap = ESP.getFreeHeap();
    
    // Auto-persist decoded devices in SQLite (insert new + update existing)
    if (!isUndecoded && !isClassified && signal.model.length() > 0) {
        // Build device_key: model_id (same format as frontend getDeviceId)
        // Same key rule as the web UI: model_id, ids may be strings (Elster, wM-Bus)
        String deviceKey = signal.model;
        if (doc["id"].is<const char*>()) deviceKey += "_" + doc["id"].as<String>();
        else if (!doc["id"].isNull()) deviceKey += "_" + String(doc["id"].as<long long>());
        // New devices reach this point only after the second sighting (confirmation
        // rule above). wM-Bus meters (100+ neighbours) are not auto-saved. Known
        // devices are rewritten at most every 15 min: every SQLite write erases
        // LittleFS sectors, and a flash erase stalls both cores.
        // Bookkeeping only for stored devices: a map entry per wM-Bus neighbour cost
        // ~25 KB of internal heap (2026-11-22).
        const bool known = DeviceDatabase::getInstance().isDevicePersisted(deviceKey.c_str());
        const bool isWmbus = signal.model.startsWith("wMBus");
        static std::map<String, uint32_t> lastWrite;   // stored devices only
        bool write = false;
        if (known) {
            uint32_t& lw = lastWrite[deviceKey];
            if (!lw || millis() - lw > 900000UL) { lw = millis() | 1; write = true; }
        } else if (!isWmbus) {
            lastWrite[deviceKey] = millis() | 1;
            write = true;
        }
        if (lastWrite.size() > 120) lastWrite.clear();
        if (write) {
            DeviceDatabase::getInstance().insertOrUpdate(
                deviceKey.c_str(), signal.model.c_str(), signal.id,
                signal.channel, signal.temperature, signal.humidity,
                signal.battery_ok, signal.rssi, signal.frequency, "{}");
        }
        // MQTT gets every reading of a released device (independent of the 15-min
        // DB write). publishReading only enqueues; it never blocks the radio path.
        if (DeviceDatabase::getInstance().isMqttEnabled(deviceKey.c_str()))
            MqttBridge::getInstance().publishReading(deviceKey.c_str(), signal);
    }
    
    // Classified (undecoded) signals: lightweight publish — no full JsonDocument overhead
    // With PUBLISH_UNPARSED=true, 868 produces dozens/sec → each JsonDocument costs ~1-2KB SRAM
    if (isClassified) {
        if (freeHeap > 12000) {
            // Always include the receive band; missing metadata must not become 433 MHz.
            doc["band"] = is868 ? "868" : "433";
            if (havePidx) doc["rx_profile"] = RX_PROFILES[pidx].id;
            if (havePidx) doc["modulation"] = RX_PROFILES[pidx].mod == RxMod::OOK ? "OOK" : "FSK";
            doc["band"] = is868 ? "868" : "433";
            doc["frequency"] = signal.frequency;
            doc["rssi"] = signal.rssi;
            doc["decoded"] = false;
            doc["classified"] = true;
            String classifiedJson;
            serializeJson(doc, classifiedJson);
            EventBus::getInstance().publish(SystemEvent::RF_SIGNAL_RECEIVED, classifiedJson);
        }
        return; // Skip full SSE enrichment path + user callback for classified
    }
    
    if (freeHeap < 8192) {
        // CRITICAL: Skip event publishing to prevent crash
        static unsigned long lastHeapWarn = 0;
        if (millis() - lastHeapWarn > 5000) {
            Serial.printf("[RF] ⚠️ HEAP CRITICAL: %u bytes free — skipping SSE publish\n", freeHeap);
            lastHeapWarn = millis();
        }
    } else if (!isUndecoded) {
        // Enrich raw rtl_433 JSON with fields the frontend needs
        // (rtl_433 doesn't include frequency, decoded, or protocol fields)
        JsonDocument sseDoc;
        deserializeJson(sseDoc, signal.data);
        sseDoc["frequency"] = signal.frequency;
        sseDoc["protocol"] = signal.protocol;
        sseDoc["decoded"] = true;
        if (havePidx) sseDoc["rx_profile"] = RX_PROFILES[pidx].id;
        { extern volatile int currentSignalSource; if (!is868) sseDoc["capture"] = currentSignalSource ? "rmt" : "isr"; }
        if (noiseDbm < -20.0f) { sseDoc["noise_dBm"] = (int)noiseDbm; sseDoc["snr_dB"] = signal.rssi - (int)noiseDbm; }
        if (fEst != 0x7FFF) sseDoc["freq_offset_kHz"] = serialized(String(fEst * 1.587f, 1));
        if (havePidx) sseDoc["modulation"] = RX_PROFILES[pidx].mod == RxMod::OOK ? "OOK" : "FSK";
        RfHunt::get().record(sseDoc, is868, havePidx ? RX_PROFILES[pidx].id : nullptr, signal.rssi,
                             noiseDbm < -20.0f ? signal.rssi - (int)noiseDbm : -99);
        sseDoc["band"] = is868 ? "868" : "433";
        // Tell frontend if this device is persisted in SQLite
        if (signal.model.length() > 0) {
            String dk = signal.model;
            if (sseDoc["id"].is<const char*>()) dk += "_" + sseDoc["id"].as<String>();
            else if (!sseDoc["id"].isNull()) dk += "_" + String(sseDoc["id"].as<long long>());
            if (DeviceDatabase::getInstance().isDevicePersisted(dk.c_str())) {
                sseDoc["persisted"] = true;
            }
        }
        String sseJson;
        serializeJson(sseDoc, sseJson);
        EventBus::getInstance().publish(
            SystemEvent::RF_SIGNAL_RECEIVED,
            sseJson
        );
    }
    
    // User callback
    if (signalCallback) {
        signalCallback(signal, is868);
    }
    
    if (is868) trace868Add('D', 0, 0, signal.rssi, 0, 0, signal.model.c_str());
    Serial.printf("[RF] %s decoded: %s (RSSI %d, profile %s)\n", 
        is868 ? "868" : "433", signal.model.c_str(), signal.rssi,
        havePidx ? RX_PROFILES[pidx].id : "?");
}

// ===================================================================
//  STATUS
// ===================================================================
String RFManager::getDiag868JSON() {
    char buf[512];
    uint8_t marcState = 0xFF;
    uint8_t rawRssi = 0;
    uint8_t iocfg0 = 0xFF;
    if (mod868.radio && mod868.radioModule) {
        // Read status registers via Module (need burst bit for status regs)
        marcState = mod868.radioModule->SPIreadRegister(0x35 | 0x40);  // MARCSTATE
        rawRssi = mod868.radioModule->SPIreadRegister(0x34 | 0x40);    // Raw RSSI
        iocfg0 = mod868.radioModule->SPIreadRegister(0x02);            // IOCFG0 (config reg, no burst needed)
    }
    // Call rx868_getRSSI() directly from this context to compare
    int directRssiResult = rx868_getRSSI();
    snprintf(buf, sizeof(buf),
        "{\"rssi\":%d,\"directRssi\":%d,\"rawRssiReg\":\"0x%02X\",\"marcState\":\"0x%02X\","
        "\"iocfg0\":\"0x%02X\",\"gdo0Edges\":%lu,\"taskLoops\":%lu,"
        "\"enabled\":%s,\"threshold\":%d,\"averageRssi\":%d,"
        "\"totalSignals\":%d,\"ignoredSignals\":%d,"
        "\"gdo0Pin\":%d,\"gdo0State\":%d,\"moduleOk\":%s,"
        "\"instanceOk\":%s,\"radioModuleOk\":%s}",
        rx868_currentRssi, directRssiResult, rawRssi, marcState, iocfg0,
        (unsigned long)rx868_isrEdgeCount, (unsigned long)rx868_taskLoopCount,
        rx868_enabledReceiver ? "true" : "false",
        rx868_rssiThreshold, rx868_averageRssi,
        rx868_totalSignals, rx868_ignoredSignals,
        CC1101_GDO0_868, digitalRead(CC1101_GDO0_868),
        mod868.moduleOk ? "true" : "false",
        instance ? "true" : "false",
        (instance && instance->mod868.radioModule) ? "true" : "false");
    return String(buf);
}

RFStatusCache RFManager::getStatusCache() {
    RFStatusCache cache;
    cache.module433 = getModuleStatus(false);
    cache.module868 = getModuleStatus(true);
    cache.lastUpdate = millis();
    return cache;
}

RFModuleStatus RFManager::getModuleStatus(bool is868) {
    RFModuleStatus status;
    
    if (is868) {
        status.moduleOk = mod868.moduleOk;
        status.receiving = mod868.receiving;
        status.currentFreq = mod868.currentFreq;
        status.rssi = rx868_currentRssi;
        status.totalSignals = mod868.totalSignals;
        status.decodedSignals = mod868.decodedSignals;
        status.is868 = true;
    } else {
        status.moduleOk = mod433.moduleOk;
        status.receiving = mod433.receiving;
        status.currentFreq = mod433.currentFreq;
        status.rssi = rtl_433_ESP::currentRssi;
        status.totalSignals = mod433.totalSignals;
        status.decodedSignals = mod433.decodedSignals;
        status.is868 = false;
    }
    
    status.lastUpdate = millis();
    return status;
}

// ===================================================================
//  SIGNAL HISTORY
// ===================================================================
std::vector<RFSignal> RFManager::getLastSignals(int count) {
    std::vector<RFSignal> result;
    
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    int n = min(count, (int)signalHistory.size());
    for (int i = 0; i < n; i++) {
        result.push_back(signalHistory[i]);
    }
    xSemaphoreGive(historyMutex);
    
    return result;
}

void RFManager::clearSignals() {
    xSemaphoreTake(historyMutex, portMAX_DELAY);
    signalHistory.clear();
    xSemaphoreGive(historyMutex);
}

// ===================================================================
//  CONTROL
// ===================================================================
void RFManager::startReceiving() {
    mod433.receiving = true;
    mod868.receiving = true;
    rx868_enableReceiver();
}

void RFManager::stopReceiving() {
    mod433.receiving = false;
    mod868.receiving = false;
    rx868_disableReceiver();
}

bool RFManager::isModuleOk(bool is868) {
    return is868 ? mod868.moduleOk : mod433.moduleOk;
}

// ===================================================================
//  SCAN MODE — Frequency sweep across ISM bands
//  433 module: sweeps 433.0-435.0 MHz (OOK)
//  868 module: sweeps 868.0-870.0 MHz (FSK)
// ===================================================================

// Forward declaration of sweep task
static TaskHandle_t sweepTaskHandle = nullptr;

void RFManager::sweepTaskFunc(void* pvParameters) {
    RFManager* self = (RFManager*)pvParameters;
    
    Serial.printf("[RF] Sweep started: %.1f-%.1f MHz, BW=%.0fkHz, dwell=%dms\n",
        self->sweepCfg.startFreqMHz, self->sweepCfg.endFreqMHz,
        self->sweepCfg.bwKHz(), self->sweepCfg.dwellMs);
    
    // Calculate frequency steps
    float stepMHz = self->sweepCfg.effectiveStepMHz();
    float range = self->sweepCfg.endFreqMHz - self->sweepCfg.startFreqMHz;
    int numSteps = (int)(range / stepMHz) + 1;
    if (numSteps > SWEEP_MAX_FREQS) numSteps = SWEEP_MAX_FREQS;
    if (numSteps < 1) numSteps = 1;
    
    self->sweepFreqCount = numSteps;
    self->sweepStartMs = millis();
    self->sweepCycleCount = 0;
    
    // Allocate heatmap if needed
    if (self->heatmap) free(self->heatmap);
    self->heatmap = (FreqHeatmapEntry*)calloc(numSteps, sizeof(FreqHeatmapEntry));
    
    // Initialize heatmap entries
    if (self->heatmap) {
        for (int i = 0; i < numSteps; i++) {
            self->heatmap[i].freqMHz = self->sweepCfg.startFreqMHz + i * stepMHz;
            self->heatmap[i].rssiPeak = -140;
            self->heatmap[i].rssiAvgCurrent = -128;
        }
    }
    
    Serial.printf("[RF] Sweep: %d steps, %.3f MHz/step\n", numSteps, stepMHz);
    
    // Determine which module to sweep based on frequency range
    bool is868Band = self->sweepCfg.startFreqMHz > 800.0f;
    const float previous433Frequency = self->mod433.currentFreq;
    const uint8_t previous433Bandwidth = rtl_433_ESP::readCC1101Reg(0x10);
    
    // Set bandwidth register for 433 if needed
    if (!is868Band) {
        rf433.disableReceiver();
        rf433.setBandwidthReg(self->sweepCfg.bwMDMCFG4);
        rf433.enableReceiver();
    } else {
        // For 868, set bandwidth via RadioLib
        if (self->mod868.radio) {
            self->mod868.radio->standby();
            self->mod868.radio->setRxBandwidth(self->sweepCfg.bwKHz());
            self->mod868.radio->receiveDirectAsync();
        }
    }
    
    for (;;) {
        // Check if sweep should stop
        if (self->scanMode != ScanMode::CONTINUOUS_SWEEP) {
            break;
        }
        
        // Check duration limit
        if (self->sweepCfg.durationMs > 0 && 
            (millis() - self->sweepStartMs) > self->sweepCfg.durationMs) {
            Serial.println("[RF] Sweep: Duration limit reached");
            break;
        }
        
        // Step through frequencies
        for (int i = 0; i < numSteps; i++) {
            if (self->scanMode != ScanMode::CONTINUOUS_SWEEP) break;
            
            self->sweepCurrentIdx = i;
            float freq = self->sweepCfg.startFreqMHz + i * stepMHz;
            self->sweepStepStartMs = millis();
            
            // Change frequency on the appropriate module
            if (!is868Band) {
                rf433.disableReceiver();
                rf433.setFrequency(freq);
                rf433.enableReceiver();
                self->mod433.currentFreq = freq;
            } else {
                self->rx868_disableReceiver();
                if (self->mod868.radio) {
                    self->mod868.radio->standby();
                    self->mod868.radio->setFrequency(freq);
                    self->mod868.radio->receiveDirectAsync();
                }
                self->rx868_enableReceiver();
                self->mod868.currentFreq = freq;
            }
            
            // Dwell on this frequency
            uint32_t dwellStart = millis();
            int rssiPeak = -140;
            int rssiSum = 0;
            int rssiSamples = 0;
            
            while ((millis() - dwellStart) < self->sweepCfg.dwellMs) {
                if (self->scanMode != ScanMode::CONTINUOUS_SWEEP) break;
                
                // Read current RSSI
                int rssi;
                if (!is868Band) {
                    rssi = rtl_433_ESP::currentRssi;
                } else {
                    rssi = rx868_currentRssi;
                }
                
                if (rssi > rssiPeak) rssiPeak = rssi;
                rssiSum += rssi;
                rssiSamples++;
                
                vTaskDelay(pdMS_TO_TICKS(10));  // Sample RSSI every 10ms
            }
            
            // Update heatmap
            if (self->heatmap && rssiSamples > 0) {
                self->heatmap[i].rssiAvgCurrent = rssiSum / rssiSamples;
                if (rssiPeak > self->heatmap[i].rssiPeak) {
                    self->heatmap[i].rssiPeak = rssiPeak;
                }
                self->heatmap[i].rssiSum += rssiSum;
                self->heatmap[i].rssiSamples += rssiSamples;
                self->heatmap[i].sweepCycles = self->sweepCycleCount + 1;
            }
        }
        
        self->sweepCycleCount++;
        Serial.printf("[RF] Sweep cycle %d complete\n", self->sweepCycleCount);
    }
    
    // Sweep complete — return to fixed frequency
    Serial.println("[RF] Sweep stopped, returning to fixed frequency");
    self->scanMode = ScanMode::FIXED;
    
    if (!is868Band) {
        rf433.disableReceiver();
        rf433.setFrequency(previous433Frequency);
        rf433.setBandwidthReg(previous433Bandwidth);
        rf433.enableReceiver();
        self->mod433.currentFreq = previous433Frequency;
    } else {
        self->rx868_disableReceiver();
        if (self->mod868.radio) {
            self->mod868.radio->standby();
            self->mod868.radio->setFrequency(868.35);
            self->mod868.radio->setRxBandwidth(270.0);  // Restore original FSK BW
            self->mod868.radio->receiveDirectAsync();
        }
        self->rx868_enableReceiver();
        self->mod868.currentFreq = 868.35;
    }
    
    sweepTaskHandle = nullptr;
    vTaskDelete(NULL);
}

// ===================================================================
//  DUAL SCAN — Parallel frequency sweep on both modules
//  433 sweeps 433.0–435.0 MHz (OOK), 868 sweeps 868.0–870.0 MHz (FSK)
//  Smart dwell: stays on frequency while signal is active
//  Both modules step independently and asynchronously
// ===================================================================

void RFManager::dualScanInitModule(ModuleSweepState& state, bool is868) {
    float stepMHz = state.cfg.effectiveStepMHz();
    float range = state.cfg.endFreqMHz - state.cfg.startFreqMHz;
    int numSteps = (int)(range / stepMHz) + 1;
    if (numSteps > SWEEP_MAX_FREQS) numSteps = SWEEP_MAX_FREQS;
    if (numSteps < 1) numSteps = 1;
    
    state.freqCount = numSteps;
    state.currentIdx = 0;
    state.stepStartMs = 0;  // MUST reset — dualScanStepModule checks ==0 for first call
    state.cycleCount = 0;
    state.complete = false;
    state.smartDwellActive = false;
    state.smartDwellStartMs = 0;
    state.rssiPeak = -140;
    state.rssiSum = 0;
    state.rssiSamples = 0;
    state.currentFreqMHz = 0;
    state.signalsThisStep = 0;
    
    // Allocate heatmap in PSRAM (internal heap is tight)
    if (state.heatmap) { free(state.heatmap); state.heatmap = nullptr; }
    state.heatmap = (FreqHeatmapEntry*)heap_caps_calloc(numSteps, sizeof(FreqHeatmapEntry), MALLOC_CAP_SPIRAM);
    if (state.heatmap) {
        for (int i = 0; i < numSteps; i++) {
            state.heatmap[i].freqMHz = state.cfg.startFreqMHz + i * stepMHz;
            state.heatmap[i].rssiPeak = -140;
            state.heatmap[i].rssiAvgCurrent = -128;
        }
    } else {
        Serial.printf("[RF] DualScan %s: PSRAM alloc failed for %d entries!\n", 
            is868 ? "868" : "433", numSteps);
    }
    
    // NOTE: SPI operations (bandwidth, frequency) are done in startDualScan() 
    // BEFORE the task is created, to avoid cross-core SPI conflicts.
    
    Serial.printf("[RF] DualScan %s: %d steps, %.3f MHz/step, dwell %dms, BW %.0fkHz\n",
        is868 ? "868" : "433", numSteps, stepMHz, state.cfg.dwellMs, state.cfg.bwKHz());
}

// Returns true if this module should continue, false if all cycles done
bool RFManager::dualScanStepModule(ModuleSweepState& state, bool is868) {
    if (state.complete) return false;
    
    uint32_t now = millis();
    
    // First call — set initial frequency
    if (state.stepStartMs == 0) {
        state.stepStartMs = now;
        state.currentFreqMHz = state.cfg.startFreqMHz;
        state.rssiPeak = -140;
        state.rssiSum = 0;
        state.rssiSamples = 0;
        state.signalsThisStep = 0;
        
        // Set frequency
        if (!is868) {
            rf433.disableReceiver();
            rf433.setFrequency(state.currentFreqMHz);
            rf433.enableReceiver();
            instance->mod433.currentFreq = state.currentFreqMHz;
        } else {
            rx868_disableReceiver();
            if (instance->mod868.radio) {
                instance->mod868.radio->standby();
                instance->mod868.radio->setFrequency(state.currentFreqMHz);
                instance->mod868.radio->receiveDirectAsync();
            }
            rx868_enableReceiver();
            instance->mod868.currentFreq = state.currentFreqMHz;
        }
        return true;
    }
    
    // Sample RSSI from receiver task variables (updated on Core 0)
    // DO NOT use direct SPI reads from Core 1 — causes SPI bus conflicts and crashes
    int rssi;
    bool signalActive;
    if (!is868) {
        rssi = rtl_433_ESP::currentRssi;
        signalActive = (rssi > (rtl_433_ESP::averageRssi + 10));
    } else {
        rssi = rx868_currentRssi;
        signalActive = (rssi > (rx868_averageRssi + 10));
    }
    
    if (rssi > state.rssiPeak) state.rssiPeak = rssi;
    state.rssiSum += rssi;
    state.rssiSamples++;
    
    // Smart dwell: if a real signal is detected, extend dwell to finish decoding
    if (signalActive) {
        state.smartDwellActive = true;
        state.smartDwellStartMs = now;  // Keep updating while signal active
    }
    if (!signalActive && state.smartDwellActive) {
        // Signal ended — wait 500ms for decoder to process, then release
        if (now - state.smartDwellStartMs > 500) {
            state.smartDwellActive = false;
        }
    }
    
    // Check if dwell time elapsed AND no active signal (smart dwell)
    uint32_t elapsed = now - state.stepStartMs;
    bool dwellDone = elapsed >= state.cfg.dwellMs;
    // Hard limit: even with smart dwell, don't stay more than 3x dwell time
    bool hardLimit = elapsed >= (state.cfg.dwellMs * 3);
    
    if (dwellDone && (!state.smartDwellActive || hardLimit)) {
        // Update heatmap for current step
        if (state.heatmap && state.rssiSamples > 0) {
            int idx = state.currentIdx;
            state.heatmap[idx].rssiAvgCurrent = state.rssiSum / state.rssiSamples;
            if (state.rssiPeak > state.heatmap[idx].rssiPeak) {
                state.heatmap[idx].rssiPeak = state.rssiPeak;
            }
            state.heatmap[idx].rssiSum += state.rssiSum;
            state.heatmap[idx].rssiSamples += state.rssiSamples;
            state.heatmap[idx].sweepCycles = state.cycleCount + 1;
            state.heatmap[idx].signalsDecoded += state.signalsThisStep;
        }
        
        // Advance to next step
        state.currentIdx++;
        if (state.currentIdx >= state.freqCount) {
            // Cycle complete
            state.cycleCount++;
            Serial.printf("[RF] DualScan %s: cycle %d complete\n", 
                is868 ? "868" : "433", state.cycleCount);
            
            // Check if we've done enough cycles
            uint32_t maxCycles = state.cfg.durationMs > 0 ? 
                (uint32_t)((float)state.cfg.durationMs / (state.freqCount * state.cfg.dwellMs)) : 0;
            if (maxCycles > 0 && state.cycleCount >= maxCycles) {
                state.complete = true;
                return false;
            }
            state.currentIdx = 0;  // Start next cycle
        }
        
        // Set new frequency
        float stepMHz = state.cfg.effectiveStepMHz();
        state.currentFreqMHz = state.cfg.startFreqMHz + state.currentIdx * stepMHz;
        state.stepStartMs = now;
        state.rssiPeak = -140;
        state.rssiSum = 0;
        state.rssiSamples = 0;
        state.smartDwellActive = false;
        state.signalsThisStep = 0;
        
        if (!is868) {
            rf433.disableReceiver();
            rf433.setFrequency(state.currentFreqMHz);
            rf433.enableReceiver();
            instance->mod433.currentFreq = state.currentFreqMHz;
        } else {
            rx868_disableReceiver();
            if (instance->mod868.radio) {
                instance->mod868.radio->standby();
                instance->mod868.radio->setFrequency(state.currentFreqMHz);
                instance->mod868.radio->receiveDirectAsync();
            }
            rx868_enableReceiver();
            instance->mod868.currentFreq = state.currentFreqMHz;
        }
    }
    
    return true;
}

void RFManager::dualScanRestoreModule(bool is868) {
    if (!is868) {
        rf433.disableReceiver();
        rf433.setFrequency(433.92);
        rf433.setBandwidthReg(0x07);
        rf433.enableReceiver();
        mod433.currentFreq = 433.92;
    } else {
        rx868_disableReceiver();
        if (mod868.radio) {
            mod868.radio->standby();
            mod868.radio->setFrequency(868.35);
            mod868.radio->setRxBandwidth(270.0);
            mod868.radio->receiveDirectAsync();
        }
        rx868_enableReceiver();
        mod868.currentFreq = 868.35;
    }
}

void RFManager::dualScanTaskFunc(void* pvParameters) {
    RFManager* self = (RFManager*)pvParameters;
    
    Serial.printf("[RF] Dual Scan task RUNNING on Core %d — heap=%d, stackHWM=%d\n",
        xPortGetCoreID(), ESP.getFreeHeap(), uxTaskGetStackHighWaterMark(NULL));
    
    // Init was already done in startDualScan() before task creation
    self->dualScanStartMs = millis();
    
    uint32_t loopCount = 0;
    const char* exitReason = "unknown";
    
    for (;;) {
        // Check if scan should stop
        if (self->scanMode != ScanMode::DUAL_SCAN) {
            exitReason = "scanMode changed to non-DUAL_SCAN";
            break;
        }
        
        // Check global duration limit
        if (self->dualScanDurationMs > 0 && 
            (millis() - self->dualScanStartMs) > self->dualScanDurationMs) {
            exitReason = "global duration limit reached";
            break;
        }
        
        // Step both modules independently
        bool active433 = self->dualScanStepModule(self->dualSweep433, false);
        bool active868 = self->dualScanStepModule(self->dualSweep868, true);
        
        loopCount++;
        // Log every ~5 seconds (500 × 10ms ticks)
        if (loopCount % 500 == 0) {
            Serial.printf("[DS] 433:%d/%d c=%d r=%d 868:%d/%d c=%d r=%d heap=%d stk=%d\n",
                self->dualSweep433.currentIdx, self->dualSweep433.freqCount,
                self->dualSweep433.cycleCount,
                (int)rtl_433_ESP::currentRssi,
                self->dualSweep868.currentIdx, self->dualSweep868.freqCount,
                self->dualSweep868.cycleCount,
                (int)rx868_currentRssi,
                ESP.getFreeHeap(), uxTaskGetStackHighWaterMark(NULL));
        }
        
        // If both modules are done, stop
        if (!active433 && !active868) {
            exitReason = "both modules completed all cycles";
            break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));  // 10ms tick — RSSI sampled ~100/sec
    }
    
    Serial.printf("[RF] Dual Scan EXIT: reason='%s', loops=%lu, elapsed=%lums, "
        "433: cycles=%d step=%d/%d complete=%d, 868: cycles=%d step=%d/%d complete=%d, heap=%d\n",
        exitReason, loopCount, millis() - self->dualScanStartMs,
        self->dualSweep433.cycleCount, self->dualSweep433.currentIdx, self->dualSweep433.freqCount, self->dualSweep433.complete,
        self->dualSweep868.cycleCount, self->dualSweep868.currentIdx, self->dualSweep868.freqCount, self->dualSweep868.complete,
        ESP.getFreeHeap());
    
    // Restore fixed frequencies
    Serial.println("[RF] Dual Scan complete, restoring fixed frequencies");
    self->dualScanRestoreModule(false);
    self->dualScanRestoreModule(true);
    self->scanMode = ScanMode::FIXED;
    
    sweepTaskHandle = nullptr;
    vTaskDelete(NULL);
}

void RFManager::startDualScan(const SweepConfig& cfg433, const SweepConfig& cfg868) {
    Serial.printf("[RF] startDualScan() enter — heap=%d, PSRAM=%d, scanMode=%d, taskHandle=%p\n",
        ESP.getFreeHeap(), ESP.getFreePsram(), (int)scanMode, (void*)sweepTaskHandle);
    
    // Stop any existing scan
    setScanMode(ScanMode::FIXED);
    
    // Brief yield to let the stopped task clean up (if any)
    // Use taskYIELD instead of vTaskDelay to avoid blocking the HTTP handler
    for (int i = 0; i < 5; i++) {
        taskYIELD();
        delayMicroseconds(1000);  // 1ms non-blocking delay × 5 = 5ms total
    }
    
    Serial.printf("[RF] startDualScan() after stop — heap=%d, taskHandle=%p\n",
        ESP.getFreeHeap(), (void*)sweepTaskHandle);
    
    dualSweep433.cfg = cfg433;
    dualSweep868.cfg = cfg868;
    
    // Initialize module states (heatmap allocation, step calculation)
    // This runs in the HTTP handler context — safe for memory operations
    dualScanInitModule(dualSweep433, false);
    dualScanInitModule(dualSweep868, true);
    
    Serial.printf("[RF] startDualScan() modules initialized — 433: %d steps, 868: %d steps, heap=%d\n",
        dualSweep433.freqCount, dualSweep868.freqCount, ESP.getFreeHeap());
    
    // Set bandwidths BEFORE starting the task — SPI must be done from 
    // the same context as the receiver tasks to avoid cross-core conflicts
    rf433.disableReceiver();
    rf433.setBandwidthReg(cfg433.bwMDMCFG4);
    rf433.enableReceiver();
    
    if (mod868.radio) {
        rx868_disableReceiver();
        mod868.radio->standby();
        mod868.radio->setRxBandwidth(cfg868.bwKHz());
        mod868.radio->receiveDirectAsync();
        rx868_enableReceiver();
    }
    
    // Use the larger duration as global limit (0 = use cycle-based termination)
    dualScanDurationMs = max(cfg433.durationMs, cfg868.durationMs);
    
    scanMode = ScanMode::DUAL_SCAN;
    
    // Allocate task stack from PSRAM to preserve scarce internal heap
    const uint32_t stackSize = 8192;  // 8KB in PSRAM (plenty of room)
    if (dualScanStack) { heap_caps_free(dualScanStack); dualScanStack = nullptr; }
    dualScanStack = (StackType_t*)heap_caps_malloc(stackSize, MALLOC_CAP_SPIRAM);
    if (!dualScanStack) {
        Serial.printf("[RF] DualScan: PSRAM stack alloc FAILED! Free PSRAM=%d\n", ESP.getFreePsram());
        scanMode = ScanMode::FIXED;
        return;
    }
    
    sweepTaskHandle = xTaskCreateStaticPinnedToCore(
        dualScanTaskFunc,
        "dualScan",
        stackSize / sizeof(StackType_t),  // Stack size in words
        this,
        1,                     // Lower priority than receiver tasks (2)
        dualScanStack,
        &dualScanTaskBuf,
        1                      // Core 1 (receiver tasks on Core 0)
    );
    
    Serial.printf("[RF] Dual Scan STARTED: taskHandle=%p, 433(%.1f-%.1f dwell=%lu dur=%lu) + 868(%.1f-%.1f dwell=%lu dur=%lu) globalDur=%lu heap=%d\n",
        (void*)sweepTaskHandle,
        cfg433.startFreqMHz, cfg433.endFreqMHz, cfg433.dwellMs, cfg433.durationMs,
        cfg868.startFreqMHz, cfg868.endFreqMHz, cfg868.dwellMs, cfg868.durationMs,
        dualScanDurationMs, ESP.getFreeHeap());
}

String RFManager::getDualScanStatusJSON() {
    JsonDocument doc;
    doc["mode"] = (scanMode == ScanMode::DUAL_SCAN) ? "dualscan" : "fixed";
    doc["elapsedMs"] = (scanMode == ScanMode::DUAL_SCAN) ? millis() - dualScanStartMs : 0;
    doc["durationMs"] = dualScanDurationMs;
    
    // Module 433 status
    JsonObject m433 = doc["module433"].to<JsonObject>();
    m433["freq"] = dualSweep433.currentFreqMHz;
    m433["step"] = dualSweep433.currentIdx;
    m433["steps"] = dualSweep433.freqCount;
    m433["cycle"] = dualSweep433.cycleCount;
    m433["complete"] = dualSweep433.complete;
    m433["smartDwell"] = dualSweep433.smartDwellActive;
    m433["dwellMs"] = dualSweep433.cfg.dwellMs;
    m433["startMHz"] = dualSweep433.cfg.startFreqMHz;
    m433["endMHz"] = dualSweep433.cfg.endFreqMHz;
    m433["signalsThisStep"] = dualSweep433.signalsThisStep;
    
    // Module 868 status
    JsonObject m868 = doc["module868"].to<JsonObject>();
    m868["freq"] = dualSweep868.currentFreqMHz;
    m868["step"] = dualSweep868.currentIdx;
    m868["steps"] = dualSweep868.freqCount;
    m868["cycle"] = dualSweep868.cycleCount;
    m868["complete"] = dualSweep868.complete;
    m868["smartDwell"] = dualSweep868.smartDwellActive;
    m868["dwellMs"] = dualSweep868.cfg.dwellMs;
    m868["startMHz"] = dualSweep868.cfg.startFreqMHz;
    m868["endMHz"] = dualSweep868.cfg.endFreqMHz;
    m868["signalsThisStep"] = dualSweep868.signalsThisStep;
    
    String json;
    serializeJson(doc, json);
    return json;
}

String RFManager::getDualScanHeatmapJSON() {
    JsonDocument doc;
    
    // 433 heatmap
    JsonArray arr433 = doc["heatmap433"].to<JsonArray>();
    if (dualSweep433.heatmap) {
        for (int i = 0; i < dualSweep433.freqCount; i++) {
            JsonObject e = arr433.add<JsonObject>();
            e["freq"] = dualSweep433.heatmap[i].freqMHz;
            e["rssiPeak"] = dualSweep433.heatmap[i].rssiPeak;
            e["rssiAvg"] = dualSweep433.heatmap[i].rssiAvgCurrent;
            e["signals"] = dualSweep433.heatmap[i].signalsDecoded;
            e["cycles"] = dualSweep433.heatmap[i].sweepCycles;
        }
    }
    
    // 868 heatmap
    JsonArray arr868 = doc["heatmap868"].to<JsonArray>();
    if (dualSweep868.heatmap) {
        for (int i = 0; i < dualSweep868.freqCount; i++) {
            JsonObject e = arr868.add<JsonObject>();
            e["freq"] = dualSweep868.heatmap[i].freqMHz;
            e["rssiPeak"] = dualSweep868.heatmap[i].rssiPeak;
            e["rssiAvg"] = dualSweep868.heatmap[i].rssiAvgCurrent;
            e["signals"] = dualSweep868.heatmap[i].signalsDecoded;
            e["cycles"] = dualSweep868.heatmap[i].sweepCycles;
        }
    }
    
    String json;
    serializeJson(doc, json);
    return json;
}

void RFManager::setScanMode(ScanMode mode, const SweepConfig& cfg) {
    Serial.printf("[RF] setScanMode(%d) — current=%d, taskHandle=%p, heap=%d\n",
        (int)mode, (int)scanMode, (void*)sweepTaskHandle, ESP.getFreeHeap());
    
    // Stop existing sweep
    if (sweepTaskHandle != nullptr) {
        scanMode = ScanMode::FIXED;  // Signal task to exit
        vTaskDelay(pdMS_TO_TICKS(100));  // Give task time to clean up
        if (sweepTaskHandle != nullptr) {
            Serial.printf("[RF] setScanMode: force-deleting task %p\n", (void*)sweepTaskHandle);
            vTaskDelete(sweepTaskHandle);
            sweepTaskHandle = nullptr;
        } else {
            Serial.println("[RF] setScanMode: task exited cleanly");
        }
        
    }
    
    // Sweeps and fixed mode both start from the home profiles (OOK 433 / FSK 868).
    // Holding radioMutex keeps the profile task from switching underneath.
    xSemaphoreTake(radioMutex, portMAX_DELAY);
    applyProfile(homeProfile(false));
    applyProfile(homeProfile(true));
    scanMode = mode;
    sweepCfg = cfg;
    xSemaphoreGive(radioMutex);
    
    if (mode == ScanMode::CONTINUOUS_SWEEP) {
        // Start sweep task
        xTaskCreatePinnedToCore(
            sweepTaskFunc,
            "rfSweep",
            8192,
            this,
            1,                     // Lower priority than receiver tasks
            &sweepTaskHandle,
            1                      // Core 1 (keep receiver tasks on Core 0)
        );
    }
}

ScanMode RFManager::getScanMode() {
    return scanMode;
}

String RFManager::getSweepStatusJSON() {
    JsonDocument doc;
    
    // Mode as string (frontend expects "fixed", "sweep", "dualscan", "dualmod")
    switch (scanMode) {
        case ScanMode::FIXED:             doc["mode"] = "fixed"; break;
        case ScanMode::CONTINUOUS_SWEEP:  doc["mode"] = "sweep"; break;
        case ScanMode::DUAL_SCAN:         doc["mode"] = "dualscan"; break;
        case ScanMode::DUAL_MODULATION:   doc["mode"] = "dualmod"; break;
    }
    
    doc["freq433"] = mod433.currentFreq;
    doc["freq868"] = mod868.currentFreq;
    
    if (scanMode == ScanMode::CONTINUOUS_SWEEP) {
        doc["currentFreq"] = (sweepCfg.startFreqMHz > 800.0f) ? 
            mod868.currentFreq : mod433.currentFreq;
        doc["cycle"] = sweepCycleCount;
        doc["freqCount"] = sweepFreqCount;
        doc["dwellMs"] = sweepCfg.dwellMs;
        doc["durationMs"] = sweepCfg.durationMs;
        doc["startMHz"] = sweepCfg.startFreqMHz;
        doc["endMHz"] = sweepCfg.endFreqMHz;
        
        // BW as string for frontend
        switch (sweepCfg.bwMDMCFG4) {
            case 0xf7: doc["bw"] = "58k"; break;
            case 0xc7: doc["bw"] = "102k"; break;
            case 0x87: doc["bw"] = "203k"; break;
            case 0x57: doc["bw"] = "325k"; break;
            default:   doc["bw"] = "102k"; break;
        }
        
        uint32_t elapsed = millis() - sweepStartMs;
        doc["elapsedMs"] = elapsed;
        
        if (sweepCfg.durationMs > 0 && elapsed < sweepCfg.durationMs) {
            doc["remainingMs"] = sweepCfg.durationMs - elapsed;
        }
        
        // Current dwell remaining
        if (sweepStepStartMs > 0) {
            uint32_t dwellElapsed = millis() - sweepStepStartMs;
            if (dwellElapsed < sweepCfg.dwellMs) {
                doc["dwellRemaining"] = sweepCfg.dwellMs - dwellElapsed;
            }
        }
        
        // Band indicator
        doc["band"] = (sweepCfg.startFreqMHz > 800.0f) ? "868" : "433";
    }
    
    String json;
    serializeJson(doc, json);
    return json;
}

String RFManager::getHeatmapJSON() {
    if (!heatmap || sweepFreqCount == 0) return "[]";
    
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < sweepFreqCount; i++) {
        JsonObject entry = arr.add<JsonObject>();
        entry["freq"] = heatmap[i].freqMHz;
        entry["rssiPeak"] = heatmap[i].rssiPeak;
        entry["rssiAvg"] = heatmap[i].rssiAvgCurrent;
        entry["signals"] = heatmap[i].signalsDecoded;
        entry["cycles"] = heatmap[i].sweepCycles;
    }
    String json;
    serializeJson(doc, json);
    return json;
}

String RFManager::getDualModStatusJSON() {
    JsonDocument doc;
    doc["hardwareDual"] = true;
    doc["ookFreq"] = 433.92;
    doc["fskFreq"] = 868.35;
    doc["ookActive"] = mod433.receiving;
    doc["fskActive"] = mod868.receiving;
    doc["ookSignals"] = mod433.totalSignals;
    doc["fskSignals"] = mod868.totalSignals;
    String json;
    serializeJson(doc, json);
    return json;
}

// ===================================================================
//  RSSI SENSITIVITY
// ===================================================================
void RFManager::setRSSISensitivity(int level, bool is868) {
    // Same scale on both radios: threshold = noise floor + 3/6/9/12 dB
    if (level < 0 || level > 3) level = 1;
    if (is868) {
        mod868.rssiSensitivityLevel = level;  // applied in next RSSI averaging cycle
    } else {
        mod433.rssiSensitivityLevel = level;
        rf433.setRSSIThreshold(3 + 3 * level);
    }
}

int RFManager::getRSSISensitivity(bool is868) {
    return is868 ? mod868.rssiSensitivityLevel : mod433.rssiSensitivityLevel;
}

float RFManager::getCurrentFrequency(bool is868) {
    return is868 ? mod868.currentFreq : mod433.currentFreq;
}

int RFManager::getCurrentRSSI(bool is868) {
    if (is868) {
        return rx868_currentRssi;
    }
    return rtl_433_ESP::currentRssi;
}

void RFManager::onSignal(SignalCallback cb) {
    signalCallback = cb;
}

// ===================================================================
//  DEVICE PROFILES (stubs — future feature)
// ===================================================================
String RFManager::getDeviceProfilesJSON() {
    return "[]";
}

void RFManager::tagDevice(const char* deviceKey, bool tagged) {
    // TODO: Implement with PSRAM database
}

// ===================================================================
//  RECEIVE PROFILES
//  Time-division over modulation / data-rate filter / bandwidth so that
//  FSK decoders also run on 433, OOK decoders on 868, and 868 FSK devices
//  with other bitrates than 17.24 kbit/s become visible.
// ===================================================================
int RFManager::homeProfile(bool is868) const {
    for (int i = 0; i < RX_PROFILE_COUNT; i++)
        if (RX_PROFILES[i].is868 == is868 && RX_PROFILES[i].home) return i;
    return -1;
}

int RFManager::nextAlternative(bool is868) {
    const int m = is868 ? 1 : 0;
    for (int step = 1; step <= RX_PROFILE_COUNT; step++) {
        int i = (lastAlternative[m] + step + RX_PROFILE_COUNT) % RX_PROFILE_COUNT;
        if (RX_PROFILES[i].is868 == is868 && !RX_PROFILES[i].home && !RX_PROFILES[i].testOnly) {
            static uint8_t skipped[RX_PROFILE_COUNT] = {0};
            if (++skipped[i] < probeEvery(RX_PROFILES[i].id)) continue;   // probe only every Nth round
            skipped[i] = 0;
            lastAlternative[m] = i;
            return i;
        }
    }
    return homeProfile(is868);
}

// Fixed dwell times (measured 2026-11-21, see rf_profiles.h). The former
// "double after a decode" rule made the rotation unpredictable (wM-Bus 40 s).
uint32_t RFManager::effectiveDwell(int idx) const {
    return RX_PROFILES[idx].dwellMs;
}


RxProfile RFManager::effectiveProfile(int idx) const {
    RxProfile p = RX_PROFILES[idx];
    const bool slot433 = strcmp(p.id, "tune433") == 0, slot868 = strcmp(p.id, "tune868") == 0;
    if ((slot433 && tuneSet[0]) || (slot868 && tuneSet[1])) {
        const RxProfile& t = tuneParams[slot868 ? 1 : 0];
        p.mod = t.mod; p.freqMHz = t.freqMHz; p.bitrateKbps = t.bitrateKbps; p.devKHz = t.devKHz;
        p.bwKHz = t.bwKHz; p.agc2 = t.agc2; p.agc1 = t.agc1; p.agc0 = t.agc0; p.frend1 = t.frend1;
        p.fsctrl1 = t.fsctrl1; if (t.minPulseUs) p.minPulseUs = t.minPulseUs;
    }
    return p;
}

bool RFManager::setTune(const RxProfile& params, uint32_t durationMs) {
    const int slot = params.is868 ? 1 : 0;
    xSemaphoreTake(radioMutex, portMAX_DELAY);
    tuneParams[slot] = params; tuneSet[slot] = true;
    const int idx = [&]() { for (int i = 0; i < RX_PROFILE_COUNT; i++)
        if (strcmp(RX_PROFILES[i].id, params.is868 ? "tune868" : "tune433") == 0) return i; return -1; }();
    if (idx >= 0 && activeProfile[slot] == idx) applyProfile(idx);   // re-apply new values at once
    xSemaphoreGive(radioMutex);
    return idx >= 0 && pinProfile(RX_PROFILES[idx].id, durationMs);
}

int16_t RFManager::applyProfile(int idx) {
    if (idx < 0 || idx >= RX_PROFILE_COUNT) return -1;
    const RxProfile p = effectiveProfile(idx);
    const bool ook = p.mod == RxMod::OOK;
    const int m = p.is868 ? 1 : 0;
    CC1101* r = p.is868 ? mod868.radio : (CC1101*)rtl_433_ESP::radioHandle();
    Module* mod = p.is868 ? mod868.radioModule : (Module*)rtl_433_ESP::moduleHandle();
    if (!r || !mod) return -1;

    if (p.is868) { wmbusActive = false; rx868_disableReceiver(); } else rf433.disableReceiver();
    vTaskDelay(pdMS_TO_TICKS(15));  // let receiver tasks finish in-flight SPI reads
    if (p.is868 && p.mod != RxMod::WMBUS) wmbusLeave();  // restore every register touched by packet mode
    if (p.mod == RxMod::WMBUS) {
        const uint32_t now = millis();
        if (activeProfile[m] >= 0) rxStats[activeProfile[m]].activeMs += now - profileStartMs[m];
        activeProfile[m] = idx; profileStartMs[m] = now; rxStats[idx].visits++;
        mod868.currentFreq = p.freqMHz;
        trace868Add('P', 0, 0, 0, 0, 0, p.id);
        wmbusEnter(p);
        return RADIOLIB_ERR_NONE;
    }

    int16_t err = RADIOLIB_ERR_NONE;
    auto chk = [&err](int16_t s) { if (s != RADIOLIB_ERR_NONE && err == RADIOLIB_ERR_NONE) err = s; };
    r->standby();
    chk(r->setOOK(ook));
    if (!ook) chk(r->setFrequencyDeviation(p.devKHz));
    chk(r->setBitRate(p.bitrateKbps));
    chk(r->setRxBandwidth(p.bwKHz));
    chk(r->setFrequency(p.freqMHz));
    r->standby();
    chk(mod->SPIsetRegValue(RADIOLIB_CC1101_REG_PKTLEN, 0));
    chk(mod->SPIsetRegValue(RADIOLIB_CC1101_REG_FSCTRL1, p.fsctrl1));
    chk(mod->SPIsetRegValue(RADIOLIB_CC1101_REG_FOCCFG, ook ? 0x18 : 0x1A));
    chk(mod->SPIsetRegValue(RADIOLIB_CC1101_REG_AGCCTRL2, p.agc2));
    chk(mod->SPIsetRegValue(RADIOLIB_CC1101_REG_AGCCTRL1, p.agc1));
    chk(mod->SPIsetRegValue(RADIOLIB_CC1101_REG_AGCCTRL0, p.agc0));
    chk(mod->SPIsetRegValue(RADIOLIB_CC1101_REG_FREND1, p.frend1));
    // Calibrate the synthesizer on every IDLE->RX. A power-cycled CC1101 boots with
    // FS_AUTOCAL off and is practically deaf until this is set again.
    chk(mod->SPIsetRegValue(RADIOLIB_CC1101_REG_MCSM0, 0x18));
    chk(r->receiveDirectAsync());
    snapshotRegs(m);

    const uint32_t now = millis();
    if (activeProfile[m] >= 0) rxStats[activeProfile[m]].activeMs += now - profileStartMs[m];
    activeProfile[m] = idx;
    profileStartMs[m] = now;
    rxStats[idx].visits++;

    if (p.is868) {
        trace868Add('P', 0, 0, 0, 0, 0, p.id);
        rx868_ook = ook;
        rx868_minPulseUs = p.minPulseUs;
        rx868_tagFreqHz = p.freqMHz * 1e6f;
        rx868_pulseTag = rxProfileTag(idx);
        mod868.currentFreq = p.freqMHz;
        rx868_resetReceiver();
        rx868_enableReceiver();
    } else {
        rtl_433_ESP::ookModulation = ook;
        rtl_433_ESP::updatePulseParams(ook);
        rtl_433_ESP::tagFreqHz = p.freqMHz * 1e6f;
        rtl_433_ESP::pulseTag = rxProfileTag(idx);
        mod433.currentFreq = p.freqMHz;
        rf433.enableReceiver();
    }
    if (err != RADIOLIB_ERR_NONE)
        Serial.printf("[RF] Profile %s: config error %d\n", p.id, err);
    return err;
}

void RFManager::applyHomeProfiles() {
    xSemaphoreTake(radioMutex, portMAX_DELAY);
    applyProfile(homeProfile(false));
    applyProfile(homeProfile(true));
    xSemaphoreGive(radioMutex);
}

void RFManager::setProfileRotation(bool enabled) {
    profileRotation = enabled;
    if (!enabled) { pinnedProfile = -1; applyHomeProfiles(); }
}

bool RFManager::pinProfile(const char* id, uint32_t durationMs) {
    for (int i = 0; i < RX_PROFILE_COUNT; i++) {
        if (strcmp(RX_PROFILES[i].id, id) == 0) {
            pinnedUntilMs = millis() + durationMs;
            pinnedProfile = i;
            return true;
        }
    }
    return false;
}

void RFManager::profileTaskFunc(void* pvParameters) {
    RFManager* self = (RFManager*)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(5000));  // let RSSI bootstrap finish in home profiles
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (self->scanMode != ScanMode::FIXED) continue;  // sweeps own the radios
        xSemaphoreTake(self->radioMutex, portMAX_DELAY);
        if (self->scanMode == ScanMode::FIXED) {
            const uint32_t now = millis();
            int pinned = self->pinnedProfile;
            if (pinned >= 0 && (int32_t)(now - self->pinnedUntilMs) >= 0) {
                self->pinnedProfile = pinned = -1;
            }
            for (int m = 0; m < 2; m++) {
                const bool is868 = m == 1;
                const int cur = self->activeProfile[m];
                int next = cur;
                const int hunt = is868 && pinned < 0 ? RfHunt::get().profile868(now) : -1;
                if (cur >= 0) RfHunt::get().noteDwell(cur, 200);
                if (pinned >= 0 && RX_PROFILES[pinned].is868 == is868) {
                    next = pinned;
                } else if (hunt >= 0) {
                    next = hunt;    // weather-station hunt owns the 868 module
                } else if (!self->profileRotation) {
                    next = self->homeProfile(is868);
                } else if (cur < 0 || now - self->profileStartMs[m] >= self->effectiveDwell(cur)) {
                    // Don't cut a telegram in half (max 3 s grace).
                    const bool busy = is868 ? rx868_receiveMode : rtl_433_ESP::getReceiveMode();
                    if (busy && cur >= 0 && now - self->profileStartMs[m] < self->effectiveDwell(cur) + 3000)
                        continue;
                    next = (cur >= 0 && RX_PROFILES[cur].home) ? self->nextAlternative(is868)
                                                               : self->homeProfile(is868);
                }
                if (next == cur && cur >= 0 && now - self->lastRegCheck[m] > 2000 &&
                    !(is868 && self->wmbusActive)) {
                    self->lastRegCheck[m] = now;
                    if (!self->checkRegs(m)) {
                        Serial.printf("[RF] %s: CC1101-Register fremd verändert (%s) – Profil neu geschrieben\n",
                                      is868 ? "868" : "433", self->regErrorLast[m]);
                        self->applyProfile(cur);
                    }
                }
                if (next != cur) self->applyProfile(next);
                else if (cur >= 0 && now - self->profileStartMs[m] >= self->effectiveDwell(cur))
                    self->profileStartMs[m] = now;  // single-profile module: restart dwell
            }
        }
        xSemaphoreGive(self->radioMutex);
    }
}

String RFManager::getRxProfilesJSON() {
    JsonDocument doc;
    doc["rotation"] = profileRotation;
    doc["uptimeMs"] = millis();
    // Live per-radio metrics (both CC1101)
    JsonArray mods = doc["modules"].to<JsonArray>();
    for (int m = 0; m < 2; m++) {
        const bool is868 = m == 1;
        JsonObject o = mods.add<JsonObject>();
        o["band"] = is868 ? "868" : "433";
        o["ok"] = is868 ? mod868.moduleOk : mod433.moduleOk;
        o["freqMHz"] = is868 ? mod868.currentFreq : mod433.currentFreq;
        o["sensitivity"] = is868 ? mod868.rssiSensitivityLevel : mod433.rssiSensitivityLevel;
        o["rssi"] = is868 ? rx868_currentRssi : rtl_433_ESP::currentRssi;
        o["noiseFloor"] = is868 ? rx868_averageRssi : rtl_433_ESP::averageRssi;
        o["threshold"] = is868 ? rx868_rssiThreshold : rtl_433_ESP::rssiThreshold;
        o["receiving"] = is868 ? (bool)rx868_receiveMode : rtl_433_ESP::getReceiveMode();
        o["rawSignals"] = is868 ? rx868_totalSignals : rtl_433_ESP::totalSignals;
        o["ignored"] = is868 ? rx868_ignoredSignals : rtl_433_ESP::ignoredSignals;
        o["decoded"] = is868 ? mod868.decodedSignals : mod433.decodedSignals;
        extern volatile uint32_t linkraQueueDrops433, linkraQueueDrops868;
        o["queueDrops"] = is868 ? linkraQueueDrops868 : linkraQueueDrops433;
        if (!is868) {
            o["captureOverruns"] = rtl_433_ESP::captureOverruns;
            o["capture"] = rtl_433_ESP::externalCapture ? "rmt" : "isr";
            const Rx433Rmt::Stats& r = Rx433Rmt::stats();
            JsonObject rm = o["rmt"].to<JsonObject>();
            rm["frames"] = r.frames; rm["accepted"] = r.accepted; rm["tooShort"] = r.tooShort;
            rm["belowLevel"] = r.belowLevel; rm["overflow"] = r.overflow; rm["noSlot"] = r.noSlot;
            rm["lastPulses"] = r.lastPulses; rm["lastPeak"] = r.lastPeak;
        }
        {
            extern volatile uint32_t linkraDecN[2][3], linkraDecUs[2][3], linkraDecMax[2][3], linkraQueueHigh[2];
            const int l = is868 ? 1 : 0;
            JsonArray dec = o["decoder"].to<JsonArray>();
            for (int b = 0; b < 3; b++) {
                JsonObject d = dec.add<JsonObject>();
                d["n"] = linkraDecN[l][b];
                d["avgUs"] = linkraDecN[l][b] ? linkraDecUs[l][b] / linkraDecN[l][b] : 0;
                d["maxUs"] = linkraDecMax[l][b];
            }
            o["queueHigh"] = linkraQueueHigh[l];
        }
        if (!is868) o["forcedEnds"] = rtl_433_ESP::forcedEnds;
        if (is868) o["isrEdges"] = rx868_isrEdgeCount;
        o["regErrors"] = regErrors[m];
        if (regErrorLast[m][0]) o["regErrorLast"] = regErrorLast[m];
        if (is868) {
            JsonObject w = o["wmbus"].to<JsonObject>();
            w["active"] = (bool)wmbusActive; w["syncs"] = wmbusStats.syncs; w["ok"] = wmbusStats.ok;
            w["bad"] = wmbusStats.bad; w["overflow"] = wmbusStats.overflow; w["lastError"] = wmbusStats.lastError;
            w["chipVersion"] = wmbusStats.version; w["marcState"] = wmbusStats.marc; w["rxbytesRaw"] = wmbusStats.rxbytes;
            w["pktctrl0"] = wmbusStats.pktctrl0; w["mdmcfg2"] = wmbusStats.mdmcfg2; w["iocfg0"] = wmbusStats.iocfg0;
            w["isr"] = wmbusStats.isr; w["wakes"] = wmbusStats.wakes; w["maxFill"] = wmbusStats.maxFill;
            w["firstRx"] = wmbusStats.firstRx; w["latencyUs"] = wmbusStats.latencyUs;
            w["wakeUs"] = wmbusStats.wakeUs; w["spiUs"] = wmbusStats.spiUs; w["txnUs"] = cc868TxnUs; w["rdyUs"] = cc868RdyUs;
            char regs[64]; snprintf(regs, sizeof regs, "MDMCFG4=%02x MDMCFG3=%02x DEV=%02x FREQ=%02x%02x%02x",
                wmbusStats.mdmcfg4, wmbusStats.mdmcfg3, wmbusStats.deviatn, wmbusStats.freq2, wmbusStats.freq1, wmbusStats.freq0);
            w["regs"] = regs;
        }
        const int a = activeProfile[m];
        o["profile"] = a >= 0 ? RX_PROFILES[a].id : nullptr;
        if (a >= 0) {
            const RxProfile ep = effectiveProfile(a);
            o["modulation"] = ep.mod == RxMod::OOK ? "OOK" : ep.mod == RxMod::WMBUS ? "wM-Bus" : "FSK";
            o["bitrateKbps"] = ep.bitrateKbps;
            o["bwKHz"] = ep.bwKHz;
            o["devKHz"] = ep.devKHz;
            char regs[64]; snprintf(regs, sizeof regs, "AGC %02X/%02X/%02X FREND1 %02X FSCTRL1 %02X",
                                    ep.agc2, ep.agc1, ep.agc0, ep.frend1, ep.fsctrl1);
            o["regs"] = regs;
            const uint32_t el = millis() - profileStartMs[m];
            const uint32_t dw = effectiveDwell(a);
            o["profileElapsedMs"] = el;
            o["profileDwellMs"] = dw;
        }
    }
    {
        doc["gateRun"] = linkra_gate_run; doc["gateSkipped"] = linkra_gate_skipped;
    }
    doc["implausibleDrops"] = implausibleDrops;
    doc["unconfirmedDrops"] = unconfirmedDrops;
    doc["implausibleLast"] = implausibleLast;
    doc["hunt"] = RfHunt::get().active();
    doc["pinned"] = pinnedProfile >= 0 ? RX_PROFILES[pinnedProfile].id : nullptr;
    doc["pinnedRemainingMs"] = pinnedProfile >= 0 ? (int32_t)(pinnedUntilMs - millis()) : 0;
    doc["active433"] = activeProfile[0] >= 0 ? RX_PROFILES[activeProfile[0]].id : nullptr;
    doc["active868"] = activeProfile[1] >= 0 ? RX_PROFILES[activeProfile[1]].id : nullptr;
    JsonArray arr = doc["profiles"].to<JsonArray>();
    const uint32_t now = millis();
    for (int i = 0; i < RX_PROFILE_COUNT; i++) {
        const RxProfile& p = RX_PROFILES[i];
        JsonObject o = arr.add<JsonObject>();
        o["id"] = p.id;
        o["band"] = p.is868 ? "868" : "433";
        o["modulation"] = p.mod == RxMod::OOK ? "OOK" : p.mod == RxMod::WMBUS ? "wM-Bus" : "FSK";
        o["freqMHz"] = p.freqMHz;
        o["bitrateKbps"] = p.bitrateKbps;
        o["bwKHz"] = p.bwKHz;
        o["dwellMs"] = effectiveDwell(i);
        o["home"] = p.home;
        o["note"] = p.note;
        uint32_t active = rxStats[i].activeMs;
        const int m = p.is868 ? 1 : 0;
        if (activeProfile[m] == i) active += now - profileStartMs[m];
        o["visits"] = rxStats[i].visits;
        o["activeMs"] = active;
        o["decodes"] = rxStats[i].decodes;
        o["unparsed"] = rxStats[i].unparsed;
        o["lastDecodeAgoS"] = rxStats[i].lastDecodeMs ? (int32_t)((now - rxStats[i].lastDecodeMs) / 1000) : -1;
    }
    String out;
    serializeJson(doc, out);
    return out;
}


// ===================================================================
//  WIRELESS M-BUS (T1/C1) — CC1101 packet mode on the 868 radio
//  The chip searches the 0x543D sync word itself; the receiver task then
//  drains the RX FIFO (polling, no GDO2 needed) and hands complete frames
//  to wmbus::decode (3-of-6, CRC, header, unencrypted records).
// ===================================================================
// Same clock as RadioLib: a different frequency makes every beginTransaction()
// recompute the clock divider, which cost ~10 ms per switch.
static const SPISettings CC868_SPI(2000000, MSBFIRST, SPI_MODE0);

static inline void cc868Select(SPIClass* spi) {
    const uint32_t t0 = micros();
    spi->beginTransaction(CC868_SPI);
    const uint32_t t1 = micros();
    digitalWrite(CC1101_CS_868, LOW);   // crystal runs in RX/IDLE: chip is always ready, no CHIP_RDYn wait
    cc868TxnUs = t1 - t0; cc868RdyUs = micros() - t1;
}
static inline void cc868Release(SPIClass* spi) { digitalWrite(CC1101_CS_868, HIGH); spi->endTransaction(); }

uint8_t RFManager::cc868Status(uint8_t reg) {
    cc868Select(mod868.spi); mod868.spi->transfer(reg | 0xC0); uint8_t v = mod868.spi->transfer(0); cc868Release(mod868.spi);
    return v;
}
void RFManager::cc868Strobe(uint8_t cmd) {
    cc868Select(mod868.spi); mod868.spi->transfer(cmd); cc868Release(mod868.spi);
}
void RFManager::cc868ReadFifo(uint8_t* buf, uint8_t n) {
    cc868Select(mod868.spi); mod868.spi->transfer(0xFF);
    for (uint8_t i = 0; i < n; i++) buf[i] = mod868.spi->transfer(0);
    cc868Release(mod868.spi);
}

void RFManager::wmbusEnter(const RxProfile& p) {
    if (!mod868.radio || !mod868.radioModule) return;
    Module* mod = mod868.radioModule;
    if (!wmbusRegsSaved) {
        for (uint8_t r = 0; r < 0x2F; r++) wmbusSavedRegs[r] = mod->SPIreadRegister(r);
        wmbusRegsSaved = true;
    }
    mod868.radio->standby();
    mod868.radio->setOOK(false);
    mod868.radio->setFrequency(p.freqMHz);
    mod868.radio->setBitRate(p.bitrateKbps);
    mod868.radio->setFrequencyDeviation(p.devKHz);
    mod868.radio->setRxBandwidth(p.bwKHz);
    // Packet engine: sync 0x543D (16/16), infinite length, no CRC/whitening/address, FIFO mode
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_IOCFG0, 0x01);   // GDO0: RX FIFO >= threshold or end of packet
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_IOCFG2, 0x2E);
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_FIFOTHR, 0x40);  // RX threshold 4 bytes, ADC retention
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_SYNC1, 0x54);
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_SYNC0, 0x3D);
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_PKTLEN, 0xFF);
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_PKTCTRL1, 0x00);
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_PKTCTRL0, 0x02);
    mod->SPIsetRegValue(RADIOLIB_CC1101_REG_MDMCFG2, 0x02);   // 2-FSK, no Manchester, 16/16 sync
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_MCSM1, 0x00);   // back to IDLE after packet
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_MCSM0, 0x18);   // autocal IDLE->RX
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_FOCCFG, 0x2E);
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_BSCFG, 0xBF);   // data-rate offset compensation (meter clocks drift)
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_AGCCTRL2, 0x43);
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_AGCCTRL1, 0x09);
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_AGCCTRL0, 0xB5);
    mod->SPIwriteRegister(RADIOLIB_CC1101_REG_FREND1, 0xB6);
    cc868Strobe(RADIOLIB_CC1101_CMD_IDLE);
    cc868Strobe(RADIOLIB_CC1101_CMD_FLUSH_RX);
    cc868Strobe(RADIOLIB_CC1101_CMD_RX);
    wmbusActive = true;
    pinMode(CC1101_GDO0_868, INPUT);
    attachInterrupt(digitalPinToInterrupt(CC1101_GDO0_868), wmbusFifoIsr, RISING);

}

void RFManager::wmbusLeave() {
    if (!wmbusRegsSaved || !mod868.radioModule) return;
    wmbusActive = false;
    detachInterrupt(digitalPinToInterrupt(CC1101_GDO0_868));
    cc868Strobe(RADIOLIB_CC1101_CMD_IDLE);
    cc868Strobe(RADIOLIB_CC1101_CMD_FLUSH_RX);
    for (uint8_t r = 0; r < 0x2F; r++) mod868.radioModule->SPIwriteRegister(r, wmbusSavedRegs[r]);
    wmbusRegsSaved = false;
}

void IRAM_ATTR RFManager::wmbusFifoIsr() {
    BaseType_t woken = pdFALSE;
    if (instance) { instance->wmbusStats.isr++; instance->wmbusStats.isrUs = micros(); }
    if (instance && instance->wmbusTaskHandle)
        vTaskNotifyGiveFromISR(instance->wmbusTaskHandle, &woken);
    if (woken) portYIELD_FROM_ISR();
}

// Dedicated drain task on core 1 (core 0 carries WiFi + rtl_433 decoding, which
// delayed wake-ups by >20 ms; the 64-byte FIFO overflows after ~5 ms).
void RFManager::wmbusTaskFunc(void* pv) {
    RFManager* self = (RFManager*)pv;
    for (;;) {
        if (!self->wmbusActive) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)); continue; }
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50))) { self->wmbusStats.wakes++; self->wmbusStats.wakeUs = micros() - self->wmbusStats.isrUs; }
        const uint32_t t0 = millis();
        // Yield between FIFO reads: 1 ms = ~12 bytes of a 64-byte FIFO. Busy-waiting
        // here starved the profile task, the decoder and the web server on core 1.
        while (self->wmbusActive && self->wmbusPoll(self->activeProfile[1]) && millis() - t0 < 200)
            vTaskDelay(1);
    }
}

bool RFManager::wmbusPoll(int profileIdx) {
    static uint8_t buf[640];
    static size_t len = 0, want = 0;
    static uint32_t lastByteUs = 0;
    static int rssiAtSync = -128, lqi = 0;
    auto restart = [&]() {
        cc868Strobe(RADIOLIB_CC1101_CMD_IDLE);
        cc868Strobe(RADIOLIB_CC1101_CMD_FLUSH_RX);
        cc868Strobe(RADIOLIB_CC1101_CMD_RX);
        len = 0; want = 0;
    };
    // RXBYTES must be read until two reads agree (CC1101 errata)
    static uint32_t lastDiag = 0;
    if (len == 0 && millis() - lastDiag > 1000 && !digitalRead(CC1101_GDO0_868)) {  // only while idle
        lastDiag = millis();
        wmbusStats.version = cc868Status(0x31);
        wmbusStats.marc = cc868Status(0x35) & 0x1F;
        wmbusStats.rxbytes = cc868Status(0x3B);
        wmbusStats.pktctrl0 = mod868.radioModule->SPIreadRegister(RADIOLIB_CC1101_REG_PKTCTRL0);
        wmbusStats.mdmcfg2 = mod868.radioModule->SPIreadRegister(RADIOLIB_CC1101_REG_MDMCFG2);
        wmbusStats.iocfg0 = mod868.radioModule->SPIreadRegister(RADIOLIB_CC1101_REG_IOCFG0);
        wmbusStats.mdmcfg4 = mod868.radioModule->SPIreadRegister(RADIOLIB_CC1101_REG_MDMCFG4);
        wmbusStats.mdmcfg3 = mod868.radioModule->SPIreadRegister(RADIOLIB_CC1101_REG_MDMCFG3);
        wmbusStats.deviatn = mod868.radioModule->SPIreadRegister(RADIOLIB_CC1101_REG_DEVIATN);
        wmbusStats.freq2 = mod868.radioModule->SPIreadRegister(RADIOLIB_CC1101_REG_FREQ2);
        wmbusStats.freq1 = mod868.radioModule->SPIreadRegister(RADIOLIB_CC1101_REG_FREQ1);
        wmbusStats.freq0 = mod868.radioModule->SPIreadRegister(RADIOLIB_CC1101_REG_FREQ0);
    }
    // CC1101 errata: RXBYTES can be wrong if read while it changes. While a frame
    // streams it changes every 80 us, so never loop until stable: read twice and
    // take the lower value (bytes only ever get added).
    const uint32_t tSpi = micros();
    uint8_t n = cc868Status(0x3B);
    wmbusStats.spiUs = micros() - tSpi;
    const uint8_t n2 = cc868Status(0x3B);
    if ((n & 0x7F) > (n2 & 0x7F)) n = (n & 0x80) | (n2 & 0x7F);
    if (len == 0 && n) { wmbusStats.firstRx = n; wmbusStats.latencyUs = micros() - wmbusStats.isrUs; }
    if (n & 0x80) { wmbusStats.overflow++; restart(); return false; }
    n &= 0x7F;
    if (n > wmbusStats.maxFill) wmbusStats.maxFill = n;
    if (n == 0) {
        if (len && micros() - lastByteUs > 30000) {      // frame stalled
            wmbusStats.bad++; snprintf(wmbusStats.lastError, sizeof wmbusStats.lastError, "Abbruch nach %u Bytes", (unsigned)len);
            if (profileIdx >= 0) rxStats[profileIdx].unparsed++;
            restart();
        }
        return len > 0;
    }
    if (len == 0) {
        wmbusStats.syncs++;
        const uint8_t r = cc868Status(0x34);
        rssiAtSync = r >= 128 ? ((int)r - 256) / 2 - 74 : r / 2 - 74;
        lqi = cc868Status(0x33) & 0x7F;
    }
    // Leave one byte in the FIFO while receiving (CC1101 errata on last-byte reads)
    uint8_t take = n > 1 ? n - 1 : n;
    if (want && len + n >= want) take = n;
    if (len + take > sizeof(buf)) take = sizeof(buf) - len;
    cc868ReadFifo(buf + len, take);
    len += take; lastByteUs = micros();
    if (!want && len >= 3) {
        wmbus::Mode mode;
        want = wmbus::expectedRawLength(buf, len, mode);
        if (!want) {   // sync matched noise
            wmbusStats.bad++; snprintf(wmbusStats.lastError, sizeof wmbusStats.lastError, "ungültiger Rahmenanfang");
            restart(); return false;
        }
    }
    if (want && len >= want) {
        wmbus::Frame f = wmbus::decode(buf, want);
        if (f.ok) {
            wmbusStats.ok++;
            std::string js = wmbus::toJson(f, rssiAtSync, lqi);
            js.pop_back();  // add gateway metadata to the object
            char meta[160];
            snprintf(meta, sizeof meta, ",\"protocol\":\"Wireless M-Bus %s\",\"frequency\":%.2f}",
                     wmbus::modeName(f.mode), RX_PROFILES[profileIdx >= 0 ? profileIdx : 0].freqMHz);
            js += meta;
            Serial.printf("[wMBus] %s %s %s typ=%s enc=%d rssi=%d\n", wmbus::modeName(f.mode), f.manufacturer.c_str(),
                          f.id.c_str(), wmbus::deviceTypeName(f.type), f.encryption, rssiAtSync);
            std::vector<char> msg(js.begin(), js.end()); msg.push_back(0);
            processSignal(msg.data(), true, profileIdx);
        } else {
            wmbusStats.bad++;
            snprintf(wmbusStats.lastError, sizeof wmbusStats.lastError, "%s", f.error.c_str());
            if (profileIdx >= 0) rxStats[profileIdx].unparsed++;
        }
        restart();
        return false;
    }
    if (len >= sizeof(buf)) { wmbusStats.bad++; restart(); return false; }
    return true;
}


// ===================================================================
//  REGISTER WATCHDOG
// ===================================================================
static const uint8_t WATCH_REGS[9] = {0x02 /*IOCFG0*/, 0x08 /*PKTCTRL0*/, 0x0D, 0x0E, 0x0F /*FREQ*/,
                                      0x10 /*MDMCFG4*/, 0x12 /*MDMCFG2*/, 0x18 /*MCSM0*/, 0x1B /*AGCCTRL2*/};
static const char* WATCH_NAMES[9] = {"IOCFG0", "PKTCTRL0", "FREQ2", "FREQ1", "FREQ0", "MDMCFG4", "MDMCFG2", "MCSM0", "AGCCTRL2"};

static Module* moduleFor(RFManager* self, int m, Module* m868) {
    return m == 1 ? m868 : (Module*)rtl_433_ESP::moduleHandle();
}

void RFManager::snapshotRegs(int m) {
    Module* mod = moduleFor(this, m, mod868.radioModule);
    if (!mod) return;
    for (int i = 0; i < 9; i++) regSnap[m][i] = mod->SPIreadRegister(WATCH_REGS[i]);
    regSnapValid[m] = true;
    lastRegCheck[m] = millis();
}

bool RFManager::checkRegs(int m) {
    Module* mod = moduleFor(this, m, mod868.radioModule);
    if (!mod || !regSnapValid[m]) return true;
    for (int i = 0; i < 9; i++) {
        const uint8_t v = mod->SPIreadRegister(WATCH_REGS[i]);
        if (v != regSnap[m][i]) {
            // read twice: a single corrupted SPI read must not count
            if (mod->SPIreadRegister(WATCH_REGS[i]) == regSnap[m][i]) continue;
            regErrors[m]++;
            snprintf(regErrorLast[m], sizeof regErrorLast[m], "%s soll %02X ist %02X (%lus)",
                     WATCH_NAMES[i], regSnap[m][i], v, (unsigned long)(millis() / 1000));
            return false;
        }
    }
    return true;
}
