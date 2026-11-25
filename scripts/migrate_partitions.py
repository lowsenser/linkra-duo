#!/usr/bin/env python3
"""One-time migration of a Linkra Duo from the old single-app layout (0.6.x) to
the OTA layout (0.7.0, partitions/partitions_16mb.csv).

What changes on the flash:
  old: factory app 0x10000 (3 MB) | coredump 0x310000 | LittleFS 0x320000, 12.9 MB
  new: ota_0 0x10000 (3 MB) | otadata 0x310000 | coredump 0x312000 | LittleFS 0x320000, 9.9 MB | ota_1 0xD00000 (3 MB)
NVS (0x9000-0xEFFF: WLAN, login, MQTT) and phy_init are not touched.

LittleFS stores its size in the superblock, so the file system cannot simply be
shortened: all files (device database, web UI, hunt results) are copied into a
new, smaller image. Steps:

  1. python3 scripts/migrate_partitions.py backup   -> backups/s3_full_<time>.bin (read only)
  2. python3 scripts/migrate_partitions.py convert backups/s3_full_<time>.bin [--web]
                                                     -> build/lfs_new.bin, file-by-file check
     --web replaces the stored web UI by the current build in data/.
  3. python3 scripts/migrate_partitions.py flash build/lfs_new.bin
     writes bootloader, partition table, empty otadata, firmware (ota_0) and the
     new LittleFS image. Needs `pio run -e linkra-s3-breadboard` first.

Restore if anything goes wrong:
  esptool --chip esp32s3 --port <PORT> write-flash 0 backups/s3_full_<time>.bin

Requires: esptool, littlefs-python (both in requirements.txt / the project .venv).
"""
import glob, hashlib, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / ".pio" / "build" / "linkra-s3-breadboard"
OLD_LFS = (0x320000, 0xCE0000)
NEW_LFS = (0x320000, 0x9E0000)
BLOCK = 4096
NAME_MAX = 64          # CONFIG_LITTLEFS_OBJ_NAME_LEN of the ESP32 build


def port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) or sorted(glob.glob("/dev/ttyACM*"))
    if not ports:
        sys.exit("Kein Linkra Duo per USB gefunden.")
    return os.environ.get("LINKRA_PORT", ports[0])


def esptool(*args):
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port(), "--baud", os.environ.get("LINKRA_BAUD", "460800"), *args]
    print("+", " ".join(cmd[2:]))
    subprocess.run(cmd, check=True)


def sha(data):
    return hashlib.sha256(data).hexdigest()[:16]


def walk(fs, path="/"):
    for name in fs.listdir(path):
        p = path.rstrip("/") + "/" + name
        if fs.stat(p).type == 2:   # LFS_TYPE_DIR
            yield p, None
            yield from walk(fs, p)
        else:
            with fs.open(p, "rb") as f:
                yield p, f.read()


def cmd_backup():
    out = ROOT / "backups" / f"s3_full_{time.strftime('%Y-%m-%d_%H%M')}.bin"
    out.parent.mkdir(exist_ok=True)
    esptool("read-flash", "0", "0x1000000", str(out))
    print(f"Sicherung: {out}  sha256 {sha(out.read_bytes())}")


def web_files():
    """index.html + assets from data/, stored like scripts/deploy_web.py does."""
    import gzip
    root = ROOT / "data"
    out = [("/assets", None)]
    for f in sorted((root / "assets").iterdir()):
        if f.name.endswith(".gz"):
            continue
        raw = f.read_bytes()
        if f.suffix in (".js", ".css", ".svg", ".json"):
            out.append((f"/assets/{f.name}.gz", gzip.compress(raw, 9, mtime=0)))
        else:
            out.append((f"/assets/{f.name}", raw))
    out.append(("/index.html", (root / "index.html").read_bytes()))
    return out


def cmd_convert(full_backup, with_web=False):
    from littlefs import LittleFS
    data = Path(full_backup).read_bytes()
    if len(data) != 0x1000000:
        sys.exit("Erwartet eine vollständige 16-MB-Sicherung.")
    old_img = bytearray(data[OLD_LFS[0]:OLD_LFS[0] + OLD_LFS[1]])
    old = LittleFS(block_size=BLOCK, block_count=OLD_LFS[1] // BLOCK, name_max=NAME_MAX, mount=False)
    old.context.buffer = old_img
    old.mount()
    files = list(walk(old))
    # leftover SQLite temp files are not migrated
    files = [(p, d) for p, d in files if not (p.startswith("/sqltmp_") or p.startswith("/tmp_t"))]
    if with_web:   # replace the old web UI by the current build in data/
        files = [(p, d) for p, d in files if not (p == "/index.html" or p == "/assets" or p.startswith("/assets/"))]
        files += web_files()
    total = sum(len(d) for _, d in files if d is not None)
    if total > NEW_LFS[1] * 0.85:
        sys.exit(f"Zu viele Daten für das neue Dateisystem: {total} Bytes")
    new = LittleFS(block_size=BLOCK, block_count=NEW_LFS[1] // BLOCK, name_max=NAME_MAX)
    for p, d in files:
        if d is None:
            new.makedirs(p, exist_ok=True)
        else:
            with new.open(p, "wb") as f:
                f.write(d)
    # verify by re-mounting the new image from its bytes
    img = bytes(new.context.buffer)
    chk = LittleFS(block_size=BLOCK, block_count=NEW_LFS[1] // BLOCK, name_max=NAME_MAX, mount=False)
    chk.context.buffer = bytearray(img)
    chk.mount()
    got = {p: d for p, d in walk(chk)}
    for p, d in files:
        if got.get(p, b"\0missing") != d:
            sys.exit(f"Prüfung fehlgeschlagen: {p}")
    out = ROOT / "build" / "lfs_new.bin"
    out.parent.mkdir(exist_ok=True)
    out.write_bytes(img)
    for p, d in files:
        print(f"  {p:40} {'<dir>' if d is None else str(len(d)) + ' B'}")
    print(f"{len(files)} Einträge, {total} Bytes übernommen und geprüft -> {out}")


def cmd_flash(lfs_image):
    img = Path(lfs_image).read_bytes()
    if len(img) != NEW_LFS[1]:
        sys.exit("Falsche Imagegröße für die neue LittleFS-Partition.")
    for f in ("bootloader.bin", "partitions.bin", "firmware.bin"):
        if not (BUILD / f).exists():
            sys.exit(f"{f} fehlt – erst `pio run -e linkra-s3-breadboard` ausführen.")
    esptool("erase-region", "0x310000", "0x2000")          # otadata empty -> boots ota_0
    esptool("write-flash",
            "0x0", str(BUILD / "bootloader.bin"),
            "0x8000", str(BUILD / "partitions.bin"),
            "0x10000", str(BUILD / "firmware.bin"),
            hex(NEW_LFS[0]), str(lfs_image))
    print("Fertig. Das Gerät startet mit der neuen Aufteilung.")


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("backup", "convert", "flash"):
        sys.exit(__doc__)
    {"backup": lambda: cmd_backup(),
     "convert": lambda: cmd_convert(sys.argv[2], "--web" in sys.argv[3:]),
     "flash": lambda: cmd_flash(sys.argv[2])}[sys.argv[1]]()
