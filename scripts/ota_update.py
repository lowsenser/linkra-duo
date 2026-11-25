#!/usr/bin/env python3
"""Install a firmware image on a running Linkra Duo over the network (OTA).

usage: python3 scripts/ota_update.py [host] [firmware.bin]
       host: argument, else LINKRA_HOST, else linkra-duo.local
       firmware: default .pio/build/linkra-s3-breadboard/firmware.bin

Asks for user, password and 2FA code. The device writes the image into the
free OTA slot, restarts, and keeps it only if it runs healthy for 90 s
(otherwise the bootloader returns to the previous version).
"""
import json, sys, time, uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import linkra_client as lc

args = [a for a in sys.argv[1:]]
fw = Path(next((a for a in args if a.endswith(".bin")), Path(__file__).resolve().parent.parent /
                ".pio/build/linkra-s3-breadboard/firmware.bin"))
host = next((a for a in args if not a.endswith(".bin")), None)
lc.set_host(host)
data = fw.read_bytes()
if data[:1] != b"\xe9":
    sys.exit(f"{fw} ist kein ESP32-Firmware-Image.")
lc.login()
before = json.loads(lc.req("GET", "/api/system"))
print(f"Gerät: {lc.host}  Version {before.get('version')}  Slot {before.get('partition')}")
print(f"Lade {fw.name} hoch ({len(data) // 1024} KB) …")
b = uuid.uuid4().hex
body = (f"--{b}\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"firmware.bin\"\r\n"
        f"Content-Type: application/octet-stream\r\n\r\n").encode() + data + f"\r\n--{b}--\r\n".encode()
t0 = time.time()
print(lc.req("POST", "/api/ota", body, {"Content-Type": f"multipart/form-data; boundary={b}"}, timeout=300).decode())
print(f"Hochgeladen in {time.time() - t0:.0f} s. Das Gerät startet neu.")
