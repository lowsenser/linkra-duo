# Flashen, Web-Deploy und Wiederherstellung

Gilt ab Firmware 0.7.0-beta, Umgebung `linkra-s3-breadboard`, Partitionstabelle
`partitions/partitions_16mb.csv` mit zwei OTA-Slots. Geräte mit 0.6.x (alte Aufteilung mit
nur einer `factory`-Partition) müssen **einmalig per USB umziehen**, siehe Abschnitt 6.

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
> Einstellungen, MQTT-Zugang und Anmeldedaten). Updates per Netzwerk einspielen oder per USB
> wie in Abschnitt 1b.

## Speicheraufteilung (16 MB Flash)

| Bereich | Offset | Größe | Inhalt | Beim Update schreiben? |
|---|---|---|---|---|
| Bootloader | `0x0` | – | `bootloader.bin` | nur Erstinstallation |
| Partitionstabelle | `0x8000` | 0x1000 | `partitions.bin` | nur Erstinstallation |
| `nvs` | `0x9000` | 0x6000 | Einstellungen (NVS) | **nie** |
| `phy_init` | `0xF000` | 0x1000 | Funkkalibrierung | nie |
| `ota_0` | `0x10000` | 0x300000 | Firmware, Slot 0 | per OTA automatisch, per USB siehe 1b |
| `otadata` | `0x310000` | 0x2000 | welcher Slot startet, Rollback-Zustand | nur löschen (1b) |
| `coredump` | `0x312000` | 0xE000 | Absturzabbild | nie |
| `spiffs` (LittleFS) | `0x320000` | 0x9E0000 | Web-UI, Gerätedatenbank, Konfiguration | **nie** (Web-UI nur per `deploy_web.py`) |
| `ota_1` | `0xD00000` | 0x300000 | Firmware, Slot 1 | per OTA automatisch |

Ein OTA-Update schreibt immer in den Slot, der gerade **nicht** läuft. Welcher Slot aktiv
ist, zeigt die Weboberfläche unter „System“ (App-Partition `ota_0`/`ota_1`).

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

## 1. Firmware-Update

### 1a. Über das Netzwerk (Normalfall)

```bash
pio run -e linkra-s3-breadboard
python3 scripts/ota_update.py linkra-duo.local          # oder die IP; optional eigene .bin als 2. Argument
```

Das Skript fragt Benutzer, Passwort und – falls aktiv – den 2FA-Code ab, zeigt Version und
aktiven Slot, lädt `firmware.bin` hoch und wartet auf den Neustart. Alternativ in der
Weboberfläche unter **System → Firmware-Update (OTA)** eine `firmware.bin` auswählen.

**Automatischer Rollback:** Das neue Image startet zunächst „auf Probe“. Erst wenn es
90 Sekunden mit WLAN-Verbindung läuft, wird es als gültig markiert. Stürzt es vorher ab
oder startet neu, kehrt der Bootloader beim nächsten Start von selbst zur vorigen Version
zurück – ein kaputtes Update braucht also kein USB-Kabel.

Nur App-Images (`firmware.bin`) werden angenommen, Dateisystem-Abbilder lehnt das Gerät ab.

### 1b. Per USB (wenn das Netz nicht geht)

Schreibt die Firmware in Slot 0 und löscht `otadata`, damit garantiert Slot 0 startet
(sonst liefe nach einem früheren OTA-Update weiter Slot 1). NVS und LittleFS bleiben
unverändert.

```bash
esptool --chip esp32s3 --port <PORT> erase-region 0x310000 0x2000
esptool --chip esp32s3 --port <PORT> --baud 921600 write-flash 0x10000 .pio/build/linkra-s3-breadboard/firmware.bin
```

(Ältere esptool-Versionen heißen `esptool.py` und schreiben `erase_region`/`write_flash`
mit Unterstrich – beide Schreibweisen funktionieren.)

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
   Ohne `otadata` startet Slot 0 (`ota_0`).
4. WLAN: Die Zugangsdaten kommen aus `config/secrets.h` (zur Bauzeit). Scheitert die
   Verbindung, startet der Hotspot `Linkra-Duo-Setup` (WPA2, `http://192.168.4.1`). Das
   Passwort erzeugt jedes Gerät beim ersten Start selbst; es steht im seriellen Log
   (`[WiFi] Setup AP started …`) und nach der Anmeldung unter **Einstellungen → WLAN**.
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
esptool --chip esp32s3 --port <PORT> --baud 921600 read-flash 0x320000 0x9E0000 backups/littlefs_$(date +%F).bin
esptool --chip esp32s3 --port <PORT> --baud 921600 read-flash 0x9000 0x6000 backups/nvs_$(date +%F).bin
shasum -a 256 backups/*.bin > backups/SHA256SUMS
```

`backups/` ist in `.gitignore` eingetragen – die Abbilder enthalten persönliche Daten
(WLAN-Zugang, Geräte-IDs) und gehören nicht ins Repository. Zurückschreiben einer
Sicherung (`write-flash <offset> <datei>`) überschreibt den jeweiligen Bereich vollständig
und ist nur für Notfälle gedacht.

Für eine vollständige Sicherung des ganzen Flashs (16 MB) gibt es
`python3 scripts/migrate_partitions.py backup` (siehe Abschnitt 6).

## 6. Umzug von 0.6.x auf die OTA-Aufteilung (einmalig, per USB)

Bis 0.6.x gab es nur eine `factory`-Partition und ein 12,9 MB großes LittleFS. Ab 0.7.0
liegen dort zwei Firmware-Slots, das LittleFS ist 9,9 MB groß. Weil LittleFS seine Größe im
Superblock speichert, wird es nicht einfach gekürzt, sondern Datei für Datei in ein neues,
kleineres Abbild kopiert und geprüft. NVS (WLAN, Anmeldung, MQTT) bleibt unberührt.

```bash
. .venv/bin/activate                                       # esptool + littlefs-python
pio run -e linkra-s3-breadboard                            # neue bootloader/partitions/firmware.bin
python3 scripts/migrate_partitions.py backup               # liest 16 MB -> backups/s3_full_<Zeit>.bin
python3 scripts/migrate_partitions.py convert backups/s3_full_<Zeit>.bin --web   # -> build/lfs_new.bin
python3 scripts/migrate_partitions.py flash build/lfs_new.bin
```

- `convert` liest nur die Sicherung und schreibt nichts aufs Gerät. Es listet alle
  übernommenen Dateien und bricht ab, wenn eine Datei nach dem Kopieren nicht identisch ist
  oder die Daten nicht ins neue Dateisystem passen. `--web` ersetzt dabei die gespeicherte
  Weboberfläche durch den aktuellen Stand aus `data/`.
- `flash` schreibt Bootloader, neue Partitionstabelle, leeres `otadata`, die Firmware in
  `ota_0` und das neue LittleFS.
- **Zurück zum alten Stand**, falls etwas schiefgeht:
  `esptool --chip esp32s3 --port <PORT> write-flash 0 backups/s3_full_<Zeit>.bin`

Danach laufen alle weiteren Updates über das Netzwerk (Abschnitt 1a).
