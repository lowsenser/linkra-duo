# Mitmachen

Beiträge sind willkommen – Fehlerberichte, Decoder-Verbesserungen, Doku, Code.

## Ablauf

1. Bei größeren Änderungen zuerst ein Issue eröffnen und den Ansatz abstimmen.
2. Fork anlegen, Feature-Branch erstellen.
3. Bauen und testen (siehe README, Abschnitt „Bauen“ und „Tests“):
   - `pio run -e linkra-s3-breadboard`
   - `cd web && npm ci && npm run build`, `node --test web/tests`
   - Host-Tests in `tests/` mit `-fsanitize=address,undefined`
4. Pull Request mit kurzer Beschreibung: was, warum, wie getestet (gern mit Gerät/Frequenz).

## Regeln

- **Keine persönlichen Daten** in Code, Tests, Logs oder Issues: keine WLAN-Namen/Passwörter,
  IP-/MAC-Adressen, Zählernummern oder Funk-IDs realer Geräte. Für Tests Platzhalter-IDs
  (z. B. `12345678`) oder öffentliche Testvektoren verwenden.
- `config/secrets.h` niemals einchecken (Vorlage: `config/secrets.example.h`).
- In Anleitungen und Skripten **nie** `pio run -t uploadfs` verwenden (löscht die
  Gerätedatenbank, siehe `docs/FLASHING.md`).
- Reine Logik möglichst ohne Arduino-Abhängigkeit schreiben und mit einem Host-Test in
  `tests/` absichern.
- Commit-Nachrichten kurz und aussagekräftig; Deutsch oder Englisch.
- Code in `lib/rtl_433_ESP_433/src/rtl_433/` stammt aus rtl_433. Allgemeine Decoder-Fehler
  bitte möglichst auch upstream melden.

## Lizenz

Mit einem Beitrag erklärst du dich einverstanden, dass er unter der
[GPL-3.0](LICENSE) veröffentlicht wird.
