> **Archiv (historisch, Stand Februar 2026).** Pins, Decoderzahlen und offene Punkte sind veraltet. Aktueller Stand: [README](../../README.md), [CHANGELOG](../../CHANGELOG.md).

# Linkra DUO – Architektur & Status (Stand: Phase 2 Complete)

## 1. Hardware-Architektur

| Modul | Frequenz | Modulation | SPI-Bus | CS | GDO0 | SCK | MISO | MOSI |
|-------|----------|------------|---------|-----|------|-----|------|------|
| CC1101 #1 | 433.92 MHz | OOK | SPI2 (FSPI) | GPIO 4 | GPIO 14 | GPIO 12 | GPIO 13 | GPIO 11 |
| CC1101 #2 | 868.35 MHz | FSK | SPI3 (HSPI) | GPIO 10 | GPIO 6 | GPIO 18 | GPIO 7 | GPIO 17 |

- **MCU:** ESP32-S3 DevKitC-1 (N16R8) — 16MB Flash, 8MB PSRAM
- **Paralleler Empfang:** Beide Module empfangen gleichzeitig, kein Time-Division
- **C6 Co-Prozessor:** UART auf GPIO 15/16 (verschoben wegen CC1101 #2)

---

## 2. Software-Architektur (Dual-Receiver, Shared-Decoder)

### Kernprinzip: EINE Bibliothek, ZWEI Empfänger, EIN Decoder

```
CC1101 #1 (433 MHz OOK)          CC1101 #2 (868 MHz FSK)
        │                                  │
  rtl_433_ESP Library              RFManager::rx868*
  (initReceiver, interrupt,       (eigener ISR, SPI3,
   receiver task, loop)            receiver task)
        │                                  │
        └──── processSignal() ─────────────┘
                     │
              rtl_433_Queue
                     │
           rtl_433_DecoderTask
           (run_ook_demods + run_fsk_demods)
                     │
             rtl433Callback()
                     │
          RFManager::processSignal(msg, is868)
                     │
              EventBus → SSE → Web-UI
```

### Warum dieser Ansatz?

**Problem:** Zwei separate Bibliotheks-Kopien (`lib/rtl_433_ESP_433` + `lib/rtl_433_ESP_868`)
verursachten hunderte Linker-Konflikte durch doppelte rtl_433 C-Core-Symbole
(`bitbuffer_clear`, `pulse_slicer_pcm`, alle 275 `r_device`-Structs, etc.).

**Lösung:**
- **Eine** rtl_433_ESP-Bibliothek (`lib/rtl_433_ESP_433/`) mit dem vollständigen C-Core
- 433 MHz nutzt die Bibliothek wie vorgesehen (initReceiver → interrupt → receiver task → decoder)
- 868 MHz hat einen **eigenen Empfänger** in `rf_manager.cpp` (gleiche Interrupt-Pattern wie die Bibliothek)
- Beide Module speisen Signale in **dieselbe Decoder-Queue**
- Der Decoder-Task läuft **beide** Demod-Pipelines (OOK + FSK) auf jedes Signal

### Band-Erkennung

Jedes `pulse_data_t` wird mit `freq1_hz` getaggt:
- 433 MHz: `freq1_hz = 0` (Standard, von Bibliothek nicht gesetzt)
- 868 MHz: `freq1_hz = 868350000.0` (explizit gesetzt in rx868ReceiverTask)

In `signalDecoder.cpp` wird vor dem Decoder-Lauf `currentSignalIs868` gesetzt:
```cpp
currentSignalIs868 = (rtl_pulses->freq1_hz > 500e6f);
```
Der Callback liest diesen Flag und routet das Signal korrekt.

---

## 3. Dateistruktur (relevante Änderungen)

| Datei | Status | Beschreibung |
|-------|--------|-------------|
| `lib/rtl_433_ESP_433/` | ✅ Einzige lib | Vollständige rtl_433_ESP mit include/ + src/ + rtl_433/ |
| `lib/rtl_433_ESP_868/` | ❌ Gelöscht | War Duplikat → Linker-Konflikte |
| `lib/rtl_433_ESP_433/include/` | ✅ Neu | Headers aus Upstream kopiert (pulse_data.h, r_api.h etc.) |
| `lib/rtl_433_ESP_433/src/signalDecoder.cpp` | ✅ Modifiziert | Läuft OOK+FSK-Demods auf jedes Signal, `currentSignalIs868` Flag |
| `lib/rtl_433_ESP_433/src/signalDecoder.h` | ✅ Modifiziert | Exportiert `currentSignalIs868` |
| `src/rf/rf_manager.h` | ✅ Neu | Dual-Modul mit eigenem 868-Receiver (ISR, Task, Pulse-Buffer) |
| `src/rf/rf_manager.cpp` | ✅ Neu | 433 via rtl_433_ESP, 868 via eigenen Receiver → shared decoder |
| `src/rf/rf_manager_dual.cpp` | ❌ Gelöscht | War veraltet |
| `platformio.ini` | ✅ Fixiert | Git-Dep entfernt, lokale Lib ist self-contained |

---

## 4. Build-Status

```
RAM:   [==        ]  21.0% (used 68784 bytes from 327680 bytes)
Flash: [==        ]  23.7% (used 1988155 bytes from 8388608 bytes)
========================= [SUCCESS] =========================
```

---

## 5. Verbleibende Aufgaben

### Sofort (Phase 3)
- [ ] Hardware-Test: Zweiten CC1101 anschließen, Empfang verifizieren
- [ ] 868 MHz FSK-Signal testen (Bresser 5-in-1, Somfy iohc, M-Bus)
- [ ] RSSI-Kalibrierung für 868 MHz optimieren
- [ ] Stack-Watermark der rx868ReceiverTask überwachen

### Frontend
- [ ] Web-UI zeigt bereits 433/868 Status-Cards — funktionale Tests mit echten Signalen
- [ ] Signal-Liste: Band-Filter (433/868) hinzufügen
- [ ] Debug-Panel: 868 RSSI-Graph, Pulse-Counter

### Optimierung
- [ ] Decoder-Split: OOK-Decoder nur für 433-Signale, FSK-Decoder nur für 868-Signale
  (aktuell laufen beide Demods auf jedes Signal — korrekt aber CPU-intensiver)
- [ ] `RECEIVER_BUFFER_SIZE` und Stack-Größen tunen nach Real-World-Test
- [ ] Deaf-Workaround für 868 MHz validieren (stündlicher Receiver-Reset)

### Zukunft
- [ ] Matter Bridge (C6 Co-Prozessor)
- [ ] Smart-Tracking: Gelernte Geräte-Intervalle für präventives Frequency-Hopping
- [ ] PCB-Version: Pin-Mapping aus `config/hardware_pcb.h` validieren
