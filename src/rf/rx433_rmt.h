#pragma once
// 433 MHz pulse capture with the ESP32-S3 RMT peripheral (replaces the GPIO
// edge interrupt of rtl_433_ESP for the 433 module).
//
// Modelled on the CC1101 setups that work in practice (ESPHome remote_receiver
// + rtl_433 decoder, rtl_433's own pulse detector):
//  - hardware timestamps for every edge, no ISR per edge
//  - a package ends after RX433_IDLE_US without an edge (rtl_433: PD_MIN_GAP_MS)
//  - glitches shorter than RX433_MIN_PULSE_US are merged away
//  - the level gate is applied per package afterwards: the peak RSSI during the
//    package must reach the receive threshold (noise floor + sensitivity),
//    otherwise it is noise and never reaches the decoders.
#include <Arduino.h>

#define RX433_IDLE_US       10000   // end of package
#define RX433_MIN_PULSE_US  50      // shortest segment of either level (rtl_433_ESP: MINIMUM_PULSE_LENGTH)
// Shortest carrier pulse. In OOK async mode the CC1101 emits 60-330 us spikes
// inside the carrier-off gaps (AGC raises the gain on noise). A GT-WT02 frame
// at -83 dBm (pulses 500 us) only decoded in rtl_433 once pulses < 150 us were
// folded into the gaps (offline test 2026-11-22). Real 433 OOK protocols use
// >= ~200 us pulses.
#define RX433_MIN_HIGH_US   150

namespace Rx433Rmt {

struct Stats {
    uint32_t frames = 0;        // packages ended by the idle timeout
    uint32_t accepted = 0;      // handed to the decoders
    uint32_t tooShort = 0;      // <= PD_MIN_PULSES pulses after filtering
    uint32_t belowLevel = 0;    // peak RSSI below the receive threshold
    uint32_t overflow = 0;      // package longer than the frame buffer (truncated)
    uint32_t noSlot = 0;        // all frame buffers busy (decoder side too slow)
    uint32_t lastPulses = 0;
    int lastPeak = -128;
};

bool begin(int gpio);           // create the RMT channel and start receiving
const Stats& stats();

}  // namespace Rx433Rmt
