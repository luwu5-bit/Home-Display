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
    {"13A", "Haus des Meeres", 0, nullptr, {{668, FP_13A_HAUS_DES_MEERES}}},
    {"14A", "Haus des Meeres", 0, nullptr, {{668, FP_14A_HAUS_DES_MEERES}}},
};
const int BLOCK_COUNT = sizeof(BLOCKS) / sizeof(BLOCKS[0]);
const int PAGE_COUNT = (BLOCK_COUNT + 1) / 2;

// Touch (XPT2046, eigene Pins laut lcdwiki E32R28T)
const int TOUCH_CS = 33;
const int TOUCH_IRQ = 36;
const int TOUCH_SCLK = 25;
const int TOUCH_MISO = 39;
const int TOUCH_MOSI = 32;
const bool TOUCH_FLIP_Y = false;  // auf true setzen, falls rauf und runter vertauscht sind
const int SWIPE_MIN = 800;        // Mindestweg für einen Wisch (Rohwerte 0 bis 4095)
const unsigned long PAGE_TIMEOUT_MS = 90000;  // danach zurück zur ersten Seite

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
unsigned long lastTouch = 0;
unsigned long lastFetch = 0;
unsigned long lastWeather = 0;
String weatherText;  // leer = kein Wetter verfügbar
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

// Kopfzeile: Wetter links, Uhrzeit rechts
void drawHeader() {
  struct tm now;
  char text[6] = "--:--";
  if (getLocalTime(&now, 0)) {
    strftime(text, sizeof(text), "%H:%M", &now);
    lastClockMinute = now.tm_min;
  }
  tft.fillRect(0, 0, tft.width(), 28, TFT_BLACK);
  drawRight(text, tft.width() - 8, 22, u8g2_font_helvB14_tf, COLOR_LED);

  u8f.setFont(u8g2_font_helvB14_tf);
  int maxWidth = tft.width() - 8 - u8f.getUTF8Width(text) - 16 - 8;
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
  if (!hasData) showStatus("WLAN...", WIFI_SSID, COLOR_LED);
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

  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter),
                      DeserializationOption::NestingLimit(20));
  http.end();
  if (err) return String("JSON Fehler: ") + err.c_str();
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
// Linie links, Trennstrich, Ziel, Minuten (oder Uhrzeit laut Fahrplan) rechts
void drawBlock(const Block &b, const Row &r, int y, int h) {
  const int lineX = 72;  // Trennstrich
  const int destX = lineX + 10;
  const int minutesRight = tft.width() - 8;

  // Linie links, senkrecht mittig
  u8f.setFont(u8g2_font_helvB24_tf);
  u8f.setForegroundColor(COLOR_LED);
  u8f.drawUTF8(8, y + h / 2 + 12, b.line);

  // Grüner Punkt unter der Linie, wenn alle Abfahrten live sind
  bool allLive = r.count > 0;
  for (int k = 0; k < r.count; k++) allLive = allLive && r.live[k];
  if (allLive) {
    int lineWidth = u8f.getUTF8Width(b.line);
    tft.fillCircle(8 + lineWidth / 2, y + h / 2 + 26, 3, COLOR_LIVE);
  }

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

    int whenWidth = max(46, (int)u8f.getUTF8Width(r.when[k].c_str()) + 10);
    u8f.setFont(fitFont(r.towards[k].c_str(), minutesRight - whenWidth - destX));
    u8f.setForegroundColor(COLOR_LED);
    u8f.drawUTF8(destX, baseline, r.towards[k].c_str());
  }
}

void drawDepartures() {
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  tft.drawFastHLine(0, 30, tft.width(), COLOR_DIM);

  // 2 Blöcke pro Seite, der Bildschirm ist sonst zu voll
  int blockH = (tft.height() - 32) / 2;
  for (int k = 0; k < 2; k++) {
    int i = currentPage * 2 + k;
    if (i >= BLOCK_COUNT) break;
    drawBlock(BLOCKS[i], rows[i], 32 + k * blockH, blockH);
  }
  tft.drawFastHLine(0, 32 + blockH, tft.width(), COLOR_DIM);

  // Seitenanzeige: Punkte unten in der Mitte
  for (int p = 0; p < PAGE_COUNT; p++) {
    int x = tft.width() / 2 + (p * 2 - (PAGE_COUNT - 1)) * 6;
    tft.fillCircle(x, tft.height() - 5, 2, p == currentPage ? COLOR_LED : COLOR_DIM);
  }
}

void showPage(int page) {
  currentPage = (page + PAGE_COUNT) % PAGE_COUNT;
  if (hasData) drawDepartures();
}

// Touch auswerten: runterwischen oder unten tippen = nächste Seite,
// raufwischen oder oben tippen = vorige Seite
void handleTouch() {
  static bool down = false;
  static int startY = 0, lastY = 0;
  static unsigned long lastSeen = 0;

  if (touch.touched()) {
    TS_Point p = touch.getPoint();
    int y = TOUCH_FLIP_Y ? 4095 - p.y : p.y;
    if (!down) {
      down = true;
      startY = y;
    }
    lastY = y;
    lastSeen = millis();
    return;
  }

  // Erst als losgelassen werten, wenn kurz kein Druck mehr da war (resistiver Touch flackert)
  if (!down || millis() - lastSeen < 80) return;
  down = false;
  lastTouch = millis();

  int dy = lastY - startY;
  Serial.printf("Touch: Start y=%d, Ende y=%d\n", startY, lastY);
  if (dy > SWIPE_MIN) showPage(currentPage + 1);
  else if (dy < -SWIPE_MIN) showPage(currentPage - 1);
  else if (startY > 2048) showPage(currentPage + 1);
  else showPage(currentPage - 1);
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

  JsonDocument doc;
  if (wifiOk) error = fetchDepartures(doc);
  if (error.length() > 0) Serial.println("Keine Live Daten: " + error);

  if (!fillRows(error.length() == 0 ? &doc : nullptr)) {
    // Ohne Live Daten und ohne Uhrzeit geht auch der Fahrplan nicht
    hasData = false;
    showStatus(wifiOk ? "API Fehler" : "WLAN Fehler", error.c_str(), COLOR_ERROR);
    return;
  }

  hasData = true;
  drawDepartures();
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

  connectWiFi();
  configTzTime(TIMEZONE, "pool.ntp.org");  // Uhrzeit aus dem Internet

  update();
  lastFetch = millis();
}

void loop() {
  if (millis() - lastFetch >= REFRESH_MS) {
    lastFetch = millis();
    update();
  }

  handleTouch();

  // Nach einer Weile ohne Berührung zurück zur ersten Seite
  if (currentPage != 0 && millis() - lastTouch >= PAGE_TIMEOUT_MS) showPage(0);

  // Uhr jede Minute neu zeichnen (nur wenn gerade Abfahrten angezeigt werden)
  struct tm now;
  if (lastClockMinute >= 0 && getLocalTime(&now, 0) && now.tm_min != lastClockMinute) {
    drawHeader();
  }
  delay(20);
}
