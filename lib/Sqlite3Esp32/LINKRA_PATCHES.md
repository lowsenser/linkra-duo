# Lokale Änderungen an Sqlite3Esp32

Quelle: https://github.com/siara-cc/esp32_arduino_sqlite3_lib, Commit
10fb9cfb254cf0261fdb298ee2d7ebc37973f907 (Version 2.5), Lizenz siehe LICENSE.
Nur `src/` übernommen (Beispiele und Bilder weggelassen).

1. `src/esp32.cpp`, `ESP32Open`: Temporäre Dateinamen wurden durch Voranstellen
   des Datenbankpfads in den von `tmpnam(NULL)` gelieferten newlib-Puffer
   geschrieben und überliefen ihn (Heap-Beschädigung, Absturz beim Beenden der
   Task, die die Datei geöffnet hatte). Jetzt lokaler Puffer mit Zähler.
   Ausgelöst durch `ORDER BY` über genug Zeilen (Sortierer lagert aus).
