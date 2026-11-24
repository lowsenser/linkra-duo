# Sicherheit / Security Policy

## Unterstützte Versionen

Linkra Duo ist im Beta-Stadium. Sicherheitskorrekturen gibt es nur für den jeweils
neuesten Stand des Hauptzweigs.

| Version | Unterstützt |
|---|---|
| 0.6.x-beta | ja |
| < 0.6 | nein |

## Bekannte Einschränkungen (Beta)

Web-UI und API sind durch Anmeldung (optional mit 2FA, siehe `docs/auth-api.md`)
geschützt, laufen aber über unverschlüsseltes HTTP im Heimnetz. Der Setup-Hotspot ist
offen und Firmware-Abbilder werden nicht signiert. Diese Punkte sind bekannt und müssen nicht gemeldet werden. Betreibe das Gerät
nur in einem vertrauenswürdigen Netz und nicht aus dem Internet erreichbar.

## Sicherheitslücke melden

Bitte **keine öffentlichen Issues** für Sicherheitslücken anlegen.

- Meldung ausschließlich über GitHub: Reiter **Security** → **„Report a vulnerability“**
  (Private Vulnerability Reporting). Die Meldung ist nur für die Maintainer sichtbar.

Hilfreich sind: betroffene Version, Beschreibung, Schritte zum Nachstellen, mögliche
Auswirkungen. Bitte keine personenbezogenen Daten Dritter mitschicken.

Wir bestätigen den Eingang in der Regel innerhalb von 7 Tagen, stimmen eine Frist für
die Veröffentlichung ab (Richtwert 90 Tage) und nennen Meldende auf Wunsch im
Changelog.

---

**English:** Please do not open public issues for security vulnerabilities. Report them only
via the repository's **Security** tab → **"Report a vulnerability"** (GitHub private
vulnerability reporting). Web UI and API require a login (optional TOTP 2FA) but run over
plain HTTP on the local network; the setup hotspot is open and firmware images are not
signed – these known beta limitations are listed above.
