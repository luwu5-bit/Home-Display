# Öffi-Display

Kleines Display für zu Hause, das die aktuellen Abfahrtszeiten der Wiener Linien (Echtzeit) anzeigt, damit man beim Rausgehen weiß, wann die U-Bahn kommt.

## Hardware
- Board: diymore 2,8" ESP32 Display mit Gehäuse, Chip ESP32-32E (LCDwiki Modell E32R28T)
- 2,8 Zoll, 240x320, resistiver Touch, WLAN
- Nicht das klassische CYD (ESP32-2432S028R): Pins und Display-Treiber laut Doku auf lcdwiki.com (E32R28T) verwenden, nicht die CYD-Standardwerte
- Betrieb dauerhaft per USB-Kabel

## Software
- VS Code mit PlatformIO, Framework Arduino
- Bauen und Hochladen über die PlatformIO-Buttons in VS Code
- Display-Bibliothek: TFT_eSPI oder LovyanGFX, Konfiguration passend zum E32R28T

## Datenquelle
- Wiener Linien Echtzeit-API (Open Government Data), kein API-Key nötig
- Endpoint: `https://www.wienerlinien.at/ogd_realtime/monitor?stopId=<ID>` (mehrere `stopId` Parameter möglich)
- Haltestellen-IDs aus der offiziellen OGD-Haltepunkte-Liste der Wiener Linien
- Abfrage etwa alle 30 bis 60 Sekunden, nicht öfter
- Haltestellen: noch festzulegen

## Anzeige
- Pro Linie: Linie, Richtung, Minuten bis zur Abfahrt (die nächsten 2)
- Platz für etwa 3 bis 4 Linien, sonst Seiten durchwechseln
- Bei WLAN- oder API-Fehler eine klare Meldung anzeigen statt alte Daten

## Arbeitsweise
- Luca ist Einsteiger bei Mikrocontrollern und arbeitet lieber mit grafischen Oberflächen als im Terminal: Schritte so erklären, dass sie in VS Code per Klick gehen
- Commits selbstständig via Github Desktop
- WLAN-Zugangsdaten in eine eigene Datei (`include/secrets.h`) auslagern und in `.gitignore` eintragen
- Kurz und knapp antworten, keine Gedankenstriche verwenden
- Erst einen minimalen Test bauen (Display zeigt Text, WLAN verbindet), dann die API, dann das Layout
