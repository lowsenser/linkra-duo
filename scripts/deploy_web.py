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
import getpass, gzip, http.cookiejar, json, os, sys, urllib.error, urllib.request, uuid
from pathlib import Path

host = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("LINKRA_HOST", "linkra-duo.local")
root = Path(__file__).resolve().parent.parent / "data"

opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

def req(method, path, body=None, headers=None):
    h = {"X-Linkra": "1"}  # CSRF header required by the firmware for non-GET requests (docs/auth-api.md)
    h.update(headers or {})
    r = urllib.request.Request(f"http://{host}{path}", data=body, method=method, headers=h)
    with opener.open(r, timeout=60) as resp:
        return resp.read()

def api_error(e):
    try:
        return json.loads(e.read()).get("error", {})
    except Exception:
        return {}

def login():
    try:
        st = json.loads(req("GET", "/api/auth/status"))
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return  # firmware without login
        raise
    if st.get("authenticated"):
        return
    if st.get("setup_required"):
        sys.exit("Auf dem Gerät gibt es noch kein Konto. Bitte zuerst im Browser einrichten.")
    user = os.environ.get("LINKRA_USER") or input("Benutzer: ")
    body = {"username": user, "password": getpass.getpass("Passwort: ")}
    for _ in range(2):
        try:
            req("POST", "/api/auth/login", json.dumps(body).encode(), {"Content-Type": "application/json"})
            return
        except urllib.error.HTTPError as e:
            err = api_error(e)
            if err.get("code") == "totp_required" and "totp" not in body:
                body["totp"] = input("2FA-Code: ").strip()
                continue
            sys.exit(f"Anmeldung fehlgeschlagen: {err.get('message') or e}")

login()

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
