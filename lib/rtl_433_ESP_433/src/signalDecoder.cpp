/*
  rtl_433_ESP - 433.92 MHz protocols library for ESP32

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 3 of the License, or (at your option) any later version.
  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with library. If not, see <http://www.gnu.org/licenses/>


  Project Structure

  rtl_433_ESP - Main Class
  decoder.cpp - Wrapper and interface for the rtl_433 classes
  receiver.cpp - Wrapper and interface for RadioLib
  rtl_433 - subset of rtl_433 package

*/

#include "signalDecoder.h"
#include <climits>

/*----------------------------- Signal Classification ----------------------------*/

/// Classify an undecoded signal based on pulse timing statistics.
/// Returns a descriptive model name and protocol hint for display in the UI.
struct SignalClassification {
    const char* model;     ///< Human-readable device type guess
    const char* protocol;  ///< Technical protocol description
};

static SignalClassification classifyUnparsedSignal(pulse_data_t* pulses, bool is868) {
    const int n = (int)pulses->num_pulses;

    if (n < 3) {
        return {"Rauschimpuls", "< 3 Pulse"};
    }

    // --- Pulse timing statistics (µs values from ISR micros()) ---
    int minPulse = INT_MAX, maxPulse = 0;
    int minGap   = INT_MAX, maxGap   = 0;
    long sumPulse = 0, sumGap = 0;
    int pCount = 0, gCount = 0;

    int limit = (n < 128) ? n : 128;  // analyse up to 128 pulses
    for (int i = 0; i < limit; i++) {
        int p = pulses->pulse[i];
        if (p > 0 && p < 100000) {
            sumPulse += p;
            if (p < minPulse) minPulse = p;
            if (p > maxPulse) maxPulse = p;
            pCount++;
        }
        // skip last gap (reset gap is huge)
        if (i < limit - 1) {
            int g = pulses->gap[i];
            if (g > 0 && g < 100000) {
                sumGap += g;
                if (g < minGap) minGap = g;
                if (g > maxGap) maxGap = g;
                gCount++;
            }
        }
    }

    if (pCount < 3) {
        return {"Rauschimpuls", "zu wenig gültige Pulse"};
    }

    int avgPulse = (int)(sumPulse / pCount);
    int avgGap   = gCount > 0 ? (int)(sumGap / gCount) : 0;
    int pulseRange = maxPulse - minPulse;
    unsigned long duration = pulses->signalDuration;

    // ==================== 868 MHz Classification ====================
    if (is868) {
        // Wireless M-Bus Mode C/T: very fast FSK ~10µs pulses, 100kbps
        if (avgPulse <= 20 && n > 100) {
            return {"868: sehr kurze Pulse ≤20 µs", "Muster passt zu ~100 kbit/s (z. B. wM-Bus C/T?) – nicht decodiert"};
        }
        // Wireless M-Bus Mode S/T: ~30µs pulses, 32.768kbps
        if (avgPulse > 20 && avgPulse < 60 && n > 50) {
            return {"868: Pulse 20–60 µs", "Muster ~32 kbit/s – häufig auch Rauschen, nicht decodiert"};
        }
        // Honeywell Evohome / CM921: Manchester FSK ~38kbps, ~26µs
        if (avgPulse > 20 && avgPulse < 35 && n > 80 && pulseRange < 30) {
            return {"868: gleichmäßige Pulse 20–35 µs", "Muster ~38 kbit/s – nicht decodiert"};
        }
        // EnOcean ESP3: 125kbps ASK/FSK, very short pulses
        if (avgPulse < 15 && n > 30 && n < 300) {
            return {"868: sehr kurze Pulse <15 µs", "Muster >100 kbit/s – nicht decodiert"};
        }
        // Bresser weather sensors (FSK): ~26µs pulses at 868
        if (avgPulse > 20 && avgPulse < 35 && n > 150) {
            return {"868: langes Paket, Pulse 20–35 µs", "nicht decodiert"};
        }
        // FS20/FHT home automation: OOK @ 868, ~400-600µs
        if (avgPulse > 300 && avgPulse < 700 && n > 30 && n < 200) {
            return {"868: Pulse 300–700 µs", "OOK-artiges Muster – nicht decodiert"};
        }
        // Generic 868 data packet
        if (n > 30) {
            return {"868: unbekanntes Paket", ">30 Pulse – nicht decodiert"};
        }
        // Short 868 burst
        if (n >= 3 && n <= 30) {
            return {"868: kurzer Burst", "≤30 Pulse – vermutlich Störung"};
        }
        return {"868: Signal", "unbekannt"};
    }

    // ==================== 433 MHz Classification ====================

    // Somfy RTS: Manchester ~640µs half-period, ~56 data pulses + frame
    if (avgPulse > 500 && avgPulse < 850 && avgGap > 400 && avgGap < 900
        && n > 40 && n < 150 && pulseRange < 500) {
        return {"433: Manchester ~640 µs", "Muster wie Somfy RTS? – nicht decodiert"};
    }

    // Weather sensors OOK PWM: short 200-500µs, long 800-2000µs
    if (minPulse > 80 && minPulse < 600 && maxPulse > 500 && maxPulse < 2500
        && pulseRange > 250 && n > 30 && n < 500) {
        return {"433: PWM kurz/lang", "nicht decodiert"};
    }

    // Weather sensors OOK PPM: constant pulse ~500µs, varying gaps
    if (pulseRange < 150 && avgPulse > 200 && avgPulse < 700
        && (maxGap - minGap) > 300 && n > 30) {
        return {"433: PPM feste Pulsbreite", "nicht decodiert"};
    }

    // Door/window/alarm sensors: short repeated bursts
    if (n >= 10 && n <= 40 && duration < 50000 && avgPulse > 100 && avgPulse < 800) {
        return {"433: kurzer Burst", "nicht decodiert"};
    }

    // Remote controls: short OOK bursts with distinct timing
    if (n >= 5 && n <= 60 && avgPulse > 100 && avgPulse < 1000) {
        return {"433: Burst", "nicht decodiert"};
    }

    // TPMS (tire pressure): short fast bursts, often FSK
    if (avgPulse < 100 && n > 50 && n < 300) {
        return {"433: schnelle Pulse", "nicht decodiert"};
    }

    // Long data packets
    if (n > 200) {
        return {"433: langes Paket", "nicht decodiert"};
    }

    // Generic 433 signal
    return {"433: Signal", "unbekanntes Muster"};
}

/*----------------------------- rtl_433_ESP Internals -----------------------------*/

#ifndef rtl_433_Decoder_Stack
#  if defined(RTL_ANALYZER) || defined(RTL_ANALYZE)
#    define rtl_433_Decoder_Stack 60000
#  elif defined(RTL_VERBOSE) || defined(RTL_DEBUG)
#    define rtl_433_Decoder_Stack 30000
#  else
#    if OOK_MODULATION
#      define rtl_433_Decoder_Stack 11500
#    else
#      define rtl_433_Decoder_Stack 20000
#    endif
#  endif
#endif

#define rtl_433_Decoder_Priority 2
#define rtl_433_Decoder_Core     1

/*----------------------------- rtl_433_ESP Internals -----------------------------*/

int rtlVerbose = 0;

r_cfg_t g_cfg; // Global config object

TaskHandle_t rtl_433_DecoderHandle;
static QueueHandle_t rtl_433_Queue;      // 433 MHz pulse trains
static QueueHandle_t rtl_433_Queue868;   // LINKRA: 868 MHz lane, served first so 433 noise can't starve it
volatile uint32_t linkraQueueDrops433 = 0, linkraQueueDrops868 = 0;
// LINKRA: decoder timing per lane [0=433,1=868] and train size bucket
// [0: <64 pulses, 1: <256, 2: >=256]: count, total us, max us
volatile uint32_t linkraDecN[2][3], linkraDecUs[2][3], linkraDecMax[2][3];
volatile uint32_t linkraQueueHigh[2];
// LINKRA: per-decoder time profile (filled from r_api.c run_*_demods)
#define LINKRA_PROF_N 400
static uint32_t *linkraProfUs, *linkraProfN, *linkraProfMax;   // PSRAM
extern "C" uint32_t linkra_micros(void) { return (uint32_t)micros(); }
extern "C" void linkra_dec_prof(int protocol, uint32_t us) {
  if (protocol < 0 || protocol >= LINKRA_PROF_N) return;
  if (!linkraProfUs) {
    linkraProfUs = (uint32_t*)heap_caps_calloc(3 * LINKRA_PROF_N, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!linkraProfUs) return;
    linkraProfN = linkraProfUs + LINKRA_PROF_N; linkraProfMax = linkraProfN + LINKRA_PROF_N;
  }
  linkraProfUs[protocol] += us; linkraProfN[protocol]++;
  if (us > linkraProfMax[protocol]) linkraProfMax[protocol] = us;
}
String linkraDecoderProfile(int top) {
  r_cfg_t* cfg = &g_cfg;
  String out = "[";
  if (!linkraProfUs) return out + "]";
  static bool used[LINKRA_PROF_N];
  memset(used, 0, sizeof(used));
  for (int k = 0; k < top; k++) {
    int best = -1;
    for (int i = 0; i < LINKRA_PROF_N && i < cfg->num_r_devices; i++)
      if (!used[i] && linkraProfN[i] && (best < 0 || linkraProfUs[i] > linkraProfUs[best])) best = i;
    if (best < 0) break;
    used[best] = true;
    char b[200];
    snprintf(b, sizeof(b), "%s{\"i\":%d,\"name\":\"%s\",\"mod\":%d,\"n\":%lu,\"totalMs\":%lu,\"avgUs\":%lu,\"maxUs\":%lu}",
             k ? "," : "", best, cfg->devices[best].name ? cfg->devices[best].name : "?", (int)cfg->devices[best].modulation,
             (unsigned long)linkraProfN[best], (unsigned long)(linkraProfUs[best] / 1000),
             (unsigned long)(linkraProfUs[best] / linkraProfN[best]), (unsigned long)linkraProfMax[best]);
    out += b;
  }
  return out + "]";
}

// LINKRA DUO: Flag to indicate current signal source band (set by decoder task)
// Read by callback to route decoded signals to correct module handler
volatile bool currentSignalIs868 = false;
volatile uint32_t currentSignalProfileTag = 0; // LINKRA: receive profile of the train being decoded
volatile int currentSignalFreqEst = 0x7FFF;   // LINKRA: FREQEST of the train (0x7FFF = n/a)
volatile float currentSignalNoise = 0;
volatile int currentSignalSource = 0;          // LINKRA: 0 = GPIO-ISR capture, 1 = RMT capture         // LINKRA: noise floor when the train was captured

// ---- LINKRA: raw capture rings (PSRAM) ----
// Ring 0 keeps the latest undecoded trains. Ring 1 keeps only strong ones
// (>= 20 dB above the noise floor): those should have decoded, and the noise
// trains would otherwise overwrite them within seconds.
#define LINKRA_RAW_SLOTS 40
#define LINKRA_STRONG_SLOTS 8
#define LINKRA_STRONG_SNR 20
struct LinkraRawCap { uint32_t t; uint32_t tag; float freq; int rssi; int noise; uint32_t dur; uint16_t n; bool fsk; int* pg; };
struct LinkraRawRing { LinkraRawCap* slots; int size, head, count; };
static LinkraRawRing linkraRings[2] = {{nullptr, LINKRA_RAW_SLOTS, 0, 0}, {nullptr, LINKRA_STRONG_SLOTS, 0, 0}};
static portMUX_TYPE linkraRawMux = portMUX_INITIALIZER_UNLOCKED;

static void linkraRingStore(LinkraRawRing& r, const pulse_data_t* p, bool fsk, uint32_t tag, float freq) {
  if (!r.slots) {
    r.slots = (LinkraRawCap*)heap_caps_calloc(r.size, sizeof(LinkraRawCap), MALLOC_CAP_SPIRAM);
    if (!r.slots) return;
    for (int i = 0; i < r.size; i++)
      r.slots[i].pg = (int*)heap_caps_malloc(sizeof(int) * 2 * PD_MAX_PULSES, MALLOC_CAP_SPIRAM);
  }
  LinkraRawCap& s = r.slots[r.head];
  if (!s.pg) return;
  const int n = p->num_pulses > PD_MAX_PULSES ? PD_MAX_PULSES : p->num_pulses;
  portENTER_CRITICAL(&linkraRawMux);
  for (int i = 0; i < n; i++) { s.pg[2 * i] = p->pulse[i]; s.pg[2 * i + 1] = p->gap[i]; }
  s.n = n; s.t = millis(); s.tag = tag; s.freq = freq; s.rssi = p->signalRssi; s.noise = (int)p->noise_db;
  s.dur = p->signalDuration; s.fsk = fsk;
  r.head = (r.head + 1) % r.size;
  if (r.count < r.size) r.count++;
  portEXIT_CRITICAL(&linkraRawMux);
}

volatile uint32_t linkraStrongUndecoded[32] = {0};   // per receive profile index

static void linkraRawStore(const pulse_data_t* p, bool fsk, uint32_t tag, float freq) {
  linkraRingStore(linkraRings[0], p, fsk, tag, freq);
  if (p->noise_db != 0 && p->signalRssi - (int)p->noise_db >= LINKRA_STRONG_SNR) {
    linkraRingStore(linkraRings[1], p, fsk, tag, freq);
    if (tag >= 1000 && (tag - 1000) / 10 < 32) linkraStrongUndecoded[(tag - 1000) / 10]++;
  }
}

// strong = true dumps the ring of strong undecoded trains
String linkraRawCaptureDump(int maxTrains, int band, bool strong) {
  String out;
  LinkraRawRing& r = linkraRings[strong ? 1 : 0];
  if (!r.slots) return out;
  out.reserve(16384);
  int emitted = 0;
  for (int k = 0; k < r.count && emitted < maxTrains; k++) {
    int idx = (r.head - 1 - k + r.size) % r.size;
    LinkraRawCap s;
    portENTER_CRITICAL(&linkraRawMux);
    s = r.slots[idx];
    portEXIT_CRITICAL(&linkraRawMux);
    const bool is868 = s.freq > 500e6f;
    if (band == 433 && is868) continue;
    if (band == 868 && !is868) continue;
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             ";pulse data\n;version 1\n;timescale 1us\n;%s %u pulses\n;freq1 %.0f\n;centerfreq %.0f Hz\n"
             ";samplerate 1000000 Hz\n;rssi %d dB\n;linkra t=%lu tag=%lu dur=%lu noise=%d\n",
             s.fsk ? "fsk" : "ook", (unsigned)s.n, s.freq, s.freq, s.rssi,
             (unsigned long)s.t, (unsigned long)s.tag, (unsigned long)s.dur, s.noise);
    out += hdr;
    char line[32];
    for (int i = 0; i < s.n; i++) {
      snprintf(line, sizeof(line), "%d %d\n", s.pg[2 * i], s.pg[2 * i + 1]);
      out += line;
    }
    out += ";end\n";
    emitted++;
  }
  return out;
}

void rtlSetup() {
  r_cfg_t* cfg = &g_cfg;

#ifdef MEMORY_DEBUG
  logprintfLn(LOG_DEBUG, "sizeof(*cfg->demod) %d", sizeof(*cfg->demod));
#endif

  if (!cfg->demod) {
    r_init_cfg(cfg);
    add_log_output(cfg, NULL);
#ifdef MEMORY_DEBUG
    logprintfLn(LOG_DEBUG, "sizeof(cfg) %d, heap %d", sizeof(cfg),
                ESP.getFreeHeap());
#endif
    cfg->conversion_mode = CONVERT_SI; // Default all output to Celsius
    // LINKRA DUO: Load ALL decoders (OOK + FSK) for dual-modulation support
    cfg->num_r_devices = NUMOF_OOK_DEVICES + NUMOF_FSK_DEVICES;
    // LINKRA: decoder table in PSRAM (internal RAM is needed by WiFi/tasks)
    cfg->devices = reinterpret_cast<r_device*>(heap_caps_calloc(cfg->num_r_devices, sizeof(r_device), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!cfg->devices) cfg->devices = reinterpret_cast<r_device*>(calloc(cfg->num_r_devices, sizeof(r_device)));
    if (!cfg->devices)
      FATAL_CALLOC("cfg->devices");

#ifdef MEMORY_DEBUG
    logprintfLn(LOG_DEBUG, "sizeof(cfg) %d, heap %d", sizeof(cfg),
                ESP.getFreeHeap());
#endif

#ifndef MY_DEVICES
    // LINKRA DUO: Load ALL decoders (OOK + FSK) unconditionally.
    // Generated from upstream NorthernMan54/rtl_433_ESP f0a8156 (2026-09-25).
    {
      // OOK decoders 0..222, FSK decoders 223..376 (incl. Linkra HomeMatic BidCoS)
      memcpy(&cfg->devices[0], &abmt, sizeof(r_device));
      memcpy(&cfg->devices[1], &acurite_rain_896, sizeof(r_device));
      memcpy(&cfg->devices[2], &acurite_th, sizeof(r_device));
      memcpy(&cfg->devices[3], &acurite_txr, sizeof(r_device));
      memcpy(&cfg->devices[4], &acurite_986, sizeof(r_device));
      memcpy(&cfg->devices[5], &acurite_606, sizeof(r_device));
      memcpy(&cfg->devices[6], &acurite_00275rm, sizeof(r_device));
      memcpy(&cfg->devices[7], &acurite_590tx, sizeof(r_device));
      memcpy(&cfg->devices[8], &acurite_01185m, sizeof(r_device));
      memcpy(&cfg->devices[9], &akhan_100F14, sizeof(r_device));
      memcpy(&cfg->devices[10], &alectov1, sizeof(r_device));
      memcpy(&cfg->devices[11], &ambient_weather, sizeof(r_device));
      memcpy(&cfg->devices[12], &ambientweather_tx8300, sizeof(r_device));
      memcpy(&cfg->devices[13], &astrostart_2000, sizeof(r_device));
      memcpy(&cfg->devices[14], &atech_ws308, sizeof(r_device));
      memcpy(&cfg->devices[15], &audiovox_pro_oe3b, sizeof(r_device));
      memcpy(&cfg->devices[16], &auriol_4ld5661, sizeof(r_device));
      memcpy(&cfg->devices[17], &auriol_aft77b2, sizeof(r_device));
      memcpy(&cfg->devices[18], &auriol_afw2a1, sizeof(r_device));
      memcpy(&cfg->devices[19], &auriol_ahfl, sizeof(r_device));
      memcpy(&cfg->devices[20], &auriol_hg02832, sizeof(r_device));
      memcpy(&cfg->devices[21], &auriol_hg04641a, sizeof(r_device));
      memcpy(&cfg->devices[22], &baldr_hcs528arf, sizeof(r_device));
      memcpy(&cfg->devices[23], &baldr_rain, sizeof(r_device));
      memcpy(&cfg->devices[24], &baldr_therm, sizeof(r_device));
      memcpy(&cfg->devices[25], &blyss, sizeof(r_device));
      memcpy(&cfg->devices[26], &bm5, sizeof(r_device));
      memcpy(&cfg->devices[27], &brennenstuhl_rcs_2044, sizeof(r_device));
      memcpy(&cfg->devices[28], &bresser_3ch, sizeof(r_device));
      memcpy(&cfg->devices[29], &bresser_st1005h, sizeof(r_device));
      memcpy(&cfg->devices[30], &bt_rain, sizeof(r_device));
      memcpy(&cfg->devices[31], &burnhardbbq, sizeof(r_device));
      memcpy(&cfg->devices[32], &calibeur_RF104, sizeof(r_device));
      memcpy(&cfg->devices[33], &cardin, sizeof(r_device));
      memcpy(&cfg->devices[34], &celsia_czc1, sizeof(r_device));
      memcpy(&cfg->devices[35], &chrysler_car_remote, sizeof(r_device));
      memcpy(&cfg->devices[36], &chuango, sizeof(r_device));
      memcpy(&cfg->devices[37], &cmr113, sizeof(r_device));
      memcpy(&cfg->devices[38], &code_alarm_frdpc2000_car_remote, sizeof(r_device));
      memcpy(&cfg->devices[39], &companion_wtr001, sizeof(r_device));
      memcpy(&cfg->devices[40], &compustar_1wg3r, sizeof(r_device));
      memcpy(&cfg->devices[41], &cotech_36_7900, sizeof(r_device));
      memcpy(&cfg->devices[42], &cotech_36_7959, sizeof(r_device));
      memcpy(&cfg->devices[43], &cotech_ft0203, sizeof(r_device));
      memcpy(&cfg->devices[44], &dickert_pwm, sizeof(r_device));
      memcpy(&cfg->devices[45], &digitech_xc0324, sizeof(r_device));
      memcpy(&cfg->devices[46], &dish_remote_6_3, sizeof(r_device));
      memcpy(&cfg->devices[47], &dsc_security, sizeof(r_device));
      memcpy(&cfg->devices[48], &dsc_security_ws4945, sizeof(r_device));
      memcpy(&cfg->devices[49], &ecowitt, sizeof(r_device));
      memcpy(&cfg->devices[50], &eurochron_efth800, sizeof(r_device));
      memcpy(&cfg->devices[51], &elro_db286a, sizeof(r_device));
      memcpy(&cfg->devices[52], &elv_em1000, sizeof(r_device));
      memcpy(&cfg->devices[53], &elv_ws2000, sizeof(r_device));
      memcpy(&cfg->devices[54], &emos_e6016, sizeof(r_device));
      memcpy(&cfg->devices[55], &emos_e6016_rain, sizeof(r_device));
      memcpy(&cfg->devices[56], &enocean_erp1, sizeof(r_device));
      memcpy(&cfg->devices[57], &ert_idm, sizeof(r_device));
      memcpy(&cfg->devices[58], &ert_netidm, sizeof(r_device));
      memcpy(&cfg->devices[59], &ert_scm, sizeof(r_device));
      memcpy(&cfg->devices[60], &esa_energy, sizeof(r_device));
      memcpy(&cfg->devices[61], &esperanza_ews, sizeof(r_device));
      memcpy(&cfg->devices[62], &esun_en2053, sizeof(r_device));
      memcpy(&cfg->devices[63], &eurochron, sizeof(r_device));
      memcpy(&cfg->devices[64], &fineoffset_WH2, sizeof(r_device));
      memcpy(&cfg->devices[65], &fineoffset_wh5rb, sizeof(r_device));
      memcpy(&cfg->devices[66], &fineoffset_WH0530, sizeof(r_device));
      memcpy(&cfg->devices[67], &fineoffset_wh1050, sizeof(r_device));
      memcpy(&cfg->devices[68], &fineoffset_wh1080, sizeof(r_device));
      memcpy(&cfg->devices[69], &florabest, sizeof(r_device));
      memcpy(&cfg->devices[70], &fordremote, sizeof(r_device));
      memcpy(&cfg->devices[71], &fs20, sizeof(r_device));
      memcpy(&cfg->devices[72], &ft004b, sizeof(r_device));
      memcpy(&cfg->devices[73], &funkbus_remote, sizeof(r_device));
      memcpy(&cfg->devices[74], &gasmate_ba1008, sizeof(r_device));
      memcpy(&cfg->devices[75], &geevon_tx16, sizeof(r_device));
      memcpy(&cfg->devices[76], &geevon_tx19, sizeof(r_device));
      memcpy(&cfg->devices[77], &generic_motion, sizeof(r_device));
      memcpy(&cfg->devices[78], &generic_remote, sizeof(r_device));
      memcpy(&cfg->devices[79], &generic_temperature_sensor, sizeof(r_device));
      memcpy(&cfg->devices[80], &gm_car_remote, sizeof(r_device));
      memcpy(&cfg->devices[81], &govee, sizeof(r_device));
      memcpy(&cfg->devices[82], &govee_h5054, sizeof(r_device));
      memcpy(&cfg->devices[83], &grill_thermometer, sizeof(r_device));
      memcpy(&cfg->devices[84], &gt_tmbbq05, sizeof(r_device));
      memcpy(&cfg->devices[85], &gt_wt_02, sizeof(r_device));
      memcpy(&cfg->devices[86], &gt_wt_03, sizeof(r_device));
      memcpy(&cfg->devices[87], &hcs200, sizeof(r_device));
      memcpy(&cfg->devices[88], &hcs361_txwak_0_bsel_0, sizeof(r_device));
      memcpy(&cfg->devices[89], &hcs361_txwak_0_bsel_1, sizeof(r_device));
      memcpy(&cfg->devices[90], &hcs361_txwak_1_bsel_0, sizeof(r_device));
      memcpy(&cfg->devices[91], &hcs361_txwak_1_bsel_1, sizeof(r_device));
      memcpy(&cfg->devices[92], &hcs361_vpwm_1_bsel_0, sizeof(r_device));
      memcpy(&cfg->devices[93], &hcs361_vpwm_1_bsel_1, sizeof(r_device));
      memcpy(&cfg->devices[94], &hcs362_pwm, sizeof(r_device));
      memcpy(&cfg->devices[95], &hcs362_mc, sizeof(r_device));
      memcpy(&cfg->devices[96], &hideki_ts04, sizeof(r_device));
      memcpy(&cfg->devices[97], &holman_ws5029pwm_ook, sizeof(r_device));
      memcpy(&cfg->devices[98], &homelead_hg9901, sizeof(r_device));
      memcpy(&cfg->devices[99], &honeywell, sizeof(r_device));
      memcpy(&cfg->devices[100], &honeywell_wdb, sizeof(r_device));
      memcpy(&cfg->devices[101], &ht680, sizeof(r_device));
      memcpy(&cfg->devices[102], &ibis_beacon, sizeof(r_device));
      memcpy(&cfg->devices[103], &infactory, sizeof(r_device));
      memcpy(&cfg->devices[104], &kw9015b, sizeof(r_device));
      memcpy(&cfg->devices[105], &interlogix, sizeof(r_device));
      memcpy(&cfg->devices[106], &intertechno, sizeof(r_device));
      memcpy(&cfg->devices[107], &jasco, sizeof(r_device));
      memcpy(&cfg->devices[108], &kedsum, sizeof(r_device));
      memcpy(&cfg->devices[109], &kerui, sizeof(r_device));
      memcpy(&cfg->devices[110], &kidde_smoke, sizeof(r_device));
      memcpy(&cfg->devices[111], &klimalogg, sizeof(r_device));
      memcpy(&cfg->devices[112], &lacrossetx, sizeof(r_device));
      memcpy(&cfg->devices[113], &lacrosse_tx141x, sizeof(r_device));
      memcpy(&cfg->devices[114], &lacrosse_tx22uit_ook, sizeof(r_device));
      memcpy(&cfg->devices[115], &lacrosse_ws7000, sizeof(r_device));
      memcpy(&cfg->devices[116], &lacrossews, sizeof(r_device));
      memcpy(&cfg->devices[117], &lightwave_rf, sizeof(r_device));
      memcpy(&cfg->devices[118], &markisol, sizeof(r_device));
      memcpy(&cfg->devices[119], &martec_mplcd, sizeof(r_device));
      memcpy(&cfg->devices[120], &maverick_et73, sizeof(r_device));
      memcpy(&cfg->devices[121], &maverick_et73x, sizeof(r_device));
      memcpy(&cfg->devices[122], &mebus433, sizeof(r_device));
      memcpy(&cfg->devices[123], &megacode, sizeof(r_device));
      memcpy(&cfg->devices[124], &six_sc_two_car_remote, sizeof(r_device));
      memcpy(&cfg->devices[125], &missil_ml0757, sizeof(r_device));
      memcpy(&cfg->devices[126], &neptune_r900, sizeof(r_device));
      memcpy(&cfg->devices[127], &new_template, sizeof(r_device));
      memcpy(&cfg->devices[128], &newkaku, sizeof(r_device));
      memcpy(&cfg->devices[129], &nexa, sizeof(r_device));
      memcpy(&cfg->devices[130], &nexus, sizeof(r_device));
      memcpy(&cfg->devices[131], &nexus_sauna, sizeof(r_device));
      memcpy(&cfg->devices[132], &nice_flor_s, sizeof(r_device));
      memcpy(&cfg->devices[133], &norgo, sizeof(r_device));
      memcpy(&cfg->devices[134], &oil_standard_ask, sizeof(r_device));
      memcpy(&cfg->devices[135], &omni, sizeof(r_device));
      memcpy(&cfg->devices[136], &opus_xt300, sizeof(r_device));
      memcpy(&cfg->devices[137], &oregon_scientific, sizeof(r_device));
      memcpy(&cfg->devices[138], &oregon_scientific_sl109h, sizeof(r_device));
      memcpy(&cfg->devices[139], &oregon_scientific_v1, sizeof(r_device));
      memcpy(&cfg->devices[140], &oria_wa150km, sizeof(r_device));
      memcpy(&cfg->devices[141], &philips_aj3650, sizeof(r_device));
      memcpy(&cfg->devices[142], &philips_aj7010, sizeof(r_device));
      memcpy(&cfg->devices[143], &proflame2, sizeof(r_device));
      memcpy(&cfg->devices[144], &prologue, sizeof(r_device));
      memcpy(&cfg->devices[145], &proove, sizeof(r_device));
      memcpy(&cfg->devices[146], &quhwa, sizeof(r_device));
      memcpy(&cfg->devices[147], &radiohead_ask, sizeof(r_device));
      memcpy(&cfg->devices[148], &sensible_living, sizeof(r_device));
      memcpy(&cfg->devices[149], &rainpoint, sizeof(r_device));
      memcpy(&cfg->devices[150], &rainpoint_hcs012arf, sizeof(r_device));
      memcpy(&cfg->devices[151], &regency_fan, sizeof(r_device));
      memcpy(&cfg->devices[152], &revolt_nc5462, sizeof(r_device));
      memcpy(&cfg->devices[153], &revolt_zx7717, sizeof(r_device));
      memcpy(&cfg->devices[154], &rftech, sizeof(r_device));
      memcpy(&cfg->devices[155], &rfxmeter, sizeof(r_device));
      memcpy(&cfg->devices[156], &risco_agility, sizeof(r_device));
      memcpy(&cfg->devices[157], &rosstech_dcu706, sizeof(r_device));
      memcpy(&cfg->devices[158], &rubicson, sizeof(r_device));
      memcpy(&cfg->devices[159], &rubicson_48659, sizeof(r_device));
      memcpy(&cfg->devices[160], &rubicson_pool_48942, sizeof(r_device));
      memcpy(&cfg->devices[161], &s3318p, sizeof(r_device));
      memcpy(&cfg->devices[162], &sainlogic_sa8, sizeof(r_device));
      memcpy(&cfg->devices[163], &schou_72543_rain, sizeof(r_device));
      memcpy(&cfg->devices[164], &schraeder, sizeof(r_device));
      memcpy(&cfg->devices[165], &schrader_EG53MA4, sizeof(r_device));
      memcpy(&cfg->devices[166], &schrader_SMD3MA4, sizeof(r_device));
      memcpy(&cfg->devices[167], &schrader_NIS315G3, sizeof(r_device));
      memcpy(&cfg->devices[168], &schrader_MRXBC5A4, sizeof(r_device));
      memcpy(&cfg->devices[169], &scmplus, sizeof(r_device));
      memcpy(&cfg->devices[170], &secplus_v1, sizeof(r_device));
      memcpy(&cfg->devices[171], &shenzhen_wale_wl_th6r, sizeof(r_device));
      memcpy(&cfg->devices[172], &siemens_5wy72xx_car_remote, sizeof(r_device));
      memcpy(&cfg->devices[173], &silvercrest, sizeof(r_device));
      memcpy(&cfg->devices[174], &ss_sensor, sizeof(r_device));
      memcpy(&cfg->devices[175], &skylink_motion, sizeof(r_device));
      memcpy(&cfg->devices[176], &smoke_gs558, sizeof(r_device));
      memcpy(&cfg->devices[177], &solight_te44, sizeof(r_device));
      memcpy(&cfg->devices[178], &somfy_rts, sizeof(r_device));
      memcpy(&cfg->devices[179], &springfield, sizeof(r_device));
      memcpy(&cfg->devices[180], &telldus_ft0385r, sizeof(r_device));
      memcpy(&cfg->devices[181], &tfa_30_3221, sizeof(r_device));
      memcpy(&cfg->devices[182], &tfa_drop_303233, sizeof(r_device));
      memcpy(&cfg->devices[183], &tfa_pool_thermometer, sizeof(r_device));
      memcpy(&cfg->devices[184], &tfa_twin_plus_303049, sizeof(r_device));
      memcpy(&cfg->devices[185], &thermopro_tp11, sizeof(r_device));
      memcpy(&cfg->devices[186], &thermopro_tp12, sizeof(r_device));
      memcpy(&cfg->devices[187], &thermopro_tx2, sizeof(r_device));
      memcpy(&cfg->devices[188], &thermopro_tx2c, sizeof(r_device));
      memcpy(&cfg->devices[189], &thermor, sizeof(r_device));
      memcpy(&cfg->devices[190], &thermor_a6n_132tx, sizeof(r_device));
      memcpy(&cfg->devices[191], &tpms_eezrv, sizeof(r_device));
      memcpy(&cfg->devices[192], &tmps_gear_hive, sizeof(r_device));
      memcpy(&cfg->devices[193], &tpms_gm, sizeof(r_device));
      memcpy(&cfg->devices[194], &tpms_imars_t240, sizeof(r_device));
      memcpy(&cfg->devices[195], &tpms_jansite_ty468, sizeof(r_device));
      memcpy(&cfg->devices[196], &tpms_schrader_motorcycle, sizeof(r_device));
      memcpy(&cfg->devices[197], &tpms_smartire, sizeof(r_device));
      memcpy(&cfg->devices[198], &tpms_trw_ook, sizeof(r_device));
      memcpy(&cfg->devices[199], &tpms_tyreguard400, sizeof(r_device));
      memcpy(&cfg->devices[200], &tr_502msv, sizeof(r_device));
      memcpy(&cfg->devices[201], &ts_ft002, sizeof(r_device));
      memcpy(&cfg->devices[202], &ttx201, sizeof(r_device));
      memcpy(&cfg->devices[203], &twogig_key2e, sizeof(r_device));
      memcpy(&cfg->devices[204], &universalfanctrl, sizeof(r_device));
      memcpy(&cfg->devices[205], &vaillant_vrt340f, sizeof(r_device));
      memcpy(&cfg->devices[206], &vauno_en8822c, sizeof(r_device));
      memcpy(&cfg->devices[207], &visonic_powercode, sizeof(r_device));
      memcpy(&cfg->devices[208], &vivint, sizeof(r_device));
      memcpy(&cfg->devices[209], &wallarge_cltx001, sizeof(r_device));
      memcpy(&cfg->devices[210], &watchman_plus, sizeof(r_device));
      memcpy(&cfg->devices[211], &watts_thermostat, sizeof(r_device));
      memcpy(&cfg->devices[212], &watts_wfht_rf, sizeof(r_device));
      memcpy(&cfg->devices[213], &waveman, sizeof(r_device));
      memcpy(&cfg->devices[214], &wec2103, sizeof(r_device));
      memcpy(&cfg->devices[215], &wg_pb12v1, sizeof(r_device));
      memcpy(&cfg->devices[216], &ws2032, sizeof(r_device));
      memcpy(&cfg->devices[217], &wssensor, sizeof(r_device));
      memcpy(&cfg->devices[218], &wt1024, sizeof(r_device));
      memcpy(&cfg->devices[219], &wt450, sizeof(r_device));
      memcpy(&cfg->devices[220], &X10_RF, sizeof(r_device));
      memcpy(&cfg->devices[221], &x10_sec, sizeof(r_device));
      memcpy(&cfg->devices[222], &yale_hsa, sizeof(r_device));
      memcpy(&cfg->devices[223], &alps_fwb1u545_car_remote, sizeof(r_device));
      memcpy(&cfg->devices[224], &ambientweather_wh31e, sizeof(r_device));
      memcpy(&cfg->devices[225], &ant_antplus, sizeof(r_device));
      memcpy(&cfg->devices[226], &apator_metra_eitn30, sizeof(r_device));
      memcpy(&cfg->devices[227], &apator_metra_erm30, sizeof(r_device));
      memcpy(&cfg->devices[228], &arad_ms_meter, sizeof(r_device));
      memcpy(&cfg->devices[229], &archos_tbh, sizeof(r_device));
      memcpy(&cfg->devices[230], &arexx_ml, sizeof(r_device));
      memcpy(&cfg->devices[231], &orion_endpoint, sizeof(r_device));
      memcpy(&cfg->devices[232], &orion_endpoint_2020, sizeof(r_device));
      memcpy(&cfg->devices[233], &badger_orion, sizeof(r_device));
      memcpy(&cfg->devices[234], &bresser_5in1, sizeof(r_device));
      memcpy(&cfg->devices[235], &bresser_6in1, sizeof(r_device));
      memcpy(&cfg->devices[236], &bresser_7in1, sizeof(r_device));
      memcpy(&cfg->devices[237], &bresser_garden, sizeof(r_device));
      memcpy(&cfg->devices[238], &bresser_leakage, sizeof(r_device));
      memcpy(&cfg->devices[239], &bresser_lightning, sizeof(r_device));
      memcpy(&cfg->devices[240], &cavius, sizeof(r_device));
      memcpy(&cfg->devices[241], &cavius_door, sizeof(r_device));
      memcpy(&cfg->devices[242], &ced7000, sizeof(r_device));
      memcpy(&cfg->devices[243], &chamberlain_cwpirc, sizeof(r_device));
      memcpy(&cfg->devices[244], &continental_car_remote, sizeof(r_device));
      memcpy(&cfg->devices[245], &honda_keyfob, sizeof(r_device));
      memcpy(&cfg->devices[246], &ctt_life_power_hybrid, sizeof(r_device));
      memcpy(&cfg->devices[247], &current_cost, sizeof(r_device));
      memcpy(&cfg->devices[248], &danfoss_CFR, sizeof(r_device));
      memcpy(&cfg->devices[249], &deltadore_x3d, sizeof(r_device));
      memcpy(&cfg->devices[250], &directv, sizeof(r_device));
      memcpy(&cfg->devices[251], &eberle_instat868r1, sizeof(r_device));
      memcpy(&cfg->devices[252], &ec3k, sizeof(r_device));
      memcpy(&cfg->devices[253], &ecodhome, sizeof(r_device));
      memcpy(&cfg->devices[254], &ecoeye, sizeof(r_device));
      memcpy(&cfg->devices[255], &efergy_e2_classic, sizeof(r_device));
      memcpy(&cfg->devices[256], &efergy_optical, sizeof(r_device));
      memcpy(&cfg->devices[257], &elero, sizeof(r_device));
      memcpy(&cfg->devices[258], &elsner_solexa, sizeof(r_device));
      memcpy(&cfg->devices[259], &elster_power_meter, sizeof(r_device));
      memcpy(&cfg->devices[260], &elster_power_meter2, sizeof(r_device));
      memcpy(&cfg->devices[261], &emax, sizeof(r_device));
      memcpy(&cfg->devices[262], &emontx, sizeof(r_device));
      memcpy(&cfg->devices[263], &en2058, sizeof(r_device));
      memcpy(&cfg->devices[264], &esic_emt7110, sizeof(r_device));
      memcpy(&cfg->devices[265], &fineoffset_WH25, sizeof(r_device));
      memcpy(&cfg->devices[266], &fineoffset_WH51, sizeof(r_device));
      memcpy(&cfg->devices[267], &tfa_303151, sizeof(r_device));
      memcpy(&cfg->devices[268], &fineoffset_wh1080_fsk, sizeof(r_device));
      memcpy(&cfg->devices[269], &fineoffset_wh31l, sizeof(r_device));
      memcpy(&cfg->devices[270], &fineoffset_wh43, sizeof(r_device));
      memcpy(&cfg->devices[271], &fineoffset_wh45, sizeof(r_device));
      memcpy(&cfg->devices[272], &fineoffset_wh46, sizeof(r_device));
      memcpy(&cfg->devices[273], &fineoffset_wh52, sizeof(r_device));
      memcpy(&cfg->devices[274], &fineoffset_wh55, sizeof(r_device));
      memcpy(&cfg->devices[275], &fineoffset_wn34, sizeof(r_device));
      memcpy(&cfg->devices[276], &fineoffset_ws80, sizeof(r_device));
      memcpy(&cfg->devices[277], &fineoffset_ws85, sizeof(r_device));
      memcpy(&cfg->devices[278], &fineoffset_ws90, sizeof(r_device));
      memcpy(&cfg->devices[279], &flowis, sizeof(r_device));
      memcpy(&cfg->devices[280], &fsl_scoreboard, sizeof(r_device));
      memcpy(&cfg->devices[281], &ge_coloreffects, sizeof(r_device));
      memcpy(&cfg->devices[282], &geo_minim, sizeof(r_device));
      memcpy(&cfg->devices[283], &govee_h5059, sizeof(r_device));
      memcpy(&cfg->devices[284], &govee_h5112, sizeof(r_device));
      memcpy(&cfg->devices[285], &govee_h5310, sizeof(r_device));
      memcpy(&cfg->devices[286], &gridstream96, sizeof(r_device));
      memcpy(&cfg->devices[287], &gridstream192, sizeof(r_device));
      memcpy(&cfg->devices[288], &gridstream384, sizeof(r_device));
      memcpy(&cfg->devices[289], &hanwell_ml4000, sizeof(r_device));
      memcpy(&cfg->devices[290], &hcs200_fsk, sizeof(r_device));
      memcpy(&cfg->devices[291], &holman_ws5029pcm, sizeof(r_device));
      memcpy(&cfg->devices[292], &holman_ws5029pwm, sizeof(r_device));
      memcpy(&cfg->devices[293], &hondaremote, sizeof(r_device));
      memcpy(&cfg->devices[294], &honeywell_cm921, sizeof(r_device));
      memcpy(&cfg->devices[295], &honeywell_wdb_fsk, sizeof(r_device));
      memcpy(&cfg->devices[296], &ikea_sparsnas, sizeof(r_device));
      memcpy(&cfg->devices[297], &inkbird_ith20r, sizeof(r_device));
      memcpy(&cfg->devices[298], &insteon, sizeof(r_device));
      memcpy(&cfg->devices[299], &lacrosse_breezepro, sizeof(r_device));
      memcpy(&cfg->devices[300], &lacrosse_r1, sizeof(r_device));
      memcpy(&cfg->devices[301], &lacrosse_th3, sizeof(r_device));
      memcpy(&cfg->devices[302], &lacrosse_tx22uit, sizeof(r_device));
      memcpy(&cfg->devices[303], &lacrosse_tx31u, sizeof(r_device));
      memcpy(&cfg->devices[304], &lacrosse_tx34, sizeof(r_device));
      memcpy(&cfg->devices[305], &lacrosse_tx29, sizeof(r_device));
      memcpy(&cfg->devices[306], &lacrosse_tx35, sizeof(r_device));
      memcpy(&cfg->devices[307], &lacrosse_wr1, sizeof(r_device));
      memcpy(&cfg->devices[308], &lacrosse_ws6868_tx232th, sizeof(r_device));
      memcpy(&cfg->devices[309], &lacrosse_ws6868_tx231rw, sizeof(r_device));
      memcpy(&cfg->devices[310], &m_bus_mode_c_t, sizeof(r_device));
      memcpy(&cfg->devices[311], &m_bus_mode_c_t_downlink, sizeof(r_device));
      memcpy(&cfg->devices[312], &m_bus_mode_s, sizeof(r_device));
      memcpy(&cfg->devices[313], &m_bus_mode_r, sizeof(r_device));
      memcpy(&cfg->devices[314], &m_bus_mode_f, sizeof(r_device));
      memcpy(&cfg->devices[315], &radian, sizeof(r_device));
      memcpy(&cfg->devices[316], &marlec_solar, sizeof(r_device));
      memcpy(&cfg->devices[317], &maverick_xr30, sizeof(r_device));
      memcpy(&cfg->devices[318], &maverick_xr50, sizeof(r_device));
      memcpy(&cfg->devices[319], &mcpower_kinetic, sizeof(r_device));
      memcpy(&cfg->devices[320], &mueller_hotrod, sizeof(r_device));
      memcpy(&cfg->devices[321], &netatmo_thw, sizeof(r_device));
      memcpy(&cfg->devices[322], &nidec_car_remote, sizeof(r_device));
      memcpy(&cfg->devices[323], &oil_smart, sizeof(r_device));
      memcpy(&cfg->devices[324], &oil_standard, sizeof(r_device));
      memcpy(&cfg->devices[325], &oil_watchman, sizeof(r_device));
      memcpy(&cfg->devices[326], &oil_watchman_advanced, sizeof(r_device));
      memcpy(&cfg->devices[327], &opel_mokka, sizeof(r_device));
      memcpy(&cfg->devices[328], &oregon_scientific_wmr500, sizeof(r_device));
      memcpy(&cfg->devices[329], &quinetic, sizeof(r_device));
      memcpy(&cfg->devices[330], &rfm69_lowpowerlab_moteino, sizeof(r_device));
      memcpy(&cfg->devices[331], &rojaflex, sizeof(r_device));
      memcpy(&cfg->devices[332], &sharp_spc775, sizeof(r_device));
      memcpy(&cfg->devices[333], &silver_spring_mesh, sizeof(r_device));
      memcpy(&cfg->devices[334], &simplisafe_gen3, sizeof(r_device));
      memcpy(&cfg->devices[335], &somfy_iohc, sizeof(r_device));
      memcpy(&cfg->devices[336], &srsmith_pool_srs_2c_tx, sizeof(r_device));
      memcpy(&cfg->devices[337], &steelmate, sizeof(r_device));
      memcpy(&cfg->devices[338], &tfa_14_1504_v2, sizeof(r_device));
      memcpy(&cfg->devices[339], &tfa_303196, sizeof(r_device));
      memcpy(&cfg->devices[340], &tfa_30_390x, sizeof(r_device));
      memcpy(&cfg->devices[341], &tfa_marbella, sizeof(r_device));
      memcpy(&cfg->devices[342], &thermopro_tp211b, sizeof(r_device));
      memcpy(&cfg->devices[343], &thermopro_tp28b, sizeof(r_device));
      memcpy(&cfg->devices[344], &thermopro_tp828b, sizeof(r_device));
      memcpy(&cfg->devices[345], &thermopro_tp829b, sizeof(r_device));
      memcpy(&cfg->devices[346], &thermopro_tp86xb, sizeof(r_device));
      memcpy(&cfg->devices[347], &thermopro_tx7b, sizeof(r_device));
      memcpy(&cfg->devices[348], &tpms_abarth124, sizeof(r_device));
      memcpy(&cfg->devices[349], &tpms_airpuxem, sizeof(r_device));
      memcpy(&cfg->devices[350], &tpms_ave, sizeof(r_device));
      memcpy(&cfg->devices[351], &tpms_bmw, sizeof(r_device));
      memcpy(&cfg->devices[352], &tpms_bmwg3, sizeof(r_device));
      memcpy(&cfg->devices[353], &tpms_citroen, sizeof(r_device));
      memcpy(&cfg->devices[354], &tpms_elantra2012, sizeof(r_device));
      memcpy(&cfg->devices[355], &tpms_ford, sizeof(r_device));
      memcpy(&cfg->devices[356], &tpms_honda, sizeof(r_device));
      memcpy(&cfg->devices[357], &tpms_hyundai_vdo, sizeof(r_device));
      memcpy(&cfg->devices[358], &tpms_jansite, sizeof(r_device));
      memcpy(&cfg->devices[359], &tpms_jansite_solar, sizeof(r_device));
      memcpy(&cfg->devices[360], &tpms_jansite_ty588, sizeof(r_device));
      memcpy(&cfg->devices[361], &tpms_jeep, sizeof(r_device));
      memcpy(&cfg->devices[362], &tpms_kia, sizeof(r_device));
      memcpy(&cfg->devices[363], &tpms_mercedes_benz, sizeof(r_device));
      memcpy(&cfg->devices[364], &tpms_nissan, sizeof(r_device));
      memcpy(&cfg->devices[365], &tpms_pmv107j, sizeof(r_device));
      memcpy(&cfg->devices[366], &tpms_porsche, sizeof(r_device));
      memcpy(&cfg->devices[367], &tpms_renault, sizeof(r_device));
      memcpy(&cfg->devices[368], &tpms_renault_0435r, sizeof(r_device));
      memcpy(&cfg->devices[369], &tpms_sefis_m3, sizeof(r_device));
      memcpy(&cfg->devices[370], &tpms_toyota, sizeof(r_device));
      memcpy(&cfg->devices[371], &tpms_truck, sizeof(r_device));
      memcpy(&cfg->devices[372], &tpms_trw_fsk, sizeof(r_device));
      memcpy(&cfg->devices[373], &typhur_sync_gold, sizeof(r_device));
      memcpy(&cfg->devices[374], &vevor_7in1, sizeof(r_device));
      memcpy(&cfg->devices[375], &watts_vision, sizeof(r_device));
      memcpy(&cfg->devices[376], &homematic_bidcos, sizeof(r_device));
    }
    // end of fragment (LINKRA DUO: 377 decoders)

#else
    memcpy(&cfg->devices[0], &lacrosse_tx141x, sizeof(r_device));
#endif

#ifdef RTL_FLEX
    // This option is non-functional. The flex decoder is too resource intensive
    // for an ESP32, and needs the ESP32 stack set to 32768 in order for the
    // flex_callback to execute Tested with
    // -DRTL_FLEX="n=Sonoff-PIR3-RF,m=OOK_PWM,s=300,l=860,r=7492,g=868,t=50,y=0,bits>=24,repeats>=5,invert,get=@0:{20}:id,get=@20:{4}:motion:[0:true
    // ],unique"

    r_device* flex_device;
    flex_device = flex_create_device(RTL_FLEX);
    memcpy(&cfg->devices[101], &flex_device, sizeof(r_device));
    register_protocol(cfg, flex_device, NULL);
    alogprintfLn(LOG_INFO, "Flex Decoder enabled: %s", RTL_FLEX);
#endif

// logprintfLn(LOG_INFO, "Location of r_devices: %p", (void *)&r_devices);
// logprintfLn(LOG_INFO, "Location of cfg: %p", (void *)&cfg);
// logprintfLn(LOG_INFO, "cfg size %d", sizeof(r_cfg_t));
// logprintfLn(LOG_INFO, "Location of cfg->devices: %p", (void *)&cfg->devices);
#ifdef MEMORY_DEBUG
    logprintfLn(LOG_INFO, "size of bitbuffer: %d", sizeof(bitbuffer_t));
    logprintfLn(LOG_INFO, "size of pulse_data: %d", sizeof(pulse_data_t));
#endif

#ifdef DEMOD_DEBUG
    logprintfLn(LOG_INFO, "# of device(s) configured %d", cfg->num_r_devices);
    logprintfLn(LOG_INFO, "ssizeof(r_device): %d", sizeof(r_device));
    logprintfLn(LOG_INFO, "cfg->devices size: %d",
                sizeof(r_device) * cfg->num_r_devices);
#endif
#ifdef RTL_DEBUG
    cfg->verbosity = RTL_DEBUG + 5; // 0=normal, 1=verbose, 2=verbose decoders,
    // 3=debug decoders, 4=trace decoding.
#else
    cfg->verbosity = rtlVerbose; // 0=normal, 1=verbose, 2=verbose decoders,
    // 3=debug decoders, 4=trace decoding.
#endif

#ifdef MEMORY_DEBUG
    logprintfLn(LOG_DEBUG, "Pre register_all_protocols heap %d",
                ESP.getFreeHeap());
#endif

    // expand register_all_protocols to determine heap impact from each decoder
    // register_all_protocols(cfg, 0);

    for (int i = 0; i < cfg->num_r_devices; i++) {
      // register all device protocols that are not disabled
      cfg->devices[i].protocol_num = i;
#ifdef MEMORY_DEBUG
      logprintfLn(LOG_DEBUG, "Pre register_protocol %d %s, heap %d", i,
                  cfg->devices[i].name, ESP.getFreeHeap());
#endif
#ifdef RESOURCE_DEBUG
      int preStack = uxTaskGetStackHighWaterMark(NULL);
      int preHeap = ESP.getFreeHeap();
#endif

      char* arg = NULL;
      char verbose[4] = "vvv";
#ifndef RTL_VERBOSE
#  define RTL_VERBOSE -1
#endif
      if (RTL_VERBOSE && i == RTL_VERBOSE) {
        arg = verbose;
      }
      // LINKRA: decoders without an integrity check that turn noise into phantom
      // devices here (Security+ v1 keyfob: random ids, id 0 repeats often enough
      // to pass the second-sighting rule). Not needed at this site.
      if (cfg->devices[i].name && strcmp(cfg->devices[i].name, "Security+ (Keyfob)") == 0)
        cfg->devices[i].disabled = 1;
      if (cfg->devices[i].disabled <= 0) {
        register_protocol(cfg, &cfg->devices[i], arg);
      }
#ifdef RESOURCE_DEBUG
      int deltaStack = preStack - uxTaskGetStackHighWaterMark(NULL);
      int deltaHeap = preHeap - ESP.getFreeHeap();
      if (deltaStack || (deltaHeap > 200)) {
        logprintfLn(LOG_DEBUG, "Process rtl_433_DecoderTask resource hit %s, deltaStack: %d, stack: %u, deltaHeap: %d, heap: %d", cfg->devices[i].name,
                    deltaStack, uxTaskGetStackHighWaterMark(NULL), deltaHeap, ESP.getFreeHeap());
      }
#endif
    }

#ifdef MEMORY_DEBUG
    logprintfLn(LOG_DEBUG, "Pre xQueueCreate heap %d", ESP.getFreeHeap());
#endif
    // LINKRA: 40 slots (pointers; trains live in PSRAM) absorb bursts of long trains
    rtl_433_Queue = xQueueCreate(40, sizeof(pulse_data_t*));
    rtl_433_Queue868 = xQueueCreate(40, sizeof(pulse_data_t*));

#ifdef MEMORY_DEBUG
    logprintfLn(LOG_DEBUG, "Pre xTaskCreatePinnedToCore heap %d",
                ESP.getFreeHeap());
#endif
#ifdef RESOURCE_DEBUG
    logprintfLn(LOG_INFO, "rtl_433_Decoder_Stack %d", rtl_433_Decoder_Stack);
#endif

    xTaskCreatePinnedToCore(
        rtl_433_DecoderTask, /* Function to implement the task */
        "rtl_433_DecoderTask", /* Name of the task */
        rtl_433_Decoder_Stack, /* Stack size in bytes */
        NULL, /* Task input parameter */
        rtl_433_Decoder_Priority, /* Priority of the task (set lower than core task) */
        &rtl_433_DecoderHandle, /* Task handle. */
        rtl_433_Decoder_Core); /* Core where the task should run */
  }
}

void _setCallback(rtl_433_ESPCallBack callback, char* messageBuffer,
                  int bufferSize) {
  // logprintfLn(LOG_DEBUG, "_setCallback location: %p", callback);

  r_cfg_t* cfg = &g_cfg;
  cfg->callback = callback;
  cfg->messageBuffer = messageBuffer;
  cfg->bufferSize = bufferSize;
}

void _setDebug(int debug) {
  rtlVerbose = debug;
  logprintfLn(LOG_INFO, "Setting rtl_433 debug to: %d", rtlVerbose);
}

// ---------------------------------------------------------------------------------------------------------

void rtl_433_DecoderTask(void* pvParameters) {
  pulse_data_t* rtl_pulses = nullptr;
  for (;;) {
    // logprintfLn(LOG_DEBUG, "rtl_433_DecoderTask awaiting signal");
    // LINKRA: 868 lane first, then 433 (short timeout keeps the 868 lane responsive)
    if (xQueueReceive(rtl_433_Queue868, &rtl_pulses, 0) != pdTRUE &&
        xQueueReceive(rtl_433_Queue, &rtl_pulses, pdMS_TO_TICKS(5)) != pdTRUE)
      continue;
    // logprintfLn(LOG_DEBUG, "rtl_433_DecoderTask signal received");
#ifdef MEMORY_DEBUG
    uint32_t signalProcessingStart = micros();
#endif

#ifdef RAW_SIGNAL_DEBUG
    logprintf(LOG_INFO, "RAW (%lu): ", rtl_pulses->signalDuration);
    for (int i = 0; i < rtl_pulses->num_pulses; i++) {
      alogprintf(LOG_INFO, "+%d", rtl_pulses->pulse[i]);
      alogprintf(LOG_INFO, "-%d", rtl_pulses->gap[i]);
#  ifdef SIGNAL_RSSI
      alogprintf(LOG_INFO, "(%d)", rtl_pulses->rssi[i]);
#  endif
    }
    alogprintfLn(LOG_INFO, " ");
#endif
#ifdef MEMORY_DEBUG
    logprintfLn(LOG_INFO, "Pre run_%s_demods: %d", rtl_433_ESP::ookModulation ? "OOK" : "FSK", ESP.getFreeHeap());
#endif
    rtl_pulses->sample_rate = 1.0e6;
    r_cfg_t* cfg = &g_cfg;
    cfg->demod->pulse_data = *rtl_pulses;
    int events = 0;

    // LINKRA DUO: Determine source band and select matching demods.
    // 868 receiver explicitly tags freq1_hz > 500 MHz.
    // 433 receiver (rtl_433_ESP::loop) leaves freq1_hz = 0 (untagged).
    // For untagged signals, fall back to OOK_MODULATION build flag
    // (preserves fsk-test compatibility where OOK_MODULATION=false).
    // LINKRA: profile tag (freq2_hz) carries the modulation the train was
    // captured with. This lets FSK decoders run on 433 and OOK decoders on 868.
    const uint32_t tag = (uint32_t)rtl_pulses->freq2_hz;
    const bool tagged = tag >= 1000;
    currentSignalProfileTag = tagged ? tag : 0;
    currentSignalFreqEst = rtl_pulses->freq1_hz > 500e6f ? rtl_pulses->fsk_f1_est : 0x7FFF;
    currentSignalNoise = rtl_pulses->noise_db;
    currentSignalSource = rtl_pulses->centerfreq_hz == 1.0f ? 1 : 0;   // LINKRA: 1 = RMT capture
    bool useFsk;
    if (tagged) {
        currentSignalIs868 = rtl_pulses->freq1_hz > 500e6f;
        useFsk = (tag % 10) == 2;
    } else if (rtl_pulses->freq1_hz > 500e6f) {
        currentSignalIs868 = true;  useFsk = true;
    } else if (rtl_pulses->freq1_hz > 0) {
        currentSignalIs868 = false; useFsk = false;
    } else {
        currentSignalIs868 = !rtl_433_ESP::ookModulation;
        useFsk = !rtl_433_ESP::ookModulation;
    }
    rtl_pulses->freq2_hz = 0; // never leak the tag into decoder output
    const uint32_t decT0 = micros();
    events += useFsk ? run_fsk_demods(&cfg->demod->r_devs, rtl_pulses)
                     : run_ook_demods(&cfg->demod->r_devs, rtl_pulses);
    {
      const uint32_t us = micros() - decT0;
      const int lane = currentSignalIs868 ? 1 : 0;
      const int b = rtl_pulses->num_pulses < 64 ? 0 : rtl_pulses->num_pulses < 256 ? 1 : 2;
      linkraDecN[lane][b]++; linkraDecUs[lane][b] += us;
      if (us > linkraDecMax[lane][b]) linkraDecMax[lane][b] = us;
    }
    if (events == 0) {
#ifdef RTL_ANALYZER
      pulse_analyzer(rtl_pulses, useFsk ? 2 : 1);
#endif
      rtl_433_ESP::unparsedSignals++;
      linkraRawStore(rtl_pulses, useFsk, currentSignalProfileTag, rtl_pulses->freq1_hz);
      rtl_433_ESP::lastUnparsedPulses = rtl_pulses->num_pulses;
      rtl_433_ESP::lastUnparsedDuration = rtl_pulses->signalDuration;
      rtl_433_ESP::lastUnparsedRssi = rtl_pulses->signalRssi;
      rtl_433_ESP::lastUnparsedIs868 = currentSignalIs868;
      for (int i = 0; i < 8 && i < rtl_pulses->num_pulses; i++) {
        rtl_433_ESP::lastUnparsedPulseGap[i*2] = rtl_pulses->pulse[i];
        rtl_433_ESP::lastUnparsedPulseGap[i*2+1] = rtl_pulses->gap[i];
      }
#ifdef PUBLISH_UNPARSED
#ifdef UNPARSED_SIGNAL_DEBUG
      logprintf(LOG_INFO, "Unparsed Signal length: %lu",
                rtl_pulses->signalDuration);
      alogprintf(LOG_INFO, ", Signal RSSI: %d", rtl_pulses->signalRssi);
      //      alogprintf(LOG_INFO, ", train: %d", _actualPulseTrain);
      //      alogprintf(LOG_INFO, ", messageCount: %d", messageCount);
      alogprintfLn(LOG_INFO, ", pulses: %d", rtl_pulses->num_pulses);

      logprintf(LOG_INFO, "RAW (%lu): ", rtl_pulses->signalDuration);
#  ifndef RAW_SIGNAL_DEBUG
      for (int i = 0; i < rtl_pulses->num_pulses; i++) {
        alogprintf(LOG_INFO, "+%d", rtl_pulses->pulse[i]);
        alogprintf(LOG_INFO, "-%d", rtl_pulses->gap[i]);
#    ifdef SIGNAL_RSSI
        alogprintf(LOG_INFO, "(%d)", rtl_pulses->rssi[i]);
#    endif
      }
      alogprintfLn(LOG_INFO, " ");
#  endif

#endif // UNPARSED_SIGNAL_DEBUG: raw dumps must not block normal decoding

      // Classify the signal and publish with device type guess
      SignalClassification sigClass = classifyUnparsedSignal(rtl_pulses, currentSignalIs868);

      // Build compact pulse fingerprint string: "avgP/avgG µs"
      int fpAvgP = 0, fpAvgG = 0;
      {
          long sp = 0, sg = 0;
          int cnt = (rtl_pulses->num_pulses < 64) ? (int)rtl_pulses->num_pulses : 64;
          for (int i = 0; i < cnt; i++) { sp += rtl_pulses->pulse[i]; sg += rtl_pulses->gap[i]; }
          if (cnt > 0) { fpAvgP = (int)(sp / cnt); fpAvgG = (int)(sg / cnt); }
      }
      char fingerprint[32];
      snprintf(fingerprint, sizeof(fingerprint), "%d/%d µs (%d Pulse)",
               fpAvgP, fpAvgG, rtl_pulses->num_pulses);

      data_t* data;
      /* clang-format off */
  data = data_make(
                "model", "",          DATA_STRING,  sigClass.model,
                "protocol", "",       DATA_STRING,  sigClass.protocol,
                "status", "",         DATA_STRING,  "classified",
                "fingerprint", "",    DATA_STRING,  fingerprint,
                "duration", "",       DATA_INT,     rtl_pulses->signalDuration,
                "rssi", "",           DATA_INT,     rtl_pulses->signalRssi,
                "pulses", "",         DATA_INT,     rtl_pulses->num_pulses,
                "band", "",           DATA_STRING,  currentSignalIs868 ? "868" : "433",
                NULL);
      /* clang-format on */

      r_cfg_t* cfg = &g_cfg;
      data_print_jsons(data, cfg->messageBuffer, cfg->bufferSize);
      (cfg->callback)(cfg->messageBuffer);
      data_free(data);

#endif
    }

#ifdef MEMORY_DEBUG
    logprintfLn(LOG_INFO, "Signal processing time: %lu",
                micros() - signalProcessingStart);
    logprintfLn(LOG_INFO, "Post run_ook_demods memory %d", ESP.getFreeHeap());
#endif
#ifdef DEMOD_DEBUG
    logprintfLn(LOG_INFO, "# of messages decoded %d", events);
#endif
    if (events > 0) {
      // alogprintfLn(LOG_INFO, " ");
    }
#if defined(MEMORY_DEBUG)
    else {
      logprintfLn(LOG_DEBUG, "Process rtl_433_DecoderTask stack free: %u",
                  uxTaskGetStackHighWaterMark(rtl_433_DecoderHandle));
      alogprintfLn(LOG_INFO, " ");
    }
#endif
#ifdef MEMORY_DEBUG
    logprintfLn(LOG_INFO, "Pre free rtl_433_DecoderTask: %d",
                ESP.getFreeHeap());
#endif
    free(rtl_pulses);
#ifdef MEMORY_DEBUG
    logprintfLn(LOG_INFO, "Post free rtl_433_DecoderTask: %d",
                ESP.getFreeHeap());
    logprintfLn(LOG_INFO, "rtl_433_DecoderTask uxTaskGetStackHighWaterMark: %d",
                uxTaskGetStackHighWaterMark(NULL));
#endif
  }
}

void processSignal(pulse_data_t* rtl_pulses) {
  // logprintfLn(LOG_DEBUG, "processSignal() about to place signal on
  // rtl_433_Queue");
  const bool is868 = rtl_pulses->freq1_hz > 500e6f;
  {
    const uint32_t waiting = uxQueueMessagesWaiting(is868 ? rtl_433_Queue868 : rtl_433_Queue);
    if (waiting > linkraQueueHigh[is868 ? 1 : 0]) linkraQueueHigh[is868 ? 1 : 0] = waiting;
  }
  if (xQueueSend(is868 ? rtl_433_Queue868 : rtl_433_Queue, &rtl_pulses, 0) != pdTRUE) {
    if (is868) linkraQueueDrops868++; else linkraQueueDrops433++;
    static uint32_t lastLog = 0;   // rate-limit: the log itself costs time under overload
    if (millis() - lastLog > 5000) {
      lastLog = millis();
      logprintfLn(LOG_ERR, "ERROR: rtl_433 %s queue full, discarding (drops 433=%lu 868=%lu)", is868 ? "868" : "433",
                  (unsigned long)linkraQueueDrops433, (unsigned long)linkraQueueDrops868);
    }
    free(rtl_pulses);
  } else {
    // logprintfLn(LOG_DEBUG, "processSignal() signal placed on rtl_433_Queue");
  }
}
