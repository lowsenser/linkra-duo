# Web-UI (web/)

Preact + @preact/signals, Tailwind, Vite. `npm run build` in `web/` schreibt nach `data/` und gibt einen Größenbericht aus (bricht ab, wenn die Startseite mehr als 60 KB JS+CSS gzip lädt).

## Befehle (in `web/`)

| Befehl | Zweck |
|---|---|
| `npm ci` | Abhängigkeiten exakt nach Lockfile |
| `npm run mock` | Entwicklungs-Mock der Geräte-API auf http://localhost:8787 |
| `npm run dev:mock` | Vite-Dev-Server (http://localhost:5173) gegen den Mock |
| `LINKRA_HOST=192.168.1.50 npm run dev` | Vite-Dev-Server gegen ein echtes Gerät (Standard: `linkra-duo.local`) |
| `npm run build` | Produktions-Build nach `data/` + Größenbericht |
| `npm test` | Unit-Tests (`node --test`), ohne Browser |

Der Dev-Proxy schreibt den `Origin`-Header auf das Ziel um – die Firmware lehnt fremde Origins ab (auch bei GET).

## Mock (web/mock/server.js)

Bildet den Vertrag [auth-api.md](auth-api.md) inklusive Firmware-Nachtrag nach: Sitzungs-Cookie, `X-Linkra`-Pflicht, Origin-Prüfung, `Content-Type`-Pflicht der JSON-Routen, Sperre nach 5 Fehlversuchen (30 s, verdoppelnd, `Retry-After`), TOTP (RFC 6238) mit festem Testgeheimnis, 10 feste Wiederherstellungscodes, Schließen aller SSE-Streams bei Logout/Passwortwechsel. Dazu simulierte Daten: Systemstatus, WLAN, beide Funkmodule mit Profil-Rotation, Geräte-DB, MQTT, SSE `rf_rssi` (5 Hz) und `rf_signal`.

```
npm run mock                         # ohne Konto → Einrichtungsassistent
node mock/server.js --account        # Konto admin / linkra-test-pw
node mock/server.js --account --totp # zusätzlich 2FA aktiv
node mock/server.js --unsynced       # Uhrzeit „nicht synchron“ → 503 time_not_synced
node mock/server.js --ttl 60         # Sitzungen laufen nach 60 s ab
```

Testgeheimnis `JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP` (in jede Authenticator-App eintragbar). Hilfsrouten nur im Mock:

- `GET /api/__mock/totp` – aktueller Code
- `POST /api/__mock/expire` – alle Sitzungen beenden (testet „Sitzung abgelaufen“)
- `POST /api/__mock/reset` – zurück auf „kein Konto“ (wie BOOT-Taste)
- `GET /api/__mock/offline?s=20` – N Sekunden keine Antwort (testet Banner und Backoff)

Ohne Vite liefert der Mock auch den fertigen Build aus `data/` aus: `npm run build && npm run mock`, dann http://localhost:8787.

## Aufbau

- `src/lib/http.js` – `api()`: Timeout, `X-Linkra: 1` bei jeder Nicht-GET-Anfrage, `ApiError` mit `code`/`retryAfter`, 401 `unauthorized` → Handler (zurück zur Anmeldung).
- `src/lib/auth.js` / `authlogic.js` – Anmeldezustand; reine Logik (Login-Zustandsmaschine, Regeln, Passwortstärke, Fehlertexte) ist getestet.
- `src/lib/scheduler.js` – zentraler Poll-Scheduler: Versatz je Aufgabe, nie überlappend, exponentieller Backoff mit Jitter (3 s → 30 s), pausiert im Hintergrund-Tab.
- `src/lib/store.js` – Daten und Live-Stream. SSE wird nach 45 s im Hintergrund geschlossen und beim Zurückkommen neu aufgebaut; Event-Warteschlange hart begrenzt; Geräteliste begrenzt (nicht gespeicherte Geräte nach 24 h bzw. über 500 Einträgen verworfen).
- `src/components/shell.jsx` – Verbindungsstatus, Banner „Werte veraltet“, Toasts (aria-live), Error-Boundary, `lazy()` für Code-Splitting.

## Netzlast pro Tab

| Quelle | Intervall |
|---|---|
| SSE `/api/events` | 1 dauerhafte Verbindung, im Hintergrund nach 45 s geschlossen |
| `/api/rf/overview` | 3 s (Expertenmodus 2 s, Seite „Funk & Profile“ 1 s) |
| `/api/system` + `/api/wifi/status` | 15 s, nacheinander |
| `/api/mqtt` | 30 s (MQTT-Reiter 5 s) |

Bei Ausfall verlängern sich alle Intervalle per Backoff bis 30 s. Das Qualitätsziel „≤ 1 Anfrage/5 s“ ist ohne Firmware-Änderung nicht erreichbar: Profil und Frequenz der Funkmodule müssten dafür im `rf_rssi`-Frame mitkommen, dann entfällt das `rf/overview`-Polling weitgehend.

## Nachladen bei Bedarf

uPlot (Pegelverläufe), uqr (QR-Code bei der 2FA-Einrichtung), der Konto-Reiter sowie die Expertenseiten Live, Funk & Profile und System werden per `import()` erst beim ersten Öffnen geladen.
