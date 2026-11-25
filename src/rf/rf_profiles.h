#pragma once
#include <stdint.h>

// Receive profiles: each one is a CC1101 async-mode configuration that lets a
// different group of rtl_433 decoders see usable pulse trains.
//
// Pulse trains are tagged with their profile (pulse_data_t.freq2_hz =
// RX_PROFILE_TAG_BASE + index*10 + modulation) so the decoder task runs the
// demods that match the modulation the train was CAPTURED with, not the band.

enum class RxMod : uint8_t { OOK = 1, FSK = 2, WMBUS = 3 };  // WMBUS = CC1101 packet mode, not pulse capture

struct RxProfile {
    const char* id;
    bool is868;          // which CC1101 module
    RxMod mod;
    float freqMHz;
    float bitrateKbps;   // post-demod data filter (async mode)
    float devKHz;        // FSK deviation (ignored for OOK)
    float bwKHz;         // RX channel filter
    uint16_t minPulseUs; // ISR glitch filter (868 receiver only)
    uint32_t dwellMs;    // time per visit
    bool home;           // home profile: visited between every alternative
    const char* note;
    uint8_t agc2 = 0x07; // AGCCTRL2/1/0; defaults = previous Flipper-style async config
    uint8_t agc1 = 0x00;
    uint8_t agc0 = 0x91;
    bool testOnly = false;  // never rotated; only active when pinned via /api/rf/profile
    uint8_t frend1 = 0xB6;  // LNA/mixer currents (FREND1)
    uint8_t fsctrl1 = 0x06; // IF frequency (FSCTRL1)
};

static constexpr uint32_t RX_PROFILE_TAG_BASE = 1000;

// Index order matters: tags encode the index. Append only.
static const RxProfile RX_PROFILES[] = {
    // ---- 433 MHz module ----
    // Home: 433.92 MHz / 203 kHz with AGC tuned by upstream rtl_433_ESP
    // (docs/CC1101_43392_TUNING_RESULTS.md: fewer zero-decode outcomes, no loss).
    {"ook433-92", false, RxMod::OOK, 433.92f, 3.79f, 0, 203, 0, 45000, true,
     "OOK 433.92 MHz, 203 kHz, Upstream-AGC: Wettersensoren, Fernbedienungen", 0xC7, 0x40, 0x93},
    // Previous wide profile (also covers Somfy RTS at 433.42) — kept for A/B comparison
    // A/B 2026-11-21: 433.67/541 and /650 lost most 433.92 sensors (noise +2..3 dB) -> pin only
    {"ook433-wide", false, RxMod::OOK, 433.67f, 3.79f, 0, 541, 0, 10000, false,
     "OOK 433.40-433.94 MHz (alt, breit): Somfy RTS + 433.92 am Filterrand", 0x07, 0x00, 0x91, true},
    // 433 FSK sensors and TPMS
    // Pin only since 2026-11-22: in FSK async mode noise toggles the data pin
    // continuously -> 800 ms trains that every FSK decoder walks (up to 1.3 s of
    // decoder time each, 433 queue overflow). Overnight it produced only phantoms.
    {"fsk433-20k", false, RxMod::FSK, 433.92f, 20.0f, 40.0f, 270, 0, 15000, false,
     "FSK 433.92 MHz ~10-20 kbit/s: TPMS, Fine-Offset/Bresser 433-FSK", 0x07, 0x00, 0x91, true},

    // ---- 868 MHz module ----
    {"fsk868-17k", true, RxMod::FSK, 868.35f, 17.24f, 40.0f, 270, 30, 25000, true,
     "FSK 868.3 MHz 17.24 kbit/s: LaCrosse, TX29, WH31E"},
    {"fsk868-8k", true, RxMod::FSK, 868.30f, 8.21f, 57.0f, 270, 50, 6000, false,
     "FSK 868.3 MHz ~8 kbit/s: Bresser 5/6/7-in-1, slow FSK"},
    {"fsk868-38k", true, RxMod::FSK, 868.30f, 38.4f, 40.0f, 325, 15, 15000, false,
     "FSK 868.3 MHz ~38 kbit/s: Honeywell CM921, Marlec, LaCrosse (hohe Rate)"},
    {"ook868", true, RxMod::OOK, 868.35f, 3.79f, 0, 325, 60, 6000, false,
     "OOK 868.35 MHz: FS20/FHT, 868-OOK-Sensoren"},
    // ---- Diagnostics (pin only): each chip listens on the other band ----
    {"xtest-433chip-868", false, RxMod::FSK, 868.35f, 17.24f, 40.0f, 270, 30, 0, false,
     "TEST: Modul an CS4 hört 868.35 FSK (Pegel von LaCrosse vergleichen)", 0x07, 0x00, 0x91, true},
    {"xtest-868chip-433", true, RxMod::OOK, 433.92f, 3.79f, 0, 203, 60, 0, false,
     "TEST: Modul an CS10 hört 433.92 OOK (Pegel von 433-Sensoren vergleichen)", 0xC7, 0x40, 0x93, true},
    // Wireless M-Bus T1 + C1 (868.95 MHz, 100 kchip/s): packet mode, decoded by src/rf/wmbus.cpp
    {"wmbus-t1c1", true, RxMod::WMBUS, 868.95f, 100.0f, 50.0f, 325, 0, 45000, false,
     "Wireless M-Bus T1/C1 868.95 MHz (Paketmodus): Wasser-, Wärme-, Strom-, Heizkostenzähler"},
    // ---- Tuning slots: parameters come from POST /api/rf/tune (pin only) ----
    {"tune433", false, RxMod::OOK, 433.92f, 3.79f, 0, 203, 0, 0, false, "TUNING 433 (Werte per /api/rf/tune)",
     0xC7, 0x40, 0x93, true},
    {"tune868", true, RxMod::FSK, 868.35f, 17.24f, 40.0f, 270, 30, 0, false, "TUNING 868 (Werte per /api/rf/tune)",
     0x07, 0x00, 0x91, true},
    // Somfy RTS remotes and sun/wind sensors, some gate openers (433.42 MHz)
    {"ook433-42", false, RxMod::OOK, 433.42f, 3.79f, 0, 203, 0, 10000, false,
     "OOK 433.42 MHz, 203 kHz: Somfy RTS, Rollladen-/Torsender", 0xC7, 0x40, 0x93},
    // Wireless M-Bus with the reference register set of culfw/rtl-wmbus/TI DN022
    // (IF 203 kHz, 103 kBaud, 38 kHz deviation, 15/16 sync + carrier sense).
    // Pin only, for an A/B test against "wmbus-t1c1" (review P3: CRC rate).
    {"wmbus-ref", true, RxMod::WMBUS, 868.95f, 103.0f, 38.0f, 325, 0, 0, false,
     "TEST: wM-Bus mit Referenz-Registern (culfw/TI) für den Vergleich", 0x43, 0x09, 0xB5, true, 0xB6, 0x08},
};

static constexpr int RX_PROFILE_COUNT = sizeof(RX_PROFILES) / sizeof(RX_PROFILES[0]);

inline uint32_t rxProfileTag(int idx) {
    return RX_PROFILE_TAG_BASE + idx * 10 + (uint32_t)RX_PROFILES[idx].mod;
}
