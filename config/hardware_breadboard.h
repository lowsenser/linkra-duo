#pragma once

#define BOARD_NAME "Linkra Duo - Breadboard"
#define BOARD_VERSION "1.0"

// ===== LEDs (Separate GPIOs) =====
#define LED_POWER_PIN       38   // Onboard RGB
#define LED_WIFI_R_PIN      35
#define LED_WIFI_G_PIN      36
#define LED_WIFI_B_PIN      37
#define LED_RADIO_A_PIN     33   // 433MHz Status
#define LED_RADIO_B_PIN     34   // 868MHz Status

// ===== SPI (Standard DevKit) =====
#define SPI_SCK_PIN         12
#define SPI_MISO_PIN        13
#define SPI_MOSI_PIN        11

// ===== E07-400MBL-01 (CC1101 #1 - 433 MHz OOK) =====
// Module pinout (8-pin header):
//   1.VCC (3.3V), 2.GND, 3.MOSI, 4.SCK, 5.MISO, 6.PC0/GDO2, 7.GDO0, 8.CSn
// IMPORTANT: Pin 6 (GDO2) is routed through STM8 MCU PC0 → STUCK LOW, unusable!
//   Pin 7 (GDO0) goes directly to CC1101 → used for async data (IOCFG0=0x0D)
// Wiring:
//   MOSI  → ESP32 GPIO 11
//   SCK   → ESP32 GPIO 12
//   MISO  → ESP32 GPIO 13
//   CSn   → ESP32 GPIO 4
//   GDO0  → ESP32 GPIO 3   (Pin 7 → Data/Interrupt)
//   VCC   → 3.3V
//   GND   → GND

#define CC1101_CS_433       4
#define CC1101_GDO0_433     3
#define CC1101_SCK_433      12
#define CC1101_MISO_433     13
#define CC1101_MOSI_433     11

// ===== CC1101 #2 (868 MHz FSK) =====
// Wiring:
//   MOSI  → ESP32 GPIO 17
//   SCK   → ESP32 GPIO 18
//   MISO  → ESP32 GPIO 7
//   CSn   → ESP32 GPIO 10
//   GDO0  → ESP32 GPIO 6
//   VCC   → 3.3V
//   GND   → GND

#define CC1101_CS_868       10
#define CC1101_GDO0_868     6
#define CC1101_SCK_868      18
#define CC1101_MISO_868     7
#define CC1101_MOSI_868     17

// (ESP32-C6 / Matter entfernt in 0.6.0; GPIO14, 5, 9 und 8 sind frei)

// ===== Features =====
#define HAS_SEPARATE_LEDS   1
#define HAS_WS2812B         0
#define HAS_PA_LNA          0
