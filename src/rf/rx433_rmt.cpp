#include "rx433_rmt.h"
#include <driver/rmt_rx.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>
#include <rtl_433_ESP.h>
extern "C" {
#include "pulse_data.h"
}

void processSignal(pulse_data_t* rtl_pulses);   // signalDecoder.cpp: decoder queue

namespace Rx433Rmt {

static constexpr int CHUNK_SYM = 256;          // user buffer the driver fills piece by piece
static constexpr int FRAME_SYM = PD_MAX_PULSES + 8;
static constexpr int FRAMES = 4;

struct Frame {
    rmt_symbol_word_t* sym;                    // PSRAM
    uint16_t n;
    bool overflow;
    uint32_t endMs;
};

static rmt_channel_handle_t chan = nullptr;
static rmt_symbol_word_t* chunk = nullptr;     // internal RAM (driver copies from the ISR)
static Frame frames[FRAMES];
static volatile int fill = 0;                  // frame being filled by the ISR, -1 = none free
static QueueHandle_t doneQ = nullptr;          // indices of completed frames
static Stats st;
static volatile bool armFailed = false;        // rmt_receive failed in the ISR -> task re-arms
static rmt_receive_config_t rxCfg;

const Stats& stats() { return st; }

static int freeFrame() {
    for (int i = 0; i < FRAMES; i++) if (frames[i].n == 0 && i != fill) return i;
    return -1;
}

// ISR context (RMT RX interrupt). Copies each piece into the current frame;
// at the end of the package hands the frame to the task and re-arms.
static bool IRAM_ATTR onRecv(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t* ed, void*) {
    BaseType_t woken = pdFALSE;
    int f = fill;
    if (f >= 0) {
        Frame& fr = frames[f];
        for (size_t i = 0; i < ed->num_symbols; i++) {
            if (fr.n < FRAME_SYM) fr.sym[fr.n++] = ed->received_symbols[i];
            else { fr.overflow = true; break; }
        }
    }
    if (ed->flags.is_last) {
        if (f >= 0 && frames[f].n > 0) {
            frames[f].endMs = millis();
            xQueueSendFromISR(doneQ, &f, &woken);
            fill = -1;
            for (int i = 0; i < FRAMES; i++)
                if (frames[i].n == 0 && i != f) { fill = i; frames[i].overflow = false; break; }
        }
        if (rmt_receive(ch, chunk, CHUNK_SYM * sizeof(rmt_symbol_word_t), &rxCfg) != ESP_OK) armFailed = true;
    }
    return woken == pdTRUE;
}

// Symbols -> alternating level segments, glitch filter, pulse/gap arrays.
static uint8_t* lv = nullptr;                  // scratch, PSRAM (task context only)
static uint32_t* du = nullptr;
static unsigned buildTrain(const Frame& fr, pulse_data_t* p, uint32_t& totalUs) {
    unsigned m = 0;
    for (unsigned i = 0; i < fr.n; i++) {
        const rmt_symbol_word_t& s = fr.sym[i];
        if (!s.duration0) break;
        lv[m] = s.level0; du[m++] = s.duration0;
        if (!s.duration1) break;
        lv[m] = s.level1; du[m++] = s.duration1;
    }
    // Carrier spikes in the gaps become gap time – only in OOK profiles and only
    // isolated ones (both neighbouring gaps at least 2x longer). Protocols with
    // short pulses (Schrader TPMS 120 us, HCS361 100 us) have pulse-length gaps
    // and stay intact; FSK profiles are never folded (review 02 B2/B3).
    if (rtl_433_ESP::ookModulation) {
        for (unsigned i = 1; i + 1 < m; i++) {
            if (lv[i] && du[i] < RX433_MIN_HIGH_US && !lv[i - 1] && !lv[i + 1] &&
                du[i - 1] >= 2 * du[i] && du[i + 1] >= 2 * du[i])
                lv[i] = 0;
        }
    }
    // merge equal neighbours and fold glitches into the surrounding level (2 passes)
    for (int pass = 0; pass < 2; pass++) {
        unsigned k = 0;
        for (unsigned i = 0; i < m; i++) {
            if (k && (lv[k - 1] == lv[i] || (pass == 0 && du[i] < RX433_MIN_PULSE_US))) { du[k - 1] += du[i]; continue; }
            lv[k] = lv[i]; du[k++] = du[i];
        }
        m = k;
    }
    unsigned n = 0, i = 0;
    totalUs = 0;
    while (i < m && lv[i] == 0) i++;           // a package starts with a pulse
    bool open = false;                          // pulse stored, gap not yet
    for (; i < m && n < PD_MAX_PULSES; i++) {
        totalUs += du[i];
        if (lv[i]) { p->pulse[n] = du[i]; open = true; }
        else { p->gap[n++] = du[i]; open = false; }
    }
    if (open && n < PD_MAX_PULSES) p->gap[n++] = RX433_IDLE_US;   // real trailing silence
    return n;
}

// Supervisor (review 02 B4): noise alone ends a package every few seconds, so a
// capture that stays silent this long is stuck (failed re-arm, driver state).
static constexpr uint32_t STALL_MS = 30000;

static void rearm() {
    rmt_disable(chan);
    rmt_enable(chan);
    for (int i = 0; i < FRAMES; i++) frames[i].n = 0;
    fill = 0;
    armFailed = false;
    if (rmt_receive(chan, chunk, CHUNK_SYM * sizeof(rmt_symbol_word_t), &rxCfg) != ESP_OK) armFailed = true;
    st.rearms++;
    Serial.printf("[RX433] Erfassung neu gestartet (%lu)\n", (unsigned long)st.rearms);
}

static void taskFunc(void*) {
    uint32_t lastFrameMs = millis();
    for (;;) {
        int f;
        if (xQueueReceive(doneQ, &f, pdMS_TO_TICKS(1000)) != pdTRUE) {
            if (armFailed || millis() - lastFrameMs > STALL_MS) { rearm(); lastFrameMs = millis(); }
            continue;
        }
        lastFrameMs = millis();
        Frame& fr = frames[f];
        st.frames++;
        if (fr.overflow) st.overflow++;
        pulse_data_t* p = (pulse_data_t*)heap_caps_calloc(1, sizeof(pulse_data_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint32_t totalUs = 0;
        const unsigned n = p ? buildTrain(fr, p, totalUs) : 0;
        const uint32_t endMs = fr.endMs - RX433_IDLE_US / 1000;
        fr.n = 0;                                // frame buffer free again
        if (fill < 0) {                          // ISR found no free slot: give it this one
            fill = f;
            st.noSlot++;
        }
        if (!p) continue;                        // out of PSRAM: frame dropped, capture keeps running
        st.lastPulses = n;
        if (n <= PD_MIN_PULSES) { st.tooShort++; free(p); continue; }
        const uint32_t startMs = endMs - totalUs / 1000 - 1;
        const int peak = rtl_433_ESP::rssiPeak(startMs, endMs + 1);
        st.lastPeak = peak;
        if (peak < rtl_433_ESP::rssiThreshold) { st.belowLevel++; free(p); continue; }
        p->num_pulses = n;
        p->sample_rate = 1000000;
        p->signalDuration = totalUs;
        p->signalRssi = peak;
        p->noise_db = (float)rtl_433_ESP::averageRssi;
        p->fsk_f1_est = 0x7FFF;
        p->centerfreq_hz = 1.0f;                // source marker: RMT capture
        if (rtl_433_ESP::pulseTag) {
            p->freq1_hz = rtl_433_ESP::tagFreqHz;
            p->freq2_hz = (float)rtl_433_ESP::pulseTag;
        } else {
            p->freq1_hz = 433.92e6f;
        }
        st.accepted++;
        rtl_433_ESP::messageCount++;
        processSignal(p);
    }
}

bool begin(int gpio) {
    for (int i = 0; i < FRAMES; i++) {
        frames[i].sym = (rmt_symbol_word_t*)heap_caps_malloc(FRAME_SYM * sizeof(rmt_symbol_word_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        frames[i].n = 0; frames[i].overflow = false;
        if (!frames[i].sym) return false;
    }
    lv = (uint8_t*)heap_caps_malloc(2 * FRAME_SYM, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    du = (uint32_t*)heap_caps_malloc(2 * FRAME_SYM * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lv || !du) return false;
    chunk = (rmt_symbol_word_t*)heap_caps_malloc(CHUNK_SYM * sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    doneQ = xQueueCreate(FRAMES, sizeof(int));
    if (!chunk || !doneQ) return false;

    rmt_rx_channel_config_t cc = {};
    cc.gpio_num = (gpio_num_t)gpio;
    cc.clk_src = RMT_CLK_SRC_DEFAULT;
    cc.resolution_hz = 1000000;                  // 1 tick = 1 us
    cc.mem_block_symbols = 192;                  // 4 hardware blocks, ping-pong
    if (rmt_new_rx_channel(&cc, &chan) != ESP_OK) { Serial.println("[RX433] RMT channel failed"); return false; }
    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = onRecv;
    rmt_rx_register_event_callbacks(chan, &cbs, nullptr);
    rmt_enable(chan);

    rxCfg = {};
    rxCfg.signal_range_min_ns = 3000;            // hardware glitch filter (max ~3.2 us)
    rxCfg.signal_range_max_ns = RX433_IDLE_US * 1000UL;
    rxCfg.flags.en_partial_rx = 1;

    xTaskCreatePinnedToCore(taskFunc, "rx433rmt", 4096, nullptr, 3, nullptr, 0);
    fill = 0;
    if (rmt_receive(chan, chunk, CHUNK_SYM * sizeof(rmt_symbol_word_t), &rxCfg) != ESP_OK) {
        Serial.println("[RX433] rmt_receive failed"); return false;
    }
    Serial.printf("[RX433] RMT capture on GPIO %d (idle %d us, glitch %d us)\n", gpio, RX433_IDLE_US, RX433_MIN_PULSE_US);
    return true;
}

}  // namespace Rx433Rmt
