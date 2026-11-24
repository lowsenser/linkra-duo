#pragma once

#define BOARD_NAME "Linkra Duo - PCB"
#define BOARD_VERSION "2.0"

// ===== LEDs (WS2812B Strip) =====
#define LED_PIN             38   // Data-In
#define LED_COUNT           8    // Anzahl LEDs

// ===== SPI (Optimiert für PCB) =====
#define SPI_SCK_PIN         12
#define SPI_MISO_PIN        13
#define SPI_MOSI_PIN        11

// ===== CC1101 (Geänderte Pins) =====
#define CC1101_CS_A_PIN     10
#define CC1101_CS_B_PIN     9    // Anderes Pin!
#define CC1101_GDO0_A_PIN   4
#define CC1101_GDO0_B_PIN   5

// ===== PCB-Extras =====
#define HAS_WS2812B         1
#define HAS_PA_LNA          1
#define PA_LNA_CTRL_PIN     15
#define PCB_BUZZER_PIN      16
