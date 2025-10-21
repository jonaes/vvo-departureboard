// ===== ESP32-WROOM + 128x64 I2C ST7567 JLX12864 (U8g2) =====
// Basis-Funktionen:
// - SLOW Marquee (250ms, 2px)
// - Reload 30s
// - Right-aligned minutes
// - Scroll-Redraw NUR Zielspalte
// + WLAN Setup (Webinterface, NVS)
// + Paging (bis 10 Einträge, 2 Seiten à 5)
// + Boot-Delay mit Setup-Hinweis (BOOT halten)
// + Setupmodus: LCD-Anleitung (Paging)

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <WebServer.h>
#include <Preferences.h>

// ---------- I2C / Display ----------
#define SDA_PIN        21
#define SCL_PIN        22
#define RST_PIN        U8X8_PIN_NONE
#define I2C_ADDR_7BIT  0x3F
U8G2_ST7567_JLX12864_F_HW_I2C u8g2(U8G2_R0, RST_PIN, SCL, SDA);

// ---------- Layout / Feintuning ----------
const uint8_t WIDTH = 128, HEIGHT = 64;
const int8_t  DISP_XOFF = 0;
const uint8_t VISIBLE_ROWS = 5;        // sichtbare Zeilen pro Seite
const uint8_t ROW_H = 12, TOP_Y = 0;
const uint8_t COL_LINE_W = 22, COL_RIGHT_W = 24, COL_GAP = 4;
const uint8_t COL_DEST_X = DISP_XOFF + COL_LINE_W + COL_GAP;
const uint8_t COL_DEST_W = 128 - DISP_XOFF - COL_DEST_X - COL_RIGHT_W - 2;

// ---------- Netzwerk & Timing ----------
#define FETCH_INTERVAL_MS   30000UL   // 30 s
#define SCROLL_INTERVAL_MS  250UL     // Marquee-Tick
#define SCROLL_STEP_PX      2
#define HTTP_TIMEOUT_MS     5000UL
#define JSON_DOC_CAP        4096
#define MAX_ERRORS          5

// ---------- Paging (Anzeige) ----------
#define MAX_ROWS            10        // bis zu 10 Einträge insgesamt
#define PAGE_INTERVAL_MS    6000UL    // Seitenwechsel alle 6 s

// ---------- Boot-Setup ----------
#define BOOT_BUTTON_PIN     0         // BOOT-Taste
#define BOOT_DELAY_MS       5000UL    // 5s Fenster für Setup

// ---------- WLAN + Konfig ----------
#define CFG_NAMESPACE "dvb"
#define CFG_SSID "ssid"
#define CFG_PW   "pw"
#define CFG_HST  "hst"
#define CFG_ORT  "ort"
#define CFG_OFF  "offset"
#define CFG_LIM  "limit"
#define CFG_SCR  "scroll"

#define DEFAULT_HST  "Reichenbachstrasse"
#define DEFAULT_ORT  "Dresden"
#define DEFAULT_OFF  0
#define DEFAULT_LIM  5
#define WIFI_CONNECT_MS 10000UL

Preferences prefs;
WebServer server(80);

struct Config {
  String ssid, pw, hst, ort;
  int offsetMin = DEFAULT_OFF;
  int limit     = DEFAULT_LIM; // 1..10
  bool scrollDest = true;
} cfg;

// ---------- Datenmodell ----------
struct Row {
  String  line, dest, mins;
  int16_t offsetPx = 0;   // Marquee-Offset
  int16_t textW    = 0;   // Pixelbreite der Destination
};
Row rows[MAX_ROWS];
uint8_t  failCount = 0;
uint32_t tFetch = 0, tScroll = 0;

// Paging-Status
uint8_t  pageIndex = 0;   // 0..pageCount-1
uint8_t  pageCount = 1;
uint32_t tPage     = 0;

// ---------- Utils ----------
static inline void sanitizeDest(String& s) {/**/ }

static inline void drawRightAlignedText(int16_t rightX, int16_t baseY, const String& s) {
  int w = u8g2.getUTF8Width(s.c_str());
  u8g2.setCursor(rightX - w, baseY);
  u8g2.print(s);
}

static inline void drawBadge(int16_t x, int16_t y, const String& lineTxt) {
  u8g2.drawRBox(x, y-10, COL_LINE_W-2, 11, 2);
  u8g2.setDrawColor(0);
  u8g2.setCursor(x+1, y-1);
  u8g2.print(lineTxt);
  u8g2.setDrawColor(1);
}

static String urlEncodeLight(const String& in) {
  // bewusst simpel: nur Space -> %20
  String out; out.reserve(in.length()*2);
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
  int i=0; while (i<(int)m.length() && isspace((unsigned char)m[i])) i++;
  int j=i; while (j<(int)m.length() && isdigit((unsigned char)m[j])) j++;
  if (j==i) return m;
  int val = m.substring(i,j).toInt();
  val += cfg.offsetMin; if (val < 0) val = 0;
  String rest = m.substring(j);
  return String(val) + rest;
}

// ---------- Seitenindikator (Rahmen bei Page > 0) ----------
static inline void drawPageIndicator() {
  if (pageCount <= 1 || pageIndex == 0) return;

  // Gepunkteter 1px-Rand um den Displaybereich
  for (uint8_t x = 0; x < WIDTH; x += 2) {
    u8g2.drawPixel(x, 0);           // oben
    u8g2.drawPixel(x, HEIGHT - 1);  // unten
  }
  for (uint8_t y = 0; y < HEIGHT; y += 2) {
    u8g2.drawPixel(0, y);           // links
    u8g2.drawPixel(WIDTH - 1, y);   // rechts
  }
}


// ---------- Render ----------
void renderFull() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  const uint8_t base = pageIndex * VISIBLE_ROWS;
  for (uint8_t r=0; r<VISIBLE_ROWS; ++r) {
    const uint8_t idx = base + r;
    if (idx >= MAX_ROWS) break;
    if (!(rows[idx].line.length() || rows[idx].dest.length())) continue;

    int16_t baseY = TOP_Y + r*ROW_H + ROW_H;
    if (rows[idx].line.length()) drawBadge(DISP_XOFF + 1, baseY, rows[idx].line);

    String d = rows[idx].dest; sanitizeDest(d);
    const int16_t textW = rows[idx].textW;
    const int16_t gapPx = 16;
    const int16_t cycle = max<int16_t>(1, textW + gapPx);

    u8g2.setClipWindow(COL_DEST_X, baseY-11, COL_DEST_X + COL_DEST_W - 1, baseY);
    const bool shouldScroll = cfg.scrollDest && textW > COL_DEST_W && d.length() != 0;
    if (!shouldScroll) {
      u8g2.setCursor(COL_DEST_X, baseY-1);
      u8g2.print(d);
    } else {
      int16_t x1 = COL_DEST_X - (rows[idx].offsetPx % cycle);
      int16_t x2 = x1 + cycle;
      u8g2.setCursor(x1, baseY-1); u8g2.print(d);
      u8g2.setCursor(x2, baseY-1); u8g2.print(d);
    }
    u8g2.setMaxClipWindow();

    String m = rows[idx].mins;
    if (m.length()) m = applyOffsetToMins(m);
    if (m.length() > 3) m.remove(3);
    drawRightAlignedText(DISP_XOFF + 128 - 2, baseY - 1, m);
  }
  drawPageIndicator();
  u8g2.sendBuffer();
}

void renderDestOnly() {
  u8g2.setFont(u8g2_font_6x12_tf);
  const uint8_t base = pageIndex * VISIBLE_ROWS;

  for (uint8_t r=0; r<VISIBLE_ROWS; ++r) {
    const uint8_t idx = base + r;
    if (idx >= MAX_ROWS) break;
    if (!rows[idx].dest.length()) continue;

    int16_t baseY = TOP_Y + r*ROW_H + ROW_H;

    // Zielbereich löschen (Original-Stil)
    u8g2.setDrawColor(0);
    u8g2.drawBox(COL_DEST_X, baseY-11, COL_DEST_W, 11);
    u8g2.setDrawColor(1);

    String d = rows[idx].dest; sanitizeDest(d);
    const int16_t textW = rows[idx].textW;
    const int16_t gapPx = 16;
    const int16_t cycle = max<int16_t>(1, textW + gapPx);

    u8g2.setClipWindow(COL_DEST_X, baseY-11, COL_DEST_X + COL_DEST_W - 1, baseY);
    const bool shouldScroll = cfg.scrollDest && textW > COL_DEST_W;
    if (!shouldScroll) {
      u8g2.setCursor(COL_DEST_X, baseY-1); u8g2.print(d);
    } else {
      int16_t x1 = COL_DEST_X - (rows[idx].offsetPx % cycle);
      int16_t x2 = x1 + cycle;
      u8g2.setCursor(x1, baseY-1); u8g2.print(d);
      u8g2.setCursor(x2, baseY-1); u8g2.print(d);
    }
    u8g2.setMaxClipWindow();
  }

  drawPageIndicator();
  u8g2.sendBuffer();
}

// ---------- Networking + JSON ----------
bool fetchOnce() {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  String url = buildUrl();
  if (!http.begin(url)) return false;

  http.addHeader("Accept","application/json");
  http.addHeader("Accept-Encoding","identity");
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }
  String payload = http.getString();
  http.end();

  StaticJsonDocument<JSON_DOC_CAP> doc;
  if (deserializeJson(doc, payload)) return false;
  if (!doc.is<JsonArrayConst>()) return false;
  JsonArrayConst arr = doc.as<JsonArrayConst>();
  if (arr.size()==0) return false;

  uint8_t r = 0, maxRows = min((int)cfg.limit, (int)MAX_ROWS);
  for (JsonVariantConst v: arr) {
    if (r >= maxRows) break;
    if (!v.is<JsonArrayConst>()) continue;
    JsonArrayConst rec = v.as<JsonArrayConst>();
    if (rec.size() < 3) continue;

    rows[r].line = rec[0].as<const char*>() ?: "";
    rows[r].dest = rec[1].as<const char*>() ?: "";
    rows[r].mins = rec[2].as<const char*>() ?: "";

    String tmp = rows[r].dest; sanitizeDest(tmp);
    rows[r].textW = u8g2.getUTF8Width(tmp.c_str());
    rows[r].offsetPx = 0;
    ++r;
  }
  for (; r<MAX_ROWS; ++r) rows[r] = Row();

  pageCount = max<uint8_t>(1, (uint8_t)((maxRows + VISIBLE_ROWS - 1) / VISIBLE_ROWS));
  if (pageIndex >= pageCount) pageIndex = 0;

  return true;
}

// ---------- NVS ----------
void loadConfig() {
  prefs.begin(CFG_NAMESPACE, true);
  cfg.ssid = prefs.getString(CFG_SSID, "");
  cfg.pw   = prefs.getString(CFG_PW,   "");
  cfg.hst  = prefs.getString(CFG_HST,  DEFAULT_HST);
  cfg.ort  = prefs.getString(CFG_ORT,  DEFAULT_ORT);
  cfg.offsetMin = prefs.getInt(CFG_OFF, DEFAULT_OFF);
  cfg.limit     = prefs.getInt(CFG_LIM, DEFAULT_LIM);
    cfg.scrollDest = prefs.getBool(CFG_SCR, true);
  prefs.end();
  cfg.limit = constrain(cfg.limit, 1, MAX_ROWS);
}

void saveConfig() {
  prefs.begin(CFG_NAMESPACE, false);
  prefs.putString(CFG_SSID, cfg.ssid);
  prefs.putString(CFG_PW,   cfg.pw);
  prefs.putString(CFG_HST,  cfg.hst);
  prefs.putString(CFG_ORT,  cfg.ort);
  prefs.putInt(CFG_OFF,     cfg.offsetMin);
  prefs.putInt(CFG_LIM,     cfg.limit);
    prefs.putBool(CFG_SCR,    cfg.scrollDest);
  prefs.end();
}

// ---------- Setup-Portal HTML (minimal) ----------
String chipSuffix() {
  uint64_t mac = ESP.getEfuseMac();
  uint16_t tail = (uint16_t)(mac & 0xFFFF);
  char buf[6]; snprintf(buf, sizeof(buf), "%04X", tail);
  return String(buf);
}

String formHtml() {
  String html = "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>DVB Setup</title><style>body{font-family:sans-serif;margin:24px}input{width:100%;padding:8px;margin:6px 0}";
  html += "label{display:block;margin:12px 0 6px}input[type=checkbox]{width:auto;padding:0;margin-right:8px}</style></head><body><h2>DVB Abfahrtsmonitor – Setup</h2><form method='POST' action='/save'>";
  html += "SSID:<input name='ssid' value='" + cfg.ssid + "'>";
  html += "Passwort:<input type='password' name='pw' value='" + cfg.pw + "'>";
  html += "Haltestelle (hst):<input name='hst' value='" + cfg.hst + "'>";
  html += "Ort (ort):<input name='ort' value='" + cfg.ort + "'>";
  html += "Minuten-Offset (vz):<input type='number' name='offset' value='" + String(cfg.offsetMin) + "'>";
  html += "Anzahl Eintraege (1..10):<input type='number' min='1' max='10' name='limit' value='" + String(cfg.limit) + "'>";
  html += "<label><input type='checkbox' name='scroll' value='1'";
  if (cfg.scrollDest) html += " checked";
  html += ">Scrollen aktivieren</label>";
  html += "<button type='submit'>Speichern & Neustarten</button></form></body></html>";
  return html;
}

void handleRoot() { server.send(200, "text/html", formHtml()); }

void handleSave() {
  if (server.method() != HTTP_POST) { server.send(405, "text/plain", "Method Not Allowed"); return; }
  if (server.hasArg("ssid"))   cfg.ssid = server.arg("ssid");
  if (server.hasArg("pw"))     cfg.pw   = server.arg("pw");
  if (server.hasArg("hst"))    cfg.hst  = server.arg("hst");
  if (server.hasArg("ort"))    cfg.ort  = server.arg("ort");
  if (server.hasArg("offset")) cfg.offsetMin = server.arg("offset").toInt();
  if (server.hasArg("limit"))  cfg.limit     = constrain(server.arg("limit").toInt(), 1, MAX_ROWS);
    cfg.scrollDest = server.hasArg("scroll");
  saveConfig();
  server.send(200, "text/html", "<html><body><h3>Gespeichert. Neustart...</h3></body></html>");
  delay(500);
  ESP.restart();
}

// ---------- Setupmodus: LCD-Anleitung (Paging) ----------
void drawSetupPage(uint8_t page, const String& apName) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  switch (page) {
    case 0:
      u8g2.setCursor(2,14); u8g2.print("Setup-Modus aktiv");
      u8g2.setCursor(2,28); u8g2.print("AP: "); u8g2.print(apName);
      u8g2.setCursor(2,42); u8g2.print("PW: collaborative");
      u8g2.setCursor(2,56); u8g2.print("BOOT = weiter");
      break;
    case 1:
      u8g2.setCursor(2,14); u8g2.print("- Mit WLAN verbinden");
      u8g2.setCursor(2,28); u8g2.print("- Mobile Daten AUS");
      u8g2.setCursor(2,42); u8g2.print("- http://192.168.4.1");
      u8g2.setCursor(2,56); u8g2.print("im Browser oeffnen");
      break;
	      case 2:
      u8g2.setCursor(2,14); u8g2.print("Setup-Modus aktiv");
      u8g2.setCursor(2,28); u8g2.print("AP: "); u8g2.print(apName);
      u8g2.setCursor(2,42); u8g2.print("PW: collaborative");
      u8g2.setCursor(2,56); u8g2.print("BOOT = weiter");
      break;
	      case 3:
      u8g2.setCursor(2,14); u8g2.print("Setup-Modus aktiv");
      u8g2.setCursor(2,28); u8g2.print("AP: "); u8g2.print(apName);
      u8g2.setCursor(2,42); u8g2.print("PW: collaborative");
      u8g2.setCursor(2,56); u8g2.print("BOOT = weiter");
      break;
  }
  u8g2.sendBuffer();
}

void startConfigPortal() {
  String apName = "Wifi@DVB-" + chipSuffix();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str(), "collaborative");

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  // LCD-Anleitung mit Paging (Auto 4s, manuell mit BOOT)
  uint8_t page = 0;
  uint32_t tp = millis();
  drawSetupPage(page, apName);

  while (true) {
    server.handleClient();
    bool btn = (digitalRead(BOOT_BUTTON_PIN) == LOW);
    static bool last = false;
    if (btn && !last) { page = (page + 1) % 4; drawSetupPage(page, apName); tp = millis(); }
    last = btn;
    if (millis() - tp >= 4000UL) { page = (page + 1) % 4; drawSetupPage(page, apName); tp = millis(); }
    delay(2);
  }
}

// ---------- Setup / Loop ----------
void setup() {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(50);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  //Wire.setClock(900000);
  
  u8g2.setI2CAddress(I2C_ADDR_7BIT << 1);
  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setContrast(185);
  u8g2.sendF("c", 0xA7);  // INVERSE mode (alles invertiert)

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.setCursor(2,14); u8g2.print("DVB Abfahrtsmonitor");
  u8g2.setCursor(2,30); u8g2.print("Drücke BOOT für Setup");
  u8g2.setCursor(2,50); u8g2.print("(Startup nach 5 Sek.)");
  u8g2.sendBuffer();

  uint32_t t0 = millis();
  while (millis() - t0 < BOOT_DELAY_MS) {
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      // Direkt ins Setup-Portal
      startConfigPortal(); // kehrt nicht zurück
    }
    delay(10);
  }

  // Konfig laden
  loadConfig();

  // WLAN verbinden
  u8g2.clearBuffer();
  u8g2.setCursor(2, 14); u8g2.print("Verbinde WLAN...");
  u8g2.sendBuffer();

  WiFi.mode(WIFI_STA);
  if (cfg.ssid.length()) WiFi.begin(cfg.ssid.c_str(), cfg.pw.c_str());

  uint32_t tc = millis();
  while (WiFi.status() != WL_CONNECTED && (millis()-tc) < WIFI_CONNECT_MS) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    startConfigPortal(); // kehrt nicht zurück
  }

  fetchOnce();

  // Paging init
  uint8_t maxRows = min(cfg.limit, (int)MAX_ROWS);
  pageCount = max<uint8_t>(1, (uint8_t)((maxRows + VISIBLE_ROWS - 1) / VISIBLE_ROWS));
  pageIndex = 0;
  tPage = millis();

  renderFull();
  tFetch = millis();
  tScroll = millis();
}

void loop() {
  uint32_t now = millis();

  // 30s Reload
  if (now - tFetch >= FETCH_INTERVAL_MS) {
    if (fetchOnce()) failCount = 0; else if (++failCount > MAX_ERRORS) failCount = 0;

    // Paging ggf. neu berechnen
    uint8_t maxRows = min(cfg.limit, (int)MAX_ROWS);
    pageCount = max<uint8_t>(1, (uint8_t)((maxRows + VISIBLE_ROWS - 1) / VISIBLE_ROWS));
    if (pageIndex >= pageCount) pageIndex = 0;

    renderFull();
    tFetch = now;
  }

  // Marquee-Ticks für sichtbare Zeilen
  if (now - tScroll >= SCROLL_INTERVAL_MS) {
    bool any = false;
    const uint8_t base = pageIndex * VISIBLE_ROWS;
    for (uint8_t r=0; r<VISIBLE_ROWS; ++r) {
      const uint8_t idx = base + r;
      if (idx >= MAX_ROWS) break;
      if (!rows[idx].dest.length()) continue;
      if (cfg.scrollDest && rows[idx].textW > COL_DEST_W) {
        rows[idx].offsetPx += SCROLL_STEP_PX;
        any = true;
      }
    }
    if (any) renderDestOnly();
    tScroll = now;
  }

  // Seitenwechsel
  if (pageCount > 1 && (now - tPage >= PAGE_INTERVAL_MS)) {
    pageIndex = (pageIndex + 1) % pageCount;
    renderFull();
    tPage = now;
  }
}
