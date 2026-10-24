// Öffi-Display: Echtzeit Abfahrten der Wiener Linien

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <U8g2_for_TFT_eSPI.h>
#include <time.h>
#include "secrets.h"

// Haltestellen (RBL Nummern aus der OGD Haltepunkte Liste)
// Reihenfolge hier = Reihenfolge am Bildschirm (oben, unten)
// minMinutes: Abfahrten, die früher kommen, sind nicht erreichbar und werden ausgeblendet
// towards: eigener kurzer Name für das Ziel, nullptr = Ziel aus der API
struct Stop {
  int rbl;
  const char *station;
  int minMinutes;
  const char *towards;
};

const Stop STOPS[] = {
    {4204, "Museumsquartier", 5, nullptr},     // U2 Richtung Seestadt, 7 Minuten Fußweg
    {8227, "Köstlergasse", 0, "Rudolfsheim"},  // 57A Richtung Rudolfsheim Anschützgasse
};
const int STOP_COUNT = sizeof(STOPS) / sizeof(STOPS[0]);

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
const int MAX_ROWS = 4;

// Farben im Stil der U-Bahn Anzeigen
const uint16_t COLOR_LED = 0xFF20;   // Gelb, RGB 255,230,0 (Farbtest B)
const uint16_t COLOR_DIM = 0x8410;   // Grau
const uint16_t COLOR_ERROR = 0xF800; // Rot

TFT_eSPI tft;
U8g2_for_TFT_eSPI u8f;

struct Row {
  String station;
  String line;
  String towards[2];  // Ziel pro Abfahrt
  int next[2];        // Minuten, -1 = keine Abfahrt
};

Row rows[MAX_ROWS];
int rowCount = 0;
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

  showStatus("WLAN...", WIFI_SSID, COLOR_LED);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Holt die Abfahrten. Gibt bei Erfolg einen leeren String zurück, sonst die Fehlermeldung.
String fetchDepartures() {
  String url = API_BASE;
  for (int i = 0; i < STOP_COUNT; i++) {
    if (i > 0) url += "&";
    url += "stopId=" + String(STOPS[i].rbl);
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

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter),
                      DeserializationOption::NestingLimit(20));
  http.end();
  if (err) return String("JSON Fehler: ") + err.c_str();

  rowCount = 0;
  JsonArray monitors = doc["data"]["monitors"];
  // In der Reihenfolge von STOPS durchgehen, damit die U2 immer oben steht
  for (const Stop &stop : STOPS) {
    for (JsonObject monitor : monitors) {
      if ((monitor["locationStop"]["properties"]["attributes"]["rbl"] | 0) != stop.rbl) continue;

      JsonArray lines = monitor["lines"];
      for (JsonObject line : lines) {
        if (rowCount >= MAX_ROWS) break;
        Row &r = rows[rowCount++];
        r.station = stop.station;
        r.line = line["name"].as<String>();

        // Die nächsten 2 Abfahrten, die noch erreichbar sind
        r.next[0] = r.next[1] = -1;
        int found = 0;
        JsonArray departures = line["departures"]["departure"];
        for (JsonObject dep : departures) {
          int minutes = dep["departureTime"]["countdown"] | -1;
          if (minutes < stop.minMinutes) continue;
          String towards = stop.towards ? String(stop.towards) : dep["vehicle"]["towards"].as<String>();
          r.towards[found] = toUpper(towards);
          r.next[found++] = minutes;
          if (found == 2) break;
        }
      }
    }
  }

  if (rowCount == 0) return "Keine Abfahrten";
  return "";
}

// Ein Block pro Linie, wie die Anzeige am Bahnsteig:
// Linie links, Trennstrich, Ziel, Minuten rechts
void drawBlock(const Row &r, int y, int h) {
  const int lineX = 72;  // Trennstrich
  const int destX = lineX + 10;
  const int minutesRight = tft.width() - 8;
  const int destMaxWidth = minutesRight - 46 - destX;

  // Linie links, senkrecht mittig
  u8f.setFont(u8g2_font_helvB24_tf);
  u8f.setForegroundColor(COLOR_LED);
  u8f.drawUTF8(8, y + h / 2 + 12, r.line.c_str());

  // Trennstrich
  tft.drawFastVLine(lineX, y + 10, h - 20, COLOR_LED);
  tft.drawFastVLine(lineX + 1, y + 10, h - 20, COLOR_LED);

  // Haltestelle klein darüber
  u8f.setFont(u8g2_font_helvB10_tf);
  u8f.setForegroundColor(COLOR_DIM);
  u8f.drawUTF8(destX, y + 20, r.station.c_str());

  // Zwei Zeilen: Ziel und Minuten
  for (int k = 0; k < 2; k++) {
    int baseline = y + 52 + k * 32;
    if (r.next[k] < 0) {
      if (k == 0) {
        u8f.setFont(u8g2_font_helvB12_tf);
        u8f.setForegroundColor(COLOR_DIM);
        u8f.drawUTF8(destX, baseline, "Keine Abfahrt erreichbar");
      }
      continue;
    }
    u8f.setFont(fitFont(r.towards[k].c_str(), destMaxWidth));
    u8f.setForegroundColor(COLOR_LED);
    u8f.drawUTF8(destX, baseline, r.towards[k].c_str());
    drawRight(String(r.next[k]).c_str(), minutesRight, baseline + 2, u8g2_font_helvB24_tf,
              COLOR_LED);
  }
}

void drawDepartures() {
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  tft.drawFastHLine(0, 30, tft.width(), COLOR_DIM);

  // Bis zu 2 Linien gleichzeitig, der Bildschirm ist sonst zu voll
  int shown = min(rowCount, 2);
  int blockH = (tft.height() - 32) / 2;
  for (int i = 0; i < shown; i++) {
    drawBlock(rows[i], 32 + i * blockH, blockH);
  }
  tft.drawFastHLine(0, 32 + blockH, tft.width(), COLOR_DIM);
}

void update() {
  if (!connectWiFi()) {
    showStatus("WLAN Fehler", "Keine Verbindung", COLOR_ERROR);
    return;
  }

  if (lastWeather == 0 || weatherText.length() == 0 || millis() - lastWeather >= WEATHER_MS) {
    lastWeather = millis();
    fetchWeather();
  }

  String error = fetchDepartures();
  if (error.length() > 0) {
    showStatus("API Fehler", error.c_str(), COLOR_ERROR);
    Serial.println("API Fehler: " + error);
    return;
  }

  drawDepartures();
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);  // Querformat
  u8f.begin(tft);
  u8f.setFontMode(1);  // transparenter Hintergrund

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

  // Uhr jede Minute neu zeichnen (nur wenn gerade Abfahrten angezeigt werden)
  struct tm now;
  if (lastClockMinute >= 0 && getLocalTime(&now, 0) && now.tm_min != lastClockMinute) {
    drawHeader();
  }
  delay(200);
}
