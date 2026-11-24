# Flashen, Web-Deploy und Wiederherstellung

Gilt für Firmware 0.6.0-beta, Umgebung `linkra-s3-breadboard`, Partitionstabelle
`partitions/partitions_16mb.csv`. Wenn sich die Partitionstabelle ändert (z. B. für OTA),
muss dieses Dokument angepasst werden.

> [!CAUTION]
> ## NIEMALS `pio run -t uploadfs`
> `uploadfs` schreibt ein neues LittleFS-Abbild über die komplette Datenpartition.
> **Gerätedatenbank, Einstellungen und alle gespeicherten Geräte sind danach weg.**
> Die Weboberfläche wird ausschließlich mit `scripts/deploy_web.py` aktualisiert.
>
> Ebenso **kein Dateisystem-Abbild über `/api/ota`** hochladen – das überschreibt das
> gemountete LittleFS ebenfalls.

> [!WARNING]
> **`pio run -t upload` auf einem eingerichteten Gerät vermeiden.** PlatformIO schreibt dabei
> neben Bootloader und Partitionstabelle auch `boot_app0.bin` an `0xe000`. Diese Adresse liegt
> mit der aktuellen Partitionstabelle **im NVS-Bereich** (`0x9000`–`0xEFFF`, dort liegen
> Einstellungen, MQTT-Zugang und künftig die Anmeldedaten). Für Updates nur die App an
> `0x10000` schreiben (siehe unten).

## Speicheraufteilung (16 MB Flash)

| Bereich | Offset | Größe | Inhalt | Beim Update schreiben? |
|---|---|---|---|---|
| Bootloader | `0x0` | – | `bootloader.bin` | nur Erstinstallation |
| Partitionstabelle | `0x8000` | 0x1000 | `partitions.bin` | nur Erstinstallation |
| `nvs` | `0x9000` | 0x6000 | Einstellungen (NVS) | **nie** |
| `phy_init` | `0xF000` | 0x1000 | Funkkalibrierung | nie |
| `factory` | `0x10000` | 0x300000 | Firmware (`firmware.bin`) | **ja** |
| `coredump` | `0x310000` | 0x10000 | Absturzabbild | nie |
| `spiffs` (LittleFS) | `0x320000` | 0xCE0000 | Web-UI, Gerätedatenbank, Konfiguration | **nie** (Web-UI nur per `deploy_web.py`) |

## Vorbereitung

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt          # esptool, pyserial
pio run -e linkra-s3-breadboard          # baut .pio/build/linkra-s3-breadboard/{bootloader,partitions,firmware}.bin
```

Serielle Schnittstelle `<PORT>` finden:

| System | Typischer Name |
|---|---|
| macOS | `/dev/cu.usbmodem…` (`ls /dev/cu.usb*`) |
| Linux | `/dev/ttyACM0` |
| Windows | `COM3`, `COM4`, … (Geräte-Manager) |

Falls das Board nicht in den Download-Modus wechselt: **BOOT** gedrückt halten, **RESET**
kurz drücken, BOOT loslassen. Nach dem Flashen einmal RESET drücken.

## 1. Firmware-Update (Normalfall)

Schreibt **nur** die App-Partition. NVS und LittleFS bleiben unverändert.

```bash
esptool --chip esp32s3 --port <PORT> --baud 921600 write_flash 0x10000 .pio/build/linkra-s3-breadboard/firmware.bin
```

(Ältere esptool-Versionen heißen `esptool.py`; ab esptool 5 heißt der Befehl auch
`write-flash` – beide Schreibweisen funktionieren.)

## 2. Weboberfläche aktualisieren

Lädt `data/index.html` und `data/assets/*` über HTTP auf das laufende Gerät und löscht
veraltete Assets. Die Gerätedatenbank und alle anderen Dateien bleiben unberührt.

```bash
cd web && npm ci && npm run build && cd ..      # erzeugt data/
LINKRA_HOST=linkra-duo.local python3 scripts/deploy_web.py
# oder mit IP:  python3 scripts/deploy_web.py 192.0.2.10
```

Ohne Argument und ohne `LINKRA_HOST` wird `linkra-duo.local` verwendet.

> Das Skript fragt Benutzer, Passwort und – falls aktiv – den 2FA-Code ab (Benutzer auch
> über `LINKRA_USER`). Gibt es noch kein Konto, zuerst im Browser einrichten.

## 3. Erstinstallation eines leeren Boards

Nur für ein **neues oder komplett zu löschendes** Board. `erase_flash` löscht alles,
auch eine vorhandene Gerätedatenbank – vorher ggf. sichern (Abschnitt 5).

```bash
# 1. Flash komplett löschen (nur bei neuen/defekten Boards)
esptool --chip esp32s3 --port <PORT> erase_flash

# 2. Bootloader, Partitionstabelle und Firmware schreiben (ohne boot_app0)
esptool --chip esp32s3 --port <PORT> --baud 921600 write_flash \
  0x0     .pio/build/linkra-s3-breadboard/bootloader.bin \
  0x8000  .pio/build/linkra-s3-breadboard/partitions.bin \
  0x10000 .pio/build/linkra-s3-breadboard/firmware.bin
```

3. Board neu starten. Die Firmware formatiert das leere LittleFS beim ersten Start selbst.
4. WLAN: Die Zugangsdaten kommen aus `config/secrets.h` (zur Bauzeit). Scheitert die
   Verbindung, startet der offene Hotspot `Linkra-Duo-Setup` (`http://192.168.4.1`).
5. Weboberfläche wie in Abschnitt 2 hochladen. Bis dahin zeigt das Gerät nur
   „Web interface not found“.

Die serielle Ausgabe (115200 Baud) zeigt IP-Adresse und mDNS-Namen:

```bash
pio device monitor -e linkra-s3-breadboard
```

## 4. Zugang wiederherstellen (Passwort vergessen)

Siehe auch [`auth-api.md`](auth-api.md).

1. Gerät normal einschalten (BOOT-Taste dabei **nicht** halten – sonst startet der Chip im
   Download-Modus).
2. **Innerhalb der ersten 60 Sekunden** die **BOOT-Taste (GPIO0) 10 Sekunden** gedrückt halten.
3. Es wird nur der NVS-Namespace `auth` gelöscht. Geräte, WLAN- und MQTT-Einstellungen
   bleiben erhalten. Beim nächsten Aufruf der Web-UI wird ein neues Konto angelegt.

Der Vorgang wird im seriellen Log bestätigt. Es ist **kein** Flashen nötig.

## 5. Sicherung (nur lesen)

Vor riskanten Arbeiten die Datenpartition (Datenbank, Konfiguration) und die NVS sichern.
Diese Befehle lesen nur, sie schreiben nichts:

```bash
esptool --chip esp32s3 --port <PORT> --baud 921600 read_flash 0x320000 0xCE0000 backups/littlefs_$(date +%F).bin
esptool --chip esp32s3 --port <PORT> --baud 921600 read_flash 0x9000 0x6000 backups/nvs_$(date +%F).bin
shasum -a 256 backups/*.bin > backups/SHA256SUMS
```

`backups/` ist in `.gitignore` eingetragen – die Abbilder enthalten persönliche Daten
(WLAN-Zugang, Geräte-IDs) und gehören nicht ins Repository. Zurückschreiben einer
Sicherung (`write_flash <offset> <datei>`) überschreibt den jeweiligen Bereich vollständig
und ist nur für Notfälle gedacht.
