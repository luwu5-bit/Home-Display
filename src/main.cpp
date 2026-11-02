// Öffi-Display: Echtzeit Abfahrten der Wiener Linien

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <time.h>
#include "secrets.h"
#include "fahrplan.h"  // erzeugt von tools/fahrplan.py

// Ein Steig: RBL Nummer aus der OGD Haltepunkte Liste für die Live Daten und
// der passende Fahrplan, der angezeigt wird, wenn keine Live Daten kommen
struct Source {
  int rbl;
  FpSourceId timetable;
};

// Ein Block am Bildschirm = eine Linie an einer Haltestelle
// minMinutes: Abfahrten, die früher kommen, sind nicht erreichbar und werden ausgeblendet
// towards: eigener kurzer Name für das Ziel, nullptr = Ziel aus der API
// sources: ein Steig = die nächsten 2 Abfahrten, zwei Steige = je Richtung die nächste
struct Block {
  const char *line;
  const char *station;
  int minMinutes;
  const char *towards;
  Source sources[2];
};

// Je 2 Blöcke bilden eine Seite (oben, unten)
const Block BLOCKS[] = {
    // Seite 1
    {"U2", "Museumsquartier", 5, nullptr, {{4204, FP_U2_MUSEUMSQUARTIER}}},
    {"57A", "Köstlergasse", 0, "Rudolfsheim", {{8227, FP_57A_KOESTLERGASSE}}},
    // Seite 2: beide Richtungen
    {"U4", "Kettenbrückengasse", 5, nullptr,
     {{4418, FP_U4_KETTENBRUECKENGASSE_HUETTELDORF},
      {4419, FP_U4_KETTENBRUECKENGASSE_HEILIGENSTADT}}},
    {"U3", "Neubaugasse", 5, nullptr,
     {{4918, FP_U3_NEUBAUGASSE_OTTAKRING}, {4923, FP_U3_NEUBAUGASSE_SIMMERING}}},
    // Seite 3: 13A und 14A halten am selben Steig
    {"13A", "Haus des Meeres", 4, nullptr, {{668, FP_13A_HAUS_DES_MEERES}}},
    {"14A", "Haus des Meeres", 4, nullptr, {{668, FP_14A_HAUS_DES_MEERES}}},
};
const int BLOCK_COUNT = sizeof(BLOCKS) / sizeof(BLOCKS[0]);
const int DEPARTURE_PAGES = (BLOCK_COUNT + 1) / 2;
const int SHOPPING_PAGE = DEPARTURE_PAGES;  // danach: Einkaufsliste
const int NOTES_PAGE = DEPARTURE_PAGES + 1;     // und Pinnwand
const int LIGHT_PAGE = DEPARTURE_PAGES + 2;     // und Licht
const int PAGE_COUNT = DEPARTURE_PAGES + 3;

// Bereiche nebeneinander (links/rechts wechseln), darin Seiten untereinander (oben/unten)
struct Section {
  int first;  // erste Seite des Bereichs
  int count;  // Anzahl Seiten
};
const Section SECTIONS[] = {
    {0, DEPARTURE_PAGES},  // Öffi: die Stationen
    {SHOPPING_PAGE, 2},    // WG: Einkaufsliste, Pinnwand
    {LIGHT_PAGE, 1},       // Licht
};
const int SECTION_COUNT = sizeof(SECTIONS) / sizeof(SECTIONS[0]);

int sectionOf(int page) {
  for (int s = SECTION_COUNT - 1; s > 0; s--) {
    if (page >= SECTIONS[s].first) return s;
  }
  return 0;
}

// WG: Einkaufsliste und Pinnwand
const char *WG_NAMES[] = {"Luca", "Arthur", "Daniel"};
const int WG_COUNT = sizeof(WG_NAMES) / sizeof(WG_NAMES[0]);
// Eingetragen wird über die Web-App (Ordner webapp/), das Display holt sich die Daten von dort
const int SHOPPING_MAX = 20;  // wie in webapp/lib/store.js
const int NOTES_MAX = 10;

// IKEA DIRIGERA Hub: lokale API (HTTPS, Port 8443), Hub wird per mDNS gefunden.
// Koppeln am Display, Zugangsschlüssel und Adresse werden im Flash gespeichert.
const char *HUB_CLIENT_NAME = "oeffi-display";
const unsigned long HUB_PAIR_MS = 60000;  // so lange wartet der Hub auf den Knopfdruck
const int LIGHT_BUTTON_TOP = 76;          // Knopf "Hub koppeln", zwischen den Rändern
const int LIGHT_BUTTON_H = 80;
const int LIST_TOP = 48;                  // Seite 1 nur Schalter "Alle Lichter", danach je 2 Räume
                                          // (Abstand zur Kopfzeile, die bis 40 px die Einstellungen öffnet)
const int LIST_ROW_H = 60;                // groß genug für den resistiven Touch
const char *TOP_ROOMS[] = {"Arthur", "Luca"};  // Räume, deren Name das enthält, stehen in dieser Reihenfolge oben
const int LIST_ROWS = 2;                  // Räume pro Seite, darunter frei zum Weiterblättern
const int DEVICES_MAX = 40;

// Touch (XPT2046, eigene Pins laut lcdwiki E32R28T)
const int TOUCH_CS = 33;
const int TOUCH_IRQ = 36;
const int TOUCH_SCLK = 25;
const int TOUCH_MISO = 39;
const int TOUCH_MOSI = 32;
const bool TOUCH_FLIP_Y = false;  // auf true setzen, falls rauf und runter vertauscht sind
const int TOUCH_Y_MIN = 300;      // Rohwert oben und unten am Bildschirmrand (ungefähr)
const int TOUCH_Y_MAX = 3800;
const bool TOUCH_FLIP_X = false;  // auf true setzen, falls links und rechts vertauscht sind
const int TOUCH_X_MIN = 300;      // Rohwert links und rechts am Bildschirmrand (ungefähr)
const int TOUCH_X_MAX = 3800;
const int SIDE_ZONE = 64;         // Tippen am linken oder rechten Rand wechselt den Bereich
const int SWIPE_MIN = 800;        // Mindestweg für einen Wisch (Rohwerte 0 bis 4095)
const unsigned long PAGE_TIMEOUT_MS = 90000;  // danach zurück zur ersten Seite

// Energiesparen: Display wird dunkel, Berühren macht es hell.
// Ob am Tag, ob nachts und nach welcher Zeit, lässt sich in den Einstellungen ändern.
const int NIGHT_FROM_HOUR = 23;
const int NIGHT_TO_HOUR = 6;
const int DAY_BRIGHTNESS = 255;     // 0 bis 255
const int SLEEP_BRIGHTNESS = 0;     // 0 = ganz aus, z.B. 20 = schwach leuchtend
// Auswahl für "Dunkel nach" in Sekunden
const int SLEEP_OPTIONS[] = {30, 60, 120, 300, 600, 1800};
const int SLEEP_OPTION_COUNT = sizeof(SLEEP_OPTIONS) / sizeof(SLEEP_OPTIONS[0]);

// Update über WLAN: in VS Code die Umgebung "env:ota" wählen und hochladen
const char *OTA_HOSTNAME = "oeffi-display";  // im Netzwerk als oeffi-display.local

const char *API_BASE = "https://www.wienerlinien.at/ogd_realtime/monitor?";
const char *TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";  // Wien, mit Sommerzeit

// Wetter Wien (Open-Meteo, kein API Key nötig), Regen für die nächsten 12 Stunden
const char *WEATHER_URL =
    "https://api.open-meteo.com/v1/forecast?latitude=48.2082&longitude=16.3738"
    "&current=temperature_2m,precipitation,weather_code,is_day&hourly=precipitation_probability"
    "&daily=temperature_2m_max&timezone=Europe%2FVienna&forecast_days=1&forecast_hours=12";
const int RAIN_CHANCE = 50;  // ab dieser Regenwahrscheinlichkeit in % gilt es als Regen

const unsigned long REFRESH_MS = 30000;          // Abfahrten alle 30 Sekunden
const unsigned long WEATHER_MS = 15 * 60000UL;  // Wetter alle 15 Minuten

// Farben im Stil der U-Bahn Anzeigen
const uint16_t COLOR_LED = 0xFF20;   // Gelb, RGB 255,230,0 (Farbtest B)
const uint16_t COLOR_DIM = 0x8410;   // Grau
const uint16_t COLOR_ERROR = 0xF800; // Rot
const uint16_t COLOR_LIVE = 0x07E0;  // Grün, Punkt für Live Daten

TFT_eSPI tft;
U8g2_for_TFT_eSPI u8f;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

// Abfahrten pro Block, in der Reihenfolge von BLOCKS
struct Row {
  int count;          // Anzahl Abfahrten (0 bis 2)
  String towards[2];  // Ziel pro Abfahrt
  String when[2];     // Minuten ("7") oder Uhrzeit ab einer Stunde ("05:33")
  bool live[2];       // true = Live Daten, false = Fahrplan
};

Row rows[BLOCK_COUNT];
bool hasData = false;  // false = gerade wird eine Fehlermeldung angezeigt
int currentPage = 0;
bool inSettings = false;
int brightness = DAY_BRIGHTNESS;

// Einstellungen, gespeichert im Flash (bleiben nach Neustart und Update erhalten)
Preferences prefs;
bool daySaving = false;   // auch am Tag dunkel werden
bool nightSaving = true;  // nachts dunkel
int sleepOption = 1;      // Index in SLEEP_OPTIONS, 1 = 1 Minute

// WG Daten aus der Web-App
String shopping[SHOPPING_MAX];
int shoppingCount = 0;

struct Note {
  time_t time;
  String author;
  String text;
};
Note notes[NOTES_MAX];  // älteste zuerst
int noteCount = 0;
String wgError;  // leer = Daten aus der Web-App sind aktuell

// DIRIGERA Hub
String hubIp;        // leer = noch nicht gefunden
String hubToken;     // leer = noch nicht gekoppelt
String hubCode;      // laufende Kopplung: Code vom Hub, wartet auf den Knopfdruck
String hubVerifier;  // Geheimnis zur laufenden Kopplung
unsigned long hubPairStart = 0;
String lightStatus;  // Ergebnis unten auf der Seite, z.B. "5 Lichter ausgeschaltet"
uint16_t lightStatusColor = COLOR_DIM;

// Schaltbare Geräte am Hub (Lichter und Steckdosen, an den Steckdosen hängen Lampen), nach Raum sortiert
struct Device {
  String id;
  String room;
  bool isOn;
  bool reachable;
};
Device devices[DEVICES_MAX];
int deviceCount = 0;
bool devicesLoaded = false;  // Liste wurde schon einmal geholt
int rooms[DEVICES_MAX];      // je Raum das erste Gerät in devices
int roomCount = 0;
int lightScroll = 0;  // Seite der Licht Liste: 0 = Alle Lichter, ab 1 je LIST_ROWS Räume
unsigned long lastTouch = 0;
unsigned long lastFetch = 0;
unsigned long lastWeather = 0;
String weatherText;  // leer = kein Wetter verfügbar
String liveError;    // Grund, warum keine Live Daten kamen, leer = alles ok
int lastClockMinute = -1;

// Großbuchstaben, auch für Umlaute (UTF-8)
String toUpper(String s) {
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= 'a' && c <= 'z') s[i] = c - 32;
  }
  s.replace("ä", "Ä");
  s.replace("ö", "Ö");
  s.replace("ü", "Ü");
  return s;
}

void drawCentered(const char *text, int y, const uint8_t *font, uint16_t color) {
  u8f.setFont(font);
  u8f.setForegroundColor(color);
  u8f.drawUTF8((tft.width() - u8f.getUTF8Width(text)) / 2, y, text);
}

void drawRight(const char *text, int xRight, int y, const uint8_t *font, uint16_t color) {
  u8f.setFont(font);
  u8f.setForegroundColor(color);
  u8f.drawUTF8(xRight - u8f.getUTF8Width(text), y, text);
}

// Wählt die größte Schrift, in die der Text passt
// Start bei helvB14, damit alle Ziele gleich groß sind
const uint8_t *fitFont(const char *text, int maxWidth) {
  const uint8_t *fonts[] = {u8g2_font_helvB14_tf, u8g2_font_helvB12_tf, u8g2_font_helvB10_tf};
  for (const uint8_t *f : fonts) {
    u8f.setFont(f);
    if (u8f.getUTF8Width(text) <= maxWidth) return f;
  }
  return u8g2_font_helvB10_tf;
}

void showStatus(const char *line1, const char *line2, uint16_t color) {
  tft.fillScreen(TFT_BLACK);
  drawCentered(line1, tft.height() / 2 - 8, u8g2_font_helvB18_tf, color);
  drawCentered(line2, tft.height() / 2 + 22, u8g2_font_helvB12_tf, COLOR_DIM);
  lastClockMinute = -1;
}

// Zahnrad für die Einstellungen, Mitte bei (cx, cy)
void drawGear(int cx, int cy, uint16_t color) {
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    tft.fillCircle(cx + round(cos(a) * 7), cy + round(sin(a) * 7), 2, color);
  }
  tft.fillCircle(cx, cy, 6, color);
  tft.fillCircle(cx, cy, 2, TFT_BLACK);
}

// Kopfzeile: Wetter links, Uhrzeit und Zahnrad rechts
void drawHeader() {
  struct tm now;
  char text[6] = "--:--";
  if (getLocalTime(&now, 0)) {
    strftime(text, sizeof(text), "%H:%M", &now);
    lastClockMinute = now.tm_min;
  }
  tft.fillRect(0, 0, tft.width(), 28, TFT_BLACK);
  drawGear(tft.width() - 14, 15, COLOR_DIM);
  const int clockRight = tft.width() - 30;
  drawRight(text, clockRight, 22, u8g2_font_helvB14_tf, COLOR_LED);

  u8f.setFont(u8g2_font_helvB14_tf);
  int maxWidth = clockRight - u8f.getUTF8Width(text) - 16 - 8;
  bool ok = weatherText.length() > 0;
  const char *weather = ok ? weatherText.c_str() : "Wetter nicht verfügbar";
  u8f.setFont(fitFont(weather, maxWidth));
  u8f.setForegroundColor(ok ? COLOR_LED : COLOR_DIM);
  u8f.drawUTF8(8, 22, weather);
}

// Kurze Beschreibung zum WMO Wettercode von Open-Meteo
String weatherDescription(int code, bool isDay) {
  if (code == 0) return isDay ? "sonnig" : "klar";
  if (code == 1) return isDay ? "heiter" : "fast klar";
  if (code == 2) return "teils bewölkt";
  if (code == 3) return "bewölkt";
  if (code == 45 || code == 48) return "Nebel";
  if (code >= 51 && code <= 57) return "Nieselregen";
  if (code >= 61 && code <= 67) return "Regen";
  if (code >= 71 && code <= 77) return "Schnee";
  if (code >= 80 && code <= 82) return "Regenschauer";
  if (code == 85 || code == 86) return "Schneeschauer";
  if (code >= 95) return "Gewitter";
  return "";
}

// Holt das Wetter für Wien von Open-Meteo, z.B. "11°  max 13°  Regen ab 17 Uhr".
// Bei einem Fehler bleibt weatherText leer.
void fetchWeather() {
  weatherText = "";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);  // Antwort am Stück (nicht "chunked"), sonst kann ArduinoJson sie nicht lesen
  if (!http.begin(client, WEATHER_URL)) return;
  if (http.GET() != HTTP_CODE_OK) {
    http.end();
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.println(String("Wetter JSON Fehler: ") + err.c_str());
    return;
  }

  int temp = lround(doc["current"]["temperature_2m"].as<float>());
  int max = lround(doc["daily"]["temperature_2m_max"][0].as<float>());

  // Regen: jetzt, oder die erste Stunde mit mindestens 50 % Regenwahrscheinlichkeit.
  // Ohne Regen steht das aktuelle Wetter da.
  String rain = weatherDescription(doc["current"]["weather_code"] | 0, doc["current"]["is_day"] | 1);
  if (doc["current"]["precipitation"].as<float>() > 0) {
    rain = "Regen jetzt";
  } else {
    JsonArray times = doc["hourly"]["time"];
    JsonArray chances = doc["hourly"]["precipitation_probability"];
    for (size_t i = 0; i < chances.size(); i++) {
      if ((chances[i] | 0) >= RAIN_CHANCE) {
        String time = times[i].as<String>();  // z.B. "2026-10-24T17:00"
        rain = "Regen ab " + String(time.substring(11, 13).toInt()) + " Uhr";
        break;
      }
    }
  }

  weatherText = String(temp) + "°  max " + String(max) + "°  " + rain;
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  // Nur beim Start anzeigen, sonst bleiben die Abfahrten (Fahrplan) stehen
  if (!hasData && !inSettings) showStatus("WLAN...", WIFI_SSID, COLOR_LED);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Holt die Live Abfahrten in doc. Gibt bei Erfolg einen leeren String zurück, sonst den Fehler.
String fetchDepartures(JsonDocument &doc) {
  String url = API_BASE;
  for (const Block &b : BLOCKS) {
    for (const Source &s : b.sources) {
      String param = "stopId=" + String(s.rbl);
      if (s.rbl == 0 || url.indexOf(param + "&") >= 0 || url.endsWith(param)) continue;
      if (!url.endsWith("?")) url += "&";
      url += param;
    }
  }

  WiFiClientSecure client;
  client.setInsecure();  // kein Zertifikat prüfen, reicht für öffentliche Fahrplandaten

  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);  // Antwort am Stück (nicht "chunked"), sonst kann ArduinoJson sie nicht lesen
  if (!http.begin(client, url)) return "Verbindung fehlgeschlagen";

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return "HTTP Fehler " + String(code);
  }

  // Nur die Felder einlesen, die wir brauchen (spart Speicher)
  JsonDocument filter;
  JsonObject fMonitor = filter["data"]["monitors"][0].to<JsonObject>();
  fMonitor["locationStop"]["properties"]["attributes"]["rbl"] = true;
  JsonObject fLine = fMonitor["lines"][0].to<JsonObject>();
  fLine["name"] = true;
  JsonObject fDep = fLine["departures"]["departure"][0].to<JsonObject>();
  fDep["departureTime"]["countdown"] = true;
  fDep["vehicle"]["towards"] = true;  // Ziel pro Zug (U2: Seestadt oder Aspernstraße)

  // Erst die ganze Antwort empfangen (tagsüber ca. 40 KB), dann auswerten.
  // Direkt aus der Verbindung lesen bricht bei großen Antworten ab ("IncompleteInput").
  String body = http.getString();
  http.end();
  if (body.length() == 0) return "Leere Antwort";

  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter),
                                             DeserializationOption::NestingLimit(20));
  if (err) return String("JSON Fehler: ") + err.c_str() + " (" + body.length() + " Bytes)";
  return "";
}

// Tage seit 1.1.1970 für ein Datum (wie in fahrplan.h)
int32_t daysSinceEpoch(int year, int month, int day) {
  year -= month <= 2;
  int32_t era = (year >= 0 ? year : year - 399) / 400;
  int yoe = year - era * 400;
  int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Sucht im Fahrplan die nächsten Abfahrten ab fromMinute (Minuten seit heute 00:00).
// Berücksichtigt gestern (Fahrten nach Mitternacht), heute und morgen.
// Gibt die Anzahl gefundener Abfahrten zurück, sortiert nach Zeit.
int timetableDepartures(FpSourceId id, int32_t today, int fromMinute, int maxCount,
                        int minutes[], const char *heads[]) {
  const FpSource &src = FP_SOURCES[id];
  int count = 0;
  for (int offset = -1; offset <= 1; offset++) {
    int dayIndex = today + offset - FP_FIRST_DAY;
    if (dayIndex < 0 || dayIndex >= FP_DAY_COUNT) continue;
    int pattern = src.dayPattern[dayIndex];
    const uint16_t *times = src.patterns[pattern];
    for (int i = 0; i < src.patternSizes[pattern]; i++) {
      int t = (times[i] & 0x0FFF) + offset * 1440;
      if (t < fromMinute) continue;
      // In die sortierte Liste einfügen, nur die frühesten maxCount behalten
      int pos = count;
      while (pos > 0 && minutes[pos - 1] > t) pos--;
      if (pos >= maxCount) continue;
      for (int j = min(count, maxCount - 1); j > pos; j--) {
        minutes[j] = minutes[j - 1];
        heads[j] = heads[j - 1];
      }
      minutes[pos] = t;
      heads[pos] = src.heads[times[i] >> 12];
      if (count < maxCount) count++;
    }
  }
  return count;
}

// Minuten bis zur Abfahrt als Text: "7", ab einer Stunde als Uhrzeit "05:33"
String formatWhen(int minutes, const struct tm *now) {
  if (minutes < 60 || !now) return String(minutes);
  int t = (now->tm_hour * 60 + now->tm_min + minutes) % 1440;
  char text[8];
  snprintf(text, sizeof(text), "%02d:%02d", t / 60, t % 60);
  return text;
}

// Füllt rows: Live Daten, wo vorhanden, sonst Fahrplan. doc = nullptr, wenn keine Live Daten.
// Gibt false zurück, wenn gar nichts angezeigt werden kann (keine Live Daten und keine Uhrzeit).
bool fillRows(JsonDocument *doc) {
  struct tm now;
  bool hasTime = getLocalTime(&now, 0);
  if (!doc && !hasTime) return false;

  int32_t today = hasTime ? daysSinceEpoch(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday) : 0;
  int nowMinute = hasTime ? now.tm_hour * 60 + now.tm_min : 0;
  JsonArray monitors;
  if (doc) monitors = (*doc)["data"]["monitors"];

  for (int i = 0; i < BLOCK_COUNT; i++) {
    const Block &block = BLOCKS[i];
    Row &r = rows[i];
    r.count = 0;

    // Ein Steig: die nächsten 2 Abfahrten. Zwei Steige: je Richtung die nächste.
    int sourceCount = block.sources[1].rbl ? 2 : 1;
    int perSource = 2 / sourceCount;

    for (int s = 0; s < sourceCount; s++) {
      const Source &source = block.sources[s];
      int found = 0;
      int lastLive = -1;

      // Live Daten. Nachts fehlen Haltestellen ohne Abfahrten in der Antwort ganz.
      for (JsonObject monitor : monitors) {
        if ((monitor["locationStop"]["properties"]["attributes"]["rbl"] | 0) != source.rbl) continue;
        JsonArray lines = monitor["lines"];
        for (JsonObject line : lines) {
          if (line["name"] != block.line) continue;  // z.B. 13A und 14A am selben Steig
          JsonArray departures = line["departures"]["departure"];
          for (JsonObject dep : departures) {
            if (found == perSource) break;
            int minutes = dep["departureTime"]["countdown"] | -1;
            if (minutes < block.minMinutes) continue;
            String towards =
                block.towards ? String(block.towards) : dep["vehicle"]["towards"].as<String>();
            r.towards[r.count] = toUpper(towards);
            r.when[r.count] = formatWhen(minutes, hasTime ? &now : nullptr);
            r.live[r.count++] = true;
            lastLive = minutes;
            found++;
          }
        }
      }

      // Was fehlt, kommt aus dem Fahrplan (z.B. nachts die erste Abfahrt am Morgen)
      if (found < perSource && hasTime) {
        int from = nowMinute + max(block.minMinutes, lastLive + 2);
        int minutes[2];
        const char *heads[2];
        int n = timetableDepartures(source.timetable, today, from, perSource - found, minutes, heads);
        for (int k = 0; k < n; k++) {
          r.towards[r.count] = toUpper(block.towards ? block.towards : heads[k]);
          r.when[r.count] = formatWhen(minutes[k] - nowMinute, &now);
          r.live[r.count++] = false;
        }
      }
    }
  }
  return true;
}

// Ein Block pro Linie, wie die Anzeige am Bahnsteig:
// Linie links, Trennstrich, Ziel, Minuten (oder Uhrzeit) rechts mit grünem Punkt, wenn live
void drawBlock(const Block &b, const Row &r, int y, int h) {
  const int lineX = 72;  // Trennstrich
  const int destX = lineX + 10;
  const int dotX = tft.width() - 8;           // grüner Punkt ganz rechts
  const int minutesRight = dotX - 6;          // Minuten links davon, Platz für den Punkt

  // Linie links, senkrecht mittig
  u8f.setFont(u8g2_font_helvB24_tf);
  u8f.setForegroundColor(COLOR_LED);
  u8f.drawUTF8(8, y + h / 2 + 12, b.line);

  // Trennstrich
  tft.drawFastVLine(lineX, y + 10, h - 20, COLOR_LED);
  tft.drawFastVLine(lineX + 1, y + 10, h - 20, COLOR_LED);

  // Haltestelle klein darüber
  u8f.setFont(u8g2_font_helvB10_tf);
  u8f.setForegroundColor(COLOR_DIM);
  u8f.drawUTF8(destX, y + 20, b.station);

  // Zwei Zeilen: Ziel und Minuten
  if (r.count == 0) {
    u8f.setFont(u8g2_font_helvB12_tf);
    u8f.setForegroundColor(COLOR_DIM);
    u8f.drawUTF8(destX, y + 52, "Keine Abfahrt gefunden");
  }

  for (int k = 0; k < r.count; k++) {
    int baseline = y + 52 + k * 32;

    // Uhrzeiten sind breiter als Minuten, deshalb etwas kleiner
    bool isTime = r.when[k].indexOf(':') >= 0;
    const uint8_t *whenFont = isTime ? u8g2_font_helvB18_tf : u8g2_font_helvB24_tf;
    drawRight(r.when[k].c_str(), minutesRight, baseline + 2, whenFont, COLOR_LED);

    // Grüner Punkt rechts oben an der Zeit, wenn sie live ist (sonst Fahrplan)
    if (r.live[k]) tft.fillCircle(dotX, baseline + 2 - (isTime ? 16 : 21), 3, COLOR_LIVE);

    int whenWidth = max(46, (int)u8f.getUTF8Width(r.when[k].c_str()) + 10);
    u8f.setFont(fitFont(r.towards[k].c_str(), minutesRight - whenWidth - destX));
    u8f.setForegroundColor(COLOR_LED);
    u8f.drawUTF8(destX, baseline, r.towards[k].c_str());
  }
}

// WG Seiten: Einkaufsliste und Pinnwand, eingetragen über die Webseite am Handy
const int WG_TOP = 32;

// Überschrift der WG Seite, rechts die Adresse für die Handys
void drawWgTitle(const char *title) {
  u8f.setFont(u8g2_font_helvB10_tf);
  u8f.setForegroundColor(COLOR_DIM);
  u8f.drawUTF8(12, WG_TOP + 16, title);
  if (wgError.length() > 0) {
    drawRight(wgError.c_str(), tft.width() - 12, WG_TOP + 16, u8g2_font_helvB10_tf, COLOR_ERROR);
  }
}

void drawEmptyHint(const char *text) {
  u8f.setFont(u8g2_font_helvB14_tf);
  u8f.setForegroundColor(COLOR_DIM);
  u8f.drawUTF8(12, WG_TOP + 50, text);
}

void drawShoppingPage() {
  drawWgTitle("Einkaufsliste");
  if (shoppingCount == 0) return drawEmptyHint("Nichts zu kaufen");

  // Bis zu 8 Einträge, bei mehr 7 und "+ N weitere"
  const int lineH = 22;
  int shown = shoppingCount <= 8 ? shoppingCount : 7;
  u8f.setFont(u8g2_font_helvB14_tf);
  for (int i = 0; i < shown; i++) {
    int baseline = WG_TOP + 40 + i * lineH;
    tft.fillCircle(16, baseline - 6, 3, COLOR_LED);
    u8f.setForegroundColor(COLOR_LED);
    u8f.drawUTF8(28, baseline, shopping[i].c_str());
  }
  if (shown < shoppingCount) {
    u8f.setForegroundColor(COLOR_DIM);
    u8f.drawUTF8(28, WG_TOP + 40 + 7 * lineH, ("+ " + String(shoppingCount - 7) + " weitere").c_str());
  }
}

// Bricht Text an Leerzeichen in höchstens maxLines Zeilen um, die in maxWidth passen
int wrapText(const String &text, int maxWidth, String lines[], int maxLines) {
  int count = 0;
  String line;
  int start = 0;
  while (start <= (int)text.length() && count < maxLines) {
    int end = text.indexOf(' ', start);
    if (end < 0) end = text.length();
    String word = text.substring(start, end);
    String candidate = line.length() ? line + " " + word : word;
    if (line.length() && u8f.getUTF8Width(candidate.c_str()) > maxWidth) {
      lines[count++] = line;
      line = word;
    } else {
      line = candidate;
    }
    start = end + 1;
  }
  if (count < maxLines && line.length()) lines[count++] = line;
  else if (line.length()) lines[maxLines - 1] += "...";  // passt nicht mehr ganz
  return count;
}

void drawNotesPage() {
  drawWgTitle("Pinnwand");
  if (noteCount == 0) return drawEmptyHint("Keine Nachrichten");

  // Neueste zuerst: Name und Zeit klein in Grau, darunter der Text (bis 2 Zeilen)
  int y = WG_TOP + 24;
  for (int i = noteCount - 1; i >= 0; i--) {
    u8f.setFont(u8g2_font_helvB12_tf);
    String lines[2];
    int n = wrapText(notes[i].text, tft.width() - 24, lines, 2);
    int needed = 14 + n * 18 + 6;
    if (y + needed > tft.height() - 10) {
      u8f.setFont(u8g2_font_helvB10_tf);
      u8f.setForegroundColor(COLOR_DIM);
      u8f.drawUTF8(12, y + 12, ("+ " + String(i + 1) + " ältere").c_str());
      break;
    }

    char when[16] = "";
    struct tm t;
    if (localtime_r(&notes[i].time, &t)) {
      const char *days[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
      snprintf(when, sizeof(when), "%s %02d:%02d", days[t.tm_wday], t.tm_hour, t.tm_min);
    }
    u8f.setFont(u8g2_font_helvB10_tf);
    u8f.setForegroundColor(COLOR_DIM);
    u8f.drawUTF8(12, y + 12, (notes[i].author + ", " + when).c_str());

    u8f.setFont(u8g2_font_helvB12_tf);
    u8f.setForegroundColor(COLOR_LED);
    for (int k = 0; k < n; k++) u8f.drawUTF8(12, y + 14 + (k + 1) * 18, lines[k].c_str());
    y += needed;
  }
}

bool hubPairing() { return hubCode.length() > 0 && millis() - hubPairStart < HUB_PAIR_MS; }

// Kürzt Text mit "...", bis er in maxWidth passt (aktuelle Schrift, Umlaute bleiben ganz)
String ellipsize(String s, int maxWidth) {
  if (u8f.getUTF8Width(s.c_str()) <= maxWidth) return s;
  while (s.length() > 0 && u8f.getUTF8Width((s + "...").c_str()) > maxWidth) {
    int i = s.length() - 1;
    while (i > 0 && (s[i] & 0xC0) == 0x80) i--;  // Folgebytes eines Umlauts mitnehmen
    s.remove(i);
  }
  return s + "...";
}

// Ist im Raum, der bei devices[first] beginnt, etwas an? first = -1: in allen Räumen
bool anyOn(int first = -1) {
  for (int i = first < 0 ? 0 : first; i < deviceCount; i++) {
    if (first >= 0 && devices[i].room != devices[first].room) break;
    if (devices[i].isOn && devices[i].reachable) return true;
  }
  return false;
}

// Ein Schalter über die ganze Breite: oben der Name, darunter AN (grün) oder AUS (grau), beides mittig.
// big = großer Schalter "Alle Lichter" mit größerer Schrift.
void drawSwitchRow(String label, bool on, int y, int h, bool big) {
  const int x = SIDE_ZONE + 4;  // nur in der Mitte, die Ränder wechseln den Bereich
  const int w = tft.width() - 2 * x;
  tft.drawRoundRect(x, y + 4, w, h - 8, 10, on ? COLOR_LED : COLOR_DIM);
  tft.drawRoundRect(x + 1, y + 5, w - 2, h - 10, 9, on ? COLOR_LED : COLOR_DIM);
  const uint8_t *labelFont = big ? u8g2_font_helvB18_tf : u8g2_font_helvB14_tf;
  const uint8_t *stateFont = big ? u8g2_font_helvB24_tf : u8g2_font_helvB14_tf;
  int mid = y + h / 2;
  u8f.setFont(labelFont);
  drawCentered(ellipsize(label, w - 16).c_str(), big ? mid - 6 : mid - 2, labelFont, COLOR_LED);
  drawCentered(on ? "AN" : "AUS", big ? mid + 30 : mid + 18, stateFont, on ? COLOR_LIVE : COLOR_DIM);
}

int lightPageCount() { return 1 + (roomCount + LIST_ROWS - 1) / LIST_ROWS; }

// Licht Seite, gekoppelt: Seite 1 der große Schalter "Alle Lichter", danach je 2 Räume.
// Darunter bleibt Platz zum Weiterblättern (tippen oder wischen) und für das Ergebnis.
void drawDeviceList() {
  if (lightScroll == 0) {
    drawSwitchRow("Alle Lichter", anyOn(), LIST_TOP, LIST_ROWS * LIST_ROW_H, true);
  } else {
    for (int r = 0; r < LIST_ROWS; r++) {
      int i = (lightScroll - 1) * LIST_ROWS + r;
      if (i >= roomCount) break;
      String room = devices[rooms[i]].room.length() > 0 ? devices[rooms[i]].room : "Ohne Raum";
      drawSwitchRow(room, anyOn(rooms[i]), LIST_TOP + r * LIST_ROW_H, LIST_ROW_H, false);
    }
  }

  int bottom = LIST_TOP + LIST_ROWS * LIST_ROW_H;
  drawCentered(lightStatus.c_str(), bottom + 20, u8g2_font_helvB10_tf, lightStatusColor);

  // Mehrere Seiten: Punkte senkrecht am linken Rand, Pfeile zum Blättern (links rauf, rechts runter)
  int pages = lightPageCount();
  if (!devicesLoaded) {
    drawCentered("Lade Räume...", bottom + 46, u8g2_font_helvB10_tf, COLOR_DIM);
  } else if (pages > 1) {
    for (int p = 0; p < pages; p++) {
      int y = tft.height() / 2 + 16 + (p * 2 - (pages - 1)) * 6;
      tft.fillCircle(3, y, 2, p == lightScroll ? COLOR_LED : COLOR_DIM);
    }
    int cy = bottom + 44;
    int up = tft.width() / 2 - 44, down = tft.width() / 2 + 44;
    tft.fillTriangle(up - 12, cy + 7, up + 12, cy + 7, up, cy - 7, COLOR_DIM);
    tft.fillTriangle(down - 12, cy - 7, down + 12, cy - 7, down, cy + 7, COLOR_DIM);
  }
}

// Licht Seite: gekoppelt die Geräteliste, sonst ein großer Knopf zum Koppeln
void drawLightPage() {
  if (hubToken.length() > 0 && !hubPairing()) return drawDeviceList();

  u8f.setFont(u8g2_font_helvB10_tf);
  u8f.setForegroundColor(COLOR_DIM);
  u8f.drawUTF8(12, WG_TOP + 16, "Licht");

  const char *label = "Hub koppeln";
  const char *hint = "Einmalig nötig, dauert etwa eine Minute";
  if (hubPairing()) {
    label = "Fertig gedrückt";
    hint = "Knopf auf dem Hub drücken, dann hier tippen";
  }

  // Knopf nur in der Mitte, die Ränder wechseln den Bereich
  const int x = SIDE_ZONE + 4;
  const int w = tft.width() - 2 * x;
  tft.drawRoundRect(x, LIGHT_BUTTON_TOP, w, LIGHT_BUTTON_H, 10, COLOR_LED);
  tft.drawRoundRect(x + 1, LIGHT_BUTTON_TOP + 1, w - 2, LIGHT_BUTTON_H - 2, 9, COLOR_LED);
  u8f.setFont(u8g2_font_helvB18_tf);
  const uint8_t *font = u8f.getUTF8Width(label) <= w - 16 ? u8g2_font_helvB18_tf : u8g2_font_helvB14_tf;
  drawCentered(label, LIGHT_BUTTON_TOP + LIGHT_BUTTON_H / 2 + 8, font, COLOR_LED);
  drawCentered(hint, LIGHT_BUTTON_TOP + LIGHT_BUTTON_H + 24, u8g2_font_helvB10_tf, COLOR_DIM);
  drawCentered(lightStatus.c_str(), LIGHT_BUTTON_TOP + LIGHT_BUTTON_H + 48, u8g2_font_helvB12_tf,
               lightStatusColor);
}

void drawDepartures() {
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  tft.drawFastHLine(0, 30, tft.width(), COLOR_DIM);

  if (currentPage == SHOPPING_PAGE) {
    drawShoppingPage();
  } else if (currentPage == NOTES_PAGE) {
    drawNotesPage();
  } else if (currentPage == LIGHT_PAGE) {
    drawLightPage();
  } else {
    // 2 Blöcke pro Seite, der Bildschirm ist sonst zu voll
    int blockH = (tft.height() - 32) / 2;
    for (int k = 0; k < 2; k++) {
      int i = currentPage * 2 + k;
      if (i >= BLOCK_COUNT) break;
      drawBlock(BLOCKS[i], rows[i], 32 + k * blockH, blockH);
    }
    tft.drawFastHLine(0, 32 + blockH, tft.width(), COLOR_DIM);

    // Kamen keine Live Daten, steht unten links klein der Grund
    if (liveError.length() > 0) {
      u8f.setFont(u8g2_font_helvB08_tf);
      u8f.setForegroundColor(COLOR_DIM);
      u8f.drawUTF8(4, tft.height() - 2, ("Fahrplan: " + liveError).c_str());
    }
  }

  // Seitenanzeige: Punkte unten in der Mitte
  // Bereich: Punkte unten in der Mitte. Seite im Bereich: Punkte senkrecht am linken Rand.
  int section = sectionOf(currentPage);
  for (int s = 0; s < SECTION_COUNT; s++) {
    int x = tft.width() / 2 + (s * 2 - (SECTION_COUNT - 1)) * 6;
    tft.fillCircle(x, tft.height() - 5, 2, s == section ? COLOR_LED : COLOR_DIM);
  }
  const Section &sec = SECTIONS[section];
  if (sec.count > 1) {
    for (int p = 0; p < sec.count; p++) {
      int y = tft.height() / 2 + 16 + (p * 2 - (sec.count - 1)) * 6;
      tft.fillCircle(3, y, 2, sec.first + p == currentPage ? COLOR_LED : COLOR_DIM);
    }
  }
}

// WG und Licht Seiten gehen auch ohne Abfahrten
bool isWgPage(int page) { return page >= DEPARTURE_PAGES; }

bool refreshDevices();

void showPage(int page) {
  currentPage = (page + PAGE_COUNT) % PAGE_COUNT;
  lightStatus = "";  // altes Ergebnis nicht mehr anzeigen
  lightScroll = 0;
  if (hasData || isWgPage(currentPage)) drawDepartures();
  // Licht Seite: erst die alte Liste zeigen, dann den aktuellen Stand vom Hub holen
  if (currentPage == LIGHT_PAGE && refreshDevices()) drawDepartures();
}

int lightPageCount();

// Licht Liste weiterblättern: +1 nächste Seite, -1 vorige, am Ende wieder von vorne
void scrollDevices(int delta) {
  int pages = lightPageCount();
  if (pages < 2) return;
  lightScroll = (lightScroll + delta + pages) % pages;
  drawDepartures();
}

// Links/rechts: anderer Bereich, immer bei dessen erster Seite
void showSection(int section) {
  section = (section + SECTION_COUNT) % SECTION_COUNT;
  showPage(SECTIONS[section].first);
}

// Oben/unten: nächste (+1) oder vorige (-1) Seite im Bereich, am Ende wieder von vorne
void showSubPage(int delta) {
  const Section &sec = SECTIONS[sectionOf(currentPage)];
  if (sec.count < 2) return;
  int sub = (currentPage - sec.first + delta + sec.count) % sec.count;
  showPage(sec.first + sub);
}

// Holt Einkaufsliste und Pinnwand von der Web-App (nur lesen, mit dem Display-Schlüssel)
void fetchWg() {
  if (strlen(WEBAPP_URL) == 0) {
    wgError = "Web-App fehlt";
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);
  if (!http.begin(client, String(WEBAPP_URL) + "/api/state")) {
    wgError = "Nicht erreichbar";
    return;
  }
  http.addHeader("x-display-key", DISPLAY_KEY);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    wgError = code == 401 ? "Falscher Schlüssel" : "Fehler " + String(code);
    return;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    wgError = "Ungültige Daten";
    return;
  }

  shoppingCount = 0;
  for (JsonObject item : doc["shopping"].as<JsonArray>()) {
    if (shoppingCount == SHOPPING_MAX) break;
    shopping[shoppingCount++] = item["text"].as<String>();
  }
  noteCount = 0;
  for (JsonObject note : doc["notes"].as<JsonArray>()) {
    if (noteCount == NOTES_MAX) break;
    notes[noteCount++] = {(time_t)(note["time"] | 0L), note["author"].as<String>(),
                          note["text"].as<String>()};
  }
  wgError = "";
}

// Sucht den DIRIGERA Hub im WLAN (mDNS Dienst _ihsp._tcp).
// mDNS läuft schon, ArduinoOTA hat es gestartet.
bool findHub() {
  int n = MDNS.queryService("ihsp", "tcp");
  for (int i = 0; i < n; i++) {
    IPAddress ip = MDNS.address(i);
    if (ip == IPAddress()) ip = MDNS.queryHost(MDNS.hostname(i));
    if (ip == IPAddress()) continue;
    hubIp = ip.toString();
    prefs.putString("hubIp", hubIp);
    Serial.println("DIRIGERA Hub gefunden: " + hubIp);
    return true;
  }
  return false;
}

// Eine Anfrage an den Hub. Der Hub hat ein eigenes Zertifikat, daher ohne Prüfung.
// client und http über mehrere Anfragen behalten, dann bleibt die Verbindung offen (schneller).
int hubRequest(WiFiClientSecure &client, HTTPClient &http, const char *method, const String &path,
               const String &body, String &response) {
  if (!http.begin(client, "https://" + hubIp + ":8443/v1/" + path)) return -1;
  http.setTimeout(10000);
  if (hubToken.length() > 0) http.addHeader("Authorization", "Bearer " + hubToken);
  if (body.length() > 0) {
    bool json = body[0] == '[' || body[0] == '{';
    http.addHeader("Content-Type", json ? "application/json" : "application/x-www-form-urlencoded");
  }
  int code = http.sendRequest(method, body);
  response = http.getString();
  http.end();
  return code;
}

// Koppeln Schritt 1: Code beim Hub anfordern, danach muss jemand den Knopf am Hub drücken
String hubStartPairing() {
  if (!findHub()) return "Hub nicht gefunden";

  // Zufälliges Geheimnis, der Hub bekommt nur seinen SHA256 Wert (OAuth PKCE)
  const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
  hubVerifier = "";
  for (int i = 0; i < 128; i++) hubVerifier += alphabet[esp_random() % 66];

  uint8_t hash[32];
  mbedtls_sha256((const uint8_t *)hubVerifier.c_str(), hubVerifier.length(), hash, 0);
  unsigned char b64[48];
  size_t len = 0;
  mbedtls_base64_encode(b64, sizeof(b64), &len, hash, sizeof(hash));
  String challenge = String((char *)b64).substring(0, len);
  challenge.replace("+", "-");
  challenge.replace("/", "_");
  challenge.replace("=", "");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String response;
  hubToken = "";
  int code = hubRequest(client, http, "GET",
                        "oauth/authorize?audience=homesmart.local&response_type=code"
                        "&code_challenge_method=S256&code_challenge=" + challenge,
                        "", response);
  JsonDocument doc;
  if (code != HTTP_CODE_OK || deserializeJson(doc, response) || !doc["code"].is<const char *>()) {
    return "Hub Fehler " + String(code);
  }
  hubCode = doc["code"].as<String>();
  hubPairStart = millis();
  return "";
}

// Koppeln Schritt 2: nach dem Knopfdruck den Zugangsschlüssel abholen
String hubFinishPairing() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String response;
  int code = hubRequest(client, http, "POST", "oauth/token",
                        "code=" + hubCode + "&name=" + HUB_CLIENT_NAME +
                            "&grant_type=authorization_code&code_verifier=" + hubVerifier,
                        response);
  JsonDocument doc;
  if (code != HTTP_CODE_OK || deserializeJson(doc, response) ||
      !doc["access_token"].is<const char *>()) {
    return "Knopf nicht erkannt, nochmal tippen";
  }
  hubCode = "";
  hubToken = doc["access_token"].as<String>();
  prefs.putString("hubToken", hubToken);
  return "";
}

// Fehlertext zu einer Antwort vom Hub. 401 = Kopplung wurde in der IKEA App entfernt.
String hubError(int code) {
  if (code == 401) {
    hubToken = "";
    prefs.remove("hubToken");
    deviceCount = 0;
    roomCount = 0;
    devicesLoaded = false;
    return "Bitte neu koppeln";
  }
  return code < 0 ? "Hub nicht erreichbar" : "Hub Fehler " + String(code);
}

// Platz eines Raums in der Liste: zuerst TOP_ROOMS, dann die übrigen, Geräte ohne Raum ganz unten
int roomRank(const String &room) {
  const int top = sizeof(TOP_ROOMS) / sizeof(TOP_ROOMS[0]);
  if (room.length() == 0) return top + 1;
  for (int i = 0; i < top; i++) {
    if (room.indexOf(TOP_ROOMS[i]) >= 0) return i;
  }
  return top;
}

// Kommt a in der Liste vor b? Nach roomRank, sonst nach Name.
bool deviceBefore(const Device &a, const Device &b) {
  int ra = roomRank(a.room), rb = roomRank(b.room);
  if (ra != rb) return ra < rb;
  return a.room < b.room;
}

// Holt alle Lichter und Steckdosen vom Hub in devices. Gibt bei Erfolg einen leeren String zurück, sonst den Fehler.
String hubFetchDevices(WiFiClientSecure &client, HTTPClient &http) {
  String response;
  int code = hubIp.length() > 0 ? hubRequest(client, http, "GET", "devices", "", response) : -1;
  if (code < 0) {
    // Hub hat vielleicht eine neue Adresse bekommen
    if (!findHub()) return "Hub nicht gefunden";
    code = hubRequest(client, http, "GET", "devices", "", response);
  }
  if (code != HTTP_CODE_OK) return hubError(code);

  // Nur die Felder einlesen, die wir brauchen
  JsonDocument filter;
  JsonObject f = filter[0].to<JsonObject>();
  f["id"] = true;
  f["deviceType"] = true;
  f["isReachable"] = true;
  f["room"]["name"] = true;
  f["attributes"]["isOn"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response, DeserializationOption::Filter(filter));
  response = "";  // Speicher freigeben
  if (err) return String("JSON Fehler: ") + err.c_str();

  deviceCount = 0;
  for (JsonObject d : doc.as<JsonArray>()) {
    if (deviceCount == DEVICES_MAX) break;
    String type = d["deviceType"] | "";
    if ((type != "light" && type != "outlet") || !d["attributes"]["isOn"].is<bool>()) continue;
    Device &dev = devices[deviceCount++];
    dev.id = d["id"].as<String>();
    dev.room = d["room"]["name"] | "";
    dev.reachable = d["isReachable"] | true;
    dev.isOn = d["attributes"]["isOn"];
  }

  // Sortieren (wenige Geräte, einfaches Einfügen reicht)
  for (int i = 1; i < deviceCount; i++) {
    for (int k = i; k > 0 && deviceBefore(devices[k], devices[k - 1]); k--) {
      Device tmp = devices[k];
      devices[k] = devices[k - 1];
      devices[k - 1] = tmp;
    }
  }

  roomCount = 0;
  for (int i = 0; i < deviceCount; i++) {
    if (i == 0 || devices[i].room != devices[i - 1].room) rooms[roomCount++] = i;
  }
  if (lightScroll >= lightPageCount()) lightScroll = 0;
  devicesLoaded = true;
  return "";
}

// Holt die Geräteliste neu, falls gekoppelt und WLAN da. Fehler stehen danach unten auf der Seite.
// Gibt false zurück, wenn gar nicht gefragt wurde.
bool refreshDevices() {
  if (hubToken.length() == 0 || hubPairing() || WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String error = hubFetchDevices(client, http);
  if (error.length() > 0) {
    lightStatus = error;
    lightStatusColor = COLOR_ERROR;
  } else if (lightStatusColor == COLOR_ERROR) {
    lightStatus = "";  // alter Fehler ist erledigt
  }
  return true;
}

// Schaltet alle erreichbaren Lichter und Steckdosen ein oder aus (an den Steckdosen hängen Lampen),
// bei oneRoom nur die im Raum room.
// Gibt bei Erfolg einen leeren String zurück und das Ergebnis in result, sonst den Fehler.
String hubSetAll(bool on, String &result, bool oneRoom = false, String room = "") {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String error = hubFetchDevices(client, http);  // aktueller Stand, wer gerade an ist
  if (error.length() > 0) return error;

  String response;
  int switched = 0, failed = 0;
  for (int i = 0; i < deviceCount; i++) {
    Device &d = devices[i];
    if (d.isOn == on || !d.reachable || (oneRoom && d.room != room)) continue;
    int code = hubRequest(client, http, "PATCH", "devices/" + d.id,
                          on ? "[{\"attributes\":{\"isOn\":true}}]" : "[{\"attributes\":{\"isOn\":false}}]",
                          response);
    if (code >= 200 && code < 300) {
      d.isOn = on;
      switched++;
    } else {
      failed++;
    }
  }
  if (failed > 0) return String(failed) + " nicht erreichbar";
  if (switched == 0) result = on ? "War schon alles an" : "War schon alles aus";
  else result = String(switched) + (switched == 1 ? " Lampe " : " Lampen ") + (on ? "eingeschaltet" : "ausgeschaltet");
  return "";
}

// Tippen auf der Licht Seite, screenY = Höhe am Bildschirm.
// Nicht gekoppelt: der große Knopf koppelt. Gekoppelt: Schalter "Alle Lichter" bzw. ein Raum,
// ist etwas an, geht alles aus, sonst geht alles an.
// Unter den Schaltern tippen: linke Hälfte = vorige Seite, rechte Hälfte = nächste.
void lightTap(int screenX, int screenY) {
  bool paired = hubToken.length() > 0 && !hubPairing();
  int row = (screenY - LIST_TOP) / LIST_ROW_H;
  bool oneRoom = false;
  String room;
  bool turnOn = !anyOn();
  if (!paired) {
    if (screenY < LIGHT_BUTTON_TOP - 10 || screenY > LIGHT_BUTTON_TOP + LIGHT_BUTTON_H + 10) return;
  } else {
    if (screenY < LIST_TOP) return;
    if (row >= LIST_ROWS) return scrollDevices(screenX < tft.width() / 2 ? -1 : 1);
    if (lightScroll > 0) {
      int i = (lightScroll - 1) * LIST_ROWS + row;
      if (i >= roomCount) return;
      oneRoom = true;
      room = devices[rooms[i]].room;
      turnOn = !anyOn(rooms[i]);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    lightStatus = "Kein WLAN";
    lightStatusColor = COLOR_ERROR;
    drawDepartures();
    return;
  }

  if (!paired) lightStatus = "Verbinde mit dem Hub...";
  else lightStatus = turnOn ? "Wird eingeschaltet..." : "Wird ausgeschaltet...";
  lightStatusColor = COLOR_DIM;
  drawDepartures();

  String error;
  String result;
  bool justPaired = false;
  if (hubPairing()) {
    error = hubFinishPairing();
    result = "Gekoppelt";
    justPaired = error.length() == 0;
  } else if (!paired) {
    error = hubStartPairing();
  } else {
    error = hubSetAll(turnOn, result, oneRoom, room);
  }

  lightStatus = error.length() > 0 ? error : result;
  lightStatusColor = error.length() > 0 ? COLOR_ERROR : COLOR_LIVE;
  if (justPaired) refreshDevices();
  lastTouch = millis();  // nicht gleich dunkel werden oder zurückspringen
  drawDepartures();
}

// Einstellungen: 3 große Zeilen zum Antippen und "Fertig" unten.
// Nur die Höhe der Berührung zählt, das ist auf dem resistiven Touch am zuverlässigsten.
const int SETTINGS_TOP = 32;
const int SETTINGS_ROW_H = 54;

String sleepLabel() {
  int s = SLEEP_OPTIONS[sleepOption];
  return s < 60 ? String(s) + " s" : String(s / 60) + " min";
}

void drawSettingsRow(int row, const char *title, const char *hint, String value, bool on) {
  int y = SETTINGS_TOP + row * SETTINGS_ROW_H;
  u8f.setFont(u8g2_font_helvB14_tf);
  u8f.setForegroundColor(COLOR_LED);
  u8f.drawUTF8(12, y + 24, title);
  u8f.setFont(u8g2_font_helvB08_tf);
  u8f.setForegroundColor(COLOR_DIM);
  u8f.drawUTF8(12, y + 42, hint);
  drawRight(value.c_str(), tft.width() - 12, y + 32, u8g2_font_helvB18_tf, on ? COLOR_LIVE : COLOR_DIM);
  tft.drawFastHLine(0, y + SETTINGS_ROW_H, tft.width(), COLOR_DIM);
}

void drawSettings() {
  tft.fillScreen(TFT_BLACK);
  u8f.setFont(u8g2_font_helvB14_tf);
  u8f.setForegroundColor(COLOR_LED);
  u8f.drawUTF8(12, 22, "Einstellungen");
  drawGear(tft.width() - 14, 15, COLOR_LED);
  tft.drawFastHLine(0, 30, tft.width(), COLOR_DIM);

  drawSettingsRow(0, "Energiesparen am Tag", "Tippen zum Ein- und Ausschalten",
                  daySaving ? "AN" : "AUS", daySaving);
  drawSettingsRow(1, "Nachts dunkel (23-6 Uhr)", "Tippen zum Ein- und Ausschalten",
                  nightSaving ? "AN" : "AUS", nightSaving);
  drawSettingsRow(2, "Dunkel nach", "Tippen zum Ändern", sleepLabel(), true);

  int y = SETTINGS_TOP + 3 * SETTINGS_ROW_H;
  drawCentered("Fertig", y + (tft.height() - y) / 2 + 8, u8g2_font_helvB18_tf, COLOR_LED);
}

void saveSettings() {
  prefs.putBool("daySaving", daySaving);
  prefs.putBool("nightSaving", nightSaving);
  prefs.putInt("sleepOption", sleepOption);
}

void openSettings() {
  inSettings = true;
  drawSettings();
}

void closeSettings() {
  inSettings = false;
  if (hasData) drawDepartures();
}

// Tippen in den Einstellungen, screenY = Höhe am Bildschirm
void settingsTap(int screenY) {
  int row = (screenY - SETTINGS_TOP) / SETTINGS_ROW_H;
  if (screenY < SETTINGS_TOP) return;
  if (row == 0) daySaving = !daySaving;
  else if (row == 1) nightSaving = !nightSaving;
  else if (row == 2) sleepOption = (sleepOption + 1) % SLEEP_OPTION_COUNT;
  else {
    closeSettings();
    return;
  }
  saveSettings();
  drawSettings();
}

void setBrightness(int value) {
  if (value == brightness) return;
  brightness = value;
  ledcWrite(TFT_BL, value);
}

// Dunkel, wenn Energiesparen gerade gilt (am Tag und/oder nachts), außer kurz nach einer Berührung
void updateBrightness() {
  struct tm now;
  bool night = getLocalTime(&now, 0) &&
               (now.tm_hour >= NIGHT_FROM_HOUR || now.tm_hour < NIGHT_TO_HOUR);
  bool saving = night ? nightSaving : daySaving;
  bool awake = millis() - lastTouch < (unsigned long)SLEEP_OPTIONS[sleepOption] * 1000;
  setBrightness(saving && !awake ? SLEEP_BRIGHTNESS : DAY_BRIGHTNESS);
}

void setupOta() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    setBrightness(DAY_BRIGHTNESS);
    showStatus("Update...", "Bitte nicht ausstecken", COLOR_LED);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    int w = tft.width() - 80;
    tft.drawRect(40, tft.height() - 50, w, 12, COLOR_DIM);
    tft.fillRect(42, tft.height() - 48, (w - 4) * done / total, 8, COLOR_LED);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    showStatus("Update Fehler", ("Fehler " + String(error)).c_str(), COLOR_ERROR);
    hasData = false;  // beim nächsten Update wieder Abfahrten zeigen
  });
  ArduinoOTA.begin();
  Serial.println(String("Update über WLAN bereit: ") + OTA_HOSTNAME + ".local");
}

// Touch Rohwert in Höhe am Bildschirm umrechnen (0 = oben)
int toScreenY(int raw) {
  return constrain(map(raw, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, tft.height()), 0, tft.height() - 1);
}

int toScreenX(int raw) {
  return constrain(map(raw, TOUCH_X_MIN, TOUCH_X_MAX, 0, tft.width()), 0, tft.width() - 1);
}

// Touch auswerten: Kopfzeile tippen = Einstellungen,
// rechter Rand oder nach links wischen = nächster Bereich, linker Rand oder nach rechts wischen = voriger,
// in der Mitte unten tippen oder runterwischen = nächste Seite im Bereich, oben oder rauf = vorige
void handleTouch() {
  static bool down = false;
  static bool wakeOnly = false;  // Berührung hat nur das dunkle Display aufgeweckt
  static int startX = 0, startY = 0, lastX = 0, lastY = 0;
  static unsigned long lastSeen = 0;

  if (touch.touched()) {
    TS_Point p = touch.getPoint();
    int x = TOUCH_FLIP_X ? 4095 - p.x : p.x;
    int y = TOUCH_FLIP_Y ? 4095 - p.y : p.y;
    if (!down) {
      down = true;
      startX = x;
      startY = y;
      wakeOnly = brightness != DAY_BRIGHTNESS;
      lastTouch = millis();  // sofort hell machen
    }
    lastX = x;
    lastY = y;
    lastSeen = millis();
    return;
  }

  // Erst als losgelassen werten, wenn kurz kein Druck mehr da war (resistiver Touch flackert)
  if (!down || millis() - lastSeen < 80) return;
  down = false;
  lastTouch = millis();
  if (wakeOnly) return;

  int dx = lastX - startX;
  int dy = lastY - startY;
  int screenX = toScreenX(startX);
  int screenY = toScreenY(startY);
  bool tap = abs(dx) < SWIPE_MIN && abs(dy) < SWIPE_MIN;
  Serial.printf("Touch: Start x=%d y=%d, Ende x=%d y=%d, Bildschirm x=%d y=%d\n", startX, startY,
                lastX, lastY, screenX, screenY);

  if (inSettings) {
    if (abs(dy) < SWIPE_MIN) settingsTap(screenY);
    return;
  }
  if (tap && screenY < 40) {
    openSettings();  // Kopfzeile mit dem Zahnrad
    return;
  }

  // Wischen: die längere Richtung zählt
  if (!tap) {
    if (abs(dx) > abs(dy)) showSection(sectionOf(currentPage) + (dx < 0 ? 1 : -1));
    else if (currentPage == LIGHT_PAGE) scrollDevices(dy > 0 ? 1 : -1);
    else showSubPage(dy > 0 ? 1 : -1);
    return;
  }

  // Tippen am Rand: Bereich wechseln
  if (screenX >= tft.width() - SIDE_ZONE) return showSection(sectionOf(currentPage) + 1);
  if (screenX < SIDE_ZONE) return showSection(sectionOf(currentPage) - 1);

  // Licht Seite: Knöpfe und Geräte
  if (currentPage == LIGHT_PAGE) return lightTap(screenX, screenY);

  // Tippen in der Mitte: Seite im Bereich wechseln
  showSubPage(screenY > tft.height() / 2 ? 1 : -1);
}

// Holt Live Daten und Wetter. Klappt das nicht, zeigen die Linien den Fahrplan.
// Eine Fehlermeldung kommt nur, wenn gar nichts angezeigt werden kann.
void update() {
  bool wifiOk = connectWiFi();
  String error = wifiOk ? "" : "Keine WLAN Verbindung";

  if (!wifiOk) {
    weatherText = "";  // kein altes Wetter anzeigen
  } else if (lastWeather == 0 || weatherText.length() == 0 || millis() - lastWeather >= WEATHER_MS) {
    lastWeather = millis();
    fetchWeather();
  }

  if (wifiOk) fetchWg();
  else wgError = "Kein WLAN";
  if (currentPage == LIGHT_PAGE && !inSettings) refreshDevices();  // Licht auch per App geschaltet

  JsonDocument doc;
  if (wifiOk) error = fetchDepartures(doc);
  if (error.length() > 0) Serial.println("Keine Live Daten: " + error);
  liveError = error;
  Serial.printf("Freier Speicher: %lu Bytes\n", (unsigned long)ESP.getFreeHeap());

  if (!fillRows(error.length() == 0 ? &doc : nullptr)) {
    // Ohne Live Daten und ohne Uhrzeit geht auch der Fahrplan nicht
    hasData = false;
    if (!inSettings) showStatus(wifiOk ? "API Fehler" : "WLAN Fehler", error.c_str(), COLOR_ERROR);
    return;
  }

  hasData = true;
  if (!inSettings) drawDepartures();  // Einstellungen nicht überzeichnen
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(3);  // Querformat, um 180 Grad gedreht (USB Anschluss auf der anderen Seite)
  u8f.begin(tft);
  u8f.setFontMode(1);  // transparenter Hintergrund

  touchSpi.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(3);  // wie das Display

  // Hintergrundbeleuchtung per PWM, damit sie sich dimmen lässt
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, brightness);

  prefs.begin("oeffi", false);
  daySaving = prefs.getBool("daySaving", daySaving);
  nightSaving = prefs.getBool("nightSaving", nightSaving);
  sleepOption = constrain(prefs.getInt("sleepOption", sleepOption), 0, SLEEP_OPTION_COUNT - 1);
  hubIp = prefs.getString("hubIp", "");
  hubToken = prefs.getString("hubToken", "");

  connectWiFi();
  configTzTime(TIMEZONE, "pool.ntp.org");  // Uhrzeit aus dem Internet

  update();
  lastFetch = millis();
  lastTouch = millis();  // nach dem Start erst einmal hell
}

void loop() {
  // Update über WLAN starten, sobald das WLAN da ist
  static bool otaReady = false;
  if (!otaReady && WiFi.status() == WL_CONNECTED) {
    setupOta();
    otaReady = true;
  }
  if (otaReady) ArduinoOTA.handle();

  if (millis() - lastFetch >= REFRESH_MS) {
    lastFetch = millis();
    update();
  }

  handleTouch();
  updateBrightness();

  // Nach einer Weile ohne Berührung zurück zur ersten Seite
  if (millis() - lastTouch >= PAGE_TIMEOUT_MS) {
    if (inSettings) closeSettings();
    if (currentPage != 0) showPage(0);
  }

  // Uhr jede Minute neu zeichnen (nur wenn gerade Abfahrten angezeigt werden)
  struct tm now;
  if (!inSettings && lastClockMinute >= 0 && getLocalTime(&now, 0) &&
      now.tm_min != lastClockMinute) {
    drawHeader();
  }
  delay(20);
}
