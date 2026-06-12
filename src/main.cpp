#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEUtils.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"

TFT_eSPI tft;

static constexpr int TOUCH_IRQ = 36;
static constexpr int TOUCH_MOSI = 32;
static constexpr int TOUCH_MISO = 39;
static constexpr int TOUCH_CLK = 25;
static constexpr int TOUCH_CS = 33;

static BLEUUID BATTERY_SERVICE_UUID("0000ff00-0000-1000-8000-00805f9b34fb");
static BLEUUID BATTERY_DATA_UUID("0000ff02-0000-1000-8000-00805f9b34fb");
static const char *BATTERY_DEVICE_PREFIX = "HM_B2500";
static const char *SETUP_AP_SSID = "Balkonkraftwerk-Setup";

struct AppConfig {
  String wifiSsid;
  String wifiPassword;
  String dtuHost;
  String dtuUser;
  String dtuPassword;
  String inverterSerial;

  bool valid() const {
    return wifiSsid.length() > 0 && dtuHost.length() > 0 && dtuUser.length() > 0 &&
           dtuPassword.length() > 0 && inverterSerial.length() > 0;
  }
};

struct DtuData {
  bool valid = false;
  bool reachable = false;
  bool producing = false;
  float powerW = NAN;
  float yieldDayWh = NAN;
  float limitW = NAN;
  float limitPercent = NAN;
  float stringPower[2] = {NAN, NAN};
  float stringVoltage[2] = {NAN, NAN};
  String error = "";
};

struct BatteryData {
  bool valid = false;
  bool connected = false;
  int soc = -1;
  int in1W = -1;
  int in2W = -1;
  int out1W = -1;
  int out2W = -1;
  int tempC = 0;
  int minMv = 0;
  int maxMv = 0;
  int avgMv = 0;
  int spreadMv = 0;
  int cellCount = 0;
  String status = "SUCHE";
};

static DtuData dtu;
static BatteryData battery;
static AppConfig appConfig;
static Preferences preferences;
static WebServer setupServer(80);

static BLERemoteCharacteristic *batteryChar = nullptr;
static BLEAdvertisedDevice *targetDevice = nullptr;
static BLEClient *batteryClient = nullptr;
static bool batteryResponseReady = false;
static std::vector<uint8_t> batteryResponse;

static unsigned long lastDtuRefresh = 0;
static unsigned long lastBatteryRefresh = 0;
static unsigned long lastWifiTry = 0;
static unsigned long lastBleTry = 0;
static unsigned long lastTouch = 0;
static unsigned long wifiConnectStarted = 0;
static uint8_t currentPage = 0;
static int8_t renderedPage = -1;
static bool screenReady = false;
static bool setupMode = false;

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(r, g, b);
}

String formatNumber(float value, uint8_t decimals, const char *unit) {
  if (isnan(value)) return String("-- ") + unit;
  char buf[24];
  dtostrf(value, 0, decimals, buf);
  return String(buf) + " " + unit;
}

String formatWatts(float value) {
  if (isnan(value)) return "-- W";
  return String((int)round(value)) + " W";
}

String formatDayYield(float wh) {
  if (isnan(wh)) return "-- Wh";
  if (wh >= 1000.0f) return formatNumber(wh / 1000.0f, 2, "kWh");
  return formatNumber(wh, 0, "Wh");
}

String formatIntWatts(int value) {
  if (value < 0) return "-- W";
  return String(value) + " W";
}

String formatMv(int mv) {
  if (mv <= 0) return "-.---";
  return String(mv / 1000.0f, 3);
}

uint16_t socColor(int soc) {
  if (soc < 0) return rgb(145, 162, 184);
  if (soc < 10) return rgb(255, 118, 118);
  if (soc < 50) return rgb(255, 226, 138);
  return rgb(117, 240, 193);
}

uint16_t readTouch12(uint8_t command) {
  uint16_t value = 0;
  digitalWrite(TOUCH_CS, LOW);
  delayMicroseconds(2);
  for (int8_t bit = 7; bit >= 0; bit--) {
    digitalWrite(TOUCH_MOSI, (command >> bit) & 1);
    digitalWrite(TOUCH_CLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(TOUCH_CLK, LOW);
    delayMicroseconds(1);
  }
  for (uint8_t i = 0; i < 16; i++) {
    digitalWrite(TOUCH_CLK, HIGH);
    delayMicroseconds(1);
    value <<= 1;
    if (digitalRead(TOUCH_MISO)) value |= 1;
    digitalWrite(TOUCH_CLK, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(TOUCH_CS, HIGH);
  return value >> 3;
}

bool isTouchPressed() {
  if (digitalRead(TOUCH_IRQ) == LOW) return true;
  uint16_t z1 = readTouch12(0xB1);
  uint16_t z2 = readTouch12(0xC1);
  int pressure = (int)z1 + 4095 - (int)z2;
  return pressure > 350;
}

String htmlEscape(const String &text) {
  String out;
  out.reserve(text.length() + 8);
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

bool loadConfig() {
  preferences.begin("setup", true);
  appConfig.wifiSsid = preferences.getString("ssid", "");
  appConfig.wifiPassword = preferences.getString("wpass", "");
  appConfig.dtuHost = preferences.getString("dtu", "");
  appConfig.dtuUser = preferences.getString("user", "admin");
  appConfig.dtuPassword = preferences.getString("dpass", "");
  appConfig.inverterSerial = preferences.getString("serial", "");
  preferences.end();
  return appConfig.valid();
}

void saveConfig() {
  preferences.begin("setup", false);
  preferences.putString("ssid", appConfig.wifiSsid);
  preferences.putString("wpass", appConfig.wifiPassword);
  preferences.putString("dtu", appConfig.dtuHost);
  preferences.putString("user", appConfig.dtuUser);
  preferences.putString("dpass", appConfig.dtuPassword);
  preferences.putString("serial", appConfig.inverterSerial);
  preferences.end();
}

String setupPageHtml(const String &message = "") {
  String page;
  page.reserve(6500);
  page += F("<!doctype html><html lang='de'><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>Balkonkraftwerk Setup</title><style>");
  page += F(":root{color-scheme:dark}body{margin:0;background:#10141b;color:#f5f8ff;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}");
  page += F(".wrap{max-width:520px;margin:0 auto;padding:20px 16px 34px}.head{background:#192f4a;border-radius:18px;padding:18px 18px 16px;margin-bottom:24px}");
  page += F("h1{font-size:30px;line-height:1.1;margin:0 0 8px}p{color:#a9bbd1;font-size:17px;line-height:1.35;margin:0}.sec{color:#82e9ff;font-weight:700;margin:24px 0 12px;font-size:16px;text-transform:uppercase;letter-spacing:.04em}");
  page += F("label{display:block;font-size:18px;margin:16px 0 7px}.hint{font-size:14px;color:#8fa2b8;margin-top:7px}");
  page += F("input{box-sizing:border-box;width:100%;height:54px;border-radius:12px;border:2px solid #36506e;background:#0b121d;color:#fff;font-size:18px;padding:0 14px;outline:none}");
  page += F("input:focus{border-color:#75f0c1}.btn{width:100%;height:60px;border:0;border-radius:14px;background:#75f0c1;color:#062d24;font-weight:800;font-size:19px;margin-top:26px}");
  page += F(".note{background:#24384f;border-radius:14px;padding:14px 16px;margin-top:20px}.msg{background:#214834;color:#bfffe7;border-radius:12px;padding:12px 14px;margin-bottom:16px;font-size:16px}");
  page += F("</style></head><body><main class='wrap'><div class='head'><h1>Balkonkraftwerk Setup</h1><p>Hotspot: Balkonkraftwerk-Setup<br>Adresse: http://192.168.4.1</p></div>");
  if (message.length()) page += "<div class='msg'>" + htmlEscape(message) + "</div>";
  page += F("<form method='post' action='/save'><div class='sec'>WLAN</div>");
  page += F("<label>WLAN-Name</label><input name='ssid' autocomplete='off' required value='");
  page += htmlEscape(appConfig.wifiSsid);
  page += F("'><label>WLAN-Passwort</label><input name='wpass' type='password' autocomplete='off' required value='");
  page += htmlEscape(appConfig.wifiPassword);
  page += F("'><div class='sec'>OpenDTU-OnBattery</div><label>OpenDTU-OnBattery IP</label><input name='dtu' inputmode='decimal' required placeholder='192.168.1.100' value='");
  page += htmlEscape(appConfig.dtuHost);
  page += F("'><label>Benutzername</label><input name='user' autocomplete='off' required value='");
  page += htmlEscape(appConfig.dtuUser.length() ? appConfig.dtuUser : "admin");
  page += F("'><label>Passwort</label><input name='dpass' type='password' autocomplete='off' required value='");
  page += htmlEscape(appConfig.dtuPassword);
  page += F("'><label>Inverter-Seriennummer</label><input name='serial' inputmode='numeric' autocomplete='off' required value='");
  page += htmlEscape(appConfig.inverterSerial);
  page += F("'><div class='hint'>Die Seriennummer steht in deiner OpenDTU-OnBattery beim Wechselrichter.</div>");
  page += F("<button class='btn' type='submit'>Speichern und Neustarten</button></form>");
  page += F("<div class='note'><p>Nach dem Speichern verbindet sich das Display mit deinem WLAN und liest die OpenDTU-OnBattery-Daten. Der Marstek B2500 Akku wird automatisch per Bluetooth gesucht.</p></div>");
  page += F("</main></body></html>");
  return page;
}

void drawStatusPill(int x, int y, int w, const String &text, uint16_t color) {
  tft.fillRoundRect(x, y, w, 16, 4, color);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_BLACK, color);
  tft.drawString(text, x + w / 2, y + 8, 2);
}

void drawHeader(const char *title) {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 320, 29, rgb(16, 32, 51));
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, rgb(16, 32, 51));
  tft.drawString(title, 9, 15, 4);
  uint16_t color = dtu.reachable ? rgb(117, 240, 193) : rgb(255, 118, 118);
  drawStatusPill(250, 6, 60, dtu.reachable ? "ONLINE" : "OFF", color);
}

void drawCard(int x, int y, int w, int h, bool strong = false) {
  uint16_t border = strong ? rgb(54, 80, 110) : rgb(36, 56, 79);
  tft.drawRoundRect(x, y, w, h, 6, border);
}

void drawBatteryBar(int x, int y, int w, int h, int soc) {
  tft.fillRoundRect(x + w / 2 - 7, y, 14, 5, 2, rgb(145, 162, 184));
  tft.drawRoundRect(x, y + 5, w, h, 5, rgb(145, 162, 184));
  tft.fillRoundRect(x + 4, y + 10, w - 8, h - 10, 3, rgb(20, 28, 39));
  int fillH = soc >= 0 ? map(constrain(soc, 0, 100), 0, 100, 0, h - 16) : 0;
  uint16_t color = socColor(soc);
  tft.fillRoundRect(x + 4, y + h - 1 - fillH, w - 8, fillH, 3, color);
  tft.drawFastHLine(x + 4, y + h - 1 - (h - 16) / 2, w - 8, TFT_BLACK);
}

void drawPageHint() {
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(rgb(143, 162, 184), rgb(16, 32, 51));
  int x = currentPage == 0 ? 237 : 235;
  tft.drawString(String(currentPage + 1) + "/2", x, 15, 2);
}

void drawWifiScreen(const char *message) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Balkonkraftwerk", 160, 70, 4);
  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString(message, 160, 116, 2);
  tft.drawString(appConfig.wifiSsid.length() ? appConfig.wifiSsid : "keine WLAN-Daten", 160, 142, 2);
}

void drawSetupScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Setup-Modus", 160, 54, 4);
  tft.setTextColor(rgb(130, 233, 255), TFT_BLACK);
  tft.drawString("WLAN: Balkonkraftwerk-Setup", 160, 102, 2);
  tft.setTextColor(rgb(255, 226, 138), TFT_BLACK);
  tft.drawString("Browser: 192.168.4.1", 160, 132, 2);
  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("Daten eingeben und speichern", 160, 176, 2);
}

void handleSetupRoot() {
  setupServer.send(200, "text/html", setupPageHtml());
}

void handleSetupSave() {
  appConfig.wifiSsid = setupServer.arg("ssid");
  appConfig.wifiPassword = setupServer.arg("wpass");
  appConfig.dtuHost = setupServer.arg("dtu");
  appConfig.dtuUser = setupServer.arg("user");
  appConfig.dtuPassword = setupServer.arg("dpass");
  appConfig.inverterSerial = setupServer.arg("serial");
  appConfig.wifiSsid.trim();
  appConfig.dtuHost.trim();
  appConfig.dtuUser.trim();
  appConfig.inverterSerial.trim();

  if (!appConfig.valid()) {
    setupServer.send(400, "text/html", setupPageHtml("Bitte alle Felder ausfuellen."));
    return;
  }

  saveConfig();
  setupServer.send(200, "text/html", setupPageHtml("Gespeichert. Das Display startet neu."));
  delay(800);
  ESP.restart();
}

void startSetupPortal() {
  setupMode = true;
  WiFi.disconnect(true);
  delay(150);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_SSID);
  setupServer.on("/", HTTP_GET, handleSetupRoot);
  setupServer.on("/save", HTTP_POST, handleSetupSave);
  setupServer.onNotFound(handleSetupRoot);
  setupServer.begin();
  drawSetupScreen();
}

void drawPage1() {
  drawHeader("Balkonkraftwerk");
  drawCard(7, 36, 148, 78, true);
  drawCard(165, 36, 148, 78, true);
  drawCard(7, 123, 148, 61);
  drawCard(165, 123, 148, 61);
  drawCard(7, 193, 306, 36, true);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("Wechselrichter", 16, 48, 2);
  tft.setTextColor(rgb(255, 226, 138), TFT_BLACK);
  tft.drawString(formatWatts(dtu.powerW), 16, 73, 4);
  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("Heute", 16, 96, 2);
  tft.setTextColor(rgb(255, 226, 138), TFT_BLACK);
  tft.drawString(formatDayYield(dtu.yieldDayWh), 64, 96, 2);

  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("Akku", 174, 48, 2);
  tft.setTextColor(socColor(battery.soc), TFT_BLACK);
  tft.drawString(battery.soc >= 0 ? String(battery.soc) : "--", 174, 63, 6);
  tft.drawString("%", 252, 88, 2);
  drawBatteryBar(277, 48, 26, 58, battery.soc);

  int totalIn = max(0, battery.in1W) + max(0, battery.in2W);
  int totalOut = max(0, battery.out1W) + max(0, battery.out2W);
  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("Eingang gesamt", 16, 134, 2);
  tft.setTextColor(rgb(130, 233, 255), TFT_BLACK);
  tft.drawString(battery.valid ? String(totalIn) + " W" : "-- W", 16, 154, 4);

  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("Ausgang gesamt", 174, 134, 2);
  tft.setTextColor(rgb(255, 226, 138), TFT_BLACK);
  tft.drawString(battery.valid ? String(totalOut) + " W" : "-- W", 174, 154, 4);

  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("Limit", 16, 202, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(formatNumber(dtu.limitW, 0, "W") + " / " + formatNumber(dtu.limitPercent, 0, "%"), 92, 202, 4);
  drawPageHint();
}

void drawPage2() {
  drawHeader("Details");
  drawCard(7, 36, 148, 66, true);
  drawCard(165, 36, 148, 66, true);
  drawCard(7, 111, 306, 56);
  drawCard(7, 170, 306, 59, true);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("String 1", 16, 50, 2);
  tft.setTextColor(rgb(130, 233, 255), TFT_BLACK);
  tft.drawString(formatWatts(dtu.stringPower[0]), 16, 75, 4);
  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString(formatNumber(dtu.stringVoltage[0], 1, "V"), 90, 77, 2);

  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("String 2", 174, 50, 2);
  tft.setTextColor(rgb(130, 233, 255), TFT_BLACK);
  tft.drawString(formatWatts(dtu.stringPower[1]), 174, 75, 4);
  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString(formatNumber(dtu.stringVoltage[1], 1, "V"), 248, 77, 2);

  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("Akku Leistung", 16, 124, 2);
  tft.drawString("IN1", 16, 148, 2);
  tft.setTextColor(rgb(117, 240, 193), TFT_BLACK);
  tft.drawString(formatIntWatts(battery.in1W), 46, 148, 2);
  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("IN2", 109, 148, 2);
  tft.setTextColor(rgb(117, 240, 193), TFT_BLACK);
  tft.drawString(formatIntWatts(battery.in2W), 139, 148, 2);
  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("OUT", 205, 148, 2);
  tft.setTextColor(rgb(255, 226, 138), TFT_BLACK);
  tft.drawString(formatIntWatts(max(0, battery.out1W) + max(0, battery.out2W)), 240, 148, 2);

  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("Temp. Akku", 16, 181, 2);
  tft.setTextColor(rgb(255, 226, 138), TFT_BLACK);
  tft.drawString(battery.valid ? String(battery.tempC) + " C" : "-- C", 16, 202, 4);
  tft.drawFastVLine(95, 178, 42, rgb(36, 56, 79));
  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("Zellen", 193, 181, 2);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Min", 110, 200, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(formatMv(battery.minMv), 140, 200, 2);
  tft.setTextColor(rgb(145, 162, 184), TFT_BLACK);
  tft.drawString("Max", 199, 200, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(formatMv(battery.maxMv), 229, 200, 2);
  tft.setTextColor(rgb(255, 226, 138), TFT_BLACK);
  tft.drawString("Diff " + formatMv(battery.spreadMv) + " V", 148, 218, 1);
  drawPageHint();
}

void drawScreen() {
  if (currentPage == 0) drawPage1();
  else drawPage2();
  renderedPage = currentPage;
  screenReady = true;
}

bool httpGetJson(const String &path, JsonDocument &doc, String &error) {
  HTTPClient http;
  String url = String("http://") + appConfig.dtuHost + path;
  http.begin(url);
  http.setAuthorization(appConfig.dtuUser.c_str(), appConfig.dtuPassword.c_str());
  http.setTimeout(3500);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    error = "HTTP " + String(code);
    http.end();
    return false;
  }
  DeserializationError jsonError = deserializeJson(doc, http.getStream());
  http.end();
  if (jsonError) {
    error = jsonError.c_str();
    return false;
  }
  return true;
}

bool refreshDtu() {
  JsonDocument live;
  String error;
  String path = String("/api/livedata/status?inv=") + appConfig.inverterSerial;
  if (!httpGetJson(path, live, error)) {
    dtu.valid = false;
    dtu.reachable = false;
    dtu.error = error;
    return false;
  }

  DtuData next;
  JsonObject inv = live["inverters"][0];
  next.reachable = inv["reachable"] | false;
  next.producing = inv["producing"] | false;
  next.limitW = inv["limit_absolute"] | NAN;
  next.limitPercent = inv["limit_relative"] | NAN;
  next.powerW = live["total"]["Power"]["v"] | NAN;
  next.yieldDayWh = live["total"]["YieldDay"]["v"] | NAN;
  for (uint8_t i = 0; i < 2; i++) {
    String key = String(i);
    next.stringPower[i] = inv["DC"][key]["Power"]["v"] | NAN;
    next.stringVoltage[i] = inv["DC"][key]["Voltage"]["v"] | NAN;
  }
  next.valid = true;
  dtu = next;
  return true;
}

uint16_t u16le(const std::vector<uint8_t> &d, int i) {
  if (i + 1 >= (int)d.size()) return 0;
  return d[i] | (d[i + 1] << 8);
}

int16_t s16le(const std::vector<uint8_t> &d, int i) {
  return (int16_t)u16le(d, i);
}

bool parseRuntime(const std::vector<uint8_t> &d) {
  if (d.size() < 39 || d[0] != 0x73 || d[2] != 0x23 || d[3] != 0x03) return false;
  battery.in1W = u16le(d, 6);
  battery.in2W = u16le(d, 8);
  battery.soc = u16le(d, 10) / 10;
  battery.out1W = u16le(d, 24);
  battery.out2W = u16le(d, 26);
  battery.tempC = s16le(d, 33);
  battery.valid = true;
  return true;
}

bool parseCell(const std::vector<uint8_t> &d) {
  String s;
  for (uint8_t b : d) {
    if ((b >= '0' && b <= '9') || b == '_') s += (char)b;
  }
  int parts[24] = {0};
  int count = 0;
  int start = 0;
  while (start < (int)s.length() && count < 24) {
    int sep = s.indexOf('_', start);
    String p = sep < 0 ? s.substring(start) : s.substring(start, sep);
    if (p.length()) parts[count++] = p.toInt();
    if (sep < 0) break;
    start = sep + 1;
  }
  if (count < 6) return false;
  battery.cellCount = min(count - 3, 20);
  battery.minMv = 99999;
  battery.maxMv = 0;
  int sumMv = 0;
  for (int i = 0; i < battery.cellCount; i++) {
    int mv = parts[i + 3];
    sumMv += mv;
    battery.minMv = min(battery.minMv, mv);
    battery.maxMv = max(battery.maxMv, mv);
  }
  battery.avgMv = battery.cellCount > 0 ? sumMv / battery.cellCount : 0;
  battery.spreadMv = battery.cellCount > 0 ? battery.maxMv - battery.minMv : 0;
  battery.valid = true;
  return true;
}

class AdvertisedCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    String name = advertisedDevice.getName().c_str();
    if (name.startsWith(BATTERY_DEVICE_PREFIX)) {
      BLEDevice::getScan()->stop();
      if (targetDevice) delete targetDevice;
      targetDevice = new BLEAdvertisedDevice(advertisedDevice);
    }
  }
};

void notifyCallback(BLERemoteCharacteristic*, uint8_t *data, size_t length, bool) {
  batteryResponse.assign(data, data + length);
  batteryResponseReady = true;
}

bool connectBattery() {
  battery.status = "SUCHE";
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new AdvertisedCallbacks(), true);
  scan->setActiveScan(true);
  scan->start(5, false);
  scan->clearResults();
  if (!targetDevice) {
    battery.status = "KEIN BLE";
    battery.connected = false;
    return false;
  }

  if (batteryClient) {
    delete batteryClient;
    batteryClient = nullptr;
  }
  batteryClient = BLEDevice::createClient();
  if (!batteryClient->connect(targetDevice)) {
    battery.status = "BLE FEHLER";
    battery.connected = false;
    return false;
  }
  batteryClient->setMTU(517);
  BLERemoteService *service = batteryClient->getService(BATTERY_SERVICE_UUID);
  if (!service) {
    batteryClient->disconnect();
    battery.status = "FF00 FEHLT";
    battery.connected = false;
    return false;
  }
  batteryChar = service->getCharacteristic(BATTERY_DATA_UUID);
  if (!batteryChar) {
    batteryClient->disconnect();
    battery.status = "FF02 FEHLT";
    battery.connected = false;
    return false;
  }
  if (batteryChar->canNotify()) batteryChar->registerForNotify(notifyCallback);
  battery.connected = true;
  battery.status = "BLE OK";
  return true;
}

bool sendBatteryCommand(uint8_t command, uint16_t waitMs) {
  if (!battery.connected || !batteryChar) return false;
  uint8_t msg[5] = {0x73, 0x05, 0x23, command, 0x00};
  msg[4] = msg[0] ^ msg[1] ^ msg[2] ^ msg[3];
  batteryResponseReady = false;
  batteryChar->writeValue(msg, sizeof(msg), false);
  unsigned long start = millis();
  while (millis() - start < waitMs) {
    if (batteryResponseReady) return true;
    delay(10);
  }
  return false;
}

bool refreshBattery() {
  if (!battery.connected || !batteryClient || !batteryClient->isConnected()) {
    battery.connected = false;
    batteryChar = nullptr;
    return false;
  }

  bool ok = false;
  if (sendBatteryCommand(0x03, 1200)) ok = parseRuntime(batteryResponse) || ok;
  delay(120);
  if (sendBatteryCommand(0x0F, 1200)) ok = parseCell(batteryResponse) || ok;
  battery.status = ok ? "BLE OK" : "BLE FEHLER";
  return ok;
}

void connectWifi() {
  if (!appConfig.valid()) {
    startSetupPortal();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(appConfig.wifiSsid.c_str(), appConfig.wifiPassword.c_str());
  wifiConnectStarted = millis();
  lastWifiTry = millis();
  drawWifiScreen("Verbinde mit WLAN");
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  tft.init();
  tft.setRotation(1);
  tft.setTextFont(2);
  tft.setTextSize(1);

  pinMode(TOUCH_CS, OUTPUT);
  pinMode(TOUCH_CLK, OUTPUT);
  pinMode(TOUCH_MOSI, OUTPUT);
  pinMode(TOUCH_MISO, INPUT);
  pinMode(TOUCH_IRQ, INPUT_PULLUP);
  digitalWrite(TOUCH_CS, HIGH);
  digitalWrite(TOUCH_CLK, LOW);
  digitalWrite(TOUCH_MOSI, LOW);

  loadConfig();
  if (appConfig.valid()) connectWifi();
  else startSetupPortal();
  BLEDevice::init("Balkonkraftwerk_Display");
  BLEDevice::setPower(ESP_PWR_LVL_P9);
}

void loop() {
  if (setupMode) {
    setupServer.handleClient();
    delay(10);
    return;
  }

  if (isTouchPressed() && millis() - lastTouch > 450) {
    lastTouch = millis();
    currentPage = (currentPage + 1) % 2;
    drawScreen();
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnectStarted == 0) wifiConnectStarted = millis();
    if (wifiConnectStarted > 0 && millis() - wifiConnectStarted > 30000) {
      startSetupPortal();
      return;
    }
    if (millis() - lastWifiTry > 10000) {
      lastWifiTry = millis();
      WiFi.disconnect();
      WiFi.begin(appConfig.wifiSsid.c_str(), appConfig.wifiPassword.c_str());
      drawWifiScreen("WLAN erneut verbinden");
      screenReady = false;
    }
  } else {
    wifiConnectStarted = 0;
    if (millis() - lastDtuRefresh >= DTU_REFRESH_SECONDS * 1000UL || lastDtuRefresh == 0) {
      lastDtuRefresh = millis();
      refreshDtu();
      drawScreen();
    }
  }

  if (!battery.connected) {
    if (millis() - lastBleTry > 15000) {
      lastBleTry = millis();
      if (connectBattery()) {
        refreshBattery();
        lastBatteryRefresh = millis();
      }
      drawScreen();
    }
  } else if (millis() - lastBatteryRefresh >= BATTERY_REFRESH_SECONDS * 1000UL || lastBatteryRefresh == 0) {
    lastBatteryRefresh = millis();
    refreshBattery();
    drawScreen();
  }

  if (!screenReady || renderedPage != currentPage) drawScreen();
  delay(50);
}
