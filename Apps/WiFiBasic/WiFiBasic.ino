#include <stdint.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------- Pins ----------------
#define BTN_UP     D0
#define BTN_DOWN   D8
#define BTN_LEFT   D1
#define BTN_RIGHT  D7
#define BTN_A      D10
#define BTN_B      D9

// ---------------- OLED ----------------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define I2C_SDA D3
#define I2C_SCL D4
static const uint8_t OLED_ADDRS[] = { 0x3C, 0x3D };
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------------- Timing / Layout ----------------
#define DEBOUNCE_MS     18
#define CONTENT_TOP_Y   12
#define FOOTER_LINE_Y   53
#define FOOTER_TEXT_Y   55
static const uint32_t SPLASH_MS = 10000;

// ---------------- Screens ----------------
typedef uint8_t Screen;
enum : uint8_t {
  SCR_SPLASH = 0,
  SCR_SCAN_ANIM,
  SCR_FOUND_SAVED,
  SCR_MAIN,
  SCR_CONNECT_MENU,
  SCR_DISCONNECT_MENU,
  SCR_SAVED_PROMPT,
  SCR_LIST,
  SCR_PASS,
  SCR_CONNECT,
  SCR_STATUS,
  SCR_MSG
};
static Screen screen = SCR_SPLASH;

// ---------------- Buttons ----------------
enum BtnId : uint8_t { BID_UP, BID_DOWN, BID_LEFT, BID_RIGHT, BID_A, BID_B, BTN_N };

struct BtnState {
  int pin;
  bool now;
  bool prev;
  uint32_t lastChangeMs;
};

static BtnState btn[BTN_N] = {
  { BTN_UP,    false, false, 0 },
  { BTN_DOWN,  false, false, 0 },
  { BTN_LEFT,  false, false, 0 },
  { BTN_RIGHT, false, false, 0 },
  { BTN_A,     false, false, 0 },
  { BTN_B,     false, false, 0 },
};

static uint32_t lastFrame = 0;
static uint32_t splashStart = 0;

// ---------------- Preferences ----------------
Preferences prefs;
static bool haveSaved = false;
static String savedSsid;
static String savedPass;

// ---------------- Time (RTC-like) ----------------
static bool timeSynced = false;

// ---------------- Main menu ----------------
// 0 = WiFi Status (top), 1 = Connect/Disconnect (bottom)
static uint8_t mainSel = 0;

// ---------------- Submenus ----------------
static uint8_t connMenuSel = 0; // 0..1
static uint8_t discMenuSel = 0; // 0..1

// ---------------- Message screen ----------------
static char msgTitle[18] = "Message";
static char msg1[64] = "";
static char msg2[64] = "";
static Screen msgReturn = SCR_MAIN;
static uint32_t msgUntilMs = 0;
static bool msgTimed = false;

// ---------------- Scan list ----------------
static const uint8_t AP_MAX = 20;
struct ApInfo { String ssid; int32_t rssi; wifi_auth_mode_t auth; };
static ApInfo aps[AP_MAX];
static int apCount = 0;
static int apSel = 0;
static int apScroll = 0;
static bool scanStarted = false;

// ---------------- Selected AP ----------------
static String selSsid;
static wifi_auth_mode_t selAuth = WIFI_AUTH_OPEN;

// ---------------- Connect ----------------
static uint32_t connStartMs = 0;
static bool connStarted = false;

// ---------------- Password editor ----------------
static const char* CHARSET =
  " !\"#$%&'()*+,-./"
  "0123456789"
  ":;<=>?@"
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "[\\]^_`"
  "abcdefghijklmnopqrstuvwxyz"
  "{|}~";
static char passBuf[33] = {0};
static uint8_t passLen = 0;
static uint8_t passCur = 0;

// ---------------- Scan animation state ----------------
static uint32_t scanAnimStartMs = 0;
static bool savedFoundInScan = false;

// Scan-complete gating (wait for A)
static bool scanCompleteHold = false;
static int  scanFoundCount = 0;
static Screen scanNextScreen = SCR_LIST;

// ---------------- Forward declarations ----------------
static void drawScrollArrows(bool up, bool down);
static void drawWifiLogoUniversal(uint8_t level);

static void drawCharShiftMarquee(int16_t x, int16_t y, int16_t w, const String& text,
                                 uint16_t msPerChar, uint16_t pauseMs);
static void drawTextAuto(int16_t x, int16_t y, int16_t w, const String& text);
static void drawLabelAndAuto(int16_t xLabel, int16_t y, const char* label,
                             int16_t xText, int16_t wText, const String& text);

static void drawCharShiftMarqueeColors(int16_t x, int16_t y, int16_t w,
                                       const String& text,
                                       uint16_t msPerChar, uint16_t pauseMs,
                                       uint16_t fg, uint16_t bg);

static void tickSplash();
static void renderSplash();

static void startScan();
static void tickScanAnim();
static void renderScanAnim();

static void tickFoundSaved();
static void renderFoundSaved();

static void tickMain();
static void renderMain();

static void tickConnectMenu();
static void renderConnectMenu();

static void tickDisconnectMenu();
static void renderDisconnectMenu();

static void tickSavedPrompt();
static void renderSavedPrompt();

static void tickList();
static void renderList();

static void tickPass();
static void renderPass();

static void startConnect();
static void tickConnect();
static void renderConnect();

static void tickStatus();
static void renderStatus();

static void tickMsg();
static void renderMsg();

// ---------------- Low-level helpers ----------------
static bool initOled() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  for (uint8_t i = 0; i < sizeof(OLED_ADDRS); i++) {
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRS[i])) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setTextWrap(false);
      display.display();
      return true;
    }
  }
  return false;
}

static bool readBtnRaw(int pin) { return digitalRead(pin) == LOW; }
static bool btnPressed(BtnId id) { return btn[id].now && !btn[id].prev; }

static void updateButtons() {
  uint32_t t = millis();
  for (uint8_t i = 0; i < BTN_N; i++) {
    bool raw = readBtnRaw(btn[i].pin);
    if (raw != btn[i].now && (t - btn[i].lastChangeMs) >= DEBOUNCE_MS) {
      btn[i].prev = btn[i].now;
      btn[i].now = raw;
      btn[i].lastChangeMs = t;
    } else {
      btn[i].prev = btn[i].now;
    }
  }
}

static bool anyButtonPressed() {
  return btnPressed(BID_UP) || btnPressed(BID_DOWN) || btnPressed(BID_LEFT) ||
         btnPressed(BID_RIGHT) || btnPressed(BID_A) || btnPressed(BID_B);
}

static void footerBar(const char* text) {
  display.drawFastHLine(0, FOOTER_LINE_Y, 128, SSD1306_WHITE);
  display.setCursor(0, FOOTER_TEXT_Y);
  display.print(text);
}

static bool getTimeString(char out[9]) {
  if (WiFi.status() != WL_CONNECTED || !timeSynced) return false;
  struct tm t;
  if (!getLocalTime(&t, 10)) return false;
  snprintf(out, 9, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  return true;
}

static void headerSmart(const char* title) {
  char ts[9] = {0};
  bool ok = getTimeString(ts);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print(title);

  const char* right = ok ? ts : "OFFLINE";
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(right, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(128 - (int16_t)w, 0);
  display.print(right);

  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
}

static void showMsg(const char* title, const char* l1, const char* l2, uint16_t ms, Screen ret) {
  strncpy(msgTitle, title ? title : "Message", sizeof(msgTitle) - 1);
  msgTitle[sizeof(msgTitle) - 1] = 0;

  strncpy(msg1, l1 ? l1 : "", sizeof(msg1) - 1);
  msg1[sizeof(msg1) - 1] = 0;

  strncpy(msg2, l2 ? l2 : "", sizeof(msg2) - 1);
  msg2[sizeof(msg2) - 1] = 0;

  msgReturn = ret;
  if (ms > 0) { msgTimed = true; msgUntilMs = millis() + ms; }
  else        { msgTimed = false; msgUntilMs = 0; }

  screen = SCR_MSG;
}

static void loadPrefs() {
  prefs.begin("wifibasics", true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();
  haveSaved = savedSsid.length() > 0;
}

static void saveWifiCreds(const String& ssid, const String& pass) {
  prefs.begin("wifibasics", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  savedSsid = ssid;
  savedPass = pass;
  haveSaved = savedSsid.length() > 0;
}

static void forgetWifiCreds() {
  WiFi.disconnect(true, true);
  timeSynced = false;

  prefs.begin("wifibasics", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();

  savedSsid = "";
  savedPass = "";
  haveSaved = false;
}

static bool syncTimeNtp() {
  if (WiFi.status() != WL_CONNECTED) return false;
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  struct tm t;
  for (int i = 0; i < 25; i++) {
    if (getLocalTime(&t, 120)) { timeSynced = true; return true; }
    delay(100);
  }
  return false;
}

static void doDisconnect() {
  WiFi.disconnect(true, true);
  timeSynced = false;
}

// ---------------- UI helpers ----------------
static void drawScrollArrows(bool up, bool down) {
  int16_t x = 123;
  if (up)   display.drawTriangle(x, 12, x + 4, 12, x + 2, 9,  SSD1306_WHITE);
  if (down) display.drawTriangle(x, 58, x + 4, 58, x + 2, 61, SSD1306_WHITE);
}

static void drawWifiLogoUniversal(uint8_t level) {
  const int16_t cx = 64;
  const int16_t cy = 34;
  const int16_t dotR = 4;

  auto drawTopRingArc = [&](int16_t R, int16_t thickness) {
    display.fillCircle(cx, cy, R, SSD1306_WHITE);
    display.fillCircle(cx, cy, R - thickness, SSD1306_BLACK);
    display.fillRect(cx - R - 3, cy, (2 * R) + 7, R + 10, SSD1306_BLACK);
  };

  if (level >= 3) drawTopRingArc(22, 4);
  if (level >= 2) drawTopRingArc(16, 3);
  if (level >= 1) drawTopRingArc(10, 3);

  display.fillCircle(cx, cy, dotR, SSD1306_WHITE);
}

static void drawCharShiftMarquee(int16_t x, int16_t y, int16_t w, const String& text,
                                 uint16_t msPerChar, uint16_t pauseMs) {
  display.fillRect(x, y, w, 8, SSD1306_BLACK);

  int16_t winChars = w / 6;
  if (winChars <= 0) return;

  int16_t len = (int16_t)text.length();
  if (len <= winChars) {
    display.setCursor(x, y);
    display.print(text);
    return;
  }

  int16_t maxStart = len - winChars;
  uint32_t scrollMs = (uint32_t)maxStart * (uint32_t)msPerChar;
  uint32_t cycleMs  = (uint32_t)pauseMs + scrollMs + (uint32_t)pauseMs;

  uint32_t t = millis() % cycleMs;

  int16_t startIdx = 0;
  if (t < pauseMs) startIdx = 0;
  else if (t < pauseMs + scrollMs) startIdx = (int16_t)min((uint32_t)maxStart, (t - pauseMs) / msPerChar);
  else startIdx = maxStart;

  String view = text.substring(startIdx, startIdx + winChars);

  display.setCursor(x, y);
  display.print(view);
}

static void drawTextAuto(int16_t x, int16_t y, int16_t w, const String& text) {
  drawCharShiftMarquee(x, y, w, text, 260, 1000);
}

static void drawLabelAndAuto(int16_t xLabel, int16_t y, const char* label,
                             int16_t xText, int16_t wText, const String& text) {
  display.setCursor(xLabel, y);
  display.print(label);
  drawTextAuto(xText, y, wText, text);
}

static void drawCharShiftMarqueeColors(int16_t x, int16_t y, int16_t w,
                                       const String& text,
                                       uint16_t msPerChar, uint16_t pauseMs,
                                       uint16_t fg, uint16_t bg) {
  int16_t winChars = w / 6;
  if (winChars <= 0) return;

  int16_t len = (int16_t)text.length();
  String view;

  if (len <= winChars) {
    view = text;
  } else {
    int16_t maxStart = len - winChars;
    uint32_t scrollMs = (uint32_t)maxStart * (uint32_t)msPerChar;
    uint32_t cycleMs  = (uint32_t)pauseMs + scrollMs + (uint32_t)pauseMs;

    uint32_t t = millis() % cycleMs;

    int16_t startIdx = 0;
    if (t < pauseMs) startIdx = 0;
    else if (t < pauseMs + scrollMs) startIdx = (int16_t)min((uint32_t)maxStart, (t - pauseMs) / msPerChar);
    else startIdx = maxStart;

    view = text.substring(startIdx, startIdx + winChars);
  }

  if ((int)view.length() < winChars) {
    String padded = view;
    while ((int)padded.length() < winChars) padded += ' ';
    view = padded;
  }

  display.setTextColor(fg, bg);
  display.setCursor(x, y);
  display.print(view);
  display.setTextColor(SSD1306_WHITE);
}

// ---------------- Password editor helpers ----------------
static int charsetIndex(char c) {
  const char* p = strchr(CHARSET, c);
  return p ? (int)(p - CHARSET) : 0;
}
static int charsetLen() { return (int)strlen(CHARSET); }

static void bumpChar(int dir) {
  if (passLen == 0) return;
  char &c = passBuf[passCur];
  int idx = charsetIndex(c);
  int n = charsetLen();
  idx += dir;
  if (idx < 0) idx = n - 1;
  if (idx >= n) idx = 0;
  c = CHARSET[idx];
}
static void appendChar(char c) {
  if (passLen >= 32) return;
  passBuf[passLen++] = c;
  passBuf[passLen] = 0;
  passCur = passLen - 1;
}
static void backspace() {
  if (passLen == 0) return;
  for (uint8_t i = passCur; i + 1 < passLen; i++) passBuf[i] = passBuf[i + 1];
  passLen--;
  passBuf[passLen] = 0;
  if (passCur >= passLen && passLen > 0) passCur = passLen - 1;
  if (passLen == 0) passCur = 0;
}

// ---------------- Screen: Splash ----------------
static void tickSplash() {
  if (splashStart == 0) splashStart = millis();
  if (anyButtonPressed()) { scanStarted = false; screen = SCR_SCAN_ANIM; return; }
  if (millis() - splashStart >= SPLASH_MS) { scanStarted = false; screen = SCR_SCAN_ANIM; return; }
}

static void renderSplash() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(10, 18);
  display.print("WiFi");
  display.setCursor(10, 38);
  display.print("Basics");

  display.setTextSize(1);
  display.display();
}

// ---------------- Scan workflow ----------------
static void startScan() {
  apCount = 0; apSel = 0; apScroll = 0;
  scanStarted = true;
  savedFoundInScan = false;

  scanCompleteHold = false;
  scanFoundCount = 0;
  scanNextScreen = SCR_LIST;

  scanAnimStartMs = millis();

  WiFi.mode(WIFI_STA);
  WiFi.scanDelete();
  WiFi.scanNetworks(true, false);
}

static void tickScanAnim() {
  if (scanCompleteHold) {
    if (btnPressed(BID_A)) screen = scanNextScreen;
    if (btnPressed(BID_B)) screen = SCR_MAIN;
    return;
  }

  if (!scanStarted) startScan();

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    if (btnPressed(BID_B)) {
      WiFi.scanDelete();
      scanStarted = false;
      screen = SCR_MAIN;
    }
    return;
  }

  if (n < 0) {
    scanStarted = false;
    showMsg("Scan", "Scan failed", "Back to main", 1200, SCR_MAIN);
    return;
  }

  scanFoundCount = n;

  apCount = min(n, (int)AP_MAX);
  for (int i = 0; i < apCount; i++) {
    aps[i].ssid = WiFi.SSID(i);
    aps[i].rssi = WiFi.RSSI(i);
    aps[i].auth = WiFi.encryptionType(i);
  }

  WiFi.scanDelete();
  scanStarted = false;

  savedFoundInScan = false;
  if (apCount > 0 && haveSaved) {
    for (int i = 0; i < apCount; i++) {
      if (aps[i].ssid == savedSsid) { savedFoundInScan = true; break; }
    }
  }

  if (apCount == 0) scanNextScreen = SCR_MAIN;
  else              scanNextScreen = savedFoundInScan ? SCR_FOUND_SAVED : SCR_LIST;

  scanCompleteHold = true;
}

static void renderScanAnim() {
  display.clearDisplay();
  headerSmart("WiFi");

  uint32_t step = (millis() - scanAnimStartMs) / 700;
  uint8_t level = (uint8_t)(step % 4);

  drawWifiLogoUniversal(scanCompleteHold ? 3 : level);

  if (!scanCompleteHold) {
    const char* txt = "Scanning...";
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((128 - (int16_t)w) / 2, 44);
    display.print(txt);
    footerBar("B:Cancel");
  } else {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d networks found", scanFoundCount);
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((128 - (int16_t)w) / 2, 44);
    display.print(buf);
    footerBar("A:Continue  B:Back");
  }

  display.display();
}

// ---------------- Screen: Found Saved ----------------
static void tickFoundSaved() {
  if (!haveSaved || !savedFoundInScan) { screen = SCR_LIST; return; }

  if (btnPressed(BID_A)) {
    selSsid = savedSsid;
    selAuth = savedPass.length() ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    memset(passBuf, 0, sizeof(passBuf));
    strncpy(passBuf, savedPass.c_str(), sizeof(passBuf) - 1);
    passLen = (uint8_t)min((size_t)strlen(passBuf), sizeof(passBuf) - 1);
    passCur = passLen ? (passLen - 1) : 0;

    screen = SCR_CONNECT;
  }
  if (btnPressed(BID_B)) screen = SCR_LIST;
}

static void renderFoundSaved() {
  display.clearDisplay();
  headerSmart("Found Saved");

  display.setCursor(0, 18);
  display.print("Saved network:");

  drawTextAuto(0, 32, 128, savedSsid);

  footerBar("A:Connect  B:List");
  display.display();
}

// ---------------- Screen: Main menu ----------------
static void drawMenuRow(uint8_t row, const char* text, bool sel) {
  const int16_t rowH = 13;
  const int16_t y = CONTENT_TOP_Y + row * rowH;

  if (sel) display.fillRoundRect(0, y, 128, rowH, 2, SSD1306_WHITE);
  display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
  display.setCursor(2, y + 2);
  display.print(text);
  display.setTextColor(SSD1306_WHITE);
}

static void tickMain() {
  if (btnPressed(BID_UP))   mainSel = (mainSel == 0) ? 1 : 0;
  if (btnPressed(BID_DOWN)) mainSel = (mainSel == 1) ? 0 : 1;

  if (btnPressed(BID_A)) {
    bool connected = (WiFi.status() == WL_CONNECTED);

    if (mainSel == 0) {
      screen = SCR_STATUS;
    } else {
      if (connected) { discMenuSel = 0; screen = SCR_DISCONNECT_MENU; }
      else           { connMenuSel = 0; screen = SCR_CONNECT_MENU; }
    }
  }
}

static void renderMain() {
  display.clearDisplay();
  headerSmart("Network");

  bool connected = (WiFi.status() == WL_CONNECTED);

  drawMenuRow(0, "WiFi Status",                         mainSel == 0);
  drawMenuRow(1, connected ? "Disconnect" : "Connect",  mainSel == 1);

  footerBar("UP/DN  A:Select");
  display.display();
}

// ---------------- Screen: Connect submenu ----------------
static void tickConnectMenu() {
  if (btnPressed(BID_UP))   connMenuSel = (connMenuSel == 0) ? 1 : 0;
  if (btnPressed(BID_DOWN)) connMenuSel = (connMenuSel == 1) ? 0 : 1;

  if (btnPressed(BID_B)) { screen = SCR_MAIN; return; }

  if (btnPressed(BID_A)) {
    if (connMenuSel == 0) {
      scanStarted = false;
      screen = SCR_SCAN_ANIM;
    } else {
      if (!haveSaved) {
        showMsg("WiFi", "No saved network", "", 1000, SCR_CONNECT_MENU);
        return;
      }
      selSsid = savedSsid;
      selAuth = savedPass.length() ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

      memset(passBuf, 0, sizeof(passBuf));
      strncpy(passBuf, savedPass.c_str(), sizeof(passBuf) - 1);
      passLen = (uint8_t)min((size_t)strlen(passBuf), sizeof(passBuf) - 1);
      passCur = passLen ? (passLen - 1) : 0;

      screen = SCR_CONNECT;
    }
  }
}

static void renderConnectMenu() {
  display.clearDisplay();
  headerSmart("Connect");

  drawMenuRow(0, "Scan", connMenuSel == 0);
  drawMenuRow(1, haveSaved ? "Connect to saved" : "Connect to saved (none)", connMenuSel == 1);

  footerBar("UP/DN  A:Select  B:Back");
  display.display();
}

// ---------------- Screen: Disconnect submenu ----------------
static void tickDisconnectMenu() {
  if (btnPressed(BID_UP))   discMenuSel = (discMenuSel == 0) ? 1 : 0;
  if (btnPressed(BID_DOWN)) discMenuSel = (discMenuSel == 1) ? 0 : 1;

  if (btnPressed(BID_B)) { screen = SCR_MAIN; return; }

  if (btnPressed(BID_A)) {
    if (discMenuSel == 0) {
      doDisconnect();
      showMsg("WiFi", "Disconnected", "", 900, SCR_MAIN);
    } else {
      forgetWifiCreds();
      showMsg("WiFi", "Disconnected", "Saved WiFi forgotten", 1100, SCR_MAIN);
    }
  }
}

static void renderDisconnectMenu() {
  display.clearDisplay();
  headerSmart("Disconnect");

  drawMenuRow(0, "Disconnect",          discMenuSel == 0);
  drawMenuRow(1, "Disconnect & Forget", discMenuSel == 1);

  footerBar("UP/DN  A:Select  B:Back");
  display.display();
}

// ---------------- Screen: Saved prompt (compatibility) ----------------
static void tickSavedPrompt() {
  if (!haveSaved) { scanStarted = false; screen = SCR_SCAN_ANIM; return; }

  if (btnPressed(BID_A)) {
    selSsid = savedSsid;
    selAuth = savedPass.length() ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    memset(passBuf, 0, sizeof(passBuf));
    strncpy(passBuf, savedPass.c_str(), sizeof(passBuf) - 1);
    passLen = (uint8_t)min((size_t)strlen(passBuf), sizeof(passBuf) - 1);
    passCur = passLen ? (passLen - 1) : 0;

    screen = SCR_CONNECT;
  }
  if (btnPressed(BID_B)) { scanStarted = false; screen = SCR_SCAN_ANIM; }
}

static void renderSavedPrompt() {
  display.clearDisplay();
  headerSmart("Connect");

  display.setCursor(0, 16);
  display.print("Saved WiFi found:");
  drawTextAuto(0, 30, 128, savedSsid);

  footerBar("A:Connect  B:Scan");
  display.display();
}

// ---------------- Screen: Networks list ----------------
static void tickList() {
  if (btnPressed(BID_UP))   { if (apSel > 0) apSel--; }
  if (btnPressed(BID_DOWN)) { if (apSel + 1 < apCount) apSel++; }

  const uint8_t VIS = 3;
  if (apSel < apScroll) apScroll = apSel;
  if (apSel >= apScroll + VIS) apScroll = apSel - (VIS - 1);

  if (btnPressed(BID_A) && apCount > 0) {
    selSsid = aps[apSel].ssid;
    selAuth = aps[apSel].auth;

    memset(passBuf, 0, sizeof(passBuf));
    passLen = 0; passCur = 0;

    if (selAuth == WIFI_AUTH_OPEN) {
      screen = SCR_CONNECT;
    } else {
      if (haveSaved && savedSsid == selSsid) {
        strncpy(passBuf, savedPass.c_str(), sizeof(passBuf) - 1);
        passLen = (uint8_t)min((size_t)strlen(passBuf), sizeof(passBuf) - 1);
        passCur = passLen ? (passLen - 1) : 0;
      } else {
        passBuf[0] = 'a';
        passBuf[1] = 0;
        passLen = 1;
        passCur = 0;
      }
      screen = SCR_PASS;
    }
  }

  if (btnPressed(BID_B)) screen = SCR_MAIN;
}

static void renderList() {
  display.clearDisplay();
  headerSmart("Networks");

  const uint8_t VIS = 3;
  const int16_t rowH = 13;
  const int16_t y0 = CONTENT_TOP_Y;

  for (uint8_t r = 0; r < VIS; r++) {
    int idx = apScroll + r;
    if (idx >= apCount) break;

    int16_t y = y0 + r * rowH;
    bool sel = (idx == apSel);

    if (sel) display.fillRoundRect(0, y, 128, rowH, 2, SSD1306_WHITE);

    String s = aps[idx].ssid;
    if (s.length() == 0) s = "<hidden>";

    const int16_t xText = 2;
    const int16_t yText = y + 2;
    const int16_t wText = 106;

    uint16_t fg = sel ? SSD1306_BLACK : SSD1306_WHITE;
    uint16_t bg = sel ? SSD1306_WHITE : SSD1306_BLACK;

    drawCharShiftMarqueeColors(xText, yText, wText, s, 260, 1000, fg, bg);

    display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    display.setCursor(110, y + 2);
    display.print((aps[idx].auth == WIFI_AUTH_OPEN) ? "O" : "L");
    display.setTextColor(SSD1306_WHITE);
  }

  drawScrollArrows(apScroll > 0, (apScroll + VIS) < apCount);
  footerBar("A:Select  B:Back");
  display.display();
}

// ---------------- Screen: Password editor ----------------
static void tickPass() {
  if (btnPressed(BID_LEFT))  { if (passLen) passCur = (passCur ? passCur - 1 : 0); }
  if (btnPressed(BID_RIGHT)) { if (passLen) passCur = (passCur + 1 < passLen ? passCur + 1 : passCur); }

  if (btnPressed(BID_UP))   bumpChar(+1);
  if (btnPressed(BID_DOWN)) bumpChar(-1);

  if (btnPressed(BID_A)) {
    if (passLen < 32) {
      if (passLen == 0) appendChar('a');
      else if (passCur == passLen - 1) appendChar(passBuf[passCur]);
      else passCur++;
    }
  }

  if (btnPressed(BID_B)) {
    if (passLen > 0) backspace();
    if (passLen == 0) { screen = SCR_LIST; return; }
  }

  if (btnPressed(BID_RIGHT)) screen = SCR_CONNECT;
}

static void renderPass() {
  display.clearDisplay();
  headerSmart("Password");

  if (selSsid.length() > 0) {
    drawLabelAndAuto(0, 14, "SSID:", 30, 98, selSsid);
  } else {
    display.setCursor(0, 14);
    display.print("SSID:");
    display.setCursor(30, 14);
    display.print("-");
  }

  display.setCursor(0, 30);
  display.print("PWD:");

  const uint8_t win = 16;
  uint8_t start = 0;
  if (passLen > win && passCur >= win) start = passCur - (win - 1);

  display.setCursor(30, 30);
  for (uint8_t i = start; i < passLen && (i - start) < win; i++) {
    display.print(i == passCur ? passBuf[i] : '*');
  }

  int16_t curX = 30 + (int16_t)(passCur - start) * 6;
  display.drawFastHLine(curX, 40, 5, SSD1306_WHITE);

  footerBar("UP/DN ch  A add  B del  R go");
  display.display();
}

// ---------------- Screen: Connect ----------------
static void startConnect() {
  connStarted = true;
  connStartMs = millis();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(60);

  if (selAuth == WIFI_AUTH_OPEN || passLen == 0) WiFi.begin(selSsid.c_str());
  else                                           WiFi.begin(selSsid.c_str(), passBuf);
}

static void tickConnect() {
  if (!connStarted) startConnect();

  if (btnPressed(BID_B)) {
    doDisconnect();
    connStarted = false;
    screen = SCR_LIST;
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    connStarted = false;

    if (selAuth == WIFI_AUTH_OPEN || passLen == 0) saveWifiCreds(selSsid, "");
    else                                           saveWifiCreds(selSsid, String(passBuf));

    timeSynced = false;
    bool ok = syncTimeNtp();
    showMsg("WiFi", "Connected", ok ? "Time synced" : "NTP failed", 900, SCR_STATUS);
    return;
  }

  if (millis() - connStartMs > 20000) {
    connStarted = false;
    doDisconnect();
    showMsg("WiFi", "Connect failed", "Check pass/signal", 1200, SCR_LIST);
    return;
  }
}

static void renderConnect() {
  display.clearDisplay();
  headerSmart("Connecting");

  if (selSsid.length() > 0) {
    drawLabelAndAuto(0, 18, "SSID:", 30, 98, selSsid);
  } else {
    display.setCursor(0, 18);
    display.print("SSID:");
    display.setCursor(30, 18);
    display.print("-");
  }

  // Animated "Connecting" line: clear a fixed area and redraw each frame.
  const int16_t y = 34;
  display.fillRect(0, y, 118, 10, SSD1306_BLACK); // don't erase the spinner at x=118
  display.setCursor(0, y);
  display.print("Connecting");

  uint8_t dots = (millis() / 450) % 4; // "", ".", "..", "..."
  for (uint8_t i = 0; i < dots; i++) display.print('.');

  display.drawCircle(118, 34, 3, SSD1306_WHITE);

  footerBar("B:Cancel");
  display.display();
}

// ---------------- Screen: Status ----------------
static void tickStatus() {
  if (btnPressed(BID_B)) screen = SCR_MAIN;

  if (btnPressed(BID_A)) {
    if (WiFi.status() == WL_CONNECTED) {
      timeSynced = false;
      syncTimeNtp();
    }
  }
}

static void renderStatus() {
  display.clearDisplay();
  headerSmart("WiFi Status");

  bool conn = (WiFi.status() == WL_CONNECTED);

  display.setCursor(0, 14);
  display.print("WiFi : ");
  display.print(conn ? "Connected" : "Not conn");

  if (conn) {
    drawLabelAndAuto(0, 26, "SSID : ", 42, 86, WiFi.SSID());
  } else {
    display.setCursor(0, 26);
    display.print("SSID : -");
  }

  display.setCursor(0, 38);
  display.print("IP   : ");
  if (conn) display.print(WiFi.localIP());
  else display.print("-");

  footerBar("A:Sync time   B:Back");
  display.display();
}

// ---------------- Screen: Message ----------------
static void tickMsg() {
  if (msgTimed && millis() >= msgUntilMs) { screen = msgReturn; return; }
  if (btnPressed(BID_A) || btnPressed(BID_B)) screen = msgReturn;
}

static void renderMsg() {
  display.clearDisplay();
  headerSmart(msgTitle);

  drawTextAuto(0, 18, 128, String(msg1));
  drawTextAuto(0, 32, 128, String(msg2));

  footerBar("A/B:OK");
  display.display();
}

// ---------------- Arduino setup/loop ----------------
void setup() {
  pinMode(BTN_UP,    INPUT_PULLUP);
  pinMode(BTN_DOWN,  INPUT_PULLUP);
  pinMode(BTN_LEFT,  INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_A,     INPUT_PULLUP);
  pinMode(BTN_B,     INPUT_PULLUP);

  initOled();
  loadPrefs();

  splashStart = millis();
  scanStarted = false;
  scanCompleteHold = false;
  screen = SCR_SPLASH;
}

void loop() {
  uint32_t now = millis();
  if (now - lastFrame < 18) return;
  lastFrame = now;

  updateButtons();

  switch (screen) {
    case SCR_SPLASH:          tickSplash();         renderSplash();         break;
    case SCR_SCAN_ANIM:       tickScanAnim();       renderScanAnim();       break;
    case SCR_FOUND_SAVED:     tickFoundSaved();     renderFoundSaved();     break;
    case SCR_MAIN:            tickMain();           renderMain();           break;
    case SCR_CONNECT_MENU:    tickConnectMenu();    renderConnectMenu();    break;
    case SCR_DISCONNECT_MENU: tickDisconnectMenu(); renderDisconnectMenu(); break;
    case SCR_SAVED_PROMPT:    tickSavedPrompt();    renderSavedPrompt();    break;
    case SCR_LIST:            tickList();           renderList();           break;
    case SCR_PASS:            tickPass();           renderPass();           break;
    case SCR_CONNECT:         tickConnect();        renderConnect();        break;
    case SCR_STATUS:          tickStatus();         renderStatus();         break;
    case SCR_MSG:             tickMsg();            renderMsg();            break;
    default:                  screen = SCR_MAIN;                              break;
  }
}