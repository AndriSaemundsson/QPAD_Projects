// ============================================================
// ESP32-C6 WiFi Tool (OLED + Buttons)
//
// Features
// - Scan WiFi networks (lock/unlock icons + RSSI bars)
// - Connect to WiFi (password editor for secured networks, skip for open)
// - Save/forget WiFi credentials (NVS Preferences)
// - Startup: prompt to reconnect to saved SSID if available
// - Clock (NTP) with UTC offset timezone picker
// - Status: WiFi details + real internet verification via HTTPS (generate_204)
// - WiFi Info: SSID/BSSID (AP MAC)/CH/RSSI + IP/GW/DNS/Subnet + STA MAC
// - Public IP: HTTPS GET to api.ipify.org (plain text public IP)
// - Discovery: mDNS service discovery (shows devices/services advertising mDNS)
//
// UI Layout Rules
// - Header bar at top with separator line
// - Footer bar (line + button hints) when controls are shown
// - List views with header+footer show 3 rows maximum (no overlap)
//
// Notes
// - mDNS Discovery shows only devices advertising mDNS services (not all clients).
// - TLS uses Arduino-ESP32 CA bundle symbols (Arduino-ESP32 3.x).
// ============================================================

#include <stdint.h>
#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include <ESPmDNS.h>

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

// ---------------- Input timing ----------------
#define DEBOUNCE_MS 18
#define HOLD_MS     450

// Auto-repeat (used in password editor)
#define REPEAT_START_MS  320
#define REPEAT_STEP_MS    55

// ---------------- Layout constants ----------------
// Header separator line is drawn at y=10 (see titleBarRightText)
// Footer separator line is drawn at y=53, footer text at y=55
#define CONTENT_TOP_Y   12
#define FOOTER_LINE_Y   53
#define FOOTER_TEXT_Y   55

// ---------------- Buttons ----------------
enum BtnId : uint8_t { BID_UP, BID_DOWN, BID_LEFT, BID_RIGHT, BID_A, BID_B, BTN_N };

struct BtnState {
  int pin;
  bool now;
  bool prev;
  uint32_t lastChangeMs;
  uint32_t downSinceMs;
  bool holdLatched;
};

static BtnState btn[BTN_N] = {
  { BTN_UP,    false, false, 0, 0, false },
  { BTN_DOWN,  false, false, 0, 0, false },
  { BTN_LEFT,  false, false, 0, 0, false },
  { BTN_RIGHT, false, false, 0, 0, false },
  { BTN_A,     false, false, 0, 0, false },
  { BTN_B,     false, false, 0, 0, false },
};

static bool holdPulse[BTN_N] = {0};
static uint32_t repeatNextMs[BTN_N] = {0};

// ---------------- Screens ----------------
typedef uint8_t Screen;
enum : uint8_t {
  SCR_BOOTSCAN     = 0,
  SCR_SAVEDPROMPT  = 1,
  SCR_MENU         = 2,
  SCR_SCAN         = 3,
  SCR_LIST         = 4,
  SCR_PASS         = 5,
  SCR_CONNECT      = 6,
  SCR_CLOCK        = 7,
  SCR_TZ           = 8,
  SCR_STATUS       = 9,
  SCR_FORGET       = 10,
  SCR_ABOUT        = 11,
  SCR_MSG          = 12,
  SCR_WIFIINFO     = 13,
  SCR_PUBLICIP     = 14,
  SCR_DISCOVERY    = 15,
};

static Screen screen = SCR_MENU;

// ---------------- Storage ----------------
Preferences prefs;

// Saved WiFi
static bool haveSaved = false;
static String savedSsid;
static String savedPass;

// ---------------- Time / timezone ----------------
static int8_t tzOffsetHours = 0;     // -12..+14
static bool timeSynced = false;

// ---------------- WiFi scan list ----------------
static const uint8_t AP_MAX = 25;

struct ApInfo {
  String ssid;
  int32_t rssi;
  wifi_auth_mode_t auth;
};

static ApInfo aps[AP_MAX];
static int apCount = 0;
static int apSel = 0;
static int apScroll = 0;

static String selSsid;
static wifi_auth_mode_t selAuth = WIFI_AUTH_OPEN;

// ---------------- Password editor ----------------
static const char* CHARSET =
  " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
  "._-@!#$%&*+?/=:;,";

static char passBuf[33] = {0};
static uint8_t passLen = 0;
static uint8_t passCur = 0;
static bool passShow = true;

// ---------------- Startup saved-SSID scan ----------------
static bool bootScanStarted = false;
static bool savedSeen = false;

// ---------------- Menu ----------------
static const char* menuItems[] = {
  "Scan WiFi",
  "WiFi Info",
  "Public IP",
  "Discovery",
  "Clock",
  "Status",
  "Forget WiFi",
  "About"
};
static const uint8_t MENU_N = sizeof(menuItems) / sizeof(menuItems[0]);
static uint8_t menuSel = 0;
static uint8_t menuScroll = 0;

// ---------------- Scan / connect state ----------------
static bool scanStarted = false;

static uint32_t connStartMs = 0;
static bool connStarted = false;

// ---------------- Status paging + verification ----------------
static uint8_t statusPage = 0;

static uint32_t lastNetCheckMs = 0;
static bool internetOk = false;
static int  internetHttpCode = 0;     // 204 expected
static char internetMsg[20] = "Never checked";

// ---------------- Frame pacing ----------------
static uint32_t lastFrame = 0;

// ---------------- TLS CA bundle symbols (Arduino-ESP32 3.x) ----------------
extern const uint8_t x509_crt_imported_bundle_bin_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_imported_bundle_bin_end[]   asm("_binary_x509_crt_bundle_end");

// ---------------- WiFi disconnect reason (UX hint) ----------------
static volatile bool discReasonValid = false;
static volatile uint8_t discReason = 0;
static uint32_t discReasonAtMs = 0;

// ---------------- Message screen state ----------------
static char msgTitle[18] = "Message";
static char msgLine1[22] = "";
static char msgLine2[22] = "";
static Screen msgReturn = SCR_MENU;
static uint32_t msgUntilMs = 0;
static bool msgHasTimeout = false;

// ---------------- WiFi Info paging ----------------
static uint8_t wifiInfoPage = 0;

// ---------------- Public IP state ----------------
static char publicIp[48] = "Not checked";
static char publicIpMsg[22] = "";
static uint32_t lastPublicIpMs = 0;

// ---------------- mDNS discovery state ----------------
static bool mdnsStarted = false;
static uint8_t discSel = 0;
static uint8_t discScroll = 0;

static const uint8_t DISC_MAX = 12;
struct DiscEntry {
  char name[20];       // short label for OLED
  IPAddress ip;        // IPv4 address (from mDNS result)
  uint16_t port;       // service port
  char type[10];       // short service category label (http, wkst, ...)
};
static DiscEntry disc[DISC_MAX];
static uint8_t discCount = 0;
static char discStatus[22] = "Press A to scan";

// ============================================================
// Forward declarations
// ============================================================

static void gotoScreen(Screen s);

static bool readBtnRaw(int pin);
static bool btnEdgePress(BtnId i);
static bool btnPressed(BtnId id);
static bool btnHeldPulse(BtnId id);
static bool btnRepeat(BtnId id, uint16_t firstDelayMs, uint16_t repeatMs);
static void updateButtons(void);

static bool blinkOn(uint16_t periodMs = 450);
static void drawPulseDot(void);
static void drawSpinner(int16_t x, int16_t y);
static void drawScrollArrows(bool up, bool down);
static void drawPageArrows(bool hasUp, bool hasDown);

static void titleBarRightText(const char* title, const char* rightText);
static void titleBarSmart(const char* title);

static void footerBar(const char* text);

static void showMsg(const char* title, const char* l1, const char* l2, uint16_t ms, Screen returnTo);

static void loadWifiCreds(void);
static void saveWifiCreds(const String& ssid, const String& pass);
static void forgetWifiCreds(void);

static void loadTzOffset(void);
static void saveTzOffset(void);
static void applyTimeZone(void);
static bool syncTimeNtp(void);
static bool getTimeString(char out[9], bool* ok);

static int charsetIndex(char c);
static uint8_t charsetLen(void);
static void editorBumpChar(int dir);
static void editorMoveCursor(int dir);
static void editorAppendChar(char c);
static void editorBackspace(void);

static bool authIsOpen(wifi_auth_mode_t auth);
static bool doInternetCheckHttps(char* outMsg, size_t outMsgLen, int* outHttpCode);

static void startBootScan(void);
static void tickBootScan(void);
static void renderBootScan(void);

static void tickSavedPrompt(void);
static void renderSavedPrompt(void);

static void tickMenu(void);
static void renderMenu(void);

static void startScan(void);
static void tickScan(void);
static void renderScan(void);

static void tickList(void);
static void renderList(void);

static void tickPass(void);
static void renderPass(void);

static void startConnect(void);
static void tickConnect(void);
static void renderConnect(void);

static void tickClock(void);
static void renderClock(void);

static void tickTZ(void);
static void renderTZ(void);

static void tickStatus(void);
static void renderStatus(void);

static void tickForget(void);
static void renderForget(void);

static void tickAbout(void);
static void renderAbout(void);

static void tickMsg(void);
static void renderMsg(void);

static void tickWiFiInfo(void);
static void renderWiFiInfo(void);

static void tickPublicIp(void);
static void renderPublicIp(void);

static void tickDiscovery(void);
static void renderDiscovery(void);

static bool initOled(void);

static const char* reasonHint(uint8_t r);

// Helpers
static void formatMacShort(const uint8_t mac[6], char out[18]);
static bool httpsGetText(const char* url, char* out, size_t outLen, char* err, size_t errLen);
static void ensureMdnsStarted(void);
static void runDiscoveryScan(void);

// ============================================================
// Core helpers
// ============================================================

static void gotoScreen(Screen s) { screen = s; }

// ---------- buttons ----------
static bool readBtnRaw(int pin) { return digitalRead(pin) == LOW; }

static bool btnEdgePress(BtnId i) { return btn[i].now && !btn[i].prev; }
static bool btnPressed(BtnId id)  { return btnEdgePress(id); }

static bool btnHeldPulse(BtnId id) {
  bool v = holdPulse[id];
  holdPulse[id] = false;
  return v;
}

// Auto-repeat while button is held: returns true repeatedly after initial delay.
static bool btnRepeat(BtnId id, uint16_t firstDelayMs, uint16_t repeatMs) {
  uint32_t t = millis();
  if (!btn[id].now) return false;

  if (btnEdgePress(id)) {
    repeatNextMs[id] = t + firstDelayMs;
    return true; // initial press counts as one step
  }

  if (repeatNextMs[id] == 0) repeatNextMs[id] = t + firstDelayMs;

  if (t >= repeatNextMs[id]) {
    repeatNextMs[id] = t + repeatMs;
    return true;
  }
  return false;
}

static void updateButtons(void) {
  uint32_t t = millis();

  for (uint8_t i = 0; i < BTN_N; i++) {
    bool raw = readBtnRaw(btn[i].pin);

    if (raw != btn[i].now) {
      if (t - btn[i].lastChangeMs >= DEBOUNCE_MS) {
        btn[i].prev = btn[i].now;
        btn[i].now = raw;
        btn[i].lastChangeMs = t;

        if (btnEdgePress((BtnId)i)) {
          btn[i].downSinceMs = t;
          btn[i].holdLatched = false;
          holdPulse[i] = false;
          repeatNextMs[i] = 0;
        }

        if (!btn[i].now) {
          btn[i].holdLatched = false;
          holdPulse[i] = false;
          repeatNextMs[i] = 0;
        }
      }
    } else {
      btn[i].prev = btn[i].now;
    }

    if (btn[i].now && !btn[i].holdLatched && (t - btn[i].downSinceMs >= HOLD_MS)) {
      btn[i].holdLatched = true;
      holdPulse[i] = true;
    }
  }
}

// ---------- UI ----------
static bool blinkOn(uint16_t periodMs) {  // default only in declaration
  return ((millis() / periodMs) & 1) == 0;
}

static void drawPulseDot(void) {
  if (blinkOn(520)) display.fillCircle(124, 62, 1, SSD1306_WHITE);
}

static void drawSpinner(int16_t x, int16_t y) {
  uint8_t f = (millis() / 140) & 3;
  display.drawCircle(x, y, 3, SSD1306_WHITE);
  if (f == 0) display.drawLine(x, y, x, y - 3, SSD1306_WHITE);
  if (f == 1) display.drawLine(x, y, x + 3, y, SSD1306_WHITE);
  if (f == 2) display.drawLine(x, y, x, y + 3, SSD1306_WHITE);
  if (f == 3) display.drawLine(x, y, x - 3, y, SSD1306_WHITE);
}

static void drawScrollArrows(bool up, bool down) {
  int16_t x = 123;
  if (up)   display.drawTriangle(x, 12, x + 4, 12, x + 2, 9,  SSD1306_WHITE);
  if (down) display.drawTriangle(x, 58, x + 4, 58, x + 2, 61, SSD1306_WHITE);
}

static void drawPageArrows(bool hasUp, bool hasDown) {
  int16_t x = 122;
  if (hasUp)   display.drawTriangle(x, 14, x + 5, 14, x + 2, 11, SSD1306_WHITE);
  if (hasDown) display.drawTriangle(x, 54, x + 5, 54, x + 2, 57, SSD1306_WHITE);
}

static void titleBarRightText(const char* title, const char* rightText) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print(title);

  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(rightText, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(128 - (int16_t)w, 0);
  display.print(rightText);

  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
}

static void titleBarSmart(const char* title) {
  bool ok = false;
  char ts[9];
  getTimeString(ts, &ok);

  if (WiFi.status() != WL_CONNECTED || !ok) titleBarRightText(title, "Not online");
  else                                     titleBarRightText(title, ts);
}

// Footer bar reserves the bottom area for controls.
// This prevents content from colliding with button hints.
static void footerBar(const char* text) {
  display.drawFastHLine(0, FOOTER_LINE_Y, 128, SSD1306_WHITE);
  display.setCursor(0, FOOTER_TEXT_Y);
  display.print(text);
}

// ---------- message screen ----------
static void showMsg(const char* title, const char* l1, const char* l2, uint16_t ms, Screen returnTo) {
  strncpy(msgTitle, title ? title : "Message", sizeof(msgTitle) - 1);
  msgTitle[sizeof(msgTitle) - 1] = 0;

  strncpy(msgLine1, l1 ? l1 : "", sizeof(msgLine1) - 1);
  msgLine1[sizeof(msgLine1) - 1] = 0;

  strncpy(msgLine2, l2 ? l2 : "", sizeof(msgLine2) - 1);
  msgLine2[sizeof(msgLine2) - 1] = 0;

  msgReturn = returnTo;

  if (ms > 0) {
    msgHasTimeout = true;
    msgUntilMs = millis() + ms;
  } else {
    msgHasTimeout = false;
    msgUntilMs = 0;
  }

  gotoScreen(SCR_MSG);
}

// ============================================================
// Preferences: WiFi and TZ
// ============================================================

static void loadWifiCreds(void) {
  prefs.begin("wifiscn", true);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();
  haveSaved = (savedSsid.length() > 0);
}

static void saveWifiCreds(const String& ssid, const String& pass) {
  prefs.begin("wifiscn", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  savedSsid = ssid;
  savedPass = pass;
  haveSaved = (savedSsid.length() > 0);
}

static void forgetWifiCreds(void) {
  prefs.begin("wifiscn", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
  savedSsid = "";
  savedPass = "";
  haveSaved = false;
}

static void loadTzOffset(void) {
  prefs.begin("wifiscn", true);
  tzOffsetHours = (int8_t)prefs.getChar("tz", 0);
  prefs.end();
  if (tzOffsetHours < -12) tzOffsetHours = -12;
  if (tzOffsetHours > 14)  tzOffsetHours = 14;
}

static void saveTzOffset(void) {
  prefs.begin("wifiscn", false);
  prefs.putChar("tz", (char)tzOffsetHours);
  prefs.end();
}

static void applyTimeZone(void) {
  // POSIX TZ strings invert the sign: UTC+2 is "UTC-2"
  char tz[16];
  int8_t posix = (int8_t)(-tzOffsetHours);
  snprintf(tz, sizeof(tz), "UTC%+d", (int)posix);
  setenv("TZ", tz, 1);
  tzset();
}

static bool syncTimeNtp(void) {
  if (WiFi.status() != WL_CONNECTED) return false;

  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  struct tm t;
  for (int i = 0; i < 30; i++) {
    if (getLocalTime(&t, 100)) {
      timeSynced = true;
      return true;
    }
    delay(100);
  }
  return false;
}

static bool getTimeString(char out[9], bool* ok) {
  struct tm t;

  if (WiFi.status() != WL_CONNECTED || !timeSynced) {
    strcpy(out, "00:00:00");
    if (ok) *ok = false;
    return false;
  }

  if (!getLocalTime(&t, 10)) {
    strcpy(out, "00:00:00");
    if (ok) *ok = false;
    return false;
  }

  snprintf(out, 9, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  if (ok) *ok = true;
  return true;
}

// ============================================================
// Password editor helpers
// ============================================================

static int charsetIndex(char c) {
  const char* p = strchr(CHARSET, c);
  if (!p) return 0;
  return (int)(p - CHARSET);
}

static uint8_t charsetLen(void) { return (uint8_t)strlen(CHARSET); }

static void editorBumpChar(int dir) {
  if (passLen == 0) return;
  char &c = passBuf[passCur];
  int idx = charsetIndex(c);
  int n = (int)charsetLen();
  idx += dir;
  if (idx < 0) idx = n - 1;
  if (idx >= n) idx = 0;
  c = CHARSET[idx];
}

static void editorMoveCursor(int dir) {
  if (passLen == 0) { passCur = 0; return; }
  int v = (int)passCur + dir;
  if (v < 0) v = 0;
  if (v >= (int)passLen) v = passLen - 1;
  passCur = (uint8_t)v;
}

static void editorAppendChar(char c) {
  if (passLen >= 32) return;
  passBuf[passLen++] = c;
  passBuf[passLen] = 0;
  passCur = passLen - 1;
}

static void editorBackspace(void) {
  if (passLen == 0) return;
  for (uint8_t i = passCur; i + 1 < passLen; i++) passBuf[i] = passBuf[i + 1];
  passLen--;
  passBuf[passLen] = 0;
  if (passCur >= passLen && passLen > 0) passCur = passLen - 1;
  if (passLen == 0) passCur = 0;
}

// ============================================================
// WiFi helpers
// ============================================================

static bool authIsOpen(wifi_auth_mode_t auth) {
  return (auth == WIFI_AUTH_OPEN);
}

static const char* reasonHint(uint8_t r) {
  // Reason codes can vary across IDF versions; these are common values.
  switch (r) {
    case 2:   return "Auth fail";
    case 15:  return "4-way timeout";
    case 201: return "No AP found";
    case 202: return "Assoc fail";
    case 203: return "Handshake fail";
    default:  return "Link lost";
  }
}

// ============================================================
// Helpers (MAC formatting, HTTPS GET, mDNS)
// ============================================================

static void formatMacShort(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// HTTPS GET that returns a short text body (for ipify public IP).
static bool httpsGetText(const char* url, char* out, size_t outLen, char* err, size_t errLen) {
  if (WiFi.status() != WL_CONNECTED) {
    strncpy(err, "No WiFi", errLen);
    err[errLen - 1] = 0;
    return false;
  }

  WiFiClientSecure client;
  client.setTimeout(6000);

  const size_t bundleLen = (size_t)(x509_crt_imported_bundle_bin_end - x509_crt_imported_bundle_bin_start);
  if (bundleLen > 0) {
    client.setCACertBundle(x509_crt_imported_bundle_bin_start, bundleLen);
  }

  HTTPClient https;
  https.setTimeout(6000);

  if (!https.begin(client, url)) {
    strncpy(err, "HTTPS begin fail", errLen);
    err[errLen - 1] = 0;
    https.end();
    return false;
  }

  int code = https.GET();
  if (code <= 0) {
    strncpy(err, "HTTPS error", errLen);
    err[errLen - 1] = 0;
    https.end();
    return false;
  }

  if (code != 200) {
    snprintf(err, errLen, "HTTP %d", code);
    https.end();
    return false;
  }

  String body = https.getString();
  https.end();

  body.trim();
  if (body.length() == 0) {
    strncpy(err, "Empty reply", errLen);
    err[errLen - 1] = 0;
    return false;
  }

  strncpy(out, body.c_str(), outLen);
  out[outLen - 1] = 0;
  err[0] = 0;
  return true;
}

// Start mDNS responder (best-effort). Required before queries.
static void ensureMdnsStarted(void) {
  if (mdnsStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  const char* host = "esp32-wifitool";
  if (MDNS.begin(host)) {
    mdnsStarted = true;
    // Optional: advertise workstation service so other tools can see the ESP32
    MDNS.enableWorkstation();
  }
}

// Scan for a few common mDNS services. This returns *advertisers*, not all devices.
static void runDiscoveryScan(void) {
  discCount = 0;
  discSel = 0;
  discScroll = 0;
  strncpy(discStatus, "Scanning...", sizeof(discStatus) - 1);
  discStatus[sizeof(discStatus) - 1] = 0;

  if (WiFi.status() != WL_CONNECTED) {
    strncpy(discStatus, "Not connected", sizeof(discStatus) - 1);
    discStatus[sizeof(discStatus) - 1] = 0;
    return;
  }

  ensureMdnsStarted();
  if (!mdnsStarted) {
    strncpy(discStatus, "mDNS start fail", sizeof(discStatus) - 1);
    discStatus[sizeof(discStatus) - 1] = 0;
    return;
  }

  struct Query { const char* type; const char* proto; const char* shortName; };
  const Query q[] = {
    { "_http",        "_tcp", "http"   },
    { "_workstation", "_tcp", "wkst"   },
    { "_ipp",         "_tcp", "print"  },
    { "_airplay",     "_tcp", "airply" },
    { "_googlecast",  "_tcp", "cast"   },
  };

  for (uint8_t qi = 0; qi < (sizeof(q)/sizeof(q[0])); qi++) {
    int n = MDNS.queryService(q[qi].type, q[qi].proto);
    if (n <= 0) continue;

    for (int i = 0; i < n && discCount < DISC_MAX; i++) {
      // Arduino-ESP32 ESPmDNS provides instanceName(), hostname(), address(), port().
      String nm = MDNS.instanceName(i);
      if (nm.length() == 0) nm = MDNS.hostname(i);
      nm.trim();
      if (nm.length() == 0) nm = "device";

      nm.replace(".local", "");
      if (nm.length() > 18) nm = nm.substring(0, 18);

      DiscEntry &e = disc[discCount++];
      memset(&e, 0, sizeof(e));
      strncpy(e.name, nm.c_str(), sizeof(e.name) - 1);

      e.ip = MDNS.address(i);
      e.port = (uint16_t)MDNS.port(i);
      strncpy(e.type, q[qi].shortName, sizeof(e.type) - 1);
    }
  }

  if (discCount == 0) {
    strncpy(discStatus, "No mDNS devices", sizeof(discStatus) - 1);
  } else {
    snprintf(discStatus, sizeof(discStatus), "%u found (mDNS)", (unsigned)discCount);
  }
  discStatus[sizeof(discStatus) - 1] = 0;
}

// ============================================================
// Internet verification (DNS + HTTPS GET)
// ============================================================

static bool doInternetCheckHttps(char* outMsg, size_t outMsgLen, int* outHttpCode) {
  if (WiFi.status() != WL_CONNECTED) {
    strncpy(outMsg, "WiFi not conn", outMsgLen);
    if (outHttpCode) *outHttpCode = 0;
    return false;
  }

  IPAddress ip;
  if (!WiFi.hostByName("connectivitycheck.gstatic.com", ip)) {
    strncpy(outMsg, "DNS fail", outMsgLen);
    if (outHttpCode) *outHttpCode = 0;
    return false;
  }

  const char* url = "https://connectivitycheck.gstatic.com/generate_204";

  WiFiClientSecure client;
  client.setTimeout(6000);

  const size_t bundleLen = (size_t)(x509_crt_imported_bundle_bin_end - x509_crt_imported_bundle_bin_start);
  if (bundleLen > 0) {
    client.setCACertBundle(x509_crt_imported_bundle_bin_start, bundleLen);
  }

  HTTPClient https;
  https.setTimeout(6000);

  if (!https.begin(client, url)) {
    strncpy(outMsg, "HTTPS begin fail", outMsgLen);
    if (outHttpCode) *outHttpCode = 0;
    https.end();
    return false;
  }

  int code = https.GET();
  if (outHttpCode) *outHttpCode = code;

  if (code == 204) {
    strncpy(outMsg, "PASS (204)", outMsgLen);
    https.end();
    return true;
  }

  // Captive portal hints
  if (code >= 300 && code < 400) {
    strncpy(outMsg, "Redirect (portal?)", outMsgLen);
    https.end();
    return false;
  }
  if (code == 200) {
    strncpy(outMsg, "200 (portal?)", outMsgLen);
    https.end();
    return false;
  }

  if (code > 0) {
    snprintf(outMsg, outMsgLen, "HTTP %d", code);
    https.end();
    return false;
  }

  strncpy(outMsg, "HTTPS error", outMsgLen);
  https.end();
  return false;
}

// ============================================================
// Screens: Startup scan / reconnect prompt
// ============================================================

static void startBootScan(void) {
  bootScanStarted = true;
  savedSeen = false;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);

  WiFi.scanDelete();
  WiFi.scanNetworks(true, false);
}

static void tickBootScan(void) {
  if (!haveSaved) { gotoScreen(SCR_MENU); return; }
  if (!bootScanStarted) startBootScan();

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;

  if (n < 0) {
    WiFi.scanDelete();
    bootScanStarted = false;
    showMsg("Startup", "Scan failed", "Going to menu", 1200, SCR_MENU);
    return;
  }

  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == savedSsid) { savedSeen = true; break; }
  }

  WiFi.scanDelete();
  bootScanStarted = false;

  gotoScreen(savedSeen ? SCR_SAVEDPROMPT : SCR_MENU);
}

static void renderBootScan(void) {
  display.clearDisplay();
  titleBarSmart("Startup");

  display.setCursor(0, 18);
  display.print("Checking saved WiFi");
  display.setCursor(0, 30);
  display.print("Scanning...");
  drawSpinner(118, 30);

  footerBar("Please wait");
  drawPulseDot();
  display.display();
}

static void tickSavedPrompt(void) {
  if (btnPressed(BID_A)) {
    selSsid = savedSsid;
    selAuth = WIFI_AUTH_WPA2_PSK;

    memset(passBuf, 0, sizeof(passBuf));
    strncpy(passBuf, savedPass.c_str(), sizeof(passBuf) - 1);
    passLen = (uint8_t)min((size_t)strlen(passBuf), sizeof(passBuf) - 1);
    passCur = passLen ? (passLen - 1) : 0;
    passShow = true;

    gotoScreen(SCR_CONNECT);
  } else if (btnPressed(BID_B)) {
    gotoScreen(SCR_MENU);
  }
}

static void renderSavedPrompt(void) {
  display.clearDisplay();
  titleBarSmart("Reconnect?");

  display.setCursor(0, 16);
  display.print("Reconnect to:");
  display.setCursor(0, 28);
  String s = savedSsid;
  if (s.length() > 20) s = s.substring(0, 20);
  display.print(s);

  footerBar("A:Yes   B:No");
  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: Menu
// ============================================================

static void tickMenu(void) {
  if (btnPressed(BID_UP))   menuSel = (menuSel == 0) ? (MENU_N - 1) : (menuSel - 1);
  if (btnPressed(BID_DOWN)) menuSel = (menuSel + 1) % MENU_N;

  const uint8_t VIS = 5;
  if (menuSel < menuScroll) menuScroll = menuSel;
  if (menuSel >= menuScroll + VIS) menuScroll = menuSel - (VIS - 1);

  if (btnPressed(BID_A)) {
    switch (menuSel) {
      case 0: gotoScreen(SCR_SCAN);       break;
      case 1: gotoScreen(SCR_WIFIINFO);   break;
      case 2: gotoScreen(SCR_PUBLICIP);   break;
      case 3: gotoScreen(SCR_DISCOVERY);  break;
      case 4: gotoScreen(SCR_CLOCK);      break;
      case 5: gotoScreen(SCR_STATUS);     break;
      case 6: gotoScreen(SCR_FORGET);     break;
      case 7: gotoScreen(SCR_ABOUT);      break;
    }
  }
}

static void renderMenu(void) {
  display.clearDisplay();
  titleBarSmart("WiFi Tool");

  const uint8_t VIS = 5;
  const int16_t rowH = 10;
  const int16_t y0 = CONTENT_TOP_Y;
  bool showArrow = blinkOn(350);

  for (uint8_t r = 0; r < VIS; r++) {
    uint8_t idx = menuScroll + r;
    if (idx >= MENU_N) break;

    int16_t y = y0 + r * rowH;
    bool sel = (idx == menuSel);

    if (sel) display.fillRoundRect(0, y, 128, rowH, 2, SSD1306_WHITE);
    display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);

    display.setCursor(12, y + 1);
    display.print(menuItems[idx]);

    if (sel && showArrow) {
      display.setCursor(2, y + 1);
      display.print(">");
    }
    display.setTextColor(SSD1306_WHITE);
  }

  drawScrollArrows(menuScroll > 0, (menuScroll + VIS) < MENU_N);
  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: Scan
// ============================================================

static void startScan(void) {
  apCount = 0; apSel = 0; apScroll = 0;
  scanStarted = true;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);

  WiFi.scanDelete();
  WiFi.scanNetworks(true, false);
}

static void tickScan(void) {
  if (!scanStarted) startScan();

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    if (btnPressed(BID_B)) {
      WiFi.scanDelete();
      scanStarted = false;
      showMsg("Scan", "Cancelled", "Back to menu", 900, SCR_MENU);
    }
    return;
  }

  if (n < 0) {
    if (btnPressed(BID_A) || btnPressed(BID_B)) {
      scanStarted = false;
      showMsg("Scan", "Scan failed", "Try again", 1200, SCR_MENU);
    }
    return;
  }

  apCount = min(n, (int)AP_MAX);
  for (int i = 0; i < apCount; i++) {
    aps[i].ssid = WiFi.SSID(i);
    aps[i].rssi = WiFi.RSSI(i);
    aps[i].auth = WiFi.encryptionType(i);
  }

  WiFi.scanDelete();
  scanStarted = false;

  if (apCount == 0) {
    showMsg("Scan", "No networks", "A=Rescan B=Menu", 0, SCR_MENU);
  } else {
    gotoScreen(SCR_LIST);
  }
}

static void renderScan(void) {
  display.clearDisplay();
  titleBarSmart("Scanning");

  display.setCursor(0, 18);
  display.print("Searching networks");
  display.setCursor(0, 30);
  display.print("Please wait...");
  drawSpinner(118, 30);

  footerBar("B:Cancel");
  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: List (lock/unlock icons + RSSI bars)
// ============================================================

static void drawLockIcon(int16_t x, int16_t y) {
  display.drawRoundRect(x, y + 2, 6, 5, 1, SSD1306_WHITE);
  display.drawCircle(x + 3, y + 2, 2, SSD1306_WHITE);
  display.drawPixel(x + 3, y + 4, SSD1306_WHITE);
}

static void drawUnlockIcon(int16_t x, int16_t y) {
  display.drawCircle(x + 3, y + 2, 2, SSD1306_WHITE);
  display.drawRoundRect(x, y + 2, 6, 5, 1, SSD1306_WHITE);
  display.drawPixel(x + 2, y + 1, SSD1306_BLACK);
}

static void drawRssiBars(int16_t x, int16_t y, int rssi) {
  uint8_t bars = 0;
  if      (rssi >= -55) bars = 4;
  else if (rssi >= -67) bars = 3;
  else if (rssi >= -75) bars = 2;
  else if (rssi >= -85) bars = 1;
  else bars = 0;

  for (uint8_t i = 0; i < 4; i++) {
    int h = (i + 1) * 2;
    if (i < bars) display.fillRect(x + i * 3, y + (8 - h), 2, h, SSD1306_WHITE);
    else          display.drawRect(x + i * 3, y + (8 - h), 2, h, SSD1306_WHITE);
  }
}

static void tickList(void) {
  if (btnPressed(BID_UP))   { if (apSel > 0) apSel--; }
  if (btnPressed(BID_DOWN)) { if (apSel + 1 < apCount) apSel++; }

  const uint8_t VIS = 3; // header + footer present => 3 rows maximum
  if (apSel < apScroll) apScroll = apSel;
  if (apSel >= apScroll + VIS) apScroll = apSel - (VIS - 1);

  if (btnPressed(BID_A) && apCount > 0) {
    selSsid = aps[apSel].ssid;
    selAuth = aps[apSel].auth;

    memset(passBuf, 0, sizeof(passBuf));

    if (authIsOpen(selAuth)) {
      passLen = 0; passCur = 0; passShow = true;
      showMsg("WiFi", "Connecting (open)", "Please wait...", 800, SCR_CONNECT);
    } else {
      if (haveSaved && savedSsid == selSsid) {
        strncpy(passBuf, savedPass.c_str(), sizeof(passBuf) - 1);
        passLen = (uint8_t)min((size_t)strlen(passBuf), sizeof(passBuf) - 1);
      } else {
        passBuf[0] = 'a';
        passBuf[1] = 0;
        passLen = 1;
      }
      passCur = passLen ? (passLen - 1) : 0;
      passShow = true;
      gotoScreen(SCR_PASS);
    }
  }

  if (btnPressed(BID_B)) gotoScreen(SCR_MENU);
  if (btnHeldPulse(BID_A)) gotoScreen(SCR_SCAN); // hold A = rescan
}

static void renderList(void) {
  display.clearDisplay();
  titleBarSmart("WiFi Networks");

  if (apCount <= 0) {
    display.setCursor(0, 18);
    display.print("No networks found");
    footerBar("A:Rescan  B:Back");
    drawPulseDot();
    display.display();
    return;
  }

  const uint8_t VIS = 3;
  const int16_t rowH = 12;
  const int16_t y0 = 14;  // inside content area

  for (int row = 0; row < VIS; row++) {
    int idx = apScroll + row;
    if (idx >= apCount) break;

    int16_t y = y0 + row * rowH;
    bool sel = (idx == apSel);

    if (sel) display.fillRoundRect(0, y, 128, rowH, 2, SSD1306_WHITE);
    display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);

    String s = aps[idx].ssid;
    if (s.length() == 0) s = "<hidden>";
    if (s.length() > 14) s = s.substring(0, 14);

    display.setCursor(2, y + 2);
    display.print(s);

    bool open = authIsOpen(aps[idx].auth);
    if (open) drawUnlockIcon(92, y + 2);
    else      drawLockIcon(92, y + 2);

    drawRssiBars(110, y + 2, aps[idx].rssi);

    display.setTextColor(SSD1306_WHITE);
  }

  drawScrollArrows(apScroll > 0, (apScroll + VIS) < apCount);
  footerBar("A:Select B:Back holdA:scan");

  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: Password
// ============================================================

static void tickPass(void) {
  if (btnHeldPulse(BID_A)) passShow = !passShow;

  if (btnPressed(BID_LEFT))  editorMoveCursor(-1);
  if (btnPressed(BID_RIGHT)) editorMoveCursor(+1);

  if (btnRepeat(BID_UP, REPEAT_START_MS, REPEAT_STEP_MS))   editorBumpChar(+1);
  if (btnRepeat(BID_DOWN, REPEAT_START_MS, REPEAT_STEP_MS)) editorBumpChar(-1);

  // Tap A: move cursor forward or append a new character (seeded from current char).
  if (btnPressed(BID_A)) {
    if (passLen < 32) {
      if (passLen == 0) {
        editorAppendChar('a');
      } else if (passCur == passLen - 1) {
        char seed = passBuf[passCur];
        editorAppendChar(seed);
      } else {
        editorMoveCursor(+1);
      }
    }
  }

  // Tap B: backspace; Hold B: back to list.
  if (btnHeldPulse(BID_B)) { gotoScreen(SCR_LIST); return; }
  if (btnPressed(BID_B)) {
    if (passLen > 0) editorBackspace();
    if (passLen == 0) { gotoScreen(SCR_LIST); return; }
  }

  // Hold RIGHT: proceed to connect.
  if (btnHeldPulse(BID_RIGHT)) {
    showMsg("WiFi", "Connecting...", "Please wait", 650, SCR_CONNECT);
  }
}

static void renderPass(void) {
  display.clearDisplay();
  titleBarSmart("Password");

  display.setCursor(0, 14);
  display.print("SSID:");
  String s = selSsid;
  if (s.length() > 20) s = s.substring(0, 20);
  display.setCursor(30, 14);
  display.print(s);

  display.setCursor(0, 26);
  display.print("Mode:");
  display.setCursor(30, 26);
  display.print(passShow ? "Visible" : "Hidden");
  display.setCursor(84, 26);
  display.print("holdA");

  display.setCursor(0, 38);
  display.print("PWD:");

  const uint8_t win = 16;
  uint8_t start = 0;
  if (passLen > win && passCur >= win) start = passCur - (win - 1);

  display.setCursor(30, 38);
  for (uint8_t i = start; i < passLen && (i - start) < win; i++) {
    if (passShow) display.print(passBuf[i]);
    else {
      if (i == passCur) display.print(passBuf[i]);
      else display.print('*');
    }
  }

  int16_t curX = 30 + (int16_t)(passCur - start) * 6;
  if (blinkOn(350) && passLen > 0 && (passCur >= start) && (passCur < start + win)) {
    display.drawFastHLine(curX, 48, 5, SSD1306_WHITE);
  }

  footerBar("UP/DN edit  B del  holdR go");
  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: Connect
// ============================================================

static void startConnect(void) {
  connStarted = true;

  // Reset disconnect hint capture
  discReasonValid = false;
  discReason = 0;
  discReasonAtMs = 0;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(60);

  connStartMs = millis();

  if (passLen == 0) WiFi.begin(selSsid.c_str());
  else             WiFi.begin(selSsid.c_str(), passBuf);
}

static void tickConnect(void) {
  if (!connStarted) startConnect();

  // Cancel connect attempt
  if (btnPressed(BID_B)) {
    WiFi.disconnect(true, true);
    connStarted = false;
    showMsg("WiFi", "Cancelled", "Back to menu", 900, SCR_MENU);
    return;
  }

  // Successful connection
  if (WiFi.status() == WL_CONNECTED) {
    connStarted = false;

    if (!authIsOpen(selAuth)) saveWifiCreds(selSsid, String(passBuf));
    else                      saveWifiCreds(selSsid, "");

    applyTimeZone();
    syncTimeNtp();

    // Start mDNS responder after connect (best-effort)
    mdnsStarted = false;
    ensureMdnsStarted();

    lastNetCheckMs = 0;
    showMsg("WiFi", "Connected!", "Checking internet", 900, SCR_STATUS);
    return;
  }

  // Best-effort early failure hint
  uint32_t now = millis();
  if (discReasonValid && (now - connStartMs) > 900) {
    connStarted = false;
    WiFi.disconnect(true, true);

    const char* hint = reasonHint(discReason);
    if (!strcmp(hint, "Auth fail") || !strcmp(hint, "4-way timeout") || !strcmp(hint, "Handshake fail")) {
      showMsg("WiFi", "Connection failed", "Check password", 2300, SCR_LIST);
    } else if (!strcmp(hint, "No AP found")) {
      showMsg("WiFi", "Network not found", "Move closer/rescan", 2300, SCR_LIST);
    } else {
      showMsg("WiFi", "Connection failed", "Check signal", 2300, SCR_LIST);
    }
    return;
  }

  // Timeout
  if (now - connStartMs > 20000) {
    connStarted = false;
    WiFi.disconnect(true, true);
    showMsg("WiFi", "Connection failed", "Check pass/signal", 2600, SCR_LIST);
    return;
  }
}

static void renderConnect(void) {
  display.clearDisplay();
  titleBarSmart("Connecting");

  uint32_t elapsed = millis() - connStartMs;
  uint8_t secsLeft = 0;
  if (elapsed < 20000) secsLeft = (uint8_t)((20000 - elapsed + 999) / 1000);

  display.setCursor(0, 16);
  display.print("SSID:");
  String s = selSsid;
  if (s.length() > 20) s = s.substring(0, 20);
  display.setCursor(30, 16);
  display.print(s);

  display.setCursor(0, 30);
  display.print("Trying...");
  drawSpinner(118, 30);

  display.setCursor(0, 42);
  display.print("Timeout ");
  display.print(secsLeft);
  display.print("s");

  footerBar("B:Cancel");
  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: Clock + Time Zone
// ============================================================

static void tickClock(void) {
  if (btnPressed(BID_B)) { gotoScreen(SCR_MENU); return; }

  // Tap A: sync NTP (requires WiFi)
  if (btnPressed(BID_A)) {
    if (WiFi.status() != WL_CONNECTED) {
      showMsg("Clock", "Not online", "Connect WiFi first", 1700, SCR_CLOCK);
      return;
    }
    applyTimeZone();
    bool ok = syncTimeNtp();
    if (ok) showMsg("Clock", "Time updated", "From NTP", 1200, SCR_CLOCK);
    else    showMsg("Clock", "NTP failed", "Try again later", 1700, SCR_CLOCK);
    return;
  }

  // Tap Right: timezone picker
  if (btnPressed(BID_RIGHT)) gotoScreen(SCR_TZ);
}

static void renderClock(void) {
  display.clearDisplay();
  titleBarSmart("Clock");

  bool ok = false;
  char ts[9];
  getTimeString(ts, &ok);

  display.setTextSize(2);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(ts, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - (int16_t)w) / 2, 24);
  display.print(ok ? ts : "OFFLINE");
  display.setTextSize(1);

  footerBar("A:Sync  R:TZ  B:Back");
  drawPulseDot();
  display.display();
}

static void tickTZ(void) {
  if (btnPressed(BID_UP) || btnHeldPulse(BID_UP))     { if (tzOffsetHours < 14) tzOffsetHours++; }
  if (btnPressed(BID_DOWN) || btnHeldPulse(BID_DOWN)) { if (tzOffsetHours > -12) tzOffsetHours--; }

  if (btnPressed(BID_A)) {
    saveTzOffset();
    applyTimeZone();
    showMsg("Time Zone", "Saved", "Returning", 900, SCR_CLOCK);
    return;
  }
  if (btnPressed(BID_B)) gotoScreen(SCR_CLOCK);
}

static void renderTZ(void) {
  display.clearDisplay();
  titleBarSmart("Time Zone");

  display.setCursor(0, 16);
  display.print("UTC Offset:");

  display.setTextSize(2);
  display.setCursor(20, 30);
  if (tzOffsetHours >= 0) display.print("+");
  display.print((int)tzOffsetHours);
  display.setTextSize(1);

  footerBar("UP/DN change  A:Save B:Back");
  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: Status
// ============================================================

static void tickStatus(void) {
  if (btnPressed(BID_B)) gotoScreen(SCR_MENU);
  if (btnPressed(BID_UP) || btnPressed(BID_DOWN)) statusPage ^= 1;

  uint32_t now = millis();
  if (lastNetCheckMs == 0 || (now - lastNetCheckMs) > 15000) {
    lastNetCheckMs = now;

    int code = 0;
    bool ok = doInternetCheckHttps(internetMsg, sizeof(internetMsg), &code);
    internetOk = ok;
    internetHttpCode = code;

    if (internetOk) {
      applyTimeZone();
      syncTimeNtp();
    }
  }

  if (btnPressed(BID_A)) {
    lastNetCheckMs = 0;
    showMsg("Status", "Checking...", "Please wait", 700, SCR_STATUS);
  }
}

static void renderStatus(void) {
  display.clearDisplay();
  titleBarSmart("Status");

  bool connected = (WiFi.status() == WL_CONNECTED);
  drawPageArrows(true, true);

  if (statusPage == 0) {
    display.setCursor(0, 14);
    display.print("WiFi : ");
    display.print(connected ? "Connected" : "Not connected");

    display.setCursor(0, 26);
    display.print("SSID : ");
    String s = WiFi.SSID();
    if (s.length() == 0) s = "-";
    if (s.length() > 16) s = s.substring(0, 16);
    display.print(s);

    display.setCursor(0, 38);
    display.print("RSSI : ");
    if (connected) { display.print(WiFi.RSSI()); display.print(" dBm"); }
    else display.print("-");

    display.setCursor(0, 50);
    display.print("NET  : ");
    if (!connected) display.print("No WiFi");
    else display.print(internetOk ? "OK" : "FAIL");
  } else {
    display.setCursor(0, 14);
    display.print("IP   : ");
    if (connected) display.print(WiFi.localIP());
    else display.print("-");

    display.setCursor(0, 26);
    display.print("GW   : ");
    if (connected) display.print(WiFi.gatewayIP());
    else display.print("-");

    display.setCursor(0, 38);
    display.print("DNS  : ");
    if (connected) display.print(WiFi.dnsIP());
    else display.print("-");

    display.setCursor(0, 50);
    display.print("CHK  : ");
    display.print(internetMsg);
  }

  footerBar("A:Refresh  B:Back");
  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: WiFi Info
// ============================================================

static void tickWiFiInfo(void) {
  if (btnPressed(BID_B)) { gotoScreen(SCR_MENU); return; }
  if (btnPressed(BID_UP) || btnPressed(BID_DOWN)) wifiInfoPage ^= 1;
}

static void renderWiFiInfo(void) {
  display.clearDisplay();
  titleBarSmart("WiFi Info");

  bool connected = (WiFi.status() == WL_CONNECTED);
  drawPageArrows(true, true);

  if (!connected) {
    display.setCursor(0, 18);
    display.print("Not connected");
    display.setCursor(0, 32);
    display.print("Connect first");
    footerBar("B:Back");
    drawPulseDot();
    display.display();
    return;
  }

  if (wifiInfoPage == 0) {
    uint8_t apMac[6] = {0};
    WiFi.BSSID(apMac);
    char bssid[18];
    formatMacShort(apMac, bssid);

    uint8_t staMac[6] = {0};
    WiFi.macAddress(staMac);
    char stamac[18];
    formatMacShort(staMac, stamac);

    display.setCursor(0, 14);
    display.print("SSID:");
    String s = WiFi.SSID();
    if (s.length() > 20) s = s.substring(0, 20);
    display.setCursor(30, 14);
    display.print(s);

    display.setCursor(0, 26);
    display.print("BSSID:");
    display.setCursor(36, 26);
    display.print(bssid);

    display.setCursor(0, 38);
    display.print("CH:");
    display.print(WiFi.channel());
    display.print(" RSSI:");
    display.print(WiFi.RSSI());

    display.setCursor(0, 50);
    display.print("MAC:");
    display.setCursor(24, 50);
    display.print(stamac);
  } else {
    display.setCursor(0, 14);
    display.print("IP  : ");
    display.print(WiFi.localIP());

    display.setCursor(0, 26);
    display.print("GW  : ");
    display.print(WiFi.gatewayIP());

    display.setCursor(0, 38);
    display.print("DNS : ");
    display.print(WiFi.dnsIP());

    display.setCursor(0, 50);
    display.print("MASK: ");
    display.print(WiFi.subnetMask());
  }

  footerBar("UP/DN page  B:Back");
  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: Public IP
// ============================================================

static void tickPublicIp(void) {
  if (btnPressed(BID_B)) { gotoScreen(SCR_MENU); return; }

  if (btnPressed(BID_A)) {
    if (WiFi.status() != WL_CONNECTED) {
      showMsg("Public IP", "Not online", "Connect WiFi first", 1600, SCR_PUBLICIP);
      return;
    }

    showMsg("Public IP", "Fetching...", "Please wait", 650, SCR_PUBLICIP);

    char err[24] = {0};
    char out[48] = {0};

    bool ok = httpsGetText("https://api.ipify.org", out, sizeof(out), err, sizeof(err));
    if (ok) {
      strncpy(publicIp, out, sizeof(publicIp) - 1);
      publicIp[sizeof(publicIp) - 1] = 0;
      publicIpMsg[0] = 0;
      lastPublicIpMs = millis();
    } else {
      strncpy(publicIpMsg, err, sizeof(publicIpMsg) - 1);
      publicIpMsg[sizeof(publicIpMsg) - 1] = 0;
    }
  }
}

static void renderPublicIp(void) {
  display.clearDisplay();
  titleBarSmart("Public IP");

  bool connected = (WiFi.status() == WL_CONNECTED);

  display.setCursor(0, 16);
  display.print("Public:");
  display.setCursor(0, 26);
  if (!connected) display.print("Not connected");
  else display.print(publicIp);

  display.setCursor(0, 40);
  display.print("Local : ");
  if (connected) display.print(WiFi.localIP());
  else display.print("-");

  display.setCursor(0, 50);
  if (publicIpMsg[0]) display.print(publicIpMsg);
  else if (!lastPublicIpMs) display.print("Press A to fetch");

  footerBar("A:Fetch  B:Back");
  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: Discovery (mDNS advertisers)
// ============================================================

static void tickDiscovery(void) {
  if (btnPressed(BID_B)) { gotoScreen(SCR_MENU); return; }

  // Press A triggers a new scan and refreshes the mDNS results list.
  if (btnPressed(BID_A)) {
    showMsg("Discovery", "Scanning mDNS...", "Please wait", 650, SCR_DISCOVERY);
    runDiscoveryScan();
  }

  if (discCount > 0) {
    if (btnPressed(BID_UP))   { if (discSel > 0) discSel--; }
    if (btnPressed(BID_DOWN)) { if (discSel + 1 < discCount) discSel++; }

    const uint8_t VIS = 3; // header + footer present => 3 rows maximum
    if (discSel < discScroll) discScroll = discSel;
    if (discSel >= discScroll + VIS) discScroll = discSel - (VIS - 1);
  }
}

static void renderDiscovery(void) {
  display.clearDisplay();
  titleBarSmart("Discovery");

  if (WiFi.status() != WL_CONNECTED) {
    display.setCursor(0, 18);
    display.print("Not connected");
    display.setCursor(0, 32);
    display.print("Connect WiFi first");
    footerBar("B:Back");
    drawPulseDot();
    display.display();
    return;
  }

  display.setCursor(0, 14);
  display.print(discStatus);

  if (discCount == 0) {
    display.setCursor(0, 30);
    display.print("Shows mDNS only");
    display.setCursor(0, 40);
    display.print("Not all devices");
    footerBar("A:Scan  B:Back");
    drawPulseDot();
    display.display();
    return;
  }

  const uint8_t VIS = 3;
  const int16_t rowH = 12;
  const int16_t y0 = 24;

  for (uint8_t r = 0; r < VIS; r++) {
    uint8_t idx = discScroll + r;
    if (idx >= discCount) break;

    int16_t y = y0 + r * rowH;
    bool sel = (idx == discSel);

    if (sel) display.fillRoundRect(0, y, 128, rowH, 2, SSD1306_WHITE);
    display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);

    // Line format: "<name>  <lastOctet>:<port>  <type>"
    DiscEntry &e = disc[idx];
    display.setCursor(2, y + 2);
    display.print(e.name);

    display.setCursor(76, y + 2);
    display.print(e.ip[3]);
    display.print(":");
    display.print(e.port);

    display.setCursor(112, y + 2);
    display.print(e.type);

    display.setTextColor(SSD1306_WHITE);
  }

  drawScrollArrows(discScroll > 0, (discScroll + VIS) < discCount);
  footerBar("A:Scan  B:Back");
  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: Forget
// ============================================================

static void tickForget(void) {
  if (btnPressed(BID_B)) { gotoScreen(SCR_MENU); return; }

  if (!haveSaved) {
    if (btnPressed(BID_A)) gotoScreen(SCR_MENU);
    return;
  }

  if (btnPressed(BID_A)) {
    forgetWifiCreds();
    WiFi.disconnect(true, true);
    showMsg("Forget", "Saved WiFi cleared", "Back to menu", 1300, SCR_MENU);
    return;
  }
}

static void renderForget(void) {
  display.clearDisplay();
  titleBarSmart("Forget WiFi");

  if (!haveSaved) {
    display.setCursor(0, 18);
    display.print("No saved WiFi");
    footerBar("A/B:Back");
    drawPulseDot();
    display.display();
    return;
  }

  display.setCursor(0, 16);
  display.print("Forget saved WiFi?");
  display.setCursor(0, 28);
  String s = savedSsid;
  if (s.length() > 20) s = s.substring(0, 20);
  display.print(s);

  footerBar("A:Yes   B:No");
  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: About
// ============================================================

static void tickAbout(void) {
  if (btnPressed(BID_B) || btnPressed(BID_A)) gotoScreen(SCR_MENU);
}

static void renderAbout(void) {
  display.clearDisplay();
  titleBarSmart("About");

  display.setCursor(0, 16);
  display.print("ESP32-C6 WiFi Tool");
  display.setCursor(0, 28);
  display.print("Public IP + mDNS");
  display.setCursor(0, 40);
  display.print("Scan / Connect / NTP");

  footerBar("A/B:Back");
  drawPulseDot();
  display.display();
}

// ============================================================
// Screens: Message
// ============================================================

static void tickMsg(void) {
  if (msgHasTimeout && millis() >= msgUntilMs) {
    gotoScreen(msgReturn);
    return;
  }
  if (btnPressed(BID_A) || btnPressed(BID_B)) {
    gotoScreen(msgReturn);
    return;
  }
}

static void renderMsg(void) {
  display.clearDisplay();
  titleBarSmart(msgTitle);

  display.setCursor(0, 18);
  display.print(msgLine1);

  display.setCursor(0, 32);
  display.print(msgLine2);

  footerBar("A/B:Dismiss");
  drawPulseDot();
  display.display();
}

// ============================================================
// OLED init
// ============================================================

static bool initOled(void) {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  for (uint8_t i = 0; i < sizeof(OLED_ADDRS); i++) {
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRS[i])) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.print("OLED OK @0x");
      display.print(OLED_ADDRS[i], HEX);
      display.display();
      delay(350);
      return true;
    }
  }
  return false;
}

// ============================================================
// WiFi event handler (disconnect reason capture)
// ============================================================

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    discReason = info.wifi_sta_disconnected.reason;
    discReasonValid = true;
    discReasonAtMs = millis();
  }
}

// ============================================================
// Arduino setup/loop
// ============================================================

void setup() {
  pinMode(BTN_UP,    INPUT_PULLUP);
  pinMode(BTN_DOWN,  INPUT_PULLUP);
  pinMode(BTN_LEFT,  INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_A,     INPUT_PULLUP);
  pinMode(BTN_B,     INPUT_PULLUP);

  initOled();

  loadWifiCreds();
  loadTzOffset();
  applyTimeZone();

  WiFi.onEvent(onWiFiEvent);

  if (haveSaved) gotoScreen(SCR_BOOTSCAN);
  else           gotoScreen(SCR_MENU);
}

void loop() {
  uint32_t now = millis();
  if (now - lastFrame < 18) return;
  lastFrame = now;

  updateButtons();

  switch (screen) {
    case SCR_BOOTSCAN:    tickBootScan();    renderBootScan();    break;
    case SCR_SAVEDPROMPT: tickSavedPrompt(); renderSavedPrompt(); break;
    case SCR_MENU:        tickMenu();        renderMenu();        break;
    case SCR_SCAN:        tickScan();        renderScan();        break;
    case SCR_LIST:        tickList();        renderList();        break;
    case SCR_PASS:        tickPass();        renderPass();        break;
    case SCR_CONNECT:     tickConnect();     renderConnect();     break;
    case SCR_CLOCK:       tickClock();       renderClock();       break;
    case SCR_TZ:          tickTZ();          renderTZ();          break;
    case SCR_STATUS:      tickStatus();      renderStatus();      break;
    case SCR_FORGET:      tickForget();      renderForget();      break;
    case SCR_ABOUT:       tickAbout();       renderAbout();       break;
    case SCR_MSG:         tickMsg();         renderMsg();         break;
    case SCR_WIFIINFO:    tickWiFiInfo();    renderWiFiInfo();    break;
    case SCR_PUBLICIP:    tickPublicIp();    renderPublicIp();    break;
    case SCR_DISCOVERY:   tickDiscovery();   renderDiscovery();   break;
    default:              gotoScreen(SCR_MENU);                   break;
  }
}
