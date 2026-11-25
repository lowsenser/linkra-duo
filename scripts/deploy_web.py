#!/usr/bin/env python3
"""Upload the built web UI (data/) to a running Linkra Duo over HTTP.

Only index.html and assets/* are touched; the device database and config on
LittleFS stay untouched (unlike `pio run -t uploadfs`, which wipes them).
Assets are stored gzip-compressed; the web server serves *.gz transparently.

usage: python3 scripts/deploy_web.py [host]
       host: argument, else environment variable LINKRA_HOST, else linkra-duo.local

Since 0.6.0 the API needs a login (docs/auth-api.md). The script asks for user,
password and - if 2FA is on - the 6-digit code (user may also come from LINKRA_USER).
Firmware without login (404 on /api/auth/status) is uploaded to directly.
"""
import gzip, json, os, sys, uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import linkra_client as lc
lc.set_host(sys.argv[1] if len(sys.argv) > 1 else None)
req = lc.req
lc.login()
root = Path(__file__).resolve().parent.parent / "data"

def upload(dest, data):
    b = uuid.uuid4().hex
    body = (f"--{b}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"f\"\r\n"
            f"Content-Type: application/octet-stream\r\n\r\n").encode() + data + f"\r\n--{b}--\r\n".encode()
    req("POST", f"/api/fs/upload?path={dest}", body, {"Content-Type": f"multipart/form-data; boundary={b}"})

keep = set()
for f in sorted((root / "assets").iterdir()):
    raw = f.read_bytes()
    if f.suffix in (".js", ".css", ".svg", ".json"):
        dest, data = f"/assets/{f.name}.gz", gzip.compress(raw, 9)
    else:
        dest, data = f"/assets/{f.name}", raw
    upload(dest, data); keep.add(dest)
    print(f"  {dest:45} {len(data):>7} B")
upload("/index.html", (root / "index.html").read_bytes()); print("  /index.html")

for f in json.loads(req("GET", "/api/fs/list"))["files"]:
    p = f["path"]
    if p.startswith("/assets/") and p not in keep:
        req("POST", f"/api/fs/delete?path={p}"); print(f"  - {p}")
print("done")
