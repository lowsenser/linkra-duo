<p align="center">
  <img src="assets/brand/LINKRA_DUO_produkt_schwarz.png" alt="Linkra Duo" width="320">
</p>

# Linkra Duo – RF-Gateway für 433 und 868 MHz

> **English summary:** Linkra Duo is an open-source (GPL-3.0) RF gateway built on an
> ESP32-S3 with two CC1101 transceivers. It receives 433 MHz and 868 MHz sensor
> telegrams, decodes them with a port of [rtl_433](https://github.com/merbanan/rtl_433)
> (377 decoders) and Wireless M-Bus (T1/C1, incl. unencrypted Techem meters), keeps a
> device database (SQLite on LittleFS), shows everything in a Preact web UI and publishes
> released devices to Home Assistant via MQTT auto-discovery. Status: **beta (0.6.0-beta)**,
> breadboard hardware, web UI and API protected by login with optional TOTP 2FA.
> Documentation is in German.
> **Only receive and forward your own devices** – listening to other people's radio
> traffic may be illegal where you live.

**Status:** Firmware `0.6.0-beta`, Hardware-Revision A0.1 (Breadboard). Das Projekt ist in
aktiver Entwicklung und **nicht** für den produktiven Einsatz gedacht – siehe
[Bekannte Einschränkungen](#bekannte-einschränkungen).

## Was ist Linkra Duo?

Ein kleines Gateway, das Funktelegramme von Wettersensoren, Thermometern,
Fernbedienungen und Verbrauchszählern empfängt, dekodiert und im Heimnetz bereitstellt –
lokal, ohne Cloud. Zwei getrennte Funkmodule hören gleichzeitig auf 433 MHz (OOK/FSK)
und 868 MHz (FSK, wM-Bus).

## Funktionen (Stand 0.6.0-beta)

| Bereich | Stand |
|---|---|
| 433-MHz-Empfang | CC1101 #1, OOK/FSK, Erfassung per RMT-Hardware, Störpulsfilter, Empfangsprofile mit Rotation (433,92 / 433,42 MHz u. a.) |
| 868-MHz-Empfang | CC1101 #2, FSK (8/17/38 kbit/s) und OOK, Empfangsprofile mit Rotation |
| Decoder | rtl_433-Portierung mit 377 Decodern (Upstream-Stand `f0a8156`) |
| Wireless M-Bus | T1/C1 (Frame A/B, CRC), Techem FHKV III, MK Radio 3/4, Compact V, Vario 451, TSD2 – nur **unverschlüsselte** Telegramme |
| Gerätedatenbank | SQLite auf LittleFS; neue Geräte erst nach zweitem Empfang innerhalb 15 min (Phantom-Filter); Reparatur beschädigter Datenbanken |
| Weboberfläche | Preact + Tailwind: Übersicht, Live-Ansicht, Geräte, Funk/Profile, MQTT, System, Standard- und Expertenmodus |
| Home Assistant | MQTT mit Auto-Discovery, nur vom Nutzer freigegebene Geräte (Temperatur, Feuchte, Batterie, Signalstärke, zuletzt gesehen) |
| Werkzeuge | Roh-Mitschnitt im rtl_433-Pulsformat, Wetterstations-Suche, A/B-Tuning (`scripts/tune_matrix.py`) |

### Bekannte Einschränkungen

- **Nur HTTP:** Web-UI und API sind durch Anmeldung (optional mit 2FA) geschützt, laufen
  aber unverschlüsselt. Nur in einem vertrauenswürdigen Netz betreiben, nicht aus dem
  Internet erreichbar machen.
- **Setup-Hotspot:** Scheitert die WLAN-Verbindung, startet der Hotspot `Linkra-Duo-Setup`
  (WPA2, Passwort pro Gerät, steht im seriellen Log; Web-UI unter `http://192.168.4.1`).
- **Firmware-Updates** laufen über das Netz mit automatischem Rollback
  (`scripts/ota_update.py` oder Weboberfläche). Geräte mit 0.6.x ziehen einmalig per USB
  auf die OTA-Aufteilung um (siehe [`docs/FLASHING.md`](docs/FLASHING.md), Abschnitt 6).
- **WLAN-Zugangsdaten** werden beim Bauen aus `config/secrets.h` übernommen.
- **Nur Breadboard-Hardware:** Die Umgebung `linkra-s3-pcb` ist nicht gepflegt und baut
  derzeit nicht.
- Verschlüsselte wM-Bus-Telegramme (AES, Mode 5/7) werden nicht entschlüsselt.

## Rechtlicher Hinweis

Linkra Duo ist dafür gedacht, **die eigenen** Funksensoren und Zähler zu empfangen und
weiterzugeben. Das Abhören und Weitergeben fremder Funksignale (z. B. Zähler der
Nachbarn) kann je nach Land verboten sein – in Deutschland u. a. nach § 5 TDDDG
(früher § 89 TKG) und nach dem Datenschutzrecht. Nutze die Gerätefreigabe so, dass nur
deine eigenen Geräte gespeichert und weitergeleitet werden. Dies ist keine Rechtsberatung.

## Hardware

| Bauteil | Typ |
|---|---|
| Mikrocontroller | ESP32-S3 DevKitC-1 **N16R8** (16 MB Flash, 8 MB Octal-PSRAM) |
| Funkmodul 433 MHz | CC1101, z. B. EBYTE **E07-400MBL-01** (433-MHz-Variante), OOK |
| Funkmodul 868 MHz | CC1101-Modul für 868 MHz, FSK |
| Antennen | je eine passende Antenne für 433 bzw. 868 MHz |

Datenblatt und Handbuch des E07-400MBL-01 gibt es beim Hersteller:
<https://www.cdebyte.com/> (Produktsuche „E07-400MBL-01“). Das Handbuch ist aus
urheberrechtlichen Gründen nicht im Repository enthalten.

### Pinbelegung (Breadboard, `platformio.ini` / `config/hardware_breadboard.h`)

Beide Module haben einen **eigenen** SPI-Bus.

| Signal | CC1101 #1 (433 MHz) | CC1101 #2 (868 MHz) |
|---|---|---|
| CSn | GPIO 4 | GPIO 10 |
| SCK | GPIO 12 | GPIO 18 |
| MISO | GPIO 13 | GPIO 7 |
| MOSI | GPIO 11 | GPIO 17 |
| GDO0 (Daten/Interrupt) | GPIO 3 | GPIO 6 |
| VCC / GND | 3,3 V / GND | 3,3 V / GND |

Hinweis zum E07-400MBL-01: Pin 6 (GDO2) läuft über den STM8 des Moduls und ist nicht
nutzbar; verwendet wird **Pin 7 (GDO0)**. Die LED-Pins 35–37 in `hardware_breadboard.h`
kollidieren beim N16R8 mit dem Octal-PSRAM und werden nicht benutzt.

## Bauen

Voraussetzungen: [PlatformIO Core](https://platformio.org/) (CLI oder VS-Code-Erweiterung),
Node.js ≥ 18, Python ≥ 3.9.

```bash
# 1. WLAN-Zugangsdaten
cp config/secrets.example.h config/secrets.h      # dann SSID/Passwort eintragen (wird nicht eingecheckt)

# 2. Firmware bauen
pio run -e linkra-s3-breadboard
#   -> .pio/build/linkra-s3-breadboard/firmware.bin

# 3. Weboberfläche bauen (Ausgabe nach data/)
cd web && npm ci && npm run build && cd ..

# 4. Python-Werkzeuge (esptool, pyserial)
python3 -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt
```

### Tests

```bash
# Web
node --test web/tests

# Host-Tests der Firmware-Logik (ASan/UBSan)
c++ -std=c++17 -fsanitize=address,undefined -I src tests/wmbus_test.cpp src/rf/wmbus.cpp -o /tmp/wmbus_test && /tmp/wmbus_test
c++ -std=c++17 -fsanitize=address,undefined tests/rf_event_gate_test.cpp -o /tmp/gate_test && /tmp/gate_test
c++ -std=c++17 -fsanitize=address,undefined -I .pio/libdeps/linkra-s3-breadboard/ArduinoJson/src \
    tests/ha_discovery_test.cpp src/integration/ha_discovery.cpp -o /tmp/ha_test && /tmp/ha_test
```

## Flashen und Web-Deploy

> [!CAUTION]
> **Niemals `pio run -t uploadfs`** verwenden. Das überschreibt das komplette LittleFS –
> inklusive Gerätedatenbank und Einstellungen. Auch `pio run -t upload` ist auf einem
> eingerichteten Gerät nicht empfohlen (schreibt zusätzlich in den NVS-Bereich).

Kurzfassung (Details, Erstinstallation und Wiederherstellung in
[`docs/FLASHING.md`](docs/FLASHING.md)):

```bash
# Firmware über das Netz (OTA mit automatischem Rollback):
python3 scripts/ota_update.py linkra-duo.local

# Weboberfläche über das Netz (lässt Datenbank und Einstellungen unberührt):
LINKRA_HOST=linkra-duo.local python3 scripts/deploy_web.py
```

Nach dem Start ist das Gerät unter `http://linkra-duo.local` (mDNS) bzw. der vom Router
vergebenen IP erreichbar.

## Home Assistant (MQTT)

1. In Home Assistant einen MQTT-Broker einrichten (z. B. Add-on „Mosquitto broker“) und
   die MQTT-Integration aktivieren.
2. In der Linkra-Weboberfläche unter **MQTT** Broker-Adresse, Port, Benutzer und Passwort
   eintragen und aktivieren.
3. Unter **MQTT** oder **Geräte** die gewünschten gespeicherten Sensoren freigeben. Nur
   freigegebene Geräte werden veröffentlicht; sie erscheinen in Home Assistant unter
   *Einstellungen → Geräte & Dienste → MQTT*.

Topics (Standardwerte):

| Topic | Inhalt |
|---|---|
| `linkra/status` | `online` / `offline` (retained, Last Will) |
| `linkra/sensor/<gerät>/state` | JSON mit Messwerten je Gerät |
| `homeassistant/<komponente>/<knoten>/<gerät>_<wert>/config` | Auto-Discovery (retained) |

Die frühere Matter-Anbindung über einen ESP32-C6 wurde mit 0.6.0-beta entfernt.

## Anmeldung

Beim ersten Aufruf der Web-UI wird ein Benutzerkonto angelegt. Danach sind Web-UI und API
nur angemeldet erreichbar (Sitzungs-Cookie, Schutz vor fremden Webseiten, Sperre nach
Fehlversuchen). Optional: Zwei-Faktor-Authentifizierung per Authenticator-App (TOTP) mit
10 Wiederherstellungscodes, einzurichten unter Einstellungen → Konto.

Passwort vergessen: **innerhalb der ersten 60 Sekunden nach dem Einschalten** die BOOT-Taste
10 Sekunden gedrückt halten. Das löscht nur die Anmeldedaten; Geräte, WLAN und MQTT bleiben.
Details: [`docs/auth-api.md`](docs/auth-api.md).

## Projektstruktur

```
├── src/                 Firmware (Arduino auf ESP-IDF, PlatformIO)
│   ├── core/            Start, Event-Bus, Konfiguration, Version
│   ├── rf/              Funk: CC1101-Verwaltung, Empfangsprofile, 433-RMT-Erfassung, wM-Bus
│   ├── storage/         Gerätedatenbank (SQLite auf LittleFS)
│   ├── integration/     MQTT und Home-Assistant-Discovery
│   ├── network/         WLAN, Webserver, REST-API, SSE
│   └── hardware/        LEDs, SPI
├── lib/rtl_433_ESP_433/ angepasste rtl_433_ESP-Portierung mit rtl_433-Decodern
├── patches/             Entwurf einer Dual-CC1101-Erweiterung (derzeit nicht eingebunden)
├── config/              Pinbelegung je Hardware, secrets.example.h
├── partitions/          Partitionstabelle (16 MB)
├── web/                 Weboberfläche (Vite + Preact + Tailwind), Tests in web/tests
├── data/                gebaute Weboberfläche (wird per scripts/deploy_web.py hochgeladen)
├── tests/               Host-Tests (C++)
├── scripts/             ota_update.py, deploy_web.py, migrate_partitions.py, build_web.py, tune_matrix.py
├── docs/                FLASHING.md, auth-api.md, Prüfbericht, Archiv
└── assets/brand/        Logos
```

## Mitmachen, Sicherheit, Änderungen

- [CONTRIBUTING.md](CONTRIBUTING.md) – wie du beitragen kannst
- [SECURITY.md](SECURITY.md) – Sicherheitslücken melden
- [CHANGELOG.md](CHANGELOG.md) – Versionshistorie

## Lizenz

Linkra Duo steht unter der **GNU General Public License v3.0** – siehe [`LICENSE`](LICENSE).
Die Firmware enthält rtl_433 (GPL-2.0-or-later) und rtl_433_ESP (GPL-3.0); alle
Fremdkomponenten mit Lizenz und Quelle stehen in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
