"""Shared HTTP client for the Linkra Duo scripts: host selection, session cookie,
CSRF header and interactive login (user, password, 2FA code; never stored)."""
import getpass, http.cookiejar, json, os, sys, urllib.error, urllib.request

host = os.environ.get("LINKRA_HOST", "linkra-duo.local")

def set_host(h):
    global host
    if h:
        host = h

opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

def req(method, path, body=None, headers=None, timeout=60):
    h = {"X-Linkra": "1"}  # CSRF header required by the firmware for non-GET requests (docs/auth-api.md)
    h.update(headers or {})
    r = urllib.request.Request(f"http://{host}{path}", data=body, method=method, headers=h)
    with opener.open(r, timeout=timeout) as resp:
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

