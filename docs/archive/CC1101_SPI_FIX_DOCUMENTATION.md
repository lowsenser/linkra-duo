> **Archiv (historisch, Februar 2026).** Beschreibt einen Fix in der SmartRC-/ELECHOUSE-CC1101-Bibliothek. Die Firmware nutzt diese Bibliothek nicht mehr; der Funkzugriff läuft heute über RadioLib (via `lib/rtl_433_ESP_433`).

# CC1101 SPI-Kommunikation — Problemanalyse & Lösung

**Projekt:** Linkra Duo A0.1 — CC1101 RF Gateway  
**Datum:** 7. Februar 2026  
**Status:** ✅ GELÖST — CC1101 kommuniziert fehlerfrei über SPI, empfängt RF-Signale

---

## Inhaltsverzeichnis

1. [Zusammenfassung](#1-zusammenfassung)
2. [Ausgangssituation & Problem](#2-ausgangssituation--problem)
3. [Recherche & Ursachenanalyse](#3-recherche--ursachenanalyse)
4. [Lösung: Viproz Fork der SmartRC-Library](#4-lösung-viproz-fork-der-smartrc-library)
5. [Vorgenommene Änderungen im Detail](#5-vorgenommene-änderungen-im-detail)
6. [Aktueller Status & Verifizierung](#6-aktueller-status--verifizierung)
7. [Hardware-Referenz](#7-hardware-referenz)
8. [Bekannte Issues & Hinweise](#8-bekannte-issues--hinweise)

---

## 1. Zusammenfassung

Die SPI-Kommunikation zwischen dem **ESP32-S3 DevKitC-1** und dem **CC1101-Transceiver** (auf dem **EBYTE E07-400MBL-01** Evaluation Board) schlug fehl. MISO war permanent HIGH, der CC1101 antwortete nicht, `Init()` hing unendlich.

**Ursache:** Drei Bugs in der originalen SmartRC-CC1101-Driver-Lib (v2.5.7 von LSatan):

| Bug | Auswirkung | GitHub Issue |
|-----|-----------|-------------|
| Hardcoded SPI-Pins (18/19/23/5) für ESP32 | ESP32-S3 verwendet andere Default-SPI-Pins | [#174](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib/issues/174) |
| Doppeltes `SPI.begin()` ohne `SPI.end()` | Init() hängt auf ESP32-S3/C3/C6 | [#172](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib/issues/172) |
| `digitalRead()` nach `SPI.begin()` auf ESP32 | Liefert falsche Werte (Pin nicht mehr GPIO) | [#170](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib/issues/170) |

**Lösung:** Umstieg auf den **Viproz Fork** der SmartRC-Library, der alle drei Bugs behebt und zusätzlich MISO-Timeout-Schutz bietet.

---

## 2. Ausgangssituation & Problem

### Hardware

| Komponente | Typ | Anmerkung |
|-----------|------|----------|
| MCU | ESP32-S3-DevKitC-1 (N16R8) | 16MB Flash, 8MB PSRAM |
| RF-Modul | EBYTE E07-400MBL-01 | Evaluation Board mit CC1101 (E07-400M10S) |
| CC1101 | Texas Instruments CC1101 | 433 MHz Sub-GHz Transceiver |
| Verbindung | SPI (4-Wire + GDO0) | Direkt verdrahtet, kein Level-Shifter |

### SPI-Verdrahtung

```
ESP32-S3         E07-400MBL-01
─────────        ──────────────
GPIO 11 (MOSI) → MOSI
GPIO 12 (SCK)  → SCK
GPIO 13 (MISO) ← MISO
GPIO 10 (CS)   → CSn
GPIO  3 (GDO0) ← GDO0
3.3V           → VCC
GND            → GND
```

### Symptome

1. **MISO permanent HIGH** — CC1101 zieht MISO normalerweise LOW wenn CS LOW geht ("SPI Ready"-Signal)
2. **`Init()` blockiert unendlich** — Die Library wartet in einer Endlosschleife auf MISO LOW
3. **VERSION-Register = 0xFF** — Kein gültiger CC1101 antwortet (erwartet: 0x14)
4. **System-Hang** — Kompletter Systemstillstand beim Booten

### Chronologie der Fehlerbehebung

| Schritt | Aktion | Ergebnis |
|---------|--------|---------|
| 1 | Rohes SPI (manuell, ohne Library) | MISO stuck HIGH, VERSION=0xFF |
| 2 | Umstieg auf SmartRC-CC1101-Driver-Lib v2.5.7 | Init() hängt unendlich |
| 3 | MISO-Timeout vor Init() hinzugefügt | System hängt nicht mehr, aber CC1101 antwortet nicht |
| 4 | Verdrahtung mehrfach geprüft und neu verkabelt | Keine Änderung |
| 5 | Internet-Recherche: GitHub Issues + Datasheets | Root-Cause gefunden! |
| 6 | **Umstieg auf Viproz Fork** | ✅ **CC1101 antwortet sofort!** |

---

## 3. Recherche & Ursachenanalyse

### 3.1 SmartRC-Library Bugs auf ESP32-S3

Durch systematische Recherche der GitHub Issues wurden folgende bekannte Probleme identifiziert:

#### Issue #174: Hardcoded SPI-Pins
- **Datei:** `ELECHOUSE_CC1101_SRC_DRV.cpp`, Zeile ~297
- **Problem:** Bei `#elif ESP32` werden die Pins hart auf `SCK=18, MISO=19, MOSI=23, SS=5` gesetzt
- **ESP32-S3 Default-Pins:** `SCK=12, MISO=13, MOSI=11, SS=10` (FSPI)
- **Auswirkung:** Selbst wenn `setSpiPin()` aufgerufen wird, verwendet `setSpi()` intern die falschen Pins falls `spi==0`

#### Issue #172: Doppeltes SPI.begin() ohne SPI.end()
- **Problem:** `Init()` ruft `SpiStart()` auf (→ `SPI.begin()`), dann `RegConfigSettings()` ruft `SpiWriteReg()` auf, das ebenfalls `SpiStart()` aufruft
- **ESP32-S3 Verhalten:** Zweites `SPI.begin()` ohne vorheriges `SPI.end()` führt zu Peripheral Manager Crash
- **Log:** `perimanSetPinBus(): Pin X successfully set to type INIT (0)` → SPI-Bus wird zerstört
- **Fix:** `SpiEnd()` vor `RegConfigSettings()` einfügen

#### Issue #169/#170: Init() hängt auf ESP32-S3/C3
- **Problem:** Nach `SPI.begin()` funktioniert `digitalRead(MISO_PIN)` nicht mehr korrekt auf ESP32
- **Grund:** Der ESP32 Peripheral Manager ändert die Pin-Funktion von GPIO zu SPI; `digitalRead()` liest dann immer HIGH
- **Fix:** `gpio_get_level((gpio_num_t)MISO_PIN)` statt `digitalRead(MISO_PIN)` verwenden

### 3.2 E07-400MBL-01 Evaluation Board

**Wichtige Erkenntnis:** Das E07-400MBL-01 ist ein **Evaluation Board** (53×27mm) mit einem aufgelöteten **E07-400M10S** CC1101-Modul. Die offizielle Pin-Tabelle von cdebyte.com zeigt einen **onboard STM8 Mikrocontroller**:

| Pin | Name | Funktion |
|-----|------|----------|
| 1 | VCC | 3.3V Stromversorgung |
| 2 | GND | Masse |
| 3 | VCC | Durchverbindung |
| 4 | SAW | MCU Stromversorgung (Jumper zu Pin 3) |
| 6 | REST | STM8 MCU Reset-Pin |
| 7 | SWIM | STM8 Debug-Interface |
| 9 | PC0 | Modul-Reset |
| 10 | PB7 | MISO (durch MCU) |
| 11 | PB6 | MOSI (durch MCU) |
| 12 | PB5 | SCLK (durch MCU) |
| 13 | PB4 | NSS/CS (durch MCU) |
| 14 | TXD | UART TX |
| 15 | RXD | UART RX |
| 16 | M1 | Moduswahl |
| 18 | M0 | Moduswahl |

> **Hinweis:** In der Praxis funktioniert der SPI-Durchgriff trotz STM8 MCU, da das Board im Default-Modus den SPI-Bus durchreicht. Das eigentliche Problem war die fehlerhafte SmartRC-Library. Die STM8-MCU-Info bleibt als Fallback-Diagnose im Code für den Fall, dass die SPI-Kommunikation aus Hardware-Gründen fehlschlägt.

---

## 4. Lösung: Viproz Fork der SmartRC-Library

### Repository

- **Original:** `lsatan/SmartRC-CC1101-Driver-Lib` v2.5.7 — [GitHub](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib)
- **Fork:** `Viproz/SmartRC-CC1101-Driver-Lib` — [GitHub](https://github.com/Viproz/SmartRC-CC1101-Driver-Lib)
- **Commit:** `3664897` (9 Commits ahead of original)

### Fixes im Viproz Fork

#### 1. `waitForMisoLow()` — Timeout-geschütztes MISO-Polling
```cpp
#define MISO_TIMEOUT 10  // 10ms Timeout

bool waitForMisoLow() {
    uint32_t start = millis();
    while (
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
        gpio_get_level((gpio_num_t)MISO_PIN)  // ← ESP32-Fix!
#else
        digitalRead(MISO_PIN)
#endif
    ) {
        if (millis() - start > MISO_TIMEOUT) {
            return false;  // Timeout statt Endlosschleife!
        }
        delay(0);
    }
    return true;
}
```

**Verbesserungen:**
- `gpio_get_level()` statt `digitalRead()` auf ESP32 (funktioniert auch wenn Pin als SPI konfiguriert)
- 10ms Timeout statt Endlosschleife
- Gibt `bool` zurück (Erfolg/Fehler)

#### 2. `Init()` — Rückgabewert + SpiEnd-Fix
```cpp
bool ELECHOUSE_CC1101::Init(void) {
    setSpi();
    SpiStart();                   // SPI initialisieren
    writeSPIPin(SS_PIN, HIGH);
    writeSPIPin(SCK_PIN, HIGH);
    writeSPIPin(MOSI_PIN, LOW);
    
    if(!Reset())                  // Reset mit Timeout
        return false;
    
    SpiEnd();                     // ← KRITISCHER FIX: SPI schließen!
    RegConfigSettings();          // Register-Config (öffnet/schließt SPI pro Register)
    
    return true;
}
```

**Verbesserungen:**
- Gibt `bool` zurück (Original: `void`)
- `SpiEnd()` **vor** `RegConfigSettings()` (Fix für Issue #172)
- `Reset()` gibt `bool` zurück (Timeout-geschützt)

#### 3. `writeSPIPin()` — ESP32-kompatibles GPIO-Schreiben
```cpp
void writeSPIPin(uint8_t pin, uint8_t value) {
#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
    gpio_set_level((gpio_num_t)pin, value);
#else
    digitalWrite(pin, value);
#endif
}
```

#### 4. `Reset()` — Timeout-geschützter CC1101-Reset
```cpp
bool ELECHOUSE_CC1101::Reset(void) {
    writeSPIPin(SS_PIN, LOW);
    delay(1);
    writeSPIPin(SS_PIN, HIGH);
    delay(1);
    writeSPIPin(SS_PIN, LOW);
    if(!waitForMisoLow()) return false;   // ← Timeout!
    SPI.transfer(CC1101_SRES);
    if(!waitForMisoLow()) return false;   // ← Timeout!
    writeSPIPin(SS_PIN, HIGH);
    return true;
}
```

---

## 5. Vorgenommene Änderungen im Detail

### 5.1 `platformio.ini` — Library-Umstellung

**Vorher:**
```ini
lib_deps =
    bblanchon/ArduinoJson @ ^7.4.2
    ESP32Async/ESPAsyncWebServer @ ^3.9.6
    ESP32Async/AsyncTCP @ ^3.4.10
    lsatan/SmartRC-CC1101-Driver-Lib @ ^2.5.7
```

**Nachher:**
```ini
lib_deps =
    bblanchon/ArduinoJson @ ^7.4.2
    ESP32Async/ESPAsyncWebServer @ ^3.9.6
    ESP32Async/AsyncTCP @ ^3.4.10
    https://github.com/Viproz/SmartRC-CC1101-Driver-Lib.git
```

**Grund:** Der Viproz Fork ist nicht im PlatformIO Library Registry, daher wird er direkt von GitHub bezogen. PlatformIO erkennt ihn als `SmartRC-CC1101-Driver-Lib @ 2.5.7+sha.3664897`.

### 5.2 `src/rf/rf_manager.h` — Header (SmartRC API)

**Vorher:** Eigene SPI-Methoden (`readStatusReg`, `writeReg`, `sendCommand`, etc.)

**Nachher:** Nutzt ausschließlich die SmartRC-Library API:
```cpp
#include <ELECHOUSE_CC1101_SRC_DRV.h>
```

- Entfernt: Alle manuellen SPI-Methoden
- Beibehalten: `RFSignal`-Struct, `RFDebugInfo`-Struct, `RFManager`-Singleton
- Hinzugefügt: `configureForRx()` ersetzt `configureWideBandwidth()`

### 5.3 `src/rf/rf_manager.cpp` — Kern-Implementierung

**Entfernt:**
- `#include <SPI.h>` — Library handhabt SPI intern
- Manueller SPI-Pretest (`SPI.begin()`, `SPI.transfer()`, `SPI.end()`)
- Endlosschleifen-Schutz mit eigenem MISO-Timeout

**Hinzugefügt — `begin()` Initialisierungssequenz:**

```
1. GPIO-State-Diagnose (vor SPI-Init)
   → Liest alle SPI-Pins als INPUT, loggt Zustände
   → Hilft bei Hardware-Debugging

2. Manueller CS-Toggle-Test
   → CS LOW → wartet max. 10ms auf MISO LOW
   → Bei Timeout: STM8-MCU-Warnung ausgeben
   → Keine SPI-Library-Abhängigkeit

3. setSpiPin() — Custom SPI-Pins setzen
   → MUSS vor Init() aufgerufen werden
   → ESP32-S3 Fix (Issue #174)

4. setGDO0() — Interrupt-Pin setzen
   → GPIO 3 für RX-Daten

5. Init() — Viproz Fork
   → Gibt bool zurück (kein Hang!)
   → Timeout-geschützter Reset
   → SpiEnd() vor RegConfig

6. getCC1101() — SPI-Verbindung prüfen
   → Liest VERSION-Register
   → Erwartet > 0x00

7. Chip-Verifizierung
   → PARTNUM + VERSION lesen
   → CC1101: VERSION = 0x14

8. configureForRx()
   → ASK/OOK, 812kHz BW, kein Sync

9. SetRx(433.92)
   → MARCSTATE = 0x0D (RX) bestätigen

10. FreeRTOS Task starten
    → rfTask(), 4096 Bytes Stack, Priority 1
```

**`configureForRx()` — Empfangskonfiguration:**
```cpp
void RFManager::configureForRx() {
    ELECHOUSE_cc1101.setModulation(2);      // ASK/OOK
    ELECHOUSE_cc1101.setRxBW(812.50);       // Maximum Bandbreite
    ELECHOUSE_cc1101.setDRate(9.6);         // ~10 kBaud
    ELECHOUSE_cc1101.setSyncMode(0);        // Kein Sync-Word
    ELECHOUSE_cc1101.setCrc(0);             // Kein CRC
    ELECHOUSE_cc1101.setLengthConfig(2);    // Infinite packet length
    ELECHOUSE_cc1101.setPktFormat(0);       // Normal/FIFO
    ELECHOUSE_cc1101.setDcFilterOff(0);     // DC-Filter an
    ELECHOUSE_cc1101.setManchester(0);      // Kein Manchester
    ELECHOUSE_cc1101.setFEC(0);             // Kein FEC
    ELECHOUSE_cc1101.setPQT(0);             // Kein Preamble-Threshold
    ELECHOUSE_cc1101.setAppendStatus(1);    // RSSI + LQI anhängen
}
```

### 5.4 `config/hardware_breadboard.h` — Pin-Dokumentation

Aktualisierte Kommentare mit korrektem 8-Pin-Header-Pinout des E07-400MBL-01:
```cpp
// ===== E07-400MBL-01 (CC1101 Breakout, direct SPI) =====
// Module pinout (8-pin header):
//   1.VCC (3.3V), 2.GND, 3.MOSI, 4.SCK, 5.MISO, 6.GDO2, 7.GDO0, 8.CSn
```

### 5.5 `src/main.cpp` — Entry Point

Aktualisierter Header-Kommentar und Startup-Banner:
```cpp
/*
 * Linkra Duo A0.1 - RF Gateway
 * E07-400MBL-01 (CC1101) via SPI
 * Using SmartRC-CC1101-Driver-Lib for reliable SPI communication
 */
```

Entfernt: Alle M0/M1 Moduswahl-Referenzen (nicht benötigt für SPI-Betrieb).

---

## 6. Aktueller Status & Verifizierung

### Serial-Output nach erfolgreichem Flash

```
╔═══════════════════════════════════════════════════╗
║   Linkra Duo A0.1 - CC1101 RF Gateway (SPI)      ║
╚═══════════════════════════════════════════════════╝

Module: E07-400MBL-01 (CC1101 breakout, direct SPI)

[RF] ===== CC1101 Init (Viproz SmartRC Fork) =====
[RF] Library has MISO timeout + Init() fix for ESP32-S3
[RF] --- Pin State Diagnostic ---
[RF] GPIO 13 (MISO) = 0 (expect: floating/LOW)     ← ✅ MISO nicht mehr stuck HIGH
[RF] GPIO 11 (MOSI) = 0
[RF] GPIO 12 (SCK)  = 0
[RF] GPIO 10 (CS)   = 1
[RF] GPIO  3 (GDO0) = 1
[RF] --- Manual CS Toggle Test ---
[RF] MISO went LOW after 0 µs ✓                     ← ✅ Sofortige Antwort!
[RF] MISO: before_CS=0, after_CS=0
[RF] SPI Pins: SCK=12, MISO=13, MOSI=11, CS=10
[RF] GDO0 on GPIO 3
[RF] Calling Init() (with timeout protection)...
[RF] ✓ Init() succeeded!                            ← ✅ Kein Hang mehr!
[RF] ✓ CC1101 SPI connection verified!               ← ✅ VERSION > 0
[RF] PARTNUM=0x00, VERSION=0x14                       ← ✅ Echter CC1101!
[RF] ✓ CC1101 confirmed (VERSION=0x14)
[RF] Configuring CC1101 for RX (SmartRC)...
[RF] Configuration complete (ASK/OOK, 812kHz BW, no sync)
[RF] MARCSTATE after SetRx: 0x0D (RX) ✓             ← ✅ Im Empfangsmodus!
[RF] ===== Init Complete =====

✓ System ready - Listening for RF signals...

[RF] === SIGNAL #1 ===
[RF] Len: 61, RSSI: -50 dBm, Freq: 433.92 MHz       ← ✅ Signale werden empfangen!
```

### Verifikations-Checkliste

| Test | Ergebnis | Wert |
|------|----------|------|
| MISO nach CS LOW | ✅ LOW | 0 µs Reaktionszeit |
| Init() blockiert? | ✅ Nein | Sofort returned |
| Init() Rückgabe | ✅ true | — |
| getCC1101() | ✅ true | VERSION > 0 |
| PARTNUM | ✅ 0x00 | CC1101 Standard |
| VERSION | ✅ 0x14 | CC1101 Chip Rev |
| MARCSTATE | ✅ 0x0D | RX-Modus aktiv |
| RF-Empfang | ✅ Ja | RSSI: -50 dBm |
| Protokoll-Erkennung | ✅ Aktiv | EV1527, RAW |
| System-Stabilität | ✅ Kein Crash | FreeRTOS Task läuft |

---

## 7. Hardware-Referenz

### ESP32-S3 SPI-Pins (FSPI)

Der ESP32-S3 hat zwei verfügbare SPI-Busse: **FSPI** (SPI2) und **HSPI** (SPI3). Die Default-Pins für FSPI:

| Signal | ESP32 (classic) | ESP32-S3 (FSPI) | Unser Setup |
|--------|-----------------|------------------|-------------|
| SCK | GPIO 18 | GPIO 12 | GPIO 12 ✓ |
| MISO | GPIO 19 | GPIO 13 | GPIO 13 ✓ |
| MOSI | GPIO 23 | GPIO 11 | GPIO 11 ✓ |
| SS | GPIO 5 | GPIO 10 | GPIO 10 ✓ |

> Die originale SmartRC-Library hat die **ESP32 (classic)** Pins hardcoded. Deshalb funktionierte `setSpiPin()` zwar zum Setzen der Variablen, aber interne Funktionen verwendeten trotzdem die falschen Pins.

### E07-400MBL-01 Blockdiagramm

```
┌──────────────────────────────────────────┐
│ E07-400MBL-01 Evaluation Board           │
│                                          │
│ ┌──────────┐    SPI    ┌──────────────┐  │
│ │  STM8    │◄─────────►│ E07-400M10S  │  │
│ │  MCU     │           │ (CC1101)     │  │
│ │          │           │              │  │
│ └────┬─────┘           └──────────────┘  │
│      │                                   │
│ ─────┼─── Pin Header (20-Pin) ─────────  │
└──────┼───────────────────────────────────┘
       │
    SPI-Durchgriff
    (Default-Modus)
       │
    ESP32-S3
```

---

## 8. Bekannte Issues & Hinweise

### 8.1 Library-Version

Der Viproz Fork basiert auf SmartRC v2.5.7 + 9 Commits. Er wird direkt von GitHub bezogen:
```ini
https://github.com/Viproz/SmartRC-CC1101-Driver-Lib.git
```

**Risiko:** Kein Versions-Pinning. Bei Breaking Changes im Fork könnte der Build brechen.  
**Empfehlung:** Bei stabilem Betrieb auf einen bestimmten Commit pinnen:
```ini
https://github.com/Viproz/SmartRC-CC1101-Driver-Lib.git#3664897
```

### 8.2 STM8 MCU auf E07-400MBL-01

Das Evaluation Board hat einen **STM8 Mikrocontroller** der im Default-Modus den SPI-Bus zum CC1101 durchreicht. Sollte die SPI-Kommunikation aus Hardware-Gründen fehlschlagen:

1. **Pin 6 (REST) → GND** verbinden → Hält STM8 in Reset (High-Z auf SPI)
2. **Alternativ:** Direkt das **E07-400M10S** SMD-Modul verwenden (kein MCU)

### 8.3 Empfangs-Konfiguration

Aktuelle Einstellungen optimiert für **breitbandigen 433 MHz Empfang**:

| Parameter | Wert | Beschreibung |
|-----------|------|-------------|
| Modulation | ASK/OOK (2) | Standard für 433 MHz Fernbedienungen |
| RX Bandwidth | 812.5 kHz | Maximum — fängt alles auf |
| Data Rate | 9.6 kBaud | Guter Kompromiss |
| Sync Mode | 0 (Keine) | Alles empfangen |
| CRC | Deaktiviert | Keine Pakete verwerfen |
| Packet Length | Infinite | Rohdaten aufzeichnen |

### 8.4 Backup

Die originale rf_manager.cpp (vor SmartRC-Umstellung) liegt als Backup:
```
src/rf/rf_manager.cpp.bak
```

---

## Geänderte Dateien — Übersicht

| Datei | Änderung |
|-------|---------|
| `platformio.ini` | `lib_deps`: LSatan → Viproz Fork (GitHub URL) |
| `src/rf/rf_manager.h` | Komplett neu: SmartRC API statt Raw-SPI |
| `src/rf/rf_manager.cpp` | Komplett neu: Viproz Init(), GPIO-Diagnose, STM8-Warnung |
| `src/main.cpp` | Header-Kommentar aktualisiert, M0/M1 Referenzen entfernt |
| `config/hardware_breadboard.h` | Pin-Dokumentation für E07-400MBL-01 aktualisiert |
| `src/rf/rf_manager.cpp.bak` | Backup der originalen Version |

---

*Dokumentation erstellt am 7. Februar 2026*  
*Linkra Duo A0.1 — CC1101 RF Gateway*
