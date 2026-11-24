# Third-Party Notices / Fremdkomponenten

Linkra Duo steht unter der **GNU General Public License v3.0** (siehe [`LICENSE`](LICENSE)).
Die Firmware bindet die unten aufgeführten Fremdkomponenten ein. Ihre Lizenzen bleiben
unberührt; die vollständigen Lizenztexte liegen jeweils im Quellpaket der Komponente
(Pfad bzw. Link in der Tabelle).

Die Versionen entsprechen dem Stand, mit dem die Firmware 0.6.0-beta gebaut wurde
(aufgelöst in `.pio/libdeps/linkra-s3-breadboard` bzw. `web/package-lock.json`).
Die Lizenzangaben wurden in den Paketen selbst nachgesehen (`LICENSE`-Datei,
`library.json`/`library.properties`, `package.json`, Dateiköpfe).

## 1. Firmware (ESP32-S3)

### Im Repository enthalten

| Komponente | Version / Stand | Lizenz | Quelle | Pfad im Repo |
|---|---|---|---|---|
| **rtl_433** (Decoder, Bit-/Pulsverarbeitung) | Decoder auf Upstream-Stand `f0a8156`, angepasst | GPL-2.0-or-later (Dateiköpfe: „either version 2 of the License, or (at your option) any later version“) | https://github.com/merbanan/rtl_433 | `lib/rtl_433_ESP_433/src/rtl_433/`, `lib/rtl_433_ESP_433/include/` |
| **rtl_433_ESP** (ESP32-Portierung) | Basis 0.5.0, für zwei CC1101 und Linkra angepasst | GPL-3.0 laut `library.json` und Upstream-`LICENSE`; einzelne Dateiköpfe nennen LGPL-3.0-or-later | https://github.com/NorthernMan54/rtl_433_ESP | `lib/rtl_433_ESP_433/src/*.cpp/.h`, `patches/rtl_433_ESP_dual_spi.h` |

rtl_433 (GPL-2.0-or-later) wird hier unter GPL-3.0 weitergegeben, wie es die
„or any later version“-Klausel erlaubt. Die Linkra-Änderungen an beiden Komponenten
stehen ebenfalls unter GPL-3.0.

### Über PlatformIO bezogen (`platformio.ini`, `lib_deps`)

| Komponente | Version | Lizenz | Quelle | Lizenztext |
|---|---|---|---|---|
| ArduinoJson | 7.4.2 | MIT | https://github.com/bblanchon/ArduinoJson | `LICENSE.txt` |
| ESPAsyncWebServer (ESP32Async) | 3.9.6 | LGPL-3.0 | https://github.com/ESP32Async/ESPAsyncWebServer | `LICENSE` |
| AsyncTCP (ESP32Async) | 3.4.10 | LGPL-3.0 | https://github.com/ESP32Async/AsyncTCP | `LICENSE` |
| esp32_arduino_sqlite3_lib (Sqlite3Esp32) | 2.5 | Apache-2.0 (Bibliothek) | https://github.com/siara-cc/esp32_arduino_sqlite3_lib | `LICENSE` |
| ↳ SQLite (darin enthalten) | 3.43.2 | Public Domain (SQLite-„blessing“) | https://www.sqlite.org/copyright.html | Kopf von `src/sqlite3.c` |
| RadioLib (transitiv über rtl_433_ESP) | 7.5.0 | MIT | https://github.com/jgromes/RadioLib | `license.txt` |

### Plattform, Framework und SDK

| Komponente | Version | Lizenz | Quelle |
|---|---|---|---|
| pioarduino platform-espressif32 | 55.03.36 | Apache-2.0 | https://github.com/pioarduino/platform-espressif32 |
| Arduino-ESP32-Core | 3.3.6 | LGPL-2.1-or-later | https://github.com/espressif/arduino-esp32 |
| ESP-IDF (vorkompiliert in `framework-arduinoespressif32-libs`) | 5.5.2 | Apache-2.0 | https://github.com/espressif/esp-idf |
| ↳ FreeRTOS-Kernel | (ESP-IDF 5.5.2) | MIT | https://github.com/espressif/esp-idf/tree/master/components/freertos |
| ↳ lwIP | (ESP-IDF 5.5.2) | BSD-3-Clause | https://savannah.nongnu.org/projects/lwip/ |
| ↳ Mbed TLS | 3.6.5 | Apache-2.0 OR GPL-2.0-or-later | https://github.com/Mbed-TLS/mbedtls |
| ↳ ESP-MQTT (`mqtt_client.h`) | (ESP-IDF 5.5.2) | Apache-2.0 | https://github.com/espressif/esp-mqtt |
| ↳ esp_littlefs (joltwallet) | 1.20.3 | MIT; enthält littlefs (BSD-3-Clause) | https://github.com/joltwallet/esp_littlefs |

ESP-IDF enthält weitere Komponenten mit eigenen Lizenzen; die vollständige Liste
führt Espressif unter
https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/COPYRIGHT.html.

### Referenzen und Testdaten

| Komponente | Verwendung | Lizenz | Quelle |
|---|---|---|---|
| wmbusmeters | Byte-Layouts der Techem-Telegramme folgen den öffentlichen Formatbeschreibungen; die Techem-Testtelegramme in `tests/wmbus_test.cpp` stammen aus den Treiber-Testsuiten | GPL-3.0 | https://github.com/wmbusmeters/wmbusmeters |

## 2. Weboberfläche (`web/`, ausgeliefert als `data/`)

### Im ausgelieferten Bundle enthalten

| Paket | Version | Lizenz | Quelle |
|---|---|---|---|
| preact | 10.28.3 | MIT | https://github.com/preactjs/preact |
| @preact/signals | 1.3.3 | MIT | https://github.com/preactjs/signals |
| @preact/signals-core | 1.13.0 | MIT | https://github.com/preactjs/signals |
| lucide-preact (Icons) | 0.563.0 | ISC | https://github.com/lucide-icons/lucide |
| uPlot | 1.6.31 | MIT | https://github.com/leeoniya/uPlot |

### Nur zum Bauen (nicht im Bundle)

| Paket | Version | Lizenz | Quelle |
|---|---|---|---|
| vite | 5.4.21 | MIT | https://github.com/vitejs/vite |
| @preact/preset-vite | 2.10.3 | MIT | https://github.com/preactjs/preset-vite |
| tailwindcss | 3.4.19 | MIT | https://github.com/tailwindlabs/tailwindcss |
| postcss | 8.5.6 | MIT | https://github.com/postcss/postcss |
| autoprefixer | 10.4.24 | MIT | https://github.com/postcss/autoprefixer |
| eslint | 8.57.1 | MIT | https://github.com/eslint/eslint |

Transitive Build-Abhängigkeiten stehen in `web/package-lock.json`.

## 3. Werkzeuge (nicht Teil der Firmware)

| Werkzeug | Version | Lizenz | Quelle |
|---|---|---|---|
| esptool | 5.1.0 | GPL-2.0-or-later | https://github.com/espressif/esptool |
| pyserial | 3.5 | BSD-3-Clause | https://github.com/pyserial/pyserial |
| PlatformIO Core | 6.1.x | Apache-2.0 | https://github.com/platformio/platformio-core |

## 4. Hardware-Dokumentation

Das Datenblatt bzw. Handbuch des Funkmoduls **EBYTE E07-400MBL-01** ist urheberrechtlich
geschützt und deshalb nicht im Repository enthalten. Bezug über den Hersteller:
https://www.cdebyte.com/ (Produktsuche „E07-400MBL-01“).
