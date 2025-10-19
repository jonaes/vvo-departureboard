// ===== ESP32-WROOM + 128x64 I2C ST7567 JLX12864 (U8g2) =====


#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <WebServer.h>
#include <Preferences.h>
#include "esp_system.h"  // esp_restart()

// ---------- Pins / Display ----------
#define SDA_PIN         21
#define SCL_PIN         22
#define RST_PIN         U8X8_PIN_NONE
#define I2C_ADDR_7BIT   0x3F
U8G2_ST7567_JLX12864_F_HW_I2C u8g2(U8G2_R0, RST_PIN, SCL, SDA);

// ---------- Layout ----------
const uint8_t  WIDTH = 128, HEIGHT = 64;
const int8_t   DISP_XOFF = 0;
const uint8_t  ROWS = 5, ROW_H = 12, TOP_Y = 0;
const uint8_t  COL_LINE_W = 22, COL_RIGHT_W = 24, COL_GAP = 4;
const uint8_t  COL_DEST_X = DISP_XOFF + COL_LINE_W + COL_GAP;
const uint8_t  COL_DEST_W = 128 - DISP_XOFF - COL_DEST_X - COL_RIGHT_W - 2;

// ---------- Timing / Netzwerk ----------
#define FETCH_INTERVAL_MS   30000UL
#define SCROLL_INTERVAL_MS  250UL
#define SCROLL_STEP_PX      2
#define HTTP_TIMEOUT_MS     5000UL
#define JSON_DOC_CAP        4096
#define MAX_ERRORS          5
#define RESET_INTERVAL_MS   (60UL * 60UL * 1000UL)
#define USE_HARD_RESET      0

// ---------- Konfiguration & Setup ----------
#define CFG_NAMESPACE   "dvb"
#define CFG_KEY_SSID    "ssid"
#define CFG_KEY_PW      "pw"
#define CFG_KEY_HST     "hst"
#define CFG_KEY_ORT     "ort"
#define CFG_KEY_OFF     "offset"
#define CFG_KEY_LIM     "limit"

#define DEFAULT_SSID    ""
#define DEFAULT_PW      ""
#define DEFAULT_HST     "Reichenbachstrasse"
#define DEFAULT_ORT     "Dresden"
#define DEFAULT_OFFSET  0
#define DEFAULT_LIMIT   5

#define CONFIG_BUTTON_PIN   0            // BOOT-Taste bei vielen Devkits
#define WIFI_CONNECT_MS     10000UL
#define BOOT_DELAY_MS       5000UL       // Setup-Fenster beim Boot

// Setup-Portal
#define SETUP_PWD           "collaborative"  //
#define SETUP_PAGE_MS       4000UL           // Auto-Paging alle 4 s

Preferences prefs;
WebServer server(80);
WiFiMulti wifiMulti;

struct Config {
  String ssid, pw, hst, ort;
  int offsetMin = DEFAULT_OFFSET;
  int limit     = DEFAULT_LIMIT;
} cfg;

// ---------- Datenmodell ----------
struct Row {
  String line, dest, mins;
  int16_t offsetPx = 0;
  int16_t textW = 0;
  int16_t cyclePx = 1;
  bool    needsScroll = false;
};

Row rows[ROWS];
bool     haveRows = false;
uint8_t  failCount = 0;
uint32_t tFetch = 0, tScroll = 0, tReset = 0;

// ---------- Utils ----------
static inline void sanitizeDest(String&) {}

static inline void drawRightAlignedText(int16_t rightX, int16_t baseY, const String& s) {
  int w = u8g2.getUTF8Width(s.c_str());
  u8g2.setCursor(rightX - w, baseY);
  u8g2.print(s);
}

static inline void drawBadge(int16_t x, int16_t y, const String& lineTxt) {
  u8g2.drawRBox(x, y - 10, COL_LINE_W - 2, 11, 2);
  u8g2.setDrawColor(1);
  u8g2.setCursor(x + 1, y - 1);
  u8g2.print(lineTxt);
  u8g2.setDrawColor(0);
}

static inline void updateRowDerived(Row& row) {
  String tmp = row.dest;
  row.textW = u8g2.getUTF8Width(tmp.c_str());
  const int16_t gapPx = 16;
  row.cyclePx = max<int16_t>(1, row.textW + gapPx);
  row.needsScroll = (row.textW > COL_DEST_W);
}

// ---------- Hilfsfunktionen für URL / Minuten ----------
static String urlEncodeLight(const String& in) {
  String out; out.reserve(in.length()*3);
  for (char c : in) out += (c == ' ') ? "%20" : String(c);
  return out;
}

static String buildUrl() {
  String url = "http://widgets.vvo-online.de/abfahrtsmonitor/Abfahrten.do?hst=";
  url += urlEncodeLight(cfg.hst);
  url += "&vz=";  url += String(cfg.offsetMin);
  url += "&ort="; url += urlEncodeLight(cfg.ort);
  url += "&lim="; url += String(cfg.limit);
  return url;
}

static String applyOffsetToMins(const String& m) {
  int i = 0; while (i < (int)m.length() && isspace((unsigned char)m[i])) i++;
  int j = i; while (j < (int)m.length() && isdigit((unsigned char)m[j])) j++;
  if (j==i) return m;
  int val = m.substring(i, j).toInt();
  val += cfg.offsetMin;
  if (val < 0) val = 0;
  String rest = m.substring(j);
  return String(val) + rest;
}

// ---------- Render ----------
void renderFull() {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1); u8g2.drawBox(0,0,WIDTH,HEIGHT);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x12_tf);

  for (uint8_t r=0; r<ROWS; ++r) {
    if (!(rows[r].line.length() || rows[r].dest.length())) continue;
    int16_t baseY = TOP_Y + r*ROW_H + ROW_H;

    if (rows[r].line.length()) drawBadge(DISP_XOFF + 1, baseY, rows[r].line);

    String d = rows[r].dest;
    u8g2.setClipWindow(COL_DEST_X, baseY-11, COL_DEST_X+COL_DEST_W-1, baseY);
    if (!rows[r].needsScroll || d.isEmpty()) {
      u8g2.setCursor(COL_DEST_X, baseY-1);
      u8g2.print(d);
    } else {
      int16_t x1 = COL_DEST_X - (rows[r].offsetPx % rows[r].cyclePx);
      int16_t x2 = x1 + rows[r].cyclePx;
      u8g2.setCursor(x1, baseY-1); u8g2.print(d);
      u8g2.setCursor(x2, baseY-1); u8g2.print(d);
    }
    u8g2.setMaxClipWindow();

    String m = rows[r].mins;
    if (m.length()) m = applyOffsetToMins(m);
    if (m.length() > 3) m.remove(3);
    drawRightAlignedText(DISP_XOFF + 128 - 2, baseY - 1, m);
  }

  u8g2.sendBuffer();
}

void renderDestOnly() {
  u8g2.setFont(u8g2_font_6x12_tf);
  for (uint8_t r=0; r<ROWS; ++r) {
    if (!rows[r].dest.length()) continue;
    int16_t baseY = TOP_Y + r*ROW_H + ROW_H;
    u8g2.setDrawColor(1); u8g2.drawBox(COL_DEST_X, baseY-11, COL_DEST_W, 11);
    u8g2.setDrawColor(0);
    String d = rows[r].dest;
    u8g2.setClipWindow(COL_DEST_X, baseY-11, COL_DEST_X+COL_DEST_W-1, baseY);
    if (!rows[r].needsScroll) {
      u8g2.setCursor(COL_DEST_X, baseY-1); u8g2.print(d);
    } else {
      int16_t x1 = COL_DEST_X - (rows[r].offsetPx % rows[r].cyclePx);
      int16_t x2 = x1 + rows[r].cyclePx;
      u8g2.setCursor(x1, baseY-1); u8g2.print(d);
      u8g2.setCursor(x2, baseY-1); u8g2.print(d);
    }
    u8g2.setMaxClipWindow();
  }
  u8g2.sendBuffer();
}

// ---------- Networking ----------
bool fetchOnce() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  String url = buildUrl();
  if (!http.begin(url)) return false;
  http.addHeader("Accept","application/json");
  http.addHeader("Accept-Encoding","identity");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  String payload = http.getString(); http.end();

  StaticJsonDocument<JSON_DOC_CAP> doc;
  if (deserializeJson(doc, payload)) return false;
  if (!doc.is<JsonArrayConst>()) return false;
  JsonArrayConst arr = doc.as<JsonArrayConst>();
  if (arr.isNull()) return false;

  uint8_t r = 0;
  for (JsonVariantConst v: arr) {
    if (!v.is<JsonArrayConst>()) continue;
    JsonArrayConst rec = v.as<JsonArrayConst>();
    if (rec.size() < 3 || r >= ROWS) continue;
    rows[r].line = rec[0].as<const char*>() ?: "";
    rows[r].dest = rec[1].as<const char*>() ?: "";
    rows[r].mins = rec[2].as<const char*>() ?: "";
    updateRowDerived(rows[r]);
    rows[r].offsetPx = 0;
    ++r;
  }
  for (; r<ROWS; ++r) rows[r] = Row();
  haveRows = true;
  return true;
}

// ---------- Soft-Rebuild ----------
void softRebuild() {
  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setContrast(185);
  for (auto &r : rows) { r.offsetPx = 0; if (r.dest.length()) updateRowDerived(r); }
  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  fetchOnce();
  renderFull();
}

// ---------- Config speichern / laden ----------
void loadConfig() {
  prefs.begin(CFG_NAMESPACE, true);
  cfg.ssid = prefs.getString(CFG_KEY_SSID, DEFAULT_SSID);
  cfg.pw   = prefs.getString(CFG_KEY_PW, DEFAULT_PW);
  cfg.hst  = prefs.getString(CFG_KEY_HST, DEFAULT_HST);
  cfg.ort  = prefs.getString(CFG_KEY_ORT, DEFAULT_ORT);
  cfg.offsetMin = prefs.getInt(CFG_KEY_OFF, DEFAULT_OFFSET);
  cfg.limit     = prefs.getInt(CFG_KEY_LIM, DEFAULT_LIMIT);
  prefs.end();
  if (cfg.limit < 1 || cfg.limit > ROWS) cfg.limit = ROWS;
}

void saveConfig() {
  prefs.begin(CFG_NAMESPACE, false);
  prefs.putString(CFG_KEY_SSID, cfg.ssid);
  prefs.putString(CFG_KEY_PW,   cfg.pw);
  prefs.putString(CFG_KEY_HST,  cfg.hst);
  prefs.putString(CFG_KEY_ORT,  cfg.ort);
  prefs.putInt(CFG_KEY_OFF,     cfg.offsetMin);
  prefs.putInt(CFG_KEY_LIM,     cfg.limit);
  prefs.end();
}

// ---------- Setup-Portal HTML ----------
String chipSuffix() {
  uint64_t mac = ESP.getEfuseMac();
  uint16_t tail = (uint16_t)(mac & 0xFFFF);
  char buf[6]; snprintf(buf, sizeof(buf), "%04X", tail);
  return String(buf);
}

const char* HTML_FORM = R"HTML(
<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DVB Abfahrtsmonitor Setup</title>
<style>body{font-family:sans-serif;margin:24px}label{display:block;margin:8px 0 4px}input{width:100%%;padding:8px}button{margin-top:12px;padding:10px 16px}</style>
</head><body>
<h2>DVB Abfahrtsmonitor – Setup</h2>
<ol>
<li>Mit dem WLAN-Hotspot verbinden (siehe Display).</li>
<li>Mobile Daten am Smartphone ausschalten (sonst landet der Browser im Internet statt im lokalen Geraet).</li>
<li>Im Browser <b>http://192.168.4.1</b> oeffnen.</li>
<li>SSID, Passwort, Haltestelle, Ort, Offset, Anzahl eintragen → Speichern.</li>
</ol>
<form method="POST" action="/save">
<label>WLAN SSID</label><input name="ssid" value="%SSID%">
<label>WLAN Passwort</label><input name="pw" type="password" value="%PW%">
<label>Haltestelle (hst)</label><input name="hst" value="%HST%">
<label>Ort (ort)</label><input name="ort" value="%ORT%">
<label>Minuten-Offset (vz)</label><input name="offset" type="number" step="1" value="%OFF%">
<label>Anzahl Eintraege (lim ≤ 5)</label><input name="limit" type="number" step="1" value="%LIM%">
<button type="submit">Speichern & Neustarten</button>
</form></body></html>
)HTML";

String renderForm() {
  String html = HTML_FORM;
  html.replace("%SSID%", cfg.ssid);
  html.replace("%PW%",   cfg.pw);
  html.replace("%HST%",  cfg.hst);
  html.replace("%ORT%",  cfg.ort);
  html.replace("%OFF%",  String(cfg.offsetMin));
  html.replace("%LIM%",  String(cfg.limit));
  return html;
}

void handleRoot() { server.send(200, "text/html", renderForm()); }

void handleSave() {
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Method Not Allowed"); return; }
  if (server.hasArg("ssid"))   cfg.ssid = server.arg("ssid");
  if (server.hasArg("pw"))     cfg.pw   = server.arg("pw");
  if (server.hasArg("hst"))    cfg.hst  = server.arg("hst");
  if (server.hasArg("ort"))    cfg.ort  = server.arg("ort");
  if (server.hasArg("offset")) cfg.offsetMin = server.arg("offset").toInt();
  if (server.hasArg("limit"))  cfg.limit     = server.arg("limit").toInt();
  if (cfg.limit < 1) cfg.limit = 1;
  if (cfg.limit > ROWS) cfg.limit = ROWS;

  saveConfig();
  server.send(200, "text/html",
              "<html><body><h3>Gespeichert. Neustart...</h3>"
              "<script>setTimeout(()=>{fetch('/reboot')},500);</script></body></html>");
}

void handleReboot() {
  server.send(200, "text/plain", "Rebooting...");
  delay(200);
  esp_restart();
}

// --------- Setupmodus: LCD-Paging-Anleitung ----------
void drawSetupPage(uint8_t page, const String& apName, const IPAddress& ip) {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1); u8g2.drawBox(0,0,WIDTH,HEIGHT);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x12_tf);

  switch (page) {
    case 0: {
      u8g2.setCursor(2, 12); u8g2.print("Setup-Modus aktiv");
      u8g2.setCursor(2, 26); u8g2.print("AP: "); u8g2.print(apName);
      u8g2.setCursor(2, 40); u8g2.print("PW: "); u8g2.print(SETUP_PWD);
      u8g2.setCursor(2, 54); u8g2.print("Taste BOOT: weiter");
    } break;

    case 1: {
      u8g2.setCursor(2, 12); u8g2.print("1) Mit WLAN verbinden");
      u8g2.setCursor(2, 26); u8g2.print("   (AP siehe Seite 1)");
      u8g2.setCursor(2, 40); u8g2.print("2) Mobile Daten AUS!");
      u8g2.setCursor(2, 54); u8g2.print("   (sonst kein Portal)");
    } break;

    case 2: {
      u8g2.setCursor(2, 12); u8g2.print("3) Browser oeffnen:");
      u8g2.setCursor(2, 26); u8g2.print("   http://192.168.4.1");
      u8g2.setCursor(2, 40); u8g2.print("4) SSID/PW/HST/ORT/");
      u8g2.setCursor(2, 54); u8g2.print("   Offset/Lim speichern");
    } break;

    case 3: {
      u8g2.setCursor(2, 12); u8g2.print("Nach Speichern:");
      u8g2.setCursor(2, 26); u8g2.print("- Geraet startet neu");
      u8g2.setCursor(2, 40); u8g2.print("- verbindet ins WLAN");
      u8g2.setCursor(2, 54); u8g2.print("- Anzeige beginnt");
    } break;

    default: page = 0; break;
  }

  u8g2.sendBuffer();
}

void startConfigPortal() {
  String apName = "DVB-Setup-" + chipSuffix();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str(), SETUP_PWD);
  IPAddress ip = WiFi.softAPIP();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reboot", HTTP_GET, handleReboot);
  server.begin();

  // Paging-Loop (blockierend, bis Reboot)
  uint8_t page = 0;
  uint32_t tPage = 0;

  drawSetupPage(page, apName, ip);
  tPage = millis();

  while (true) {
    server.handleClient();

    // Button manuell: weiterblättern
    static bool lastBtn = false;
    bool btn = (digitalRead(CONFIG_BUTTON_PIN) == LOW);
    if (btn && !lastBtn) {
      page = (page + 1) % 4;
      drawSetupPage(page, apName, ip);
      tPage = millis();
    }
    lastBtn = btn;

    // Auto-Paging
    if (millis() - tPage >= SETUP_PAGE_MS) {
      page = (page + 1) % 4;
      drawSetupPage(page, apName, ip);
      tPage = millis();
    }

    delay(2);
  }
}

// ---------- Setup / Loop ----------
void setup() {
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(50);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  u8g2.setI2CAddress(I2C_ADDR_7BIT << 1);
  u8g2.begin(); u8g2.enableUTF8Print(); u8g2.setContrast(185);

  // Splash mit Setup-Hinweis
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.setCursor(2, 14); u8g2.print("DVB Abfahrtsmonitor");
  u8g2.setCursor(2, 30); u8g2.print("Halte BOOT fuer Setup");
  u8g2.setCursor(2, 44); u8g2.print("(warte 5 Sek.)");
  u8g2.sendBuffer();

  // Setup-Fenster
  uint32_t start = millis();
  while (millis() - start < BOOT_DELAY_MS) {
    if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
      WiFi.mode(WIFI_OFF);
      WiFi.disconnect(true);
      delay(100);
      startConfigPortal();  // kehrt nicht zurück
    }
    delay(10);
  }

  u8g2.clearBuffer();
  u8g2.setCursor(2, 14); u8g2.print("Verbinde WLAN...");
  u8g2.sendBuffer();

  loadConfig();

  WiFi.mode(WIFI_STA);
  if (cfg.ssid.length()) WiFi.begin(cfg.ssid.c_str(), cfg.pw.c_str());

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_MS) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    startConfigPortal();  // kehrt nicht zurück
  }

  fetchOnce();
  renderFull();
  tFetch = tScroll = tReset = millis();
}

void loop() {
  uint32_t now = millis();

  if (now - tFetch >= FETCH_INTERVAL_MS) {
    if (fetchOnce()) failCount = 0; else if (++failCount > MAX_ERRORS) failCount = 0;
    renderFull(); tFetch = now;
  }

  if (now - tScroll >= SCROLL_INTERVAL_MS) {
    bool any = false;
    for (auto &r : rows) if (r.needsScroll && r.dest.length()) { r.offsetPx += SCROLL_STEP_PX; any = true; }
    if (any) renderDestOnly();
    tScroll = now;
  }

  if (now - tReset >= RESET_INTERVAL_MS) {
#if USE_HARD_RESET
    esp_restart();
#else
    softRebuild();
#endif
    tReset = now;
  }
}
