// Öffi-Display: Echtzeit Abfahrten der Wiener Linien

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "secrets.h"

// Haltestellen (RBL Nummern aus der OGD Haltepunkte Liste)
// 4204: Museumsquartier, U2 Richtung Seestadt
// 8227: Köstlergasse, 57A Richtung Rudolfsheim Anschützgasse
const char *API_URL =
    "https://www.wienerlinien.at/ogd_realtime/monitor?stopId=4204&stopId=8227";

const unsigned long REFRESH_MS = 30000;  // alle 30 Sekunden
const int MAX_ROWS = 4;

TFT_eSPI tft;

struct Row {
  String line;
  String towards;
  int next[2];  // Minuten, -1 = keine Abfahrt
};

Row rows[MAX_ROWS];
int rowCount = 0;
unsigned long lastFetch = 0;

// Die eingebauten Schriften kennen keine Umlaute
String replaceUmlauts(String s) {
  s.replace("ä", "ae");
  s.replace("ö", "oe");
  s.replace("ü", "ue");
  s.replace("Ä", "Ae");
  s.replace("Ö", "Oe");
  s.replace("Ü", "Ue");
  s.replace("ß", "ss");
  return s;
}

void showStatus(const char *line1, const char *line2, uint16_t color) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(line1, tft.width() / 2, tft.height() / 2 - 20, 4);
  tft.drawString(line2, tft.width() / 2, tft.height() / 2 + 20, 2);
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  showStatus("WLAN...", WIFI_SSID, TFT_YELLOW);
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
  WiFiClientSecure client;
  client.setInsecure();  // kein Zertifikat prüfen, reicht für öffentliche Fahrplandaten

  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, API_URL)) return "Verbindung fehlgeschlagen";

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return "HTTP Fehler " + String(code);
  }

  // Nur die Felder einlesen, die wir brauchen (spart Speicher)
  JsonDocument filter;
  JsonObject fLine = filter["data"]["monitors"][0]["lines"][0].to<JsonObject>();
  fLine["name"] = true;
  fLine["towards"] = true;
  fLine["departures"]["departure"][0]["departureTime"]["countdown"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter),
                      DeserializationOption::NestingLimit(20));
  http.end();
  if (err) return String("JSON Fehler: ") + err.c_str();

  rowCount = 0;
  for (JsonObject monitor : doc["data"]["monitors"].as<JsonArray>()) {
    for (JsonObject line : monitor["lines"].as<JsonArray>()) {
      if (rowCount >= MAX_ROWS) break;
      Row &r = rows[rowCount++];
      r.line = line["name"].as<String>();
      r.towards = replaceUmlauts(line["towards"].as<String>());
      JsonArray deps = line["departures"]["departure"];
      for (int i = 0; i < 2; i++) {
        r.next[i] = i < (int)deps.size() ? deps[i]["departureTime"]["countdown"] | -1 : -1;
      }
    }
  }

  if (rowCount == 0) return "Keine Abfahrten";
  return "";
}

void drawDepartures() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  int y = 10;
  for (int i = 0; i < rowCount; i++) {
    Row &r = rows[i];

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(r.line, 10, y, 4);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(r.towards, 80, y + 6, 2);

    String mins = "";
    for (int k = 0; k < 2; k++) {
      if (r.next[k] < 0) continue;
      if (mins.length() > 0) mins += "  ";
      mins += String(r.next[k]) + "'";
    }
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(mins, 80, y + 26, 4);

    y += 60;
  }
}

void update() {
  if (!connectWiFi()) {
    showStatus("WLAN Fehler", "Keine Verbindung", TFT_RED);
    return;
  }

  String error = fetchDepartures();
  if (error.length() > 0) {
    showStatus("API Fehler", error.c_str(), TFT_RED);
    Serial.println("API Fehler: " + error);
    return;
  }

  drawDepartures();
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);  // Querformat

  update();
  lastFetch = millis();
}

void loop() {
  if (millis() - lastFetch >= REFRESH_MS) {
    lastFetch = millis();
    update();
  }
}
