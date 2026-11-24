# Linkra Duo – Anmeldung und 2FA: API-Vertrag (v1)

Verbindlich für Firmware (`src/auth/`, `src/network/app_web_server.cpp`) und Web-UI (`web/src`). Änderungen nur gemeinsam an beiden Seiten.

## Grundregeln

- **Öffentlich** (ohne Anmeldung): `index.html`, `/assets/*` (statische Dateien der UI), `GET /api/auth/status`, `POST /api/auth/setup` (nur solange kein Konto existiert), `POST /api/auth/login`.
- **Alles andere unter `/api/*`** einschließlich SSE `/api/events` verlangt eine gültige Sitzung, sonst `401 {"error":{"code":"unauthorized","message":"Anmeldung erforderlich"}}`.
- **Sitzung:** Cookie `linkra_session`, `HttpOnly; SameSite=Strict; Path=/`, kein `Secure` (Gerät läuft per HTTP im Heimnetz). Token: 32 Zufallsbytes (`esp_fill_random`), hex. Gleitende Gültigkeit 7 Tage, max. 8 gleichzeitige Sitzungen (älteste fliegt raus). Sitzungen liegen im RAM; nach einem Neustart ist eine neue Anmeldung nötig.
- **CSRF-Schutz:** Jede Anfrage mit anderer Methode als GET/HEAD muss den Header `X-Linkra: 1` tragen (erzwingt bei fremden Seiten einen CORS-Preflight, den das Gerät nie erlaubt). Fehlt er → `403 {"error":{"code":"csrf","message":"Ungültige Anfrage"}}`. Zusätzlich: Ist `Origin` gesetzt und passt der Host nicht zum `Host`-Header → 403. Gilt auch für `/api/auth/*`.
- **Fehlerformat überall:** `{"error":{"code":"<maschinenlesbar>","message":"<deutscher Text>"}}`.
- **Kein Endpunkt gibt jemals** Passwort, Passwort-Hash, TOTP-Geheimnis (außer einmalig bei der Einrichtung) oder Wiederherstellungscodes (außer einmalig beim Aktivieren) zurück.

## Endpunkte

### `GET /api/auth/status`
```json
{ "setup_required": false, "authenticated": true, "user": "admin",
  "totp_enabled": true, "time_synced": true }
```
`user` nur wenn angemeldet. `time_synced` = Systemzeit per NTP gesetzt (Voraussetzung für TOTP).

### `POST /api/auth/setup`  (nur wenn `setup_required`)
Body `{"username":"admin","password":"..."}` → `200` + Cookie + Status-JSON wie oben. Regeln: Benutzername 3–32 Zeichen `[A-Za-z0-9._-]`, Passwort 10–128 Zeichen. Existiert schon ein Konto → `409 {"error":{"code":"exists",...}}`.

### `POST /api/auth/login`
Body `{"username":"...","password":"...","totp":"123456"}` oder statt `totp` `"recovery_code":"abcd-efgh"`.
- Erfolg → `200` + Cookie + Status-JSON.
- Falsch → `401 {"error":{"code":"invalid_credentials",...}}` (keine Unterscheidung Benutzer/Passwort).
- Passwort richtig, 2FA aktiv, Code fehlt → `401 {"error":{"code":"totp_required",...}}`.
- 2FA-Code falsch → `401 {"error":{"code":"totp_invalid",...}}`.
- Uhrzeit nicht synchron → `503 {"error":{"code":"time_not_synced",...}}` (Wiederherstellungscode funktioniert trotzdem).
- Zu viele Versuche → `429 {"error":{"code":"locked","message":"...","retry_after_s":30}}`. Sperre nach 5 Fehlversuchen (pro Client-IP und global), Dauer verdoppelt sich je weiterer Sperre (30 s … max. 15 min).

### `POST /api/auth/logout`
Beendet die aktuelle Sitzung, löscht das Cookie. `200 {"ok":true}`.

### `POST /api/auth/logout-all`
Beendet alle Sitzungen (auch die eigene). `200 {"ok":true}`.

### `POST /api/auth/password`
Body `{"current":"...","new":"..."}`. Erfolg → `200 {"ok":true}`, alle anderen Sitzungen werden beendet. Falsches `current` → `401 invalid_credentials` (zählt als Fehlversuch).

### `POST /api/auth/totp/setup`
Erzeugt ein neues, noch **inaktives** Geheimnis (20 Bytes). → `200 {"secret":"BASE32...","otpauth_uri":"otpauth://totp/Linkra%20Duo:admin?secret=...&issuer=Linkra%20Duo&algorithm=SHA1&digits=6&period=30"}`. Ein vorher aktives Geheimnis bleibt aktiv, bis `enable` erfolgreich ist.

### `POST /api/auth/totp/enable`
Body `{"code":"123456"}` (prüft gegen das ausstehende Geheimnis). Erfolg → `200 {"recovery_codes":["abcd-efgh", ... 10 Stück]}` – nur dieses eine Mal sichtbar, gespeichert werden nur Hashes. Falsch → `400 {"error":{"code":"totp_invalid",...}}`.

### `POST /api/auth/totp/disable`
Body `{"password":"...","code":"123456"}` (oder `recovery_code`). → `200 {"ok":true}`.

## Technik (Firmware)

- Passwort: PBKDF2-HMAC-SHA256 (mbedTLS), 16 Byte Salt, Iterationen so wählen, dass eine Prüfung ca. 150–300 ms dauert (gemessen auf dem S3, Wert im Code dokumentiert). Vergleich in konstanter Zeit.
- TOTP: RFC 6238, HMAC-SHA1, 30 s, 6 Stellen, Fenster ±1 Schritt, ein Code wird nur einmal akzeptiert (letzter verwendeter Zeitschritt wird gespeichert).
- Wiederherstellungscodes: 10 Stück, Format `xxxx-xxxx` (a-z0-9 ohne verwechselbare Zeichen), gespeichert als SHA-256, jeder nur einmal verwendbar.
- Speicher: NVS-Namespace `auth` (Benutzer, Salt, Hash, Iterationen, TOTP-Geheimnis, TOTP aktiv, letzter Zeitschritt, Code-Hashes).
- **Zugang wiederherstellen** (Passwort vergessen): BOOT-Taste (GPIO0) beim Einschalten 10 s gedrückt halten → nur der NVS-Namespace `auth` wird gelöscht, danach wieder `setup_required`. Geräte, WLAN, MQTT bleiben. Wird im Seriellen Log und in der Doku beschrieben.
- Zeit: NTP-Synchronisation bei jedem WLAN-Verbindungsaufbau (nicht nur beim Boot).

## Nachtrag Firmware (2026-11-24, Umsetzung `src/auth/`)

Abweichungen und Präzisierungen gegenüber v1, wie sie in der Firmware umgesetzt sind:

- **Zugang wiederherstellen:** GPIO0 ist ein Strapping-Pin. Wird BOOT *beim Einschalten* gehalten, startet der ESP32-S3 im ROM-Download-Modus und die Firmware läuft gar nicht. Deshalb: Gerät normal starten, dann **innerhalb der ersten 60 s nach dem Start BOOT drücken und 10 s halten** → nur NVS `auth` wird gelöscht (Log `[Auth] BOOT-Taste 10 s gehalten …`, Countdown im Log), danach `setup_required`, kein Neustart nötig. Loslassen vor Ablauf bricht ab. Keine LED-Anzeige (auf dem Breadboard keine nutzbare LED).
- **Cookie:** zusätzlich `Max-Age=604800`. Damit die 7 Tage auch im Browser gleitend sind, sendet das Gerät bei einer API-Antwort höchstens einmal pro Tag das Cookie neu. Logout/Logout-all senden `linkra_session=; …; Max-Age=0`.
- **Zusätzliche Fehlercodes** (Format wie im Vertrag):
  - `400 invalid` – Setup/Passwortwechsel: Benutzername oder Passwort verletzt die Regeln.
  - `409 setup_required` – Login, obwohl noch kein Konto existiert.
  - `400 totp_not_pending` – `totp/enable` ohne vorheriges `totp/setup` (das ausstehende Geheimnis liegt nur im RAM und ist nach einem Neustart weg).
  - `503 time_not_synced` – auch bei `totp/enable` und bei `totp/disable` mit `code`.
  - `401 totp_required` – `totp/disable` bei aktiver 2FA ohne `code`/`recovery_code`.
  - `429 locked` – auch bei `password` und `totp/disable` (dort zählen falsches Passwort/falscher Code als Fehlversuch); zusätzlich HTTP-Header `Retry-After`.
  - `500 storage` – NVS-Schreibfehler.
- **Rate-Limit:** Der 5. Fehlversuch selbst antwortet noch `401`, erst die folgenden `429`. Während einer Sperre zählen weitere Versuche nicht. Erfolg setzt den Zähler der Client-IP zurück; die Sperrstufe (30 s → 60 s → … → 900 s) verfällt nach 1 h ohne Fehlversuch. `totp_required` zählt nicht als Fehlversuch.
- **`GET /api/auth/status`:** `totp_enabled` wird auch ohne Anmeldung geliefert (die Login-Maske kann das Code-Feld vorab zeigen). `time_synced` steht zusätzlich in `/api/system`.
- **JSON-Endpunkte** (`setup`, `login`, `password`, `totp/enable`, `totp/disable`) verlangen `Content-Type: application/json` (sonst 404 durch ESPAsyncWebServer). `logout`, `logout-all`, `totp/setup` brauchen keinen Body.
- **Origin-Prüfung** gilt für alle Methoden (auch GET). Ein Entwicklungs-Proxy (z. B. Vite) muss `Origin` passend zu `Host` setzen oder entfernen.
- **Pfade:** Enthält die URL `..`, `//` oder `\` → `400`. Nicht öffentliche Pfade außerhalb von `/api/` → `404` (text/plain). Öffentlich sind genau `GET/HEAD /`, `/index.html`, `/assets/*`.
- **Sitzungsende:** Logout, Logout-all und Passwortwechsel schließen alle offenen SSE-Verbindungen; gültige Clients verbinden sich neu.
- **Uploads:** `/api/fs/upload` und `/api/ota` prüfen Anmeldung und `X-Linkra` schon beim ersten Datenblock (die Middleware läuft erst nach dem Body). Upload/Löschen nur für `/index.html` und `/assets/<Segmente aus A-Z a-z 0-9 . _ ->`. Fehler: `403 forbidden_path`, `507 no_space`, `500 write_failed`, `400 no_file`; OTA: `400 filesystem_ota` (Dateisystem-Images werden nicht mehr angenommen), `409 busy`, `500 update_failed`.
- **PBKDF2:** 8000 Iterationen (Schätzung 160–280 ms auf dem S3, Messwert steht bei jeder Ableitung im Log `[Auth] PBKDF2 … ms`; Wert in `AuthManager::kPbkdf2Iterations`, je Konto gespeichert, Rehash beim nächsten Login). Läuft synchron im `async_tcp`-Task (16 KB Stack); während einer Prüfung ruhen andere HTTP-Anfragen kurz.
