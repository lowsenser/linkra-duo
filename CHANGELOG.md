# Changelog

Alle nennenswerten Änderungen an Linkra Duo. Format angelehnt an
[Keep a Changelog](https://keepachangelog.com/de/1.1.0/), Versionen nach
[Semantic Versioning](https://semver.org/lang/de/). Die Hardware-Revision (A0.1 = Breadboard)
wird getrennt von der Firmware-Version geführt.

## 0.6.0-beta – 2026-11-24

Erstes öffentliches Release.

### Hinzugefügt
- **Anmeldung**: Konto beim ersten Aufruf, PBKDF2-Passwort, Sitzungs-Cookie, CSRF-Schutz,
  Sperre nach Fehlversuchen, optional **TOTP-2FA** mit Wiederherstellungscodes; Zugang per
  BOOT-Taste zurücksetzen (`docs/auth-api.md`). Weboberfläche mit Einrichtungsassistent,
  Anmeldeseite und Reiter „Konto“; `deploy_web.py` meldet sich an.
- Absturzrekorder: letzter Panic im RTC-Speicher, `GET /api/diag/crash` (angemeldet).
- `LICENSE` (GPL-3.0), `THIRD_PARTY_NOTICES.md`, `docs/FLASHING.md`, `requirements.txt`,
  `CONTRIBUTING.md`, `SECURITY.md`, Issue-Vorlagen und CI-Workflow.
- Home Assistant per **MQTT mit Auto-Discovery**: eigener MQTT-Task (ESP-MQTT),
  Warteschlange im PSRAM, Last Will (`linkra/status`), Neuveröffentlichung beim
  Home-Assistant-Neustart.
- Nur vom Nutzer freigegebene Geräte werden veröffentlicht; Löschen eines Geräts entfernt
  auch die Discovery-Einträge.
- API: `GET/POST /api/mqtt`, `GET /api/mqtt/devices`, `POST /api/mqtt/device`.
- Weboberfläche: Reiter „MQTT“.
- Host-Test `tests/ha_discovery_test.cpp` (ASan/UBSan).

### Entfernt
- ESP32-C6 / Matter-over-Thread-Bridge samt C6-Firmware, C6-Routen, C6-Pins und
  C6-Skripten; Matter-Seite in der Weboberfläche.
- Dateisystem-Update über `/api/ota` (überschrieb die Gerätedatenbank).
- Veraltete Skripte und Dokumente, Herstellerhandbuch-PDF, persönliche Editor-Konfiguration.

### Geändert
- Weboberfläche: zentraler Abfrage-Scheduler mit Backoff, Live-Verbindung schließt im
  Hintergrund-Tab, Fehlerabfang, Banner bei Verbindungsverlust, Diagramm- und QR-Bibliothek
  werden nur bei Bedarf geladen, Barrierefreiheit (Kontrast, Tastatur, ARIA).
- Statische Auslieferung nur noch `index.html` und `/assets/*`; Datei-Upload nur angemeldet.
- WLAN: Ländercode DE; NTP bei jeder Verbindung.
- SQLite-Bibliothek als gepatchte Kopie unter `lib/Sqlite3Esp32`.
- Schlüssel-Caches der Gerätedatenbank unter Mutex.
- README neu geschrieben; Lizenzangabe korrigiert (vorher fälschlich MIT).

### Behoben
- wM-Bus: Pufferüberlauf bei Frame B mit L = 128/129 (Absturz per Funk-Telegramm), weitere
  Grenzprüfungen im Parser; Regressions- und Fuzz-Tests.
- rtl_433: Bitpuffer nach jedem Dekodierereignis leeren; M-Bus Mode R (Absturzpfad) deaktiviert.
- Absturz beim Start: SQLite-VFS überlief den `tmpnam`-Puffer (ab ca. 24 Geräten); Start-Task
  fest auf Kern 1 (Stack-Überlauf der IPC-Task beim Einrichten des 868-Interrupts).
- MQTT-API: spezifische Routen werden vor `/api/mqtt` registriert.

## 0.5.0-beta – 2026-11-22

Erste Version, in der alle Kernfunktionen durchgängig laufen.

### Hinzugefügt
- **Empfangsprofile** mit Rotation für beide CC1101 (433 OOK/FSK, 868 FSK 8/17/38 kbit/s,
  868 OOK), Scheduler-Priorität über den Decodern, feste Verweilzeiten; Profil 433,42 MHz
  (Somfy RTS).
- **rtl_433-Decoder** auf Upstream-Stand `f0a8156` (377 statt 276 Decoder).
- **Wireless M-Bus** T1/C1 im CC1101-Paketmodus; Techem FHKV III, MK Radio 3/4, Compact V,
  Vario 451, TSD2 (unverschlüsselt).
- **Gerätedatenbank** (SQLite auf LittleFS) mit Erkennung und Rettung beschädigter Dateien;
  neue Geräte werden erst nach zweitem Empfang innerhalb 15 min bestätigt (Phantom-Filter).
- Roh-Mitschnitt `/api/rf/raw` im rtl_433-Pulsformat, neutrale Klassifizierungs-Labels.
- Wetterstations-Suche auf dem Gerät mit Plausibilitätsfilter und Spitzenpegel.
- Tuning-Werkzeuge: `/api/rf/tune`, FREQEST/SNR je Telegramm, `scripts/tune_matrix.py`.
- CC1101-Register-Wächter (prüft und korrigiert die Profilregister zyklisch).
- Neue Weboberfläche (Preact + Tailwind) mit Standard- und Expertenmodus,
  Einstellungsseite, Wasserfall, 5-Hz-Pegel; Design-Variante „Konsole“ als Standard.
- Matter-over-Thread-Bridge über ESP32-C6 (in 0.6.0-beta wieder entfernt).

### Geändert
- 433-MHz-Erfassung per **RMT-Hardware** statt Interrupt, Paketende nach 10 ms Ruhe,
  Störpulsfilter; Parallelvergleich ISR/RMT.
- 433-OOK: Vorlaufpuffer, Mindestlänge 3 ms, Schwelle +6 dB; einheitliche Empfindlichkeit
  für beide Module.
- Getrennte Decoder-Warteschlangen (868 mit Vorrang), Breitenprüfung vor jedem Decoder.
- Weniger Flash-Schreibzugriffe der Datenbank.
- Secplus-Decoder deaktiviert.

### Behoben
- 868-FSK: Telegrammanfang ging verloren (Vorlaufpuffer im Interrupt); Empfangsende erst
  nach 3 ms unter der Schwelle.
- 433-Empfang: Puffer wurden vor der Übergabe überschrieben; Heap-Hänger im 433-Pfad.
- Diverse Speicherfehler.

## A0.1 – Februar 2026

Ausgangsstand auf dem Breadboard (ESP32-S3 + zwei CC1101, ESP32-C6 für Matter).
