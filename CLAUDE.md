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
- Antworten mit `http.useHTTP10(true)` abfragen, sonst kommen sie "chunked" und ArduinoJson kann sie nicht lesen
- Haltestellen (eingestellt in `BLOCKS` in `src/main.cpp`, je 2 Blöcke = 1 Seite):
  - U2 Museumsquartier Richtung Seestadt, RBL 4204, oben. Abfahrten unter 5 Minuten werden ausgeblendet (Fußweg, von Luca so eingestellt). Ziel pro Zug aus der API (Seestadt oder Aspernstraße)
  - 57A Köstlergasse Richtung Rudolfsheim Anschützgasse, RBL 8227, unten. Keine Gehzeit, Ziel wird als "Rudolfsheim" angezeigt
  - Seite 2: U4 Kettenbrückengasse (4418 Hütteldorf, 4419 Heiligenstadt) und U3 Neubaugasse (4918 Ottakring, 4923 Simmering), je Richtung die nächste Abfahrt ab 5 Minuten
  - Seite 3: 13A Richtung Hauptbahnhof und 14A Richtung Reumannplatz, beide Haus des Meeres RBL 668 (gleicher Steig, Filter nach Linienname), keine Gehzeit
  - RBL Zuordnung der Steige über GTFS Koordinaten ermittelt, da die Live-API nachts leer war
- Die Echtzeit-API liefert nur etwa die nächste Stunde. Haltestellen ohne Abfahrten fehlen nachts ganz in der Antwort, das ist kein Fehler
- Fahrplan als Ersatz: Kommen für einen Steig keine Live-Daten (API Fehler, kein WLAN, nachts keine Abfahrten), zeigt die Linie die Zeiten aus dem Fahrplan. `include/fahrplan.h` wird von `tools/fahrplan.py` aus dem GTFS der Wiener Linien erzeugt (Steige und Ziele dort in `SOURCES`). Aktueller Fahrplan gültig bis 12.12.2026, nach jedem Fahrplanwechsel neu erzeugen
- Fahrplan braucht die Uhrzeit (NTP). Nur wenn weder Live-Daten noch Uhrzeit da sind, kommt eine rote Fehlermeldung

## Wetter
- Open-Meteo für Wien, kein API-Key nötig, Abfrage alle 15 Minuten
- Kopfzeile links: aktuelle Temperatur, Tagesmaximum, dann "Regen jetzt", "Regen ab 17 Uhr" (ab 50 % Wahrscheinlichkeit, nächste 12 Stunden) oder sonst das aktuelle Wetter (z.B. bewölkt, sonnig)
- Bei Fehler grau "Wetter nicht verfügbar"

## Anzeige
- Stil wie die U-Bahn Anzeigen der Wiener Linien: schwarzer Hintergrund, gelbe Schrift (RGB 255,230,0, am Display ausgewählt)
- Schrift: U8g2_for_TFT_eSPI (wegen Umlauten), helvB (Helvetica fett), am Display ausgewählt
- Kopfzeile: Wetter links, Uhrzeit (NTP, Wiener Zeit) rechts
- Darunter 2 Hälften, je eine Linie (Punkte unten zeigen die Seite): Liniennummer groß links, gelber Trennstrich, Haltestelle klein grau, 2 Zeilen mit Ziel in Großbuchstaben und Minuten rechts
- Alle Ziele in gleicher Schriftgröße (helvB14), nur bei zu langen Namen kleiner
- Touch (XPT2046, eigene Pins CS 33, IRQ 36, CLK 25, MOSI 32, MISO 39): runterwischen oder unten tippen = nächste Seite, rauf oder oben = vorige, nach 90 Sekunden zurück zu Seite 1
- Grüner Punkt unter der Liniennummer = alle angezeigten Abfahrten sind live. Ohne Punkt sind es (teilweise) Fahrplanzeiten
- Ab 60 Minuten wird statt Minuten die Uhrzeit angezeigt
- Partition `huge_app.csv`, da Schriften viel Flash brauchen

## Arbeitsweise
- Luca ist Einsteiger bei Mikrocontrollern und arbeitet lieber mit grafischen Oberflächen als im Terminal: Schritte so erklären, dass sie in VS Code per Klick gehen
- Commits selbstständig via Github Desktop
- WLAN-Zugangsdaten in eine eigene Datei (`include/secrets.h`) auslagern und in `.gitignore` eintragen
- Kurz und knapp antworten, keine Gedankenstriche verwenden
- Erst einen minimalen Test bauen (Display zeigt Text, WLAN verbindet), dann die API, dann das Layout
