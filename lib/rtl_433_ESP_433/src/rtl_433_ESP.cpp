#include <esp_timer.h>
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

#include <rtl_433_ESP.h>

#include "receiver.h"
#include "signalDecoder.h"

/*----------------------------- Transceiver SPI Connections -----------------------------*/

#if defined(RF_MODULE_SCK) && defined(RF_MODULE_MISO) && \
    defined(RF_MODULE_MOSI) && defined(RF_MODULE_CS)
#  include <SPI.h>
#  if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3
SPIClass newSPI(FSPI);
#  else
SPIClass newSPI(VSPI);
#  endif
#endif

#ifdef RF_SX1276
SX1276 radio = RADIO_LIB_MODULE;
#endif

#ifdef RF_SX1278
SX1278 radio = RADIO_LIB_MODULE;
#endif

#ifdef RF_CC1101
CC1101 radio = RADIO_LIB_MODULE;
#endif

#if defined(RF_SX1276) || defined(RF_SX1278)
uint8_t rtl_433_ESP::OokFixedThreshold = OOK_FIXED_THRESHOLD;
#endif

Module* _mod = radio.getMod();
void* rtl_433_ESP::moduleHandle() { return _mod; }

/*----------------------------- rtl_433_ESP Internals -----------------------------*/

#ifndef rtl_433_ReceiverTask_Stack
#define rtl_433_ReceiverTask_Stack    2048
#endif
#define rtl_433_ReceiverTask_Priority 2
#define rtl_433_ReceiverTask_Core     0

/*----------------------------- Initialize variables -----------------------------*/

/**
 * Is the receiver currently receiving a signal
 */
static bool receiveMode = false;

/**
 * Timestamp in micros for start of current signal
 */
static unsigned long signalStart = micros();

/**
 * Timestamp in micros for end of most recent message aka start of current gap
 */
static unsigned long gapStart = micros();

/**
 * Timestamp in micros for end of most recent signal
 *
 */
static unsigned long signalEnd = micros();

pulse_data_t* _pulseTrains;

int rtl_433_ESP::messageCount = 0;
int rtl_433_ESP::currentRssi = -128;
int rtl_433_ESP::signalRssi = -128;
int rtl_433_ESP::rssiThreshold = MINRSSI;
bool rtl_433_ESP::_enabledReceiver = false;
volatile uint8_t rtl_433_ESP::_actualPulseTrain = 0;
uint8_t rtl_433_ESP::_avaiablePulseTrain = 0;
volatile unsigned long rtl_433_ESP::_lastChange = 0; // Timestamp of previous edge
int rtl_433_ESP::rtlVerbose = 0;
volatile int16_t rtl_433_ESP::_nrpulses;

// Variables for OOK Threshold auto calibrate function

int rtl_433_ESP::totalSignals = 0;
int rtl_433_ESP::ignoredSignals = 0;
int rtl_433_ESP::unparsedSignals = 0;
int rtl_433_ESP::lastIgnoredPulses = 0;
unsigned long rtl_433_ESP::lastIgnoredDuration = 0;
int rtl_433_ESP::lastIgnoredRssi = 0;
int rtl_433_ESP::lastUnparsedPulses = 0;
unsigned long rtl_433_ESP::lastUnparsedDuration = 0;
int rtl_433_ESP::lastUnparsedRssi = 0;
int rtl_433_ESP::lastUnparsedPulseGap[16] = {0};
bool rtl_433_ESP::lastUnparsedIs868 = false;
int rtl_433_ESP::peakRssi = -128;
int rtl_433_ESP::recvModeEntries = 0;
int rtl_433_ESP::maxPulsesInSignal = 0;
unsigned long rtl_433_ESP::maxSignalDuration = 0;
int signalRatio = 0;

// RSSI Threshold and average calculation

int rtl_433_ESP::averageRssi = 0;
int rtl_433_ESP::rssiThresholdDelta = RSSI_THRESHOLD;

bool rtl_433_ESP::ookModulation = OOK_MODULATION; // Defaults to true
volatile uint32_t rtl_433_ESP::forcedEnds = 0;
volatile uint32_t rtl_433_ESP::captureOverruns = 0;
bool rtl_433_ESP::externalCapture = false;
#define RSSI_RING_N 2048
static uint32_t* rssiRingT = nullptr;         // ms, PSRAM (task context only)
static int8_t* rssiRingV = nullptr;
static volatile uint32_t rssiRingHead = 0;
int rtl_433_ESP::rssiPeak(uint32_t fromMs, uint32_t toMs) {
  int peak = -128;
  if (!rssiRingT) return peak;
  const uint32_t head = rssiRingHead;
  for (uint32_t k = 0; k < RSSI_RING_N && k < head; k++) {
    const uint32_t i = (head - 1 - k) % RSSI_RING_N;
    const uint32_t t = rssiRingT[i];
    if ((int32_t)(t - fromMs) < 0) break;     // older than the window: done
    if ((int32_t)(t - toMs) <= 0 && rssiRingV[i] > peak) peak = rssiRingV[i];
  }
  return peak;
}
volatile uint32_t rtl_433_ESP::pulseTag = 0;
// Time base used by the upstream rtl_433 core (r_api.c, log.h). esp_timer avoids any millis() macro recursion.
extern "C" unsigned long rtl_433_millis(void) { return (unsigned long)(esp_timer_get_time() / 1000); }
volatile float rtl_433_ESP::tagFreqHz = 0;
void* rtl_433_ESP::radioHandle() { return &radio; }
int rtl_433_ESP::rtMinimumPulseLength = MINIMUM_PULSE_LENGTH;
int rtl_433_ESP::rtMinimumSignalLength = MINIMUM_SIGNAL_LENGTH;

void rtl_433_ESP::updatePulseParams(bool ook) {
  if (ook) {
    rtMinimumPulseLength = MINIMUM_PULSE_LENGTH_OOK;
    rtMinimumSignalLength = MINIMUM_SIGNAL_LENGTH_OOK;
  } else {
    rtMinimumPulseLength = MINIMUM_PULSE_LENGTH_FSK;
    rtMinimumSignalLength = MINIMUM_SIGNAL_LENGTH_FSK;
  }
}

int _totalRssi = 0;
int _rssiCount = 0;
static bool _rssiBootstrapDone = false; // first avg cycle includes all samples
volatile int rtl_433_ESP::receiverTaskLoops = 0;

int _noiseCount = 0; // Count of ticks while receiver is disabled

int rtl_433_ESP::getNoiseCount() { return _noiseCount; }
int rtl_433_ESP::getCurrentPulses() { return _nrpulses; }
bool rtl_433_ESP::getReceiveMode() { return receiveMode; }

#ifdef DEAF_WORKAROUND
unsigned long _deafWorkaround = millis();
#endif

int16_t rtl_433_ESP::_interrupt = NOT_AN_INTERRUPT;
static byte receiverGpio = -1;

static TaskHandle_t rtl_433_ReceiverHandle;

/*----------------------------- End of variable initialization -----------------------------*/

rtl_433_ESP::rtl_433_ESP() {
  _pulseTrains = (pulse_data_t*)heap_caps_calloc(
      RECEIVER_BUFFER_SIZE, sizeof(pulse_data_t), MALLOC_CAP_INTERNAL);
}

/**
 * @brief Initialize Transceiver and rtl_433 decoders
 * 
 * @param inputPin - GPIO of receiver
 * @param receiveFrequency - receive frequency
 */
void rtl_433_ESP::initReceiver(byte inputPin, float receiveFrequency) {
#if defined(RF_SX1276) || defined(RF_SX1278)
  radio.reset();
#endif

  receiverGpio = digitalPinToInterrupt(inputPin);
#ifdef MEMORY_DEBUG
  logprintfLn(LOG_INFO, "Pre initReceiver: %d", ESP.getFreeHeap());
#endif
#ifdef DEMOD_DEBUG
  logprintfLn(LOG_INFO, STR_MODULE " gpio receive pin: %d", inputPin);
  logprintfLn(LOG_INFO, STR_MODULE " receive frequency: %f", receiveFrequency);
#endif

  rtlSetup();

#ifdef MEMORY_DEBUG
  logprintfLn(LOG_INFO, "Post rtlSetup: %d", ESP.getFreeHeap());
#endif

// ESP32 defaults to VSPI, but heltec uses MOSI=27, MISO=19, SCK=5, CS=18
#if defined(RF_MODULE_SCK) && defined(RF_MODULE_MISO) && defined(RF_MODULE_MOSI) && defined(RF_MODULE_CS)
#  ifdef RF_MODULE_INIT_STATUS
  logprintfLn(LOG_INFO, STR_MODULE " SPI Config SCK: %d, MISO: %d, MOSI: %d, CS: %d", RF_MODULE_SCK, RF_MODULE_MISO, RF_MODULE_MOSI, RF_MODULE_CS);
#  endif
  newSPI.begin(RF_MODULE_SCK, RF_MODULE_MISO, RF_MODULE_MOSI, RF_MODULE_CS);
#endif

  /*----------------------------- Initialize Transceiver -----------------------------*/

#ifdef RF_CC1101
  int state = radio.begin();
#else
  int state = radio.beginFSK();
#endif
  RADIOLIB_STATE(state, "radio.begin()");

  radio.setFrequency(receiveFrequency);
  resetReceiver();
#ifdef ONBOARD_LED
  pinMode(ONBOARD_LED, OUTPUT);
  digitalWrite(ONBOARD_LED, LOW);
#endif

  if (ookModulation) {
    state = radio.setOOK(true);
    RADIOLIB_STATE(state, "setOOK");
  } else {
    state = radio.setOOK(false);
    RADIOLIB_STATE(state, "setFSK");
  }

  state = radio.setCrcFiltering(false);
  RADIOLIB_STATE(state, "setCrcFiltering");

#ifdef RF_CC1101
  if (ookModulation) {
    // set mode to standby
    radio.SPIsendCommand(RADIOLIB_CC1101_CMD_IDLE);

    state = radio.SPIsetRegValue(RADIOLIB_CC1101_REG_PKTLEN, 0);
    RADIOLIB_STATE(state, "set PKTLEN");

    // ============================================================
    // Flipper Zero battle-tested OOK async register configuration
    // Based on: subghz_device_cc1101_preset_ook_650khz_async_regs
    // Source: flipperdevices/flipperzero-firmware cc1101_configs.c
    // Adapted for 812kHz bandwidth (MDMCFG4=0x07)
    // ============================================================

    // IF Frequency: 152.3 kHz (vs chip default 381 kHz)
    // Centers the signal within the channel filter bandwidth.
    // At 812kHz BW, default 381kHz IF is near the filter edge.
    state = radio.SPIsetRegValue(RADIOLIB_CC1101_REG_FSCTRL1, 0x06);
    RADIOLIB_STATE(state, "set FSCTRL1");

    // Frequency Offset Compensation: disabled, gate off
    // Chip default 0x36 has FOC enabled with CS gate — interferes with
    // async OOK where there's no sync word to trigger the gate.
    state = radio.SPIsetRegValue(RADIOLIB_CC1101_REG_FOCCFG, 0x18);
    RADIOLIB_STATE(state, "set FOCCFG");

    // AGC Control 2: DVGA all gain steps, MAX LNA gain, MAGN_TARGET 42dB
    // For RX filter BW > 100kHz, MAGN_TARGET should be 0x7 (42dB).
    // Narrower BW (<100kHz) uses 0x3 (33dB). See Flipper/TI app notes.
    state = radio.SPIsetRegValue(RADIOLIB_CC1101_REG_AGCCTRL2, 0x07);
    RADIOLIB_STATE(state, "set AGCCTRL2");

    // AGC Control 1: LNA2 gain decreased first, no carrier sense threshold
    // Chip default 0x40 has AGC_LNA_PRIORITY=1 (LNA2 decreased first is
    // actually the same). Key: carrier sense thresholds all disabled (0x00).
    state = radio.SPIsetRegValue(RADIOLIB_CC1101_REG_AGCCTRL1, 0x00);
    RADIOLIB_STATE(state, "set AGCCTRL1");

    // AGC Control 0: Medium hysteresis, 16 samples wait, 8dB OOK boundary
    // This is the chip default (0x91) and Flipper's 650kHz OOK value.
    // OOK boundary of 8dB provides good on/off discrimination.
    state = radio.SPIsetRegValue(RADIOLIB_CC1101_REG_AGCCTRL0, 0x91);
    RADIOLIB_STATE(state, "set AGCCTRL0");

    // Frontend RX config: Increased LNA and mixer currents
    // Chip default 0x56 uses lower currents. Flipper uses 0xB6 for all
    // OOK presets — higher LNA_CURRENT and LNA2MIX_CURRENT improve
    // receive sensitivity significantly.
    state = radio.SPIsetRegValue(RADIOLIB_CC1101_REG_FREND1, 0xB6);
    RADIOLIB_STATE(state, "set FREND1");

    // Data rate: 3.79 kBaud (Flipper Zero standard for OOK async)
    state = radio.SPIsetRegValue(RADIOLIB_CC1101_REG_MDMCFG3, 0x32);
    RADIOLIB_STATE(state, "set MDMCFG3");

    // RX Bandwidth: 541kHz (tighter than Flipper's 650kHz for lower noise)
    // 650kHz had passband 433.345–433.995 → noise floor -71 dBm.
    // 541kHz: center 433.67 → passband 433.399–433.941 MHz
    // Somfy RTS at 433.42: 21kHz inside lower edge ✓
    // Standard 433.92:     21kHz inside upper edge ✓
    // Rejects more out-of-band interference → lower noise floor.
    // MDMCFG4 bits: CHANBW_E=0, CHANBW_M=2, DRATE_E=7 → 0x27
    state = radio.SPIsetRegValue(RADIOLIB_CC1101_REG_MDMCFG4, 0x27);
    RADIOLIB_STATE(state, "set MDMCFG4");
  } else {
    // From https://github.com/matthias-bs/BresserWeatherSensorReceiver/issues/41#issuecomment-1458166772
    // radio.begin(868.3, 17.24, 40, 270, 10, 32);
    // carrier frequency:                   868.3 MHz
    // bit rate:                            17.24 kbps
    // frequency deviation:                 40 kHz
    // Rx bandwidth:                        270.0 kHz (CC1101) / 250 kHz (SX1276)
    // output power:                        10 dBm
    // preamble length:                     32 bits

    state = radio.setFrequencyDeviation(40); //
    RADIOLIB_STATE(state, "setFrequencyDeviation");

    state = radio.setBitRate(17.24);
    RADIOLIB_STATE(state, "setBitRate");

    state = radio.setRxBandwidth(270); // Sweet spot found from testing
    RADIOLIB_STATE(state, "setRxBandwidth");
  }
  state = radio.disableSyncWordFiltering(false);
  RADIOLIB_STATE(state, "disableSyncWordFiltering");
#endif

#if defined(RF_SX1276) || defined(RF_SX1278)
  if (ookModulation) {
    state = radio.setDataShapingOOK(2); // Default 0 ( 0, 1, 2 )
    RADIOLIB_STATE(state, "setDataShapingOOK");

    state = radio.setOokThresholdType(
        RADIOLIB_SX127X_OOK_THRESH_PEAK); // Peak is default
    RADIOLIB_STATE(state, "OOK Thresh PEAK");

    state = radio.setOokPeakThresholdDecrement(
        RADIOLIB_SX127X_OOK_PEAK_THRESH_DEC_1_1_CHIP); // default
    RADIOLIB_STATE(state, "OOK PEAK Thresh Decrement");

    state = radio.setOokPeakThresholdStep(
        RADIOLIB_SX127X_OOK_PEAK_THRESH_STEP_0_5_DB); // default
    RADIOLIB_STATE(state, "Ook Peak Threshold Step");

    state = radio.setOokFixedOrFloorThreshold(
        OokFixedThreshold); // Default 0x0C RADIOLIB_SX127X_OOK_FIXED_THRESHOLD
    RADIOLIB_STATE(state, "OokFixedThreshold");

    state = radio.setBitRate(1.2);
    RADIOLIB_STATE(state, "setBitRate");

    state = radio.setRxBandwidth(SX127X_RXBANDWIDTH); // Lowering to 125 lowered number of received signals
    RADIOLIB_STATE(state, "setRxBandwidth");

  } else {
    // From https://github.com/matthias-bs/BresserWeatherSensorReceiver/issues/41#issuecomment-1458166772
    // radio.begin(868.3, 17.24, 40, 270, 10, 32);
    // carrier frequency:                   868.3 MHz
    // bit rate:                            17.24 kbps
    // frequency deviation:                 40 kHz
    // Rx bandwidth:                        270.0 kHz (CC1101) / 250 kHz (SX1276)
    // output power:                        10 dBm
    // preamble length:                     32 bits

    state = radio.setFrequencyDeviation(40); //
    RADIOLIB_STATE(state, "setFrequencyDeviation");

    state = radio.setBitRate(17.24);
    RADIOLIB_STATE(state, "setBitRate");

    state = radio.setRxBandwidth(
        83); // Lowering to 125 lowered number of received signals
    RADIOLIB_STATE(state, "setRxBandwidth");
  }
  state = radio.setRSSIConfig(RADIOLIB_SX127X_RSSI_SMOOTHING_SAMPLES_2, RADIOLIB_SX127X_OOK_AVERAGE_OFFSET_0_DB); // Default 8 ( 2, 4, 8, 16, 32,
  // 64, 128, 256)
  RADIOLIB_STATE(state, "RSSI Smoothing");

  state = _mod->SPIsetRegValue(RADIOLIB_SX127X_REG_PREAMBLE_DETECT,
                               RADIOLIB_SX127X_PREAMBLE_DETECTOR_OFF);
  RADIOLIB_STATE(state, "preamble detect off");

  state = radio.setDirectSyncWord(0, 0); // Disable
  RADIOLIB_STATE(state, "setDirectSyncWord");
  if (ookModulation) {
    state = radio.disableBitSync();
    RADIOLIB_STATE(state, "disableBitSync");
  }
#endif

#ifdef MEMORY_DEBUG
  logprintfLn(LOG_INFO, "Post config receivers: %d", ESP.getFreeHeap());
#endif

  // Receviers configured, start reception

#if defined(RF_SX1276) || defined(RF_SX1278)
  state = radio.receiveDirect();
#else
  state = radio.receiveDirectAsync();
#endif
  RADIOLIB_STATE(state, "receiveDirect");

#ifdef RESOURCE_DEBUG
  logprintfLn(LOG_INFO, "rtl_433_ReceiverTask_Stack %d", rtl_433_ReceiverTask_Stack);
#endif

#ifdef RF_MODULE_INIT_STATUS
  getModuleStatus();
#endif

  if (!rtl_433_ReceiverHandle) {
    xTaskCreatePinnedToCore(
        rtl_433_ESP::rtl_433_ReceiverTask, /* Function to implement the task */
        "rtl_433_ReceiverTask", /* Name of the task */
        rtl_433_ReceiverTask_Stack, /* Stack size in bytes */
        NULL, /* Task input parameter */
        rtl_433_ReceiverTask_Priority, /* Priority of the task (set lower than core task) */
        &rtl_433_ReceiverHandle, /* Task handle. */
        rtl_433_ReceiverTask_Core); /* Core where the task should run */
  }
}

/**
 * @brief Is a signal available for decoding ?
 * 
 * @return int - which pulse train
 */
int rtl_433_ESP::receivePulseTrain() {
  if (_pulseTrains[_avaiablePulseTrain].num_pulses > 0) {
    uint8_t _currentTrain = _avaiablePulseTrain;
    _avaiablePulseTrain = (_avaiablePulseTrain + 1) % RECEIVER_BUFFER_SIZE;
    return _currentTrain;
  }
  return -1;
}

/**
 * @brief Main pulse receiver logic
 * 
 */
// LINKRA: pre-trigger ring. Reception starts only when the 1-ms RSSI poll lands
// in an "on" pulse, so the start of a telegram was routinely lost. The ISR keeps
// the last 96 edges; at reception start the last 5 ms are prepended to the train.
#define PRE433_N 96
static volatile uint32_t pre433T[PRE433_N];
static volatile uint16_t pre433Dur[PRE433_N];
static volatile uint8_t  pre433High[PRE433_N];
static volatile uint32_t pre433Idx = 0, pre433LastEdge = 0;
static portMUX_TYPE pre433Mux = portMUX_INITIALIZER_UNLOCKED;

void ICACHE_RAM_ATTR rtl_433_ESP::interruptHandler() {
  if (!_enabledReceiver) { _noiseCount++; return; }
  if (!receiveMode) {
    _noiseCount++;
    const uint32_t now = micros(), d = now - pre433LastEdge;
    if (d > (uint32_t)rtMinimumPulseLength) {
      portENTER_CRITICAL_ISR(&pre433Mux);
      const uint32_t i = pre433Idx % PRE433_N;
      pre433T[i] = now; pre433Dur[i] = d > 65535 ? 65535 : d;
      pre433High[i] = digitalRead(receiverGpio) ? 0 : 1;   // now low -> the ended level was high
      pre433Idx++;
      portEXIT_CRITICAL_ISR(&pre433Mux);
      pre433LastEdge = now;
    }
    return;
  }
  volatile pulse_data_t& pulseTrain = _pulseTrains[_actualPulseTrain];
  volatile int* pulse = pulseTrain.pulse;
  volatile int* gap = pulseTrain.gap;
#ifdef SIGNAL_RSSI
  volatile int* rssi = pulseTrain.rssi;
#endif

  const unsigned long now = micros();
  const unsigned int duration = now - _lastChange;

  /* We first do some filtering (same as pilight BPF) */
  // NOTE: No per-pulse RSSI check here. For OOK, RSSI drops during "off"
  // periods, and the stale currentRssi (updated ~2ms by ReceiverTask) would
  // block ALL pulse collection. The receiveMode gate above is sufficient.
  if (duration > (unsigned int)rtMinimumPulseLength)
  {
#ifdef SIGNAL_RSSI
    rssi[_nrpulses] = currentRssi;
#endif
    if (!digitalRead(receiverGpio)) {
      pulse[_nrpulses] = duration;

      //      _nrpulses = (uint16_t)((_nrpulses + 1) % PD_MAX_PULSES);
    } else {
      if (pulse[_nrpulses] > 0) // Did we collect a + pulse ?
      {
        gap[_nrpulses] = duration;

        _nrpulses = (uint16_t)((_nrpulses + 1) % PD_MAX_PULSES);
      } else if (_nrpulses > 1) { // Have we received any data ?
        // We received a random positive blib
        gap[_nrpulses - 1] += duration;
      } else {
        gap[_nrpulses] = duration;

        _nrpulses = (uint16_t)((_nrpulses + 1) % PD_MAX_PULSES);
      }
    }
    _lastChange = now;
  }
}

/**
 * @brief Reset received signal storage
 * 
 */
void rtl_433_ESP::resetReceiver() {
  for (unsigned int i = 0; i < RECEIVER_BUFFER_SIZE; i++) {
    _pulseTrains[i].num_pulses = 0;
  }
  _avaiablePulseTrain = 0;
  _actualPulseTrain = 0;
  _nrpulses = 0;

  receiveMode = false;
  signalStart = micros();
}

/**
 * @brief Enable signal receiver logic
 * 
 * @param inputPin 
 */
void rtl_433_ESP::enableReceiver() {
  if (receiverGpio >= 0) {
    if (!externalCapture) {
      pinMode(receiverGpio, INPUT);
      attachInterrupt((uint8_t)receiverGpio, interruptHandler, CHANGE);
    }
    _enabledReceiver = true;
  }
}

/**
 * @brief Disable receiver logic, and pulse receiver
 * 
 */
void rtl_433_ESP::disableReceiver() {
  _enabledReceiver = false;
  if (!externalCapture) detachInterrupt((uint8_t)receiverGpio);
}

/**
 * @brief Change receiver frequency (for sweep mode)
 * Puts radio to standby, changes freq, restarts async RX
 */
int16_t rtl_433_ESP::setFrequency(float freqMHz) {
#ifdef RF_CC1101
  radio.SPIsendCommand(RADIOLIB_CC1101_CMD_IDLE);
  int16_t state = radio.setFrequency(freqMHz);
  if (state == RADIOLIB_ERR_NONE) {
    radio.receiveDirectAsync();
  }
  return state;
#else
  return RADIOLIB_ERR_UNKNOWN;
#endif
}

/**
 * @brief Set CC1101 MDMCFG4 register (RX bandwidth control)
 */
int16_t rtl_433_ESP::setBandwidthReg(uint8_t mdmcfg4Val) {
#ifdef RF_CC1101
  radio.SPIsendCommand(RADIOLIB_CC1101_CMD_IDLE);
  // CHANBW uses bits 7:4; retain the independently configured data-rate exponent.
  const uint8_t current = radio.SPIreadRegister(RADIOLIB_CC1101_REG_MDMCFG4);
  int16_t state = radio.SPIsetRegValue(RADIOLIB_CC1101_REG_MDMCFG4,
      (mdmcfg4Val & 0xF0) | (current & 0x0F));
  radio.receiveDirectAsync();
  return state;
#else
  return RADIOLIB_ERR_UNKNOWN;
#endif
}

/**
 * @brief watch for completed signals being received, and pass to decoder logic
 * 
 */
void rtl_433_ESP::loop() {
  if (_enabledReceiver) {
#if defined(RF_CC1101) && defined(DEAF_WORKAROUND)
    // workaround for a deaf CC1101, see issue #16
    if (millis() - _deafWorkaround > 3600000) // restart receiver every hour
    {
      _deafWorkaround = millis();
      // radio.SetRx(); // set Receive on
      radio.SPIsendCommand(RADIOLIB_CC1101_CMD_IDLE); // set Receive on
      radio.SPIsendCommand(RADIOLIB_CC1101_CMD_RX); // set Receive on

    } // workaround for a deaf CC1101
#endif

    int _receiveTrain = receivePulseTrain();
    if (_receiveTrain != -1) // Is there anything to receive ?
    {
#ifdef MEMORY_DEBUG
      logprintfLn(LOG_INFO, "Pre copy out of train: %d", ESP.getFreeHeap());
#endif
      // LINKRA: Use PSRAM to avoid internal heap exhaustion (pulse_data_t is ~10KB!)
      // With dual receivers both producing signals, MALLOC_CAP_INTERNAL runs out fast.
      pulse_data_t* rtl_pulses = (pulse_data_t*)heap_caps_calloc(1, sizeof(pulse_data_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!rtl_pulses) {
        // PSRAM alloc failed — discard signal to avoid crash
        _pulseTrains[_receiveTrain].num_pulses = 0;
        logprintfLn(LOG_WARNING, "OOM: pulse_data alloc failed, discarding 433 signal");
        return; // exit loop() — will retry on next call
      }
      memcpy(rtl_pulses, (char*)&_pulseTrains[_receiveTrain], sizeof(pulse_data_t));
      _pulseTrains[_receiveTrain].num_pulses = 0; // Make pulse train available for next train
      for (int x = 0; x < PD_MAX_PULSES; x++) {
        _pulseTrains[_receiveTrain].pulse[x] = 0;
        _pulseTrains[_receiveTrain].gap[x] = 0;
#ifdef SIGNAL_RSSI
        _pulseTrains[_receiveTrain].rssi[x] = 0;
#endif
      }
#ifdef MEMORY_DEBUG
      logprintfLn(LOG_INFO, "Post copy out of train: %d", ESP.getFreeHeap());
#endif

      if (pulseTag) {
        rtl_pulses->freq1_hz = tagFreqHz;
        rtl_pulses->freq2_hz = (float)pulseTag;
      }
      rtl_pulses->noise_db = (float)averageRssi;   // LINKRA: for SNR in the decoded event
      rtl_pulses->fsk_f1_est = 0x7FFF;
      if (rtl_pulses->num_pulses > PD_MIN_PULSES) {
        processSignal(rtl_pulses); // send received signal for decoding
      } else {
        ignoredSignals++;
#ifdef MEMORY_DEBUG
        logprintfLn(LOG_INFO, "Pre free copy out of train: %d",
                    ESP.getFreeHeap());
#endif
        free(rtl_pulses);
#ifdef MEMORY_DEBUG
        logprintfLn(LOG_INFO, "Post free copy out of train: %d",
                    ESP.getFreeHeap());
#endif
      }
    }

    // Adjust RegOokFix threshold

    if ((totalSignals % 100) == 0 && totalSignals != 0) {
#ifdef AUTOOOKFIX
#  if defined(RF_SX1276) || defined(RF_SX1278)
      OokFixedThreshold = _mod->SPIreadRegister(RADIOLIB_SX127X_REG_OOK_FIX);
#    ifdef REGOOKFIX_DEBUG
      logprintfLn(LOG_DEBUG,
                  "RegOokFix Threshold Adjust ignoredSignals %d, "
                  "unparsedSignals %d, totalSignals %d, RegOokFix 0x%.2x",
                  ignoredSignals, unparsedSignals, totalSignals,
                  OokFixedThreshold);
#    endif
      if (ignoredSignals >
          unparsedSignals) // too many ignored decrement threshold
      {
        int state = radio.setOokFixedOrFloorThreshold(--OokFixedThreshold);
        RADIOLIB_STATE(state, "OokFixedThreshold");
#    ifdef REGOOKFIX_DEBUG
        logprintfLn(LOG_DEBUG, "RegOokFix Threshold Decremented to 0x%.2x",
                    _mod->SPIreadRegister(RADIOLIB_SX127X_REG_OOK_FIX));
#    endif
      }
#  endif
#endif
      signalRatio = (totalSignals - (ignoredSignals + unparsedSignals)) / totalSignals * 100;

      totalSignals = 0;
      ignoredSignals = 0;
      unparsedSignals = 0;
    }
  }
  vTaskDelay(1);
}

/**
 * @brief Background task to monitor RSSI signal level and start / end signal receiving
 * 
 * @param pvParameters 
 */
void rtl_433_ESP::rtl_433_ReceiverTask(void* pvParameters) {
  for (;;) {
    receiverTaskLoops++;
    if (_enabledReceiver) {
      // Calculate average RSSI signal level in environment

      currentRssi = _getRSSI();
      if (currentRssi > peakRssi) peakRssi = currentRssi; // track peak RSSI
      // Only average RSSI during idle (no signal) to get true noise floor.
      // Including signal-energy samples inflates averageRssi, raising the
      // threshold and making weak devices invisible.
      // Bootstrap: first cycle includes ALL samples to escape the MINRSSI
      // chicken-and-egg (MINRSSI → permanent receiveMode → no idle samples).
      if (!receiveMode || !_rssiBootstrapDone) {
        _rssiCount++;
        _totalRssi += currentRssi;
      }

      if (_rssiCount > RSSI_SAMPLES) // Adjust RSSI Signal Threshold
      {
        averageRssi = _totalRssi / _rssiCount;

#ifdef AUTORSSITHRESHOLD
        rssiThreshold = averageRssi + rssiThresholdDelta;
        if (!_rssiBootstrapDone) {
          _rssiBootstrapDone = true;
          logprintfLn(LOG_INFO,
                      "RSSI bootstrap done: avg %d, threshold %d (delta %d)",
                      averageRssi, rssiThreshold, rssiThresholdDelta);
        }
        logprintfLn(LOG_DEBUG,
                    "Average RSSI Signal %d dbm, adjusted RSSI Threshold %d, "
                    "samples %d",
                    averageRssi, rssiThreshold, RSSI_SAMPLES);
#endif

        _totalRssi = 0;
        _rssiCount = 0;
      }

      // LINKRA: external (RMT) capture - keep the RSSI history for the per-package
      // level gate and a receive indicator, no pulse handling here.
      // LINKRA: 1-ms RSSI history (per-package level gate of the RMT capture)
      if (!rssiRingT) {
        rssiRingT = (uint32_t*)heap_caps_calloc(RSSI_RING_N, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        rssiRingV = (int8_t*)heap_caps_calloc(RSSI_RING_N, 1, MALLOC_CAP_SPIRAM);
      }
      if (rssiRingT && rssiRingV) {
        const uint32_t h = rssiRingHead;
        rssiRingT[h % RSSI_RING_N] = millis();
        rssiRingV[h % RSSI_RING_N] = (int8_t)(currentRssi < -128 ? -128 : currentRssi > 0 ? 0 : currentRssi);
        rssiRingHead = h + 1;
      }
      if (externalCapture) {
        const unsigned long nowUs2 = micros();
        if (currentRssi > rssiThreshold) { signalEnd = nowUs2; receiveMode = true; }
        else if (receiveMode && nowUs2 - signalEnd > 40000UL) receiveMode = false;
        vTaskDelay(1);
        continue;
      }
      // LINKRA: hard limits. In OOK async mode the CC1101 data pin toggles on
      // noise, so "no edge for 40 ms" alone never ended reception: the receiver
      // could stay in receiveMode indefinitely and block the profile rotation.
      const unsigned long nowUs = micros();
      const bool forceEnd = receiveMode &&
          ((nowUs - signalStart > 800000UL) || (_nrpulses >= PD_MAX_PULSES - 4));
      if (forceEnd) { signalEnd = nowUs; forcedEnds++; }

      if (!forceEnd && currentRssi > rssiThreshold) // A signal is present
      {
        // LINKRA: report the peak level of the train, not the first sample above the
        // threshold (that one is often taken on a rising edge or an OOK off period)
        if (receiveMode && currentRssi > signalRssi) signalRssi = currentRssi;
        if (!receiveMode) {
          // LINKRA: prepend the last 5 ms of edges (pre-trigger), then start
          {
            volatile pulse_data_t& tr = _pulseTrains[_actualPulseTrain];
            portENTER_CRITICAL(&pre433Mux);
            const uint32_t head = pre433Idx, nowUs = micros();
            uint32_t first = head > PRE433_N ? head - PRE433_N : 0;
            while (first < head && nowUs - pre433T[first % PRE433_N] > 5000) first++;
            int n = 0; bool started = false;
            for (uint32_t k = first; k < head && n < PD_MAX_PULSES - 2; k++) {
              const uint32_t i = k % PRE433_N;
              if (pre433High[i]) { tr.pulse[n] = pre433Dur[i]; started = true; }
              else if (started) { tr.gap[n] = pre433Dur[i]; n++; }
            }
            _nrpulses = n;
            receiveMode = true;
            _lastChange = head ? pre433T[(head - 1) % PRE433_N] : micros();
            portEXIT_CRITICAL(&pre433Mux);
          }
          recvModeEntries++;
          signalStart = micros() - 5000;
#ifdef ONBOARD_LED
          digitalWrite(ONBOARD_LED, HIGH);
#endif
          signalRssi = currentRssi;

          if (_noiseCount > 100) {
#ifdef AUTOOOKFIX
#  if defined(RF_SX1276) || defined(RF_SX1278)
            OokFixedThreshold =
                _mod->SPIreadRegister(RADIOLIB_SX127X_REG_OOK_FIX);
#    ifdef REGOOKFIX_DEBUG
            logprintfLn(
                LOG_DEBUG,
                "RegOokFix Threshold Adjust noise count %d, RegOokFix 0x%.2x",
                _noiseCount, OokFixedThreshold);
#    endif
            int state = radio.setOokFixedOrFloorThreshold(++OokFixedThreshold);
            RADIOLIB_STATE(state, "OokFixedThreshold");
#  endif
#endif
            _noiseCount = 0;
          }
        }
        signalEnd = micros();
      }
#if defined(RF_SX1276) || defined(RF_SX1278)
      // If we received a signal but had a minor drop in strength keep the
      // receiver running for an additional 150,000
      else if (micros() - signalEnd < (unsigned long)rtMinimumSignalLength)
#else
      // CC1101 OOK: Use last ISR edge time (_lastChange) for gap detection.
      // RSSI polling at ~1ms can't reliably track OOK on/off modulation —
      // any sample during an "off" period shows noise-floor RSSI.
      // _lastChange is updated by the ISR on every real edge, so it directly
      // reflects actual signal activity regardless of RSSI sampling luck.
      // (The old code also had a 30ms gate that killed the first 30ms of
      // every signal — any RSSI dip in that window terminated reception.)
      else if (!forceEnd && micros() - _lastChange < (unsigned long)rtMinimumSignalLength &&
               micros() - signalEnd < 100000UL)   // LINKRA: RSSI below threshold > 100 ms ends it
#endif
      {
        // skip over signal drop outs
      } else // A signal is not present
      {
        if (receiveMode) // Complete reception of a signal
        {
#ifdef ONBOARD_LED
          digitalWrite(ONBOARD_LED, LOW);
#endif
          receiveMode = false;
          totalSignals++;
          // Track peak signal stats (never reset, survives counter resets)
          if (_nrpulses > maxPulsesInSignal) maxPulsesInSignal = _nrpulses;
          unsigned long dur = signalEnd - signalStart;
          if (dur > maxSignalDuration) maxSignalDuration = dur;
          // LINKRA: minimum length 3 ms. rtMinimumSignalLength (40 ms for OOK) is the
          // dropout tolerance and also served as minimum length, dropping short telegrams.
          // LINKRA (from upstream f0a8156): never move on into a slot that loop()
          // has not handed off yet - the ISR would write into a pending train.
          const uint8_t nextTrain = (_actualPulseTrain + 1) % RECEIVER_BUFFER_SIZE;
          const bool nextBusy = _pulseTrains[nextTrain].num_pulses > 0;
          if (nextBusy && _nrpulses > PD_MIN_PULSES) captureOverruns++;
          if (!nextBusy && (_nrpulses > PD_MIN_PULSES) && (dur > 3000UL))
          {
            // real trailing silence as the final gap (upstream fix: PCM/RZ
            // decoders otherwise lose trailing zero bits)
            _pulseTrains[_actualPulseTrain].gap[_nrpulses] = (int)(micros() - _lastChange);
            _pulseTrains[_actualPulseTrain].num_pulses = _nrpulses + 1;
            _pulseTrains[_actualPulseTrain].signalDuration =
                signalEnd - signalStart;
            _pulseTrains[_actualPulseTrain].signalRssi = signalRssi;
#ifdef DEMOD_DEBUG
            logprintf(LOG_INFO, "Signal length: %lu",
                      _pulseTrains[_actualPulseTrain].signalDuration);
            alogprintf(LOG_INFO, ", Gap length: %lu", signalStart - gapStart);
            alogprintf(LOG_INFO, ", Signal RSSI: %d",
                       _pulseTrains[_actualPulseTrain].signalRssi);
            alogprintf(LOG_INFO, ", train: %d", _actualPulseTrain);
            alogprintf(LOG_INFO, ", messageCount: %d", messageCount);
            alogprintfLn(LOG_INFO, ", pulses: %d", _nrpulses);
#endif
            messageCount++;
            gapStart = micros();
            _actualPulseTrain = nextTrain;
            _nrpulses = 0;
          } else {
            ignoredSignals++;
            lastIgnoredPulses = _nrpulses;
            lastIgnoredDuration = signalEnd - signalStart;
            lastIgnoredRssi = signalRssi;
#ifdef DEMOD_DEBUG
            if (micros() - signalStart > 1000) {
              logprintf(LOG_INFO, "Ignored Signal length: %lu",
                        signalEnd - signalStart);

              alogprintf(LOG_INFO, ", Time since last bit length: %lu",
                         micros() - signalEnd);
              alogprintf(LOG_INFO, ", Gap length: %lu", signalStart - gapStart);
              alogprintf(LOG_INFO, ", Signal RSSI: %d", signalRssi);
              alogprintf(LOG_INFO, ", Current RSSI: %d", currentRssi);
              alogprintf(LOG_INFO, ", pulses: %d", _nrpulses);
              alogprintfLn(LOG_INFO, ", noise count: %d", _noiseCount);
              gapStart = micros();
            }
#endif
            // LINKRA: the ISR treats non-zero slots as collected pulses; clear what we used
            {
              volatile pulse_data_t& tr = _pulseTrains[_actualPulseTrain];
              const int upto = _nrpulses + 2 < PD_MAX_PULSES ? _nrpulses + 2 : PD_MAX_PULSES;
              for (int x = 0; x < upto; x++) { tr.pulse[x] = 0; tr.gap[x] = 0; }
            }
            _nrpulses = 0;
          }
#ifdef MEMORY_DEBUG
          logprintfLn(LOG_INFO,
                      "rtl_433_ReceiverTask uxTaskGetStackHighWaterMark: %d", uxTaskGetStackHighWaterMark(NULL));
#endif
        }
      }
    }
    vTaskDelay(1);
  }
}

/**
 * @brief Client callback to receive decoded signals
 * 
 * @param callback 
 * @param messageBuffer 
 * @param bufferSize 
 */
rtl_433_ESPCallBack _callback; // TODO: Use global object
char* _messageBuffer;
int _bufferSize;

void rtl_433_ESP::setCallback(rtl_433_ESPCallBack callback, char* messageBuffer,
                              int bufferSize) {
  // logprintfLn(LOG_DEBUG, "rtl_433_ESP::setCallback location: %p", callback);
  _callback = callback;
  _messageBuffer = messageBuffer;
  _bufferSize = bufferSize;
  _setCallback(callback, messageBuffer, bufferSize);
}

/**
 * @brief Set delta applied to average RSSI level for determining start and end of signal
 * 
 * @param newRssi 
 */
void rtl_433_ESP::setRSSIThreshold(int newRssi) {
  rssiThresholdDelta = newRssi;
#ifndef AUTORSSITHRESHOLD
  logprintfLn(LOG_INFO, "RSSI Threshold not available: %d", rssiThresholdDelta);
#else
  logprintfLn(LOG_INFO, "Setting RSSI Threshold Delta to: %d",
              rssiThresholdDelta);
#endif
}

/**
 * @brief set OOK Threshold
 * 
 */
#if defined(RF_SX1276) || defined(RF_SX1278)
void rtl_433_ESP::setOOKThreshold(int newOokThreshold) {
  OokFixedThreshold = newOokThreshold;
#  ifdef REGOOKFIX_DEBUG
  logprintfLn(LOG_INFO, "Setting setOokFixedOrFloorThreshold to: %d",
              OokFixedThreshold);
#  endif

  int state = radio.setOokFixedOrFloorThreshold(OokFixedThreshold);
  RADIOLIB_STATE(state, "setOokFixedThreshold");
}
#endif

/**
 * @brief This does not work
 * 
 * @param debug 
 */
void rtl_433_ESP::setDebug(int debug) {
  rtlVerbose = debug;
  logprintfLn(LOG_INFO, "Setting rtl_433 debug to: %d", rtlVerbose);
}

/**
 * @brief Send RTL_433_ESP status to serial port and client. Also send to serial port transceiver status.
 * 
 * @param status 
 */
void rtl_433_ESP::getStatus() {
  alogprintfLn(LOG_INFO, " ");
  logprintf(LOG_INFO, "Status Message: Gap length: %lu",
            signalStart - gapStart);
  alogprintf(LOG_INFO, ", Modulation: %s", ookModulation ? "OOK" : "FSK");
  alogprintf(LOG_INFO, ", Signal RSSI: %d", signalRssi);
  alogprintf(LOG_INFO, ", train: %d", _actualPulseTrain);
  alogprintf(LOG_INFO, ", messageCount: %d", messageCount);
  alogprintf(LOG_INFO, ", totalSignals: %d", totalSignals);
  alogprintf(LOG_INFO, ", signalRatio: %d", signalRatio);
  alogprintf(LOG_INFO, ", ignoredSignals: %d", ignoredSignals);
  alogprintf(LOG_INFO, ", unparsedSignals: %d", unparsedSignals);
  alogprintf(LOG_INFO, ", _enabledReceiver: %d", _enabledReceiver);
  alogprintf(LOG_INFO, ", receiveMode: %d", receiveMode);
  alogprintf(LOG_INFO, ", currentRssi: %d", currentRssi);
  alogprintf(LOG_INFO, ", rssiThreshold: %d", rssiThreshold);
  alogprintf(LOG_INFO, ", StackHWM: %d", uxTaskGetStackHighWaterMark(NULL));
  alogprintf(LOG_INFO, ", RTL_HWM: %d", uxTaskGetStackHighWaterMark(rtl_433_ReceiverHandle));
  alogprintf(LOG_INFO, ", DCD_HWM: %d", uxTaskGetStackHighWaterMark(rtl_433_DecoderHandle));
  alogprintfLn(LOG_INFO, ", pulses: %d", _nrpulses);

  data_t* data;

  /* clang-format off */
  data = data_make(
                "model",          "", DATA_STRING,  "status",
                "protocol",       "", DATA_STRING,  "rtl_433_ESP status message",
                "modulation",     "", DATA_STRING,  ookModulation ? "OOK" : "FSK",
                "RTLRssi",        "", DATA_INT,     currentRssi,
                "RTLAVGRssi",     "", DATA_INT,     averageRssi,
                "RTLRssiThresh",  "", DATA_INT,     rssiThreshold,
                "signalRssi",     "", DATA_INT,     signalRssi,

#ifdef ZradioSX127x
               "RTLOOKThresh",    "", DATA_INT,     OokFixedThreshold,
#endif

                "train",          "", DATA_INT, _actualPulseTrain,
                "RTLCnt",         "", DATA_INT, messageCount,
                "totalSignals",   "", DATA_INT, totalSignals,
                "signalRatio",    "", DATA_INT, signalRatio,
                "ignoredSignals", "", DATA_INT, ignoredSignals,
                "unparsedSignals", "", DATA_INT, unparsedSignals,
                "StackHWM",       "", DATA_INT, uxTaskGetStackHighWaterMark(NULL),
                "RTL_HWM",        "", DATA_INT, uxTaskGetStackHighWaterMark(rtl_433_ReceiverHandle),
                "DCD_HWM",        "", DATA_INT, uxTaskGetStackHighWaterMark(rtl_433_DecoderHandle),
                "freeMem",        "", DATA_INT, ESP.getFreeHeap(),
                "_enabledReceiver", "", DATA_INT, _enabledReceiver,
                "receiveMode",    "", DATA_INT, receiveMode,
                NULL);
#ifdef RF_MODULE_INIT_STATUS
  getModuleStatus();
#endif

  data_print_jsons(data, _messageBuffer, _bufferSize);
  (_callback)(_messageBuffer);
  data_free(data);
}

/****************************************************************
 *FUNCTION NAME:RSSI Level - Replacement RSSI function for the one in RadioLib
 *that doesn't work in OOK async mode for a FUNCTION     :Calculating the RSSI
 *Level INPUT        :none OUTPUT       :none
 ****************************************************************/
int rtl_433_ESP::_getRSSI(void) {
  int rssi;
#ifdef RF_CC1101
  rssi = radio.getRSSI();
#elif RADIOLIB_VERSION_MAJOR >= 6
  rssi = radio.getRSSI(true, true);
#else
  rssi = radio.getRSSI(true);
#endif
  return rssi;
}

/**
 * Send to serial output current transceiver status
 *
 */
void rtl_433_ESP::getModuleStatus() {
#ifdef RF_CC1101
  alogprintfLn(LOG_INFO, "----- CC1101 Status -----");
  alogprintfLn(LOG_INFO, "CC1101_MDMCFG1: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_MDMCFG1));
  alogprintfLn(LOG_INFO, "CC1101_MDMCFG2: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_MDMCFG2));
  alogprintfLn(LOG_INFO, "CC1101_MDMCFG3: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_MDMCFG3));
  alogprintfLn(LOG_INFO, "CC1101_MDMCFG4: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_MDMCFG4));
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "CC1101_DEVIATN: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_DEVIATN));
  alogprintfLn(LOG_INFO, "CC1101_AGCCTRL0: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_AGCCTRL0));
  alogprintfLn(LOG_INFO, "CC1101_AGCCTRL1: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_AGCCTRL1));
  alogprintfLn(LOG_INFO, "CC1101_AGCCTRL2: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_AGCCTRL2));
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "CC1101_IOCFG0: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_IOCFG0));
  alogprintfLn(LOG_INFO, "CC1101_IOCFG1: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_IOCFG1));
  alogprintfLn(LOG_INFO, "CC1101_IOCFG2: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_IOCFG2));
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "CC1101_FIFOTHR: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FIFOTHR));
  alogprintfLn(LOG_INFO, "CC1101_SYNC0: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_SYNC0));
  alogprintfLn(LOG_INFO, "CC1101_SYNC1: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_SYNC1));
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "CC1101_PKTLEN: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_PKTLEN));
  alogprintfLn(LOG_INFO, "CC1101_PKTCTRL0: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_PKTCTRL0));
  alogprintfLn(LOG_INFO, "CC1101_PKTCTRL1: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_PKTCTRL1));
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "CC1101_ADDR: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_ADDR));
  alogprintfLn(LOG_INFO, "CC1101_CHANNR: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_CHANNR));
  alogprintfLn(LOG_INFO, "CC1101_FSCTRL0: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FSCTRL0));
  alogprintfLn(LOG_INFO, "CC1101_FSCTRL1: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FSCTRL1));
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "CC1101_FREQ0: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FREQ0));
  alogprintfLn(LOG_INFO, "CC1101_FREQ1: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FREQ1));
  alogprintfLn(LOG_INFO, "CC1101_FREQ2: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FREQ2));
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "CC1101_MCSM0: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_MCSM0));
  alogprintfLn(LOG_INFO, "CC1101_MCSM1: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_MCSM1));
  alogprintfLn(LOG_INFO, "CC1101_MCSM2: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_MCSM2));
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "CC1101_FOCCFG: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FOCCFG));

  alogprintfLn(LOG_INFO, "CC1101_BSCFG: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_BSCFG));
  alogprintfLn(LOG_INFO, "CC1101_WOREVT0: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_WOREVT0));
  alogprintfLn(LOG_INFO, "CC1101_WOREVT1: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_WOREVT1));
  alogprintfLn(LOG_INFO, "CC1101_WORCTRL: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_WORCTRL));
  alogprintfLn(LOG_INFO, "CC1101_FREND0: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FREND0));
  alogprintfLn(LOG_INFO, "CC1101_FREND1: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FREND1));
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "CC1101_FSCAL0: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FSCAL0));
  alogprintfLn(LOG_INFO, "CC1101_FSCAL1: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FSCAL1));
  alogprintfLn(LOG_INFO, "CC1101_FSCAL2: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FSCAL2));
  alogprintfLn(LOG_INFO, "CC1101_FSCAL3: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_FSCAL3));
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "CC1101_RCCTRL0: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_RCCTRL0));
  alogprintfLn(LOG_INFO, "CC1101_RCCTRL1: 0x%.2x",
               radio.SPIreadRegister(RADIOLIB_CC1101_REG_RCCTRL1));
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "CC1101_PARTNUM: 0x%.2x",
               radio.SPIgetRegValue(RADIOLIB_CC1101_REG_PARTNUM));
  alogprintfLn(LOG_INFO, "CC1101_VERSION: 0x%.2x",
               radio.SPIgetRegValue(RADIOLIB_CC1101_REG_VERSION));
  alogprintfLn(LOG_INFO, "CC1101_MARCSTATE: 0x%.2x",
               radio.SPIgetRegValue(RADIOLIB_CC1101_REG_MARCSTATE));
  alogprintfLn(LOG_INFO, "CC1101_PKTSTATUS: 0x%.2x",
               radio.SPIgetRegValue(RADIOLIB_CC1101_REG_PKTSTATUS));
  alogprintfLn(LOG_INFO, "CC1101_RXBYTES: 0x%.2x",
               radio.SPIgetRegValue(RADIOLIB_CC1101_REG_RXBYTES));

  alogprintfLn(LOG_INFO, "----- CC1101 Status -----");
#endif
#if defined(RF_SX1276) || defined(RF_SX1278)

  alogprintfLn(LOG_INFO, "----- SX127x Status -----");

  OokFixedThreshold = _mod->SPIreadRegister(RADIOLIB_SX127X_REG_OOK_FIX);

  alogprintfLn(LOG_INFO, "RegOpMode: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_OP_MODE));
  alogprintfLn(LOG_INFO, "RegPacketConfig1: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_PACKET_CONFIG_2));
  alogprintfLn(LOG_INFO, "RegPacketConfig2: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_PACKET_CONFIG_2));
  alogprintfLn(LOG_INFO, "RegBitrateMsb: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_BITRATE_MSB));
  alogprintfLn(LOG_INFO, "RegBitrateLsb: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_BITRATE_LSB));
  alogprintfLn(LOG_INFO, "RegRxBw: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_RX_BW));
  alogprintfLn(LOG_INFO, "RegAfcBw: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_AFC_BW));
                if (ookModulation) {
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "RegOokPeak: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_OOK_PEAK));
  alogprintfLn(LOG_INFO, "RegOokFix: 0x%.2x", OokFixedThreshold);
  alogprintfLn(LOG_INFO, "RegOokAvg: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_OOK_AVG));
                }
  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "RegLna: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_LNA));
  alogprintfLn(LOG_INFO, "RegRxConfig: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_RX_CONFIG));
  alogprintfLn(LOG_INFO, "RegRssiConfig: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_RSSI_CONFIG));

  alogprintfLn(LOG_INFO, "-------------------------");
  alogprintfLn(LOG_INFO, "RegDioMapping1: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_DIO_MAPPING_1));

 if (!ookModulation) {
  alogprintfLn(LOG_INFO, "----------- FSK --------------");
  alogprintfLn(LOG_INFO, "FDEV_MSB: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_FDEV_MSB));
  alogprintfLn(LOG_INFO, "FDEV_LSB: 0x%.2x",
               _mod->SPIreadRegister(RADIOLIB_SX127X_REG_FDEV_LSB));
 }
  alogprintfLn(LOG_INFO, "----- SX127x Status -----");

#endif
}

/**
 * Functions used only during testing
 *
 */
#if defined(setBitrate) || defined(setFreqDev) || defined(setRxBW)
int16_t rtl_433_ESP::setFrequencyDeviation(float value) {
  return radio.setFrequencyDeviation(value);
}

int16_t rtl_433_ESP::receiveDirect() {
#if defined(RF_SX1276) || defined(RF_SX1278)
  return radio.receiveDirect();
#else
  return radio.receiveDirectAsync();
#endif
}

int16_t rtl_433_ESP::setBitRate(float value) {
  return radio.setBitRate(value);
}

int16_t rtl_433_ESP::setRxBandwidth(float value) {
  return radio.setRxBandwidth(value);
}
#endif

uint8_t rtl_433_ESP::readCC1101Reg(uint8_t addr) {
#ifdef RF_CC1101
  return _mod->SPIreadRegister(addr);
#else
  return 0xFF;
#endif
}

void rtl_433_ESP::writeCC1101Reg(uint8_t addr, uint8_t value) {
#ifdef RF_CC1101
  _mod->SPIsetRegValue(addr, value);
#endif
}
