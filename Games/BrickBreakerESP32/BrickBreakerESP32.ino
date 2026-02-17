// ============================================================
// IMPORTANT (Arduino auto-prototype fix):
// Define custom types BEFORE Arduino injects prototypes.
// ============================================================
#include <stdint.h>

// ---------- Transitions ----------
typedef uint8_t TransitionType;
enum : uint8_t { TR_NONE=0, TR_WIPE_LEFT=1, TR_WIPE_RIGHT=2, TR_SHUTTER=3 };

// ---------- Power-ups ----------
typedef uint8_t PowerKind;
enum : uint8_t {
  P_EXPAND=0,
  P_SHRINK=1,
  P_SLOW=2,
  P_FAST=3,
  P_STICKY=4,
  P_INVERT=5,
  P_LIFE=6,
  P_POINTS=7,
  P_TRIPLE=8,
  P_NAIL=9,
  P_SHIELD=10,
  P_KIND_N=11
};

// ---------- Screen state ----------
typedef uint8_t Screen;
enum : uint8_t {
  SCR_SPLASH=0,
  SCR_MENU=1,
  SCR_PLAY=2,
  SCR_SETTINGS=3,
  SCR_HISCORES=4,
  SCR_POWERUPS=5,
  SCR_ABOUT=6,
  SCR_GAMEOVER=7,
  SCR_NAMEENTRY=8,
  SCR_SET_POWERS=9,
  SCR_PRESPLASH=10
};

// ---------- Ball type ----------
struct Ball {
  bool active;
  float x, y;
  float px, py;
  float vx, vy;
  bool stuck;
  bool nail;

  // Watchdog (anti-stuck)
  float lastGoodX, lastGoodY;
  uint16_t staleX, staleY;

  // Ceiling ride
  uint16_t ceilingRide;
};

// ---------- Settings (must exist before instance) ----------
struct Settings {
  uint8_t difficulty; // 0 easy,1 normal,2 hard
  uint8_t ballTrail;  // 0/1
  uint8_t invertCtl;  // 0/1 (global toggle)
  uint8_t maxLives;   // 1..5
  uint16_t powMask;   // bitmask of enabled powerups (P_KIND_N bits)
};

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <math.h>

// ============================================================
// Brick Breaker — XIAO ESP32-C6
// Display: 128x64 SSD1306 I2C addr 0x3C (SDA=D3, SCL=D4)
// Buttons (active-LOW, internal pullups):
//   UP=D0 DOWN=D8 LEFT=D1 RIGHT=D7 A=D10 B=D9
// ============================================================

// ---------- Pins ----------
#define BTN_UP     D0
#define BTN_DOWN   D8
#define BTN_LEFT   D1
#define BTN_RIGHT  D7
#define BTN_A      D10
#define BTN_B      D9

// ---------- OLED ----------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define I2C_SDA D3
#define I2C_SCL D4
static const uint8_t OLED_ADDRS[] = { 0x3C, 0x3D };
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------- HUD + playfield bounds (inside 1px border walls) ----------
static const int16_t SCORE_H = 8;

// 1px border walls: x=0 and x=127
static const int16_t PLAY_L = 1;
static const int16_t PLAY_R = 126;
static const int16_t PLAY_T = SCORE_H;   // playfield starts under HUD
static const int16_t PLAY_B = 63;

// ============================================================
// INPUT (robust debounce + edge + hold)
// ============================================================

#define DEBOUNCE_MS 18
#define HOLD_MS     450

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

static inline bool readBtnRaw(int pin) { return digitalRead(pin) == LOW; } // active-LOW
static inline bool edgePress(BtnId i)  { return btn[i].now && !btn[i].prev; }

static void updateButtons() {
  uint32_t t = millis();
  for (uint8_t i = 0; i < BTN_N; i++) {
    bool raw = readBtnRaw(btn[i].pin);

    if (raw != btn[i].now) {
      if (t - btn[i].lastChangeMs >= DEBOUNCE_MS) {
        btn[i].prev = btn[i].now;
        btn[i].now = raw;
        btn[i].lastChangeMs = t;
        if (edgePress((BtnId)i)) {
          btn[i].downSinceMs = t;
          btn[i].holdLatched = false;
          holdPulse[i] = false;
        }
      }
    } else {
      btn[i].prev = btn[i].now;
    }

    if (!btn[i].now) {
      btn[i].holdLatched = false;
      holdPulse[i] = false;
    }
    if (btn[i].now && !btn[i].holdLatched && (t - btn[i].downSinceMs >= HOLD_MS)) {
      btn[i].holdLatched = true;
      holdPulse[i] = true; // one-shot
    }
  }
}

static inline bool btnPressed(BtnId id) { return edgePress(id); }
static inline bool btnHeldPulse(BtnId id) { bool v = holdPulse[id]; holdPulse[id] = false; return v; }

// ============================================================
// UI helpers
// ============================================================

static inline bool blinkOn(uint16_t periodMs=450) {
  return ((millis() / periodMs) & 1) == 0;
}

// subtle “alive” pulse dot for any waiting screen
static void drawPulseDot() {
  if (blinkOn(520)) display.fillCircle(124, 62, 1, SSD1306_WHITE);
}

static void drawScrollArrows(bool up, bool down) {
  int16_t x = 123;
  if (up)   display.drawTriangle(x, 12, x+4, 12, x+2, 9,  SSD1306_WHITE);  // ▲
  if (down) display.drawTriangle(x, 58, x+4, 58, x+2, 61, SSD1306_WHITE);  // ▼
}

static void titleBar(const char* title) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(title);
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
}

// ============================================================
// Hearts (lives) — 1px gap
// ============================================================

static uint8_t gameLives = 3;
static Settings settings;

static void drawHeart7(int16_t x, int16_t y, bool filled) {
  if (filled) {
    display.fillCircle(x+1, y+1, 2, SSD1306_WHITE);
    display.fillCircle(x+5, y+1, 2, SSD1306_WHITE);
    display.fillTriangle(x, y+2, x+6, y+2, x+3, y+5, SSD1306_WHITE);
  } else {
    display.drawCircle(x+1, y+1, 2, SSD1306_WHITE);
    display.drawCircle(x+5, y+1, 2, SSD1306_WHITE);
    display.drawTriangle(x, y+2, x+6, y+2, x+3, y+5, SSD1306_WHITE);
  }
}

static void drawLivesHearts() {
  uint8_t maxL = settings.maxLives;
  if (maxL < 1) maxL = 1;
  if (maxL > 5) maxL = 5;

  const int16_t HEART_W = 7;
  const int16_t GAP = 1;
  const int16_t STEP = HEART_W + GAP; // 8px spacing
  int16_t totalW = (int16_t)(maxL * STEP);

  int16_t x0 = 128 - totalW;
  int16_t y0 = 0;

  for (uint8_t i = 0; i < maxL; i++) {
    bool filled = (i < gameLives);
    drawHeart7(x0 + i * STEP, y0 + 1, filled);
  }
}

// ============================================================
// Power-up icons (redesigned: more literal)
// 7x7 glyphs, white-only; pills handle contrast.
// ============================================================

static void iconPaddle(int16_t x, int16_t y, int w) {
  int16_t sx = x + (7 - w) / 2;
  display.drawFastHLine(sx, y+6, w, SSD1306_WHITE);
  display.drawPixel(sx,     y+5, SSD1306_WHITE);
  display.drawPixel(sx+w-1, y+5, SSD1306_WHITE);
}

static void iconExpand(int16_t x, int16_t y) {
  iconPaddle(x,y,7);
  display.drawPixel(x+0, y+2, SSD1306_WHITE);
  display.drawPixel(x+1, y+1, SSD1306_WHITE);
  display.drawPixel(x+1, y+3, SSD1306_WHITE);
  display.drawPixel(x+6, y+2, SSD1306_WHITE);
  display.drawPixel(x+5, y+1, SSD1306_WHITE);
  display.drawPixel(x+5, y+3, SSD1306_WHITE);
}

static void iconShrink(int16_t x, int16_t y) {
  iconPaddle(x,y,3);
  display.drawPixel(x+2, y+2, SSD1306_WHITE);
  display.drawPixel(x+1, y+1, SSD1306_WHITE);
  display.drawPixel(x+1, y+3, SSD1306_WHITE);
  display.drawPixel(x+4, y+2, SSD1306_WHITE);
  display.drawPixel(x+5, y+1, SSD1306_WHITE);
  display.drawPixel(x+5, y+3, SSD1306_WHITE);
}

static void iconSlow(int16_t x, int16_t y) {
  display.drawCircle(x+3, y+3, 3, SSD1306_WHITE);
  display.drawPixel(x+3, y+3, SSD1306_WHITE);
  display.drawLine(x+3, y+3, x+3, y+1, SSD1306_WHITE);
  display.drawLine(x+3, y+3, x+5, y+4, SSD1306_WHITE);
}

static void iconFast(int16_t x, int16_t y) {
  display.drawLine(x+4, y+0, x+2, y+3, SSD1306_WHITE);
  display.drawLine(x+2, y+3, x+4, y+3, SSD1306_WHITE);
  display.drawLine(x+4, y+3, x+2, y+6, SSD1306_WHITE);
  display.drawPixel(x+5, y+1, SSD1306_WHITE);
}

static void iconSticky(int16_t x, int16_t y) {
  iconPaddle(x,y,5);
  display.drawPixel(x+3, y+1, SSD1306_WHITE);
  display.drawPixel(x+2, y+2, SSD1306_WHITE);
  display.drawPixel(x+3, y+2, SSD1306_WHITE);
  display.drawPixel(x+4, y+2, SSD1306_WHITE);
  display.drawPixel(x+3, y+3, SSD1306_WHITE);
  display.drawPixel(x+3, y+4, SSD1306_WHITE);
}

static void iconInvert(int16_t x, int16_t y) {
  display.drawFastHLine(x+1, y+2, 5, SSD1306_WHITE);
  display.drawPixel(x+5, y+1, SSD1306_WHITE);
  display.drawPixel(x+5, y+3, SSD1306_WHITE);
  display.drawFastHLine(x+1, y+4, 5, SSD1306_WHITE);
  display.drawPixel(x+1, y+3, SSD1306_WHITE);
  display.drawPixel(x+1, y+5, SSD1306_WHITE);
}

static void iconLife(int16_t x, int16_t y) { drawHeart7(x, y+1, true); }

static void iconPoints(int16_t x, int16_t y) {
  display.drawPixel(x+3, y+0, SSD1306_WHITE);
  display.drawPixel(x+3, y+6, SSD1306_WHITE);
  display.drawPixel(x+0, y+3, SSD1306_WHITE);
  display.drawPixel(x+6, y+3, SSD1306_WHITE);
  display.drawPixel(x+1, y+1, SSD1306_WHITE);
  display.drawPixel(x+5, y+1, SSD1306_WHITE);
  display.drawPixel(x+1, y+5, SSD1306_WHITE);
  display.drawPixel(x+5, y+5, SSD1306_WHITE);
  display.drawPixel(x+3, y+3, SSD1306_WHITE);
}

static void iconTriple(int16_t x, int16_t y) {
  display.drawCircle(x+2, y+2, 1, SSD1306_WHITE);
  display.drawCircle(x+5, y+2, 1, SSD1306_WHITE);
  display.drawCircle(x+3, y+5, 1, SSD1306_WHITE);
}

static void iconNail(int16_t x, int16_t y) {
  display.drawLine(x+3, y+0, x+3, y+6, SSD1306_WHITE);
  display.drawLine(x+2, y+1, x+3, y+0, SSD1306_WHITE);
  display.drawLine(x+4, y+1, x+3, y+0, SSD1306_WHITE);
  display.drawPixel(x+2, y+5, SSD1306_WHITE);
  display.drawPixel(x+4, y+5, SSD1306_WHITE);
}

static void iconShield(int16_t x, int16_t y) {
  display.drawFastHLine(x+2, y+1, 3, SSD1306_WHITE);
  display.drawLine(x+2, y+1, x+1, y+3, SSD1306_WHITE);
  display.drawLine(x+4, y+1, x+5, y+3, SSD1306_WHITE);
  display.drawLine(x+1, y+3, x+3, y+6, SSD1306_WHITE);
  display.drawLine(x+5, y+3, x+3, y+6, SSD1306_WHITE);
  display.drawPixel(x+3, y+3, SSD1306_WHITE);
}

static void drawPowerIcon(PowerKind k, int16_t x, int16_t y) {
  switch (k) {
    case P_EXPAND:  iconExpand(x,y);  break;
    case P_SHRINK:  iconShrink(x,y);  break;
    case P_SLOW:    iconSlow(x,y);    break;
    case P_FAST:    iconFast(x,y);    break;
    case P_STICKY:  iconSticky(x,y);  break;
    case P_INVERT:  iconInvert(x,y);  break;
    case P_LIFE:    iconLife(x,y);    break;
    case P_POINTS:  iconPoints(x,y);  break;
    case P_TRIPLE:  iconTriple(x,y);  break;
    case P_NAIL:    iconNail(x,y);    break;
    case P_SHIELD:  iconShield(x,y);  break;
    default: display.fillCircle(x+3, y+3, 1, SSD1306_WHITE); break;
  }
}

static void drawIconPill(int16_t x, int16_t y, int16_t w, int16_t h, bool filled, PowerKind kind) {
  if (filled) display.fillRoundRect(x, y, w, h, 3, SSD1306_WHITE);
  else        display.drawRoundRect(x, y, w, h, 3, SSD1306_WHITE);

  int16_t ix = x + (w - 7) / 2;
  int16_t iy = y + (h - 7) / 2;

  if (filled) display.fillRect(ix, iy, 7, 7, SSD1306_BLACK);
  drawPowerIcon(kind, ix, iy);
}

// ============================================================
// Transitions (wipe + shutter)
// ============================================================

static TransitionType trType = TR_NONE;
static uint8_t trProg = 0;
static bool trActive = false;

static void startTransition(TransitionType t) {
  trType = t;
  trProg = 0;
  trActive = (t != TR_NONE);
}

static void applyTransitionMask() {
  if (!trActive) return;

  trProg = (uint8_t)(trProg + 16);
  if (trProg >= 128) {
    trActive = false;
    trType = TR_NONE;
    return;
  }

  if (trType == TR_WIPE_LEFT) {
    display.fillRect(trProg, 0, 128 - trProg, 64, SSD1306_BLACK);
  } else if (trType == TR_WIPE_RIGHT) {
    display.fillRect(0, 0, 128 - trProg, 64, SSD1306_BLACK);
  } else if (trType == TR_SHUTTER) {
    uint8_t h = trProg / 4;
    if (h > 32) h = 32;
    display.fillRect(0, 0, 128, 32 - h, SSD1306_BLACK);
    display.fillRect(0, 32 + h, 128, 32 - h, SSD1306_BLACK);
  }
}

// ============================================================
// Persistence (settings + highscores)
// ============================================================

Preferences prefs;

static inline uint16_t powBit(PowerKind k) { return (uint16_t)(1u << (uint8_t)k); }
static inline bool powEnabled(PowerKind k) { return (settings.powMask & powBit(k)) != 0; }
static inline void powSet(PowerKind k, bool en) {
  if (en) settings.powMask |= powBit(k);
  else    settings.powMask &= (uint16_t)~powBit(k);
}

static void loadSettings() {
  prefs.begin("qpad", true);
  settings.difficulty = prefs.getUChar("diff", 1);
  settings.ballTrail  = prefs.getUChar("trail", 1);
  settings.invertCtl  = prefs.getUChar("inv", 0);
  settings.maxLives   = prefs.getUChar("lives", 3);
  settings.powMask    = (uint16_t)prefs.getUShort("pmask", 0);
  prefs.end();

  if (settings.difficulty > 2) settings.difficulty = 1;
  settings.ballTrail = settings.ballTrail ? 1 : 0;
  settings.invertCtl = settings.invertCtl ? 1 : 0;
  if (settings.maxLives < 1) settings.maxLives = 3;
  if (settings.maxLives > 5) settings.maxLives = 5;

  if (settings.powMask == 0 || (settings.powMask & ((1u << P_KIND_N) - 1u)) == 0) {
    settings.powMask = (uint16_t)((1u << P_KIND_N) - 1u);
  }
}

static void saveSettings() {
  prefs.begin("qpad", false);
  prefs.putUChar("diff", settings.difficulty);
  prefs.putUChar("trail", settings.ballTrail);
  prefs.putUChar("inv", settings.invertCtl);
  prefs.putUChar("lives", settings.maxLives);
  prefs.putUShort("pmask", settings.powMask);
  prefs.end();
}

struct ScoreEntry { char name[4]; int32_t score; };
static const uint8_t HS_MAX = 10;
static ScoreEntry hs[HS_MAX];

static void loadHighScores() {
  prefs.begin("qpad", true);
  for (uint8_t i = 0; i < HS_MAX; i++) {
    char keyN[8], keyS[8];
    snprintf(keyN, sizeof(keyN), "n%u", i);
    snprintf(keyS, sizeof(keyS), "s%u", i);
    String n = prefs.getString(keyN, "---");
    int32_t s = prefs.getInt(keyS, 0);
    hs[i].name[0] = (n.length() > 0) ? n[0] : '-';
    hs[i].name[1] = (n.length() > 1) ? n[1] : '-';
    hs[i].name[2] = (n.length() > 2) ? n[2] : '-';
    hs[i].name[3] = '\0';
    hs[i].score = s;
  }
  prefs.end();
}

static void saveHighScores() {
  prefs.begin("qpad", false);
  for (uint8_t i = 0; i < HS_MAX; i++) {
    char keyN[8], keyS[8];
    snprintf(keyN, sizeof(keyN), "n%u", i);
    snprintf(keyS, sizeof(keyS), "s%u", i);
    prefs.putString(keyN, String(hs[i].name));
    prefs.putInt(keyS, hs[i].score);
  }
  prefs.end();
}

static bool isHighScore(int32_t score) { return score > hs[HS_MAX - 1].score; }

static void insertHighScore(const char* name3, int32_t score) {
  ScoreEntry e; strncpy(e.name, name3, 3); e.name[3] = '\0'; e.score = score;
  for (uint8_t i = 0; i < HS_MAX; i++) {
    if (score > hs[i].score) {
      for (int j = HS_MAX - 1; j > (int)i; j--) hs[j] = hs[j - 1];
      hs[i] = e;
      saveHighScores();
      return;
    }
  }
}

// ============================================================
// Game data
// ============================================================

static const char* powerName(PowerKind k) {
  switch (k) {
    case P_EXPAND: return "Expand";
    case P_SHRINK: return "Shrink";
    case P_SLOW:   return "Slow";
    case P_FAST:   return "Fast";
    case P_STICKY: return "Sticky";
    case P_INVERT: return "Invert";
    case P_LIFE:   return "Life";
    case P_POINTS: return "Points";
    case P_TRIPLE: return "Triple";
    case P_NAIL:   return "Nail";
    case P_SHIELD: return "Shield";
    default:       return "?";
  }
}

static uint32_t rngState = 0xA5A5C3D1;
static uint32_t xorshift32() {
  uint32_t x = rngState;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  rngState = x;
  return x;
}
static uint8_t rnd(uint8_t n) { return (uint8_t)(xorshift32() % n); }

struct FallingPower { bool active; PowerKind kind; int16_t x, y; int16_t vy; };
static const uint8_t POW_MAX = 7;
static FallingPower drops[POW_MAX];

struct Effects {
  int32_t untilExpand, untilShrink, untilSlow, untilFast, untilSticky, untilInvert, untilNail, untilShield;
} fx;

static inline bool effectOn(int32_t untilMs) { return (untilMs > (int32_t)millis()); }

struct Paddle { int16_t x, y, w, speed; } paddle;

static const uint8_t BALL_MAX = 3;
static Ball balls[BALL_MAX];

static int32_t gameScore = 0;
static uint16_t level = 1;
static bool inPlay = false;
static bool paused = false;

// ---------- Bricks ----------
struct Brick {
  uint8_t hp;   // 0 dead, 1..n hits remaining, 255 = unbreakable
  uint8_t type; // 0 normal, 1 power, 2 bad, 3 metal, 4 unbreakable
};

static const uint8_t BRW = 10;
static const uint8_t BRH = 5;
static Brick bricks[BRW][BRH];
static uint8_t bricksH = 4;
static uint8_t aliveBricks = 0; // breakable only

static const int16_t BR_X0 = 4;
static const int16_t BR_Y0 = PLAY_T + 4;
static const int16_t BR_W  = 12;
static const int16_t BR_H  = 7;

static const uint8_t LEVELS = 8;
static const uint8_t levelH[LEVELS] = { 4,4,5,5,5,5,5,5 };

static const uint8_t levelMap[LEVELS][BRH][BRW] = {
  {
    {1,1,1,1,1,1,1,1,1,1},
    {1,2,1,4,1,1,4,1,2,1},
    {1,1,1,1,5,5,1,1,1,1},
    {1,3,1,1,1,1,1,1,3,1},
    {0,0,0,0,0,0,0,0,0,0}
  },
  {
    {5,1,1,1,6,6,1,1,1,5},
    {5,2,1,1,4,4,1,1,2,5},
    {5,1,3,1,1,1,1,3,1,5},
    {5,1,1,1,1,1,1,1,1,5},
    {0,0,0,0,0,0,0,0,0,0}
  },
  {
    {1,6,1,6,1,6,1,6,1,6},
    {6,1,6,1,6,1,6,1,6,1},
    {1,3,1,2,1,2,1,3,1,2},
    {4,1,4,1,4,1,4,1,4,1},
    {0,5,0,5,0,5,0,5,0,5}
  },
  {
    {5,5,5,5,5,5,5,5,5,5},
    {1,6,1,1,2,2,1,1,6,1},
    {1,6,1,3,1,1,3,1,6,1},
    {1,6,1,1,4,4,1,1,6,1},
    {1,1,1,1,1,1,1,1,1,1}
  },
  {
    {0,0,6,1,2,2,1,6,0,0},
    {0,6,1,3,1,1,3,1,6,0},
    {6,1,4,1,5,5,1,4,1,6},
    {0,6,1,3,1,1,3,1,6,0},
    {0,0,6,1,2,2,1,6,0,0}
  },
  {
    {1,1,1,5,1,1,5,1,1,1},
    {2,1,6,5,1,1,5,6,1,2},
    {1,3,1,5,4,4,5,1,3,1},
    {1,1,1,5,1,1,5,1,1,1},
    {6,1,2,5,1,1,5,2,1,6}
  },
  {
    {1,1,6,6,6,6,6,6,1,1},
    {1,5,1,2,1,1,2,1,5,1},
    {6,1,4,1,3,3,1,4,1,6},
    {1,5,1,1,1,1,1,1,5,1},
    {1,1,6,1,2,2,1,6,1,1}
  },
  {
    {5,1,6,1,4,4,1,6,1,5},
    {1,3,1,2,6,6,2,1,3,1},
    {6,1,5,1,2,2,1,5,1,6},
    {1,2,1,3,6,6,3,1,2,1},
    {5,1,6,1,4,4,1,6,1,5}
  }
};

static void clearEffects() {
  fx.untilExpand = fx.untilShrink = fx.untilSlow = fx.untilFast = fx.untilSticky = fx.untilInvert = 0;
  fx.untilNail = fx.untilShield = 0;
}

// ---------- Power selection ----------
static int8_t pickEnabledFromList(const PowerKind* list, uint8_t n) {
  uint8_t enabledIdx[16];
  uint8_t en = 0;
  for (uint8_t i = 0; i < n; i++) if (powEnabled(list[i])) enabledIdx[en++] = i;
  if (en == 0) return -1;
  return (int8_t)enabledIdx[rnd(en)];
}
static PowerKind pickEnabledAny(bool preferGood) {
  PowerKind goodList[] = { P_EXPAND, P_SLOW, P_FAST, P_STICKY, P_LIFE, P_POINTS, P_TRIPLE, P_NAIL, P_SHIELD };
  PowerKind anyList[]  = { P_EXPAND, P_SHRINK, P_SLOW, P_FAST, P_STICKY, P_INVERT, P_LIFE, P_POINTS, P_TRIPLE, P_NAIL, P_SHIELD };
  const PowerKind* list = preferGood ? goodList : anyList;
  uint8_t n = preferGood ? (uint8_t)(sizeof(goodList)/sizeof(goodList[0]))
                         : (uint8_t)(sizeof(anyList)/sizeof(anyList[0]));
  int8_t idx = pickEnabledFromList(list, n);
  if (idx < 0) return P_POINTS;
  return list[(uint8_t)idx];
}
static PowerKind pickEnabledBad() {
  PowerKind badList[] = { P_SHRINK, P_INVERT };
  int8_t idx = pickEnabledFromList(badList, (uint8_t)(sizeof(badList)/sizeof(badList[0])));
  if (idx < 0) return P_POINTS;
  return badList[(uint8_t)idx];
}

static void spawnDrop(PowerKind kind, int16_t x, int16_t y) {
  if (!powEnabled(kind)) return;
  for (uint8_t i = 0; i < POW_MAX; i++) {
    if (!drops[i].active) {
      drops[i].active = true;
      drops[i].kind = kind;
      drops[i].x = x;
      drops[i].y = y;
      drops[i].vy = 1 + settings.difficulty;
      return;
    }
  }
}

static void setPaddleParams() {
  int16_t w = 32;
  if (effectOn(fx.untilExpand)) w = 44;
  if (effectOn(fx.untilShrink)) w = 22;
  paddle.w = w;

  int16_t sp = (settings.difficulty == 0) ? 3 : (settings.difficulty == 1 ? 4 : 5);
  paddle.speed = sp;
}

static void normalizeBallSpeed(Ball &b) {
  float vx = b.vx, vy = b.vy;

  if (effectOn(fx.untilSlow)) {
    vx = (vx > 0) ? 1.2f : -1.2f;
    vy = (vy > 0) ? 2.0f : -2.0f;
  } else if (effectOn(fx.untilFast)) {
    vx = (vx > 0) ? 2.6f : -2.6f;
    vy = (vy > 0) ? 3.4f : -3.4f;
  }

  float maxAbs = (settings.difficulty == 0) ? 3.2f : (settings.difficulty == 1 ? 4.3f : 5.2f);
  if (vx >  maxAbs) vx =  maxAbs;
  if (vx < -maxAbs) vx = -maxAbs;
  if (vy >  maxAbs) vy =  maxAbs;
  if (vy < -maxAbs) vy = -maxAbs;

  if (fabs(vx) < 0.8f) vx = (vx >= 0) ? 0.8f : -0.8f;
  if (fabs(vy) < 1.6f) vy = (vy >= 0) ? 1.6f : -1.6f;

  b.vx = vx; b.vy = vy;
}

// ---------- Level setup ----------
static void setLevel(uint16_t lvl) {
  uint8_t idx = (uint8_t)((lvl - 1) % LEVELS);
  bricksH = levelH[idx];
  aliveBricks = 0;

  for (uint8_t y = 0; y < BRH; y++)
    for (uint8_t x = 0; x < BRW; x++)
      bricks[x][y] = {0,0};

  for (uint8_t y = 0; y < bricksH; y++) {
    for (uint8_t x = 0; x < BRW; x++) {
      uint8_t v = levelMap[idx][y][x];
      if (v == 0) continue;

      Brick b; b.hp = 1; b.type = 0;
      if (v == 2) { b.type = 1; b.hp = 1; }
      else if (v == 3) { b.type = 2; b.hp = 1; }
      else if (v == 4) { b.type = 0; b.hp = 2; }
      else if (v == 5) { b.type = 4; b.hp = 255; }
      else if (v == 6) { b.type = 3; b.hp = 3; }

      if (lvl > LEVELS && b.hp != 255 && rnd(100) < 22) b.hp++;

      bricks[x][y] = b;
      if (b.hp != 255) aliveBricks++;
    }
  }
}

static void resetBallsToPaddle() {
  for (uint8_t i = 0; i < BALL_MAX; i++) {
    balls[i].active = false;
    balls[i].stuck  = false;
    balls[i].nail   = false;
    balls[i].staleX = balls[i].staleY = 0;
    balls[i].lastGoodX = balls[i].lastGoodY = 0;
    balls[i].ceilingRide = 0;
  }
  balls[0].active = true;
  balls[0].stuck  = true;
  balls[0].x = paddle.x + paddle.w / 2;
  balls[0].y = paddle.y - 4;
  balls[0].px = balls[0].x;
  balls[0].py = balls[0].y;
  balls[0].vx = 1.2f;
  balls[0].vy = -2.2f;

  balls[0].lastGoodX = balls[0].x;
  balls[0].lastGoodY = balls[0].y;
  balls[0].staleX = balls[0].staleY = 0;
  balls[0].ceilingRide = 0;
}

static void resetGame() {
  gameScore = 0;
  level = 1;
  gameLives = settings.maxLives;
  inPlay = false;
  paused = false;
  clearEffects();

  paddle.y = 59;
  paddle.x = 128 / 2 - 16;
  paddle.w = 32;
  paddle.speed = 4;

  for (uint8_t i = 0; i < POW_MAX; i++) drops[i].active = false;

  setPaddleParams();
  setLevel(level);
  resetBallsToPaddle();
}

// ---------- Multi-ball split ----------
static void splitIntoThree(const Ball &src) {
  uint8_t activeCount = 0;
  for (uint8_t i = 0; i < BALL_MAX; i++) if (balls[i].active) activeCount++;
  if (activeCount >= 3) { gameScore += 120; return; }

  float baseVx = src.vx;
  float baseVy = src.vy;
  if (fabs(baseVx) < 1.0f) baseVx = (baseVx >= 0) ? 1.0f : -1.0f;

  float vx1 = baseVx * 0.8f;
  float vy1 = baseVy * 1.0f;
  float vx2 = -baseVx * 0.8f;
  float vy2 = baseVy * 1.0f;

  for (uint8_t i = 0; i < BALL_MAX && activeCount < 3; i++) {
    if (!balls[i].active) {
      balls[i].active = true;
      balls[i].stuck = false;
      balls[i].x = src.x; balls[i].y = src.y;
      balls[i].px = src.x; balls[i].py = src.y;
      balls[i].vx = (activeCount == 1) ? vx1 : vx2;
      balls[i].vy = (activeCount == 1) ? vy1 : vy2;
      balls[i].nail = src.nail;

      balls[i].lastGoodX = balls[i].x;
      balls[i].lastGoodY = balls[i].y;
      balls[i].staleX = 0;
      balls[i].staleY = 0;
      balls[i].ceilingRide = 0;

      normalizeBallSpeed(balls[i]);
      activeCount++;
    }
  }
}

// ---------- Apply power ----------
static void applyPower(PowerKind k) {
  if (!powEnabled(k)) return;

  uint32_t t = millis();
  const uint32_t D = 9000;

  switch (k) {
    case P_EXPAND: fx.untilExpand = t + D; fx.untilShrink = 0; break;
    case P_SHRINK: fx.untilShrink = t + D; fx.untilExpand = 0; break;
    case P_SLOW:   fx.untilSlow   = t + D; fx.untilFast   = 0; break;
    case P_FAST:   fx.untilFast   = t + D; fx.untilSlow   = 0; break;
    case P_STICKY: fx.untilSticky = t + D; break;
    case P_INVERT: fx.untilInvert = t + D; break;
    case P_LIFE:   if (gameLives < settings.maxLives) gameLives++; break;
    case P_POINTS: gameScore += 200; break;
    case P_TRIPLE:
      for (uint8_t i = 0; i < BALL_MAX; i++) if (balls[i].active) { splitIntoThree(balls[i]); break; }
      break;
    case P_NAIL:   fx.untilNail = t + D; break;
    case P_SHIELD: fx.untilShield = t + D; break;
    default: break;
  }
}

static void handleDrops() {
  for (uint8_t i = 0; i < POW_MAX; i++) {
    if (!drops[i].active) continue;
    drops[i].y += drops[i].vy;

    if (drops[i].y >= paddle.y - 2 && drops[i].y <= paddle.y + 6) {
      if (drops[i].x >= paddle.x - 2 && drops[i].x <= paddle.x + paddle.w + 2) {
        applyPower(drops[i].kind);
        drops[i].active = false;
        continue;
      }
    }
    if (drops[i].y > 66) drops[i].active = false;
  }
}

// ============================================================
// Brick art
// ============================================================

static void brickPattern_tough(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t hp) {
  display.drawRoundRect(x, y, w, h, 2, SSD1306_WHITE);
  int step = (hp >= 3) ? 2 : (hp == 2 ? 3 : 4);
  for (int i = 1; i < w + h; i += step) {
    int x1 = x + i;
    int y1 = y;
    int x2 = x;
    int y2 = y + i;
    if (x1 > x + w - 2) { y1 += (x1 - (x + w - 2)); x1 = x + w - 2; }
    if (y2 > y + h - 2) { x2 += (y2 - (y + h - 2)); y2 = y + h - 2; }
    if (x1 >= x+1 && y2 >= y+1) display.drawLine(x2, y2, x1, y1, SSD1306_WHITE);
  }
}

static void brickPattern_metal(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t hp) {
  display.drawRoundRect(x, y, w, h, 2, SSD1306_WHITE);
  for (int yy = y + 1; yy < y + h - 1; yy += 2) {
    for (int xx = x + 1; xx < x + w - 1; xx += 2) {
      if (((xx + yy) & 2) == 0) display.drawPixel(xx, yy, SSD1306_WHITE);
    }
  }
  if (hp >= 2) display.drawFastHLine(x+2, y + h/2, w-4, SSD1306_WHITE);
  if (hp >= 3) display.drawFastVLine(x + w/2, y+2, h-4, SSD1306_WHITE);
}

static void brickPattern_unbreakable(int16_t x, int16_t y, int16_t w, int16_t h) {
  display.drawRoundRect(x, y, w, h, 2, SSD1306_WHITE);
  for (int xx = x + 2; xx < x + w - 2; xx += 3) display.drawFastVLine(xx, y + 1, h - 2, SSD1306_WHITE);
  for (int yy = y + 2; yy < y + h - 2; yy += 3) display.drawFastHLine(x + 1, yy, w - 2, SSD1306_WHITE);
}

// ============================================================
// Collision + watchdog
// ============================================================

static void hitBrick(uint8_t gx, uint8_t gy) {
  Brick &br = bricks[gx][gy];
  if (br.hp == 0) return;

  if (br.hp == 255) { gameScore += 1; return; }

  br.hp--;
  if (br.hp == 0) {
    aliveBricks--;
    gameScore += (br.type == 2) ? 25 : 50;

    if (br.type == 1) {
      PowerKind pk = pickEnabledAny(true);
      if (pk == P_TRIPLE && rnd(100) < 60) pk = P_POINTS;
      spawnDrop(pk, BR_X0 + gx * BR_W + BR_W / 2, BR_Y0 + gy * BR_H + BR_H / 2);
    } else if (br.type == 2) {
      if (rnd(100) < 70) spawnDrop(pickEnabledBad(), BR_X0 + gx * BR_W + BR_W / 2, BR_Y0 + gy * BR_H + BR_H / 2);
    } else {
      if (rnd(100) < 14) {
        PowerKind pk = pickEnabledAny(false);
        if (pk == P_TRIPLE && rnd(100) < 65) pk = P_POINTS;
        spawnDrop(pk, BR_X0 + gx * BR_W + BR_W / 2, BR_Y0 + gy * BR_H + BR_H / 2);
      }
    }
  } else {
    gameScore += 10;
  }
}

static bool collideBallWithBricks(Ball &b) {
  int16_t ax1 = BR_X0;
  int16_t ay1 = BR_Y0;
  int16_t ax2 = BR_X0 + BRW * BR_W;
  int16_t ay2 = BR_Y0 + bricksH * BR_H;

  if (b.y < ay1 - 6 || b.y > ay2 + 6) return false;
  if (b.x < ax1 - 6 || b.x > ax2 + 6) return false;

  int16_t cx = (int16_t)((b.x - BR_X0) / BR_W);
  int16_t cy = (int16_t)((b.y - BR_Y0) / BR_H);

  const float r = 2.0f;

  for (int8_t oy = -1; oy <= 1; oy++) {
    for (int8_t ox = -1; ox <= 1; ox++) {
      int16_t gx = cx + ox;
      int16_t gy = cy + oy;
      if (gx < 0 || gx >= BRW || gy < 0 || gy >= bricksH) continue;

      Brick &br = bricks[gx][gy];
      if (br.hp == 0) continue;

      float rx = (float)(BR_X0 + gx * BR_W);
      float ry = (float)(BR_Y0 + gy * BR_H);
      float rw = (float)(BR_W - 1);
      float rh = (float)(BR_H - 1);

      float closestX = b.x;
      if (closestX < rx) closestX = rx;
      else if (closestX > rx + rw) closestX = rx + rw;

      float closestY = b.y;
      if (closestY < ry) closestY = ry;
      else if (closestY > ry + rh) closestY = ry + rh;

      float dx = b.x - closestX;
      float dy = b.y - closestY;
      float dist2 = dx*dx + dy*dy;
      if (dist2 > r*r) continue;

      bool unbreakable = (br.hp == 255);
      bool nailOn = (b.nail || effectOn(fx.untilNail));

      float leftPen   = (b.x + r) - rx;
      float rightPen  = (rx + rw) - (b.x - r);
      float topPen    = (b.y + r) - ry;
      float bottomPen = (ry + rh) - (b.y - r);

      float minX = (leftPen < rightPen) ? leftPen : rightPen;
      float minY = (topPen  < bottomPen) ? topPen : bottomPen;

      float pbx = b.px, pby = b.py;
      bool wasLeft  = (pbx < rx);
      bool wasRight = (pbx > rx + rw);
      bool wasAbove = (pby < ry);
      bool wasBelow = (pby > ry + rh);

      bool resolveX = (minX < minY);
      if ((wasLeft || wasRight) && !(wasAbove || wasBelow)) resolveX = true;
      if ((wasAbove || wasBelow) && !(wasLeft || wasRight)) resolveX = false;

      if (nailOn && !unbreakable) {
        hitBrick((uint8_t)gx, (uint8_t)gy);
        b.x = pbx + (b.vx * 0.35f);
        b.y = pby + (b.vy * 0.35f);

        const float minYwall = (float)PLAY_T + r;
        if (b.y <= (minYwall + 0.2f)) {
          b.y = minYwall + 1.0f;
          b.vy = fabs(b.vy);
          if (b.vy < 2.0f) b.vy = 2.2f;
          if (fabs(b.vx) < 0.9f) b.vx = (b.vx >= 0) ? 1.1f : -1.1f;
        }
        return true;
      }

      if (resolveX) {
        if (leftPen < rightPen)  b.x -= (minX + 0.12f);
        else                     b.x += (minX + 0.12f);
        b.vx = -b.vx;
      } else {
        if (topPen < bottomPen)  b.y -= (minY + 0.12f);
        else                     b.y += (minY + 0.12f);
        b.vy = -b.vy;
      }

      if (fabs(b.vx) < 0.7f) b.vx = (b.vx >= 0) ? 0.9f : -0.9f;
      if (fabs(b.vy) < 1.2f) b.vy = (b.vy >= 0) ? 1.6f : -1.6f;

      {
        const float minXwall = (float)PLAY_L + r;
        const float maxXwall = (float)PLAY_R - r;
        const float minYwall = (float)PLAY_T + r;
        bool nearCeil = (b.y <= minYwall + 0.2f);
        bool nearLeft = (b.x <= minXwall + 0.2f);
        bool nearRight= (b.x >= maxXwall - 0.2f);
        if (nearCeil && (nearLeft || nearRight)) {
          b.y = minYwall + 1.0f;
          b.vy = fabs(b.vy);
          if (b.vy < 2.0f) b.vy = 2.4f;
        }
      }

      if (!unbreakable) hitBrick((uint8_t)gx, (uint8_t)gy);
      return true;
    }
  }
  return false;
}

static bool collideBallWithPaddle(Ball &b) {
  float px1 = (float)paddle.x;
  float px2 = (float)(paddle.x + paddle.w);
  float py1 = (float)paddle.y;
  float py2 = (float)(paddle.y + 4);

  float r = 2.0f;
  bool hit =
    (b.x + r >= px1) && (b.x - r <= px2) &&
    (b.y + r >= py1) && (b.y - r <= py2);

  if (!hit) return false;

  if (b.vy > 0) {
    b.y = py1 - r - 0.2f;
    b.vy = -fabs(b.vy);

    float center = px1 + (paddle.w * 0.5f);
    float rel = (b.x - center) / (paddle.w * 0.5f);
    if (rel < -1) rel = -1;
    if (rel >  1) rel =  1;
    b.vx = rel * 2.7f;

    if (effectOn(fx.untilSticky)) {
      b.stuck = true;
      inPlay = false;
    }

    normalizeBallSpeed(b);
    return true;
  }
  return false;
}

static void watchdogUnstick(Ball &b) {
  if (!b.active || b.stuck) return;

  const float MOVE_EPS_X = 0.35f;
  const float MOVE_EPS_Y = 0.35f;
  const uint16_t STALE_LIMIT = 75;
  const uint16_t CEIL_LIMIT  = 45;

  const float r = 2.0f;
  const float minX = (float)PLAY_L + r;
  const float maxX = (float)PLAY_R - r;
  const float minY = (float)PLAY_T + r;

  float dx = fabs(b.x - b.lastGoodX);
  float dy = fabs(b.y - b.lastGoodY);

  if (dx < MOVE_EPS_X) b.staleX++; else { b.staleX = 0; b.lastGoodX = b.x; }
  if (dy < MOVE_EPS_Y) b.staleY++; else { b.staleY = 0; b.lastGoodY = b.y; }

  bool nearCeilingLine = (b.y <= (minY + 0.25f));
  if (nearCeilingLine) b.ceilingRide++; else b.ceilingRide = 0;

  bool cornerZone = nearCeilingLine && (b.x <= (minX + 0.25f) || b.x >= (maxX - 0.25f));

  if (b.staleX > STALE_LIMIT || b.staleY > STALE_LIMIT || b.ceilingRide > CEIL_LIMIT || cornerZone) {
    if (b.y <= (minY + 0.25f)) b.y = minY + 1.2f;
    if (b.x <= (minX + 0.25f)) b.x = minX + 1.2f;
    if (b.x >= (maxX - 0.25f)) b.x = maxX - 1.2f;

    b.vy = fabs(b.vy);
    if (b.vy < 2.0f) b.vy = 2.6f;

    float kick = (rnd(2) == 0) ? -1.0f : 1.0f;
    b.vx += kick * 1.1f;
    if (fabs(b.vx) < 0.9f) b.vx = (b.vx >= 0) ? 1.1f : -1.1f;

    b.staleX = b.staleY = 0;
    b.ceilingRide = 0;
    b.lastGoodX = b.x;
    b.lastGoodY = b.y;
  }
}

// ============================================================
// HUD effects strip (icons, not letters)
// ============================================================

static uint8_t activeEffectsList(PowerKind* out, uint8_t cap) {
  uint8_t n = 0;
  auto add = [&](PowerKind k) { if (n < cap) out[n++] = k; };

  if (effectOn(fx.untilExpand)) add(P_EXPAND);
  if (effectOn(fx.untilShrink)) add(P_SHRINK);
  if (effectOn(fx.untilSlow))   add(P_SLOW);
  if (effectOn(fx.untilFast))   add(P_FAST);
  if (effectOn(fx.untilSticky)) add(P_STICKY);
  if (effectOn(fx.untilNail))   add(P_NAIL);
  if (effectOn(fx.untilShield)) add(P_SHIELD);
  if (effectOn(fx.untilInvert)) add(P_INVERT);

  return n;
}

// ============================================================
// Physics + gameplay loop
// ============================================================

static void stepPhysics() {
  setPaddleParams();

  bool inv = settings.invertCtl || effectOn(fx.untilInvert);
  bool left = btn[BID_LEFT].now;
  bool right = btn[BID_RIGHT].now;
  if (inv) { bool tmp = left; left = right; right = tmp; }

  if (left)  paddle.x -= paddle.speed;
  if (right) paddle.x += paddle.speed;

  if (paddle.x < PLAY_L) paddle.x = PLAY_L;
  if (paddle.x > PLAY_R - paddle.w) paddle.x = PLAY_R - paddle.w;

  handleDrops();

  bool nailOn = effectOn(fx.untilNail);

  if (!inPlay && btnPressed(BID_A)) {
    inPlay = true;
    for (uint8_t i = 0; i < BALL_MAX; i++)
      if (balls[i].active && balls[i].stuck) balls[i].stuck = false;
  }

  uint8_t activeBalls = 0;

  for (uint8_t i = 0; i < BALL_MAX; i++) {
    Ball &b = balls[i];
    if (!b.active) continue;
    activeBalls++;

    b.nail = nailOn;

    if (b.stuck) {
      b.px = b.x; b.py = b.y;
      b.x = paddle.x + paddle.w * 0.5f;
      b.y = paddle.y - 4;
      b.lastGoodX = b.x; b.lastGoodY = b.y;
      b.staleX = b.staleY = 0;
      b.ceilingRide = 0;
      continue;
    }

    b.px = b.x; b.py = b.y;
    normalizeBallSpeed(b);

    float vx = b.vx, vy = b.vy;
    float maxStep = fmaxf(fabs(vx), fabs(vy));
    int steps = (int)ceilf(maxStep);
    if (steps < 1) steps = 1;

    float sx = vx / (float)steps;
    float sy = vy / (float)steps;

    const float r = 2.0f;
    const float minX = (float)PLAY_L + r;
    const float maxX = (float)PLAY_R - r;
    const float minY = (float)PLAY_T + r;

    for (int s = 0; s < steps; s++) {
      b.x += sx; b.y += sy;

      if (b.x < minX) { b.x = minX; b.vx = -b.vx; }
      if (b.x > maxX) { b.x = maxX; b.vx = -b.vx; }
      if (b.y < minY) { b.y = minY; b.vy = -b.vy; }

      if (b.x == minX || b.x == maxX) b.x += (b.vx > 0 ? 0.2f : -0.2f);
      if (b.y == minY)               b.y += (b.vy > 0 ? 0.2f : -0.2f);

      collideBallWithPaddle(b);
      collideBallWithBricks(b);

      if (b.y > 66) { b.active = false; activeBalls--; break; }
    }

    watchdogUnstick(b);
  }

  if (activeBalls == 0) {
    if (effectOn(fx.untilShield)) {
      fx.untilShield = 0;
      resetBallsToPaddle();
      inPlay = false;
      return;
    }

    if (gameLives > 0) gameLives--;
    if (gameLives == 0) return;

    resetBallsToPaddle();
    inPlay = false;
    return;
  }

  if (aliveBricks == 0) {
    level++;
    setLevel(level);
    gameScore += 250;
    resetBallsToPaddle();
    inPlay = false;
  }
}

// ============================================================
// Screens + navigation
// ============================================================

static Screen screen = SCR_PRESPLASH;

// --- FIX: declare these BEFORE gotoScreen (so the compiler sees them) ---
static uint32_t preSplashStart = 0;
static uint32_t splashStart    = 0;

static void gotoScreen(Screen s, TransitionType tr) {
  // reset timers when re-entering these screens so they re-init cleanly
  if (s == SCR_SPLASH) {
    splashStart = 0;
  } else if (s == SCR_PRESPLASH) {
    preSplashStart = 0;
  }
  screen = s;
  startTransition(tr);
}

// ============================================================
// Pre-splash (logo/text intro)
// ============================================================

static void tickPreSplash() {
  if (preSplashStart == 0) preSplashStart = millis();

  if (btnPressed(BID_A) || btnPressed(BID_B)) {
    gotoScreen(SCR_SPLASH, TR_WIPE_LEFT);
    return;
  }

  if (millis() - preSplashStart >= 4000) {
    gotoScreen(SCR_SPLASH, TR_WIPE_LEFT);
  }
}

static void renderPreSplash() {
  display.invertDisplay(false);
  display.clearDisplay();

  display.drawRect(0, 0, 128, 64, SSD1306_WHITE);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(18, 14);
  display.print("Fab Lab Reykjavik");

  display.setCursor(18, 26);
  display.print("in collaboration");

  display.setCursor(40, 36);
  display.print("with");

  display.setCursor(10, 46);
  display.print("Reykjavik Game Studios");

  if (blinkOn(450)) {
    display.setCursor(44, 56);
    display.print("presents");
  }

  drawPulseDot();
  applyTransitionMask();
  display.display();
}

// ============================================================
// Splash (clean layout): centered title + full border +
// 1-row bricks that break + respawn + Press A above paddle.
// ============================================================

static float spBallX = 64, spBallY = 40, spVx = 1.6f, spVy = 1.4f;
static float spPadX  = 51;

// --- Splash layout ---
static const int16_t SPL_TITLE_H = 22;      // reserved title band at top
static const int16_t SPL_MARGIN  = 2;
static const int16_t SPL_L = SPL_MARGIN;
static const int16_t SPL_R = 127 - SPL_MARGIN;
static const int16_t SPL_T = SPL_MARGIN;
static const int16_t SPL_B = 63 - SPL_MARGIN;

// Animation bounds (below title band)
static const int16_t SPL_ANIM_T = SPL_TITLE_H + 2;
static const int16_t SPL_ANIM_B = SPL_B;

// Bricks: 1 row of 10
static bool spBrickAlive[10];
static const int16_t SPL_BR_Y = SPL_ANIM_T + 2;
static const int16_t SPL_BR_W = 10;
static const int16_t SPL_BR_H = 5;
static const int16_t SPL_BR_G = 2;

static void splashResetBricks() {
  for (int i = 0; i < 10; i++) spBrickAlive[i] = true;
}

static inline int16_t splashBrickX(int i) {
  int16_t rowW = 10 * SPL_BR_W + 9 * SPL_BR_G;
  int16_t x0 = 64 - rowW / 2;
  return x0 + i * (SPL_BR_W + SPL_BR_G);
}

static void splashDrawCenteredTitle2(const char* line1, const char* line2) {
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  int16_t x1, y1; uint16_t w1, h1;
  display.getTextBounds(line1, 0, 0, &x1, &y1, &w1, &h1);
  int16_t cx1 = (128 - (int16_t)w1) / 2;
  display.setCursor(cx1, 0);
  display.print(line1);

  int16_t x2, y2; uint16_t w2, h2;
  display.getTextBounds(line2, 0, 0, &x2, &y2, &w2, &h2);
  int16_t cx2 = (128 - (int16_t)w2) / 2;
  display.setCursor(cx2, 14);
  display.print(line2);

  display.setTextSize(1);
}

static void tickSplash() {
  if (splashStart == 0) {
    splashStart = millis();
    splashResetBricks();
    spBallX = 64; spBallY = 40;
    spVx = 1.6f; spVy = 1.4f;
    spPadX = 51;
  }

  float t = (float)millis() * 0.0022f;
  spPadX = 51 + 16 * sinf(t);

  const float r = 2.0f;
  const float L = (float)SPL_L + r;
  const float R = (float)SPL_R - r;
  const float T = (float)SPL_ANIM_T + r;
  const float B = (float)SPL_ANIM_B - r;

  float prevY = spBallY;

  spBallX += spVx;
  spBallY += spVy;

  if (spBallX < L) { spBallX = L; spVx = fabs(spVx); }
  if (spBallX > R) { spBallX = R; spVx = -fabs(spVx); }
  if (spBallY < T) { spBallY = T; spVy = fabs(spVy); }
  if (spBallY > B) { spBallY = B; spVy = -fabs(spVy); }

  const float padY = (float)(SPL_ANIM_B - 6);
  const float padW = 26.0f;
  float px1 = spPadX;
  float px2 = spPadX + padW;

  if (spBallY + r >= padY && spBallY + r <= padY + 2 &&
      spBallX >= px1 && spBallX <= px2 && spVy > 0) {
    spBallY = padY - r - 0.2f;
    spVy = -fabs(spVy);

    float center = (px1 + px2) * 0.5f;
    float rel = (spBallX - center) / (padW * 0.5f);
    if (rel < -1) rel = -1;
    if (rel >  1) rel =  1;
    spVx = rel * 2.2f;
    if (fabs(spVx) < 0.7f) spVx = (spVx >= 0) ? 0.8f : -0.8f;
  }

  for (int i = 0; i < 10; i++) {
    if (!spBrickAlive[i]) continue;

    int16_t bx = splashBrickX(i);
    int16_t by = SPL_BR_Y;

    float rx = (float)bx;
    float ry = (float)by;
    float rw = (float)SPL_BR_W;
    float rh = (float)SPL_BR_H;

    float closestX = spBallX;
    if (closestX < rx) closestX = rx;
    else if (closestX > rx + rw) closestX = rx + rw;

    float closestY = spBallY;
    if (closestY < ry) closestY = ry;
    else if (closestY > ry + rh) closestY = ry + rh;

    float dx = spBallX - closestX;
    float dy = spBallY - closestY;
    if (dx*dx + dy*dy <= r*r) {
      spBrickAlive[i] = false;

      if (prevY < ry) spVy = -fabs(spVy);
      else           spVy =  fabs(spVy);

      spBallY += (spVy > 0) ? 1.0f : -1.0f;
      break;
    }
  }

  bool any = false;
  for (int i = 0; i < 10; i++) if (spBrickAlive[i]) { any = true; break; }
  if (!any) splashResetBricks();

  if (btnPressed(BID_A)) gotoScreen(SCR_MENU, TR_WIPE_LEFT);
}

static void renderSplash() {
  display.invertDisplay(false);
  display.clearDisplay();

  // Border around entire screen
  display.drawRect(0, 0, 128, 64, SSD1306_WHITE);

  // (Removed) mid horizontal separator line — intentionally gone

  splashDrawCenteredTitle2("Brick", "Breaker");

  for (int i = 0; i < 10; i++) {
    if (!spBrickAlive[i]) continue;
    int16_t bx = splashBrickX(i);
    int16_t by = SPL_BR_Y;
    display.drawRoundRect(bx, by, SPL_BR_W, SPL_BR_H, 1, SSD1306_WHITE);
    display.drawFastHLine(bx+2, by+1, SPL_BR_W-4, SSD1306_WHITE);
  }

  int16_t padY = SPL_ANIM_B - 6;
  display.fillRoundRect((int16_t)spPadX, padY, 26, 3, 2, SSD1306_WHITE);

  display.fillCircle((int16_t)spBallX, (int16_t)spBallY, 2, SSD1306_WHITE);

  // Press A moved up a bit above paddle
  if (blinkOn(420)) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(44, padY - 10);
    display.print("Press A");
  }

  drawPulseDot();
  applyTransitionMask();
  display.display();
}

// ============================================================
// Menu (B returns to Splash + blinking selector)
// ============================================================

static const char* menuItems[] = { "Play", "Settings", "High Scores", "Power-Ups", "About" };
static const uint8_t MENU_N = sizeof(menuItems) / sizeof(menuItems[0]);
static uint8_t menuSel = 0;

static void tickMenu() {
  if (btnPressed(BID_UP))   menuSel = (menuSel == 0) ? (MENU_N - 1) : (menuSel - 1);
  if (btnPressed(BID_DOWN)) menuSel = (menuSel + 1) % MENU_N;

  if (btnPressed(BID_A)) {
    if (menuSel == 0) { resetGame(); gotoScreen(SCR_PLAY, TR_SHUTTER); }
    else if (menuSel == 1) gotoScreen(SCR_SETTINGS, TR_WIPE_LEFT);
    else if (menuSel == 2) gotoScreen(SCR_HISCORES, TR_WIPE_LEFT);
    else if (menuSel == 3) gotoScreen(SCR_POWERUPS, TR_WIPE_LEFT);
    else if (menuSel == 4) gotoScreen(SCR_ABOUT, TR_WIPE_LEFT);
  }

  if (btnPressed(BID_B)) gotoScreen(SCR_SPLASH, TR_WIPE_RIGHT);
}

static void renderMenu() {
  display.invertDisplay(false);
  display.clearDisplay();
  titleBar("Brick Breaker");

  bool showArrow = blinkOn(350);

  for (uint8_t i = 0; i < MENU_N; i++) {
    int16_t y = 14 + i * 9;
    bool sel = (i == menuSel);

    if (sel) display.fillRoundRect(0, y - 1, 128, 9, 2, SSD1306_WHITE);
    display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);

    display.setCursor(12, y);
    display.print(menuItems[i]);

    if (sel && showArrow) {
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(2, y);
      display.print(">");
    }
    display.setTextColor(SSD1306_WHITE);
  }

  drawPulseDot();
  applyTransitionMask();
  display.display();
}

// ============================================================
// Settings
// ============================================================

static uint8_t setSel = 0;
static const uint8_t SETTINGS_N = 5;

static const char* diffName(uint8_t d) {
  if (d == 0) return "Easy";
  if (d == 2) return "Hard";
  return "Normal";
}

static void tickSettings() {
  if (btnPressed(BID_UP))   setSel = (setSel == 0) ? (SETTINGS_N - 1) : (setSel - 1);
  if (btnPressed(BID_DOWN)) setSel = (setSel + 1) % SETTINGS_N;

  auto incDec = [&](int dir) {
    switch (setSel) {
      case 0: { int v = (int)settings.difficulty + dir; if (v < 0) v = 2; if (v > 2) v = 0; settings.difficulty = (uint8_t)v; } break;
      case 1: { int v = (int)settings.maxLives + dir; if (v < 1) v = 5; if (v > 5) v = 1; settings.maxLives = (uint8_t)v; } break;
      case 2: settings.ballTrail = settings.ballTrail ? 0 : 1; break;
      case 3: settings.invertCtl = settings.invertCtl ? 0 : 1; break;
      default: break;
    }
    saveSettings();
  };

  if (setSel <= 3) {
    if (btnPressed(BID_LEFT))  incDec(-1);
    if (btnPressed(BID_RIGHT)) incDec(+1);
    if (btnHeldPulse(BID_LEFT))  incDec(-1);
    if (btnHeldPulse(BID_RIGHT)) incDec(+1);
  }

  if (setSel == 4 && btnPressed(BID_A)) { gotoScreen(SCR_SET_POWERS, TR_WIPE_LEFT); return; }
  if (btnPressed(BID_B)) gotoScreen(SCR_MENU, TR_WIPE_RIGHT);
}

static void renderSettings() {
  display.invertDisplay(false);
  display.clearDisplay();
  titleBar("Settings");

  bool blinkSel = blinkOn(360);

  for (uint8_t i = 0; i < SETTINGS_N; i++) {
    int16_t y = 14 + i * 10;
    bool sel = (i == setSel);
    if (sel && blinkSel) display.drawRoundRect(0, y - 1, 128, 10, 2, SSD1306_WHITE);

    display.setCursor(4, y);
    if (i == 0) display.print("Difficulty");
    if (i == 1) display.print("Max Lives");
    if (i == 2) display.print("Ball Trail");
    if (i == 3) display.print("Invert Ctrl");
    if (i == 4) display.print("Power-Ups");

    display.setCursor(84, y);
    if (i == 0) display.print(diffName(settings.difficulty));
    if (i == 1) display.print((int)settings.maxLives);
    if (i == 2) display.print(settings.ballTrail ? "On" : "Off");
    if (i == 3) display.print(settings.invertCtl ? "On" : "Off");
    if (i == 4) display.print("Edit");
  }

  drawPulseDot();
  applyTransitionMask();
  display.display();
}

// ============================================================
// Power-Ups enable/disable screen
// ============================================================

static uint8_t psetSel = 0;
static uint8_t psetScroll = 0;

static void tickSetPowers() {
  if (btnPressed(BID_UP))   { if (psetSel > 0) psetSel--; }
  if (btnPressed(BID_DOWN)) { if (psetSel + 1 < (uint8_t)P_KIND_N) psetSel++; }

  if (psetSel < psetScroll) psetScroll = psetSel;
  if (psetSel >= psetScroll + 6) psetScroll = psetSel - 5;

  if (btnPressed(BID_A)) {
    PowerKind k = (PowerKind)psetSel;
    bool en = powEnabled(k);

    if (en) {
      uint16_t mask = settings.powMask & (uint16_t)((1u << P_KIND_N) - 1u);
      if ((mask & ~powBit(k)) != 0) { powSet(k, false); saveSettings(); }
    } else {
      powSet(k, true);
      saveSettings();
    }
  }

  if (btnPressed(BID_B)) gotoScreen(SCR_SETTINGS, TR_WIPE_RIGHT);
}

static void renderSetPowers() {
  display.invertDisplay(false);
  display.clearDisplay();
  titleBar("Power-Ups");

  bool showArrow = blinkOn(350);

  for (uint8_t row = 0; row < 6; row++) {
    uint8_t idx = psetScroll + row;
    if (idx >= (uint8_t)P_KIND_N) break;

    int16_t y = 12 + row * 9;
    bool sel = (idx == psetSel);
    bool en  = powEnabled((PowerKind)idx);

    if (sel) display.fillRoundRect(0, y - 1, 128, 9, 2, SSD1306_WHITE);

    if (sel) {
      display.fillRoundRect(0, y, 14, 9, 3, SSD1306_BLACK);
      display.fillRect(3, y+1, 8, 7, SSD1306_BLACK);
      drawPowerIcon((PowerKind)idx, 3, y+1);
    } else {
      drawIconPill(0, y, 14, 9, en, (PowerKind)idx);
    }

    display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    display.setCursor(18, y + 1);
    display.print(powerName((PowerKind)idx));
    display.setCursor(116, y + 1);
    display.print(en ? "On" : "Off");
    display.setTextColor(SSD1306_WHITE);

    if (sel && showArrow) {
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(2, y+1);
      display.print(">");
      display.setTextColor(SSD1306_WHITE);
    }
  }

  drawScrollArrows(psetScroll > 0, (psetScroll + 6) < (uint8_t)P_KIND_N);
  drawPulseDot();
  applyTransitionMask();
  display.display();
}

// ============================================================
// High Scores
// ============================================================

static uint8_t hsScroll = 0;
static void tickHighScores() {
  if (btnPressed(BID_UP))   { if (hsScroll > 0) hsScroll--; }
  if (btnPressed(BID_DOWN)) { if (hsScroll + 6 < HS_MAX) hsScroll++; }
  if (btnPressed(BID_B)) gotoScreen(SCR_MENU, TR_WIPE_RIGHT);
}
static void renderHighScores() {
  display.invertDisplay(false);
  display.clearDisplay();
  titleBar("High Scores");

  for (uint8_t i = 0; i < 6; i++) {
    uint8_t idx = hsScroll + i;
    if (idx >= HS_MAX) break;
    int16_t y = 14 + i * 8;
    display.setCursor(0, y);  display.print(idx + 1); display.print(".");
    display.setCursor(18, y); display.print(hs[idx].name);
    display.setCursor(54, y); display.print(hs[idx].score);
  }

  drawScrollArrows(hsScroll > 0, (hsScroll + 6) < HS_MAX);
  drawPulseDot();
  applyTransitionMask();
  display.display();
}

// ============================================================
// Power-Ups glossary
// ============================================================

static uint8_t puScroll = 0;
static void tickPowerUps() {
  if (btnPressed(BID_UP))   { if (puScroll > 0) puScroll--; }
  if (btnPressed(BID_DOWN)) { if (puScroll + 6 < (uint8_t)P_KIND_N) puScroll++; }
  if (btnPressed(BID_B)) gotoScreen(SCR_MENU, TR_WIPE_RIGHT);
}
static void renderPowerUps() {
  display.invertDisplay(false);
  display.clearDisplay();
  titleBar("Power-Ups");

  for (uint8_t i = 0; i < 6; i++) {
    uint8_t idx = puScroll + i;
    if (idx >= (uint8_t)P_KIND_N) break;
    int16_t y = 12 + i * 9;

    drawIconPill(0, y, 14, 9, true, (PowerKind)idx);
    display.setCursor(18, y + 1);
    display.print(powerName((PowerKind)idx));
  }

  drawScrollArrows(puScroll > 0, (puScroll + 6) < (uint8_t)P_KIND_N);
  drawPulseDot();
  applyTransitionMask();
  display.display();
}

// ============================================================
// About
// ============================================================

static void tickAbout() { if (btnPressed(BID_B)) gotoScreen(SCR_MENU, TR_WIPE_RIGHT); }
static void renderAbout() {
  display.invertDisplay(false);
  display.clearDisplay();
  titleBar("About");
  display.setCursor(0, 14); display.print("Brick Breaker");
  display.setCursor(0, 24); display.print("Andri - Fab Lab");
  display.setCursor(0, 34); display.print("Reykjavik");
  display.setCursor(0, 46); display.print("XIAO ESP32-C6");
  drawPulseDot();
  applyTransitionMask();
  display.display();
}

// ============================================================
// Play
// ============================================================

static uint8_t pauseSel = 0;

static void tickPlay() {
  bool bPress = btnPressed(BID_B);
  bool wasPaused = paused;

  if (bPress) {
    paused = !paused;
    if (paused) pauseSel = 0;
  }

  if (paused) {
    if (!wasPaused) return;

    if (btnPressed(BID_UP))   pauseSel = (pauseSel == 0) ? 2 : (pauseSel - 1);
    if (btnPressed(BID_DOWN)) pauseSel = (pauseSel + 1) % 3;

    if (btnPressed(BID_A)) {
      if (pauseSel == 0) paused = false;
      else if (pauseSel == 1) resetGame();
      else if (pauseSel == 2) { paused = false; gotoScreen(SCR_MENU, TR_WIPE_RIGHT); }
    }
    return;
  }

  stepPhysics();
  if (gameLives == 0) gotoScreen(SCR_GAMEOVER, TR_SHUTTER);
}

static void renderPlay() {
  display.invertDisplay(effectOn(fx.untilInvert));
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("S:");
  display.print(gameScore);

  drawLivesHearts();

  PowerKind eff[8];
  uint8_t en = activeEffectsList(eff, 8);
  if (en > 0) {
    uint8_t maxL = settings.maxLives;
    if (maxL < 1) maxL = 1;
    if (maxL > 5) maxL = 5;
    int16_t heartsW = (int16_t)maxL * 8;
    int16_t heartsLeft = 128 - heartsW;

    int16_t pillW = 10, pillH = 8, gap = 1;
    int16_t total = en * (pillW + gap);
    int16_t x = heartsLeft - 2 - total;
    if (x < 34) x = 34;
    int16_t y = 0;

    for (uint8_t i = 0; i < en; i++) {
      drawIconPill(x + i*(pillW+gap), y, pillW, pillH, false, eff[i]);
    }
  }

  display.drawFastHLine(0, SCORE_H - 1, 128, SSD1306_WHITE);

  display.drawFastVLine(0,   PLAY_T, (PLAY_B - PLAY_T + 1), SSD1306_WHITE);
  display.drawFastVLine(127, PLAY_T, (PLAY_B - PLAY_T + 1), SSD1306_WHITE);
  display.drawFastHLine(0,   PLAY_T, 128, SSD1306_WHITE);

  for (uint8_t y = 0; y < bricksH; y++) {
    for (uint8_t x = 0; x < BRW; x++) {
      Brick &br = bricks[x][y];
      if (br.hp == 0) continue;

      int16_t rx = BR_X0 + x * BR_W;
      int16_t ry = BR_Y0 + y * BR_H;
      int16_t rw = BR_W - 1;
      int16_t rh = BR_H - 1;

      if (br.hp == 255) {
        brickPattern_unbreakable(rx, ry, rw, rh);
        continue;
      }

      if (br.type == 1) {
        display.fillRoundRect(rx, ry, rw, rh, 2, SSD1306_WHITE);
        display.drawRoundRect(rx, ry, rw, rh, 2, SSD1306_WHITE);
      } else if (br.type == 2) {
        display.drawRoundRect(rx, ry, rw, rh, 2, SSD1306_WHITE);
        display.drawLine(rx + 1, ry + 1, rx + rw - 2, ry + rh - 2, SSD1306_WHITE);
      } else if (br.type == 3) {
        brickPattern_metal(rx, ry, rw, rh, br.hp);
      } else {
        if (br.hp >= 2) brickPattern_tough(rx, ry, rw, rh, br.hp);
        else display.drawRoundRect(rx, ry, rw, rh, 2, SSD1306_WHITE);
      }
    }
  }

  display.fillRoundRect(paddle.x, paddle.y, paddle.w, 4, 2, SSD1306_WHITE);

  for (uint8_t i = 0; i < BALL_MAX; i++) {
    Ball &b = balls[i];
    if (!b.active) continue;
    if (settings.ballTrail && inPlay && !b.stuck) display.drawPixel((int16_t)b.px, (int16_t)b.py, SSD1306_WHITE);
    display.fillCircle((int16_t)b.x, (int16_t)b.y, 2, SSD1306_WHITE);
  }

  for (uint8_t i = 0; i < POW_MAX; i++) {
    if (!drops[i].active) continue;
    drawIconPill(drops[i].x - 6, drops[i].y - 5, 12, 11, true, drops[i].kind);
  }

  if (paused) {
    display.fillRoundRect(18, 20, 92, 28, 6, SSD1306_BLACK);
    display.drawRoundRect(18, 20, 92, 28, 6, SSD1306_WHITE);

    const char* items[3] = { "Resume", "Restart", "Quit" };
    for (uint8_t i = 0; i < 3; i++) {
      int16_t y = 24 + i * 9;
      bool sel = (i == pauseSel);
      if (sel && blinkOn(350)) display.fillRoundRect(24, y - 1, 80, 9, 2, SSD1306_WHITE);
      display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
      display.setCursor(30, y + 1);
      display.print(items[i]);
      display.setTextColor(SSD1306_WHITE);
    }
    drawPulseDot();
  }

  applyTransitionMask();
  display.display();
}

// ============================================================
// Game Over / Name entry
// ============================================================

static bool goHigh = false;
static char entryName[4] = { 'A','A','A','\0' };
static uint8_t entryPos = 0;

static void tickGameOver() {
  static bool init = false;
  if (!init) { goHigh = isHighScore(gameScore); init = true; }

  if (goHigh) {
    if (btnPressed(BID_A)) { init = false; gotoScreen(SCR_NAMEENTRY, TR_WIPE_LEFT); }
    if (btnPressed(BID_B)) { init = false; gotoScreen(SCR_MENU, TR_WIPE_RIGHT); }
  } else {
    if (btnPressed(BID_A)) { init = false; resetGame(); gotoScreen(SCR_PLAY, TR_SHUTTER); }
    if (btnPressed(BID_B)) { init = false; gotoScreen(SCR_MENU, TR_WIPE_RIGHT); }
  }
}

static void renderGameOver() {
  display.invertDisplay(false);
  display.clearDisplay();
  titleBar("Game Over");

  display.setCursor(0, 16);
  display.print("Score: ");
  display.print(gameScore);

  display.setCursor(0, 30);
  display.print(goHigh ? "New High Score!" : "Try again");

  drawPulseDot();
  applyTransitionMask();
  display.display();
}

static void tickNameEntry() {
  auto bump = [&](int dir) {
    char &c = entryName[entryPos];
    int v = (int)c + dir;
    if (v < 'A') v = 'Z';
    if (v > 'Z') v = 'A';
    c = (char)v;
  };

  if (btnPressed(BID_UP)) bump(+1);
  if (btnPressed(BID_DOWN)) bump(-1);
  if (btnHeldPulse(BID_UP)) bump(+1);
  if (btnHeldPulse(BID_DOWN)) bump(-1);

  if (btnPressed(BID_A)) {
    if (entryPos < 2) entryPos++;
    else { insertHighScore(entryName, gameScore); gotoScreen(SCR_HISCORES, TR_WIPE_LEFT); }
  }
  if (btnPressed(BID_B)) {
    if (entryPos > 0) entryPos--;
    else gotoScreen(SCR_MENU, TR_WIPE_RIGHT);
  }
}

static void renderNameEntry() {
  display.invertDisplay(false);
  display.clearDisplay();
  titleBar("Enter Name");

  display.setCursor(0, 14);
  display.print("Score: ");
  display.print(gameScore);

  display.setTextSize(2);
  int16_t x0 = 38;
  int16_t y0 = 28;
  display.setCursor(x0, y0);       display.print(entryName[0]);
  display.setCursor(x0 + 18, y0);  display.print(entryName[1]);
  display.setCursor(x0 + 36, y0);  display.print(entryName[2]);
  display.setTextSize(1);

  if (blinkOn(350)) display.drawFastHLine(x0 + entryPos * 18, y0 + 16, 12, SSD1306_WHITE);

  drawPulseDot();
  applyTransitionMask();
  display.display();
}

// ============================================================
// Main loop
// ============================================================

static uint32_t lastFrame = 0;

void setup() {
  Serial.begin(115200);
  delay(50);

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  bool ok = false;
  for (uint8_t i = 0; i < sizeof(OLED_ADDRS); i++) {
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRS[i])) { ok = true; break; }
  }
  if (!ok) { while (1) delay(100); }
  Wire.setClock(400000);

  rngState ^= (uint32_t)micros();

  loadSettings();
  loadHighScores();

  screen = SCR_PRESPLASH;
  preSplashStart = 0;
  splashStart = 0;

  lastFrame = millis();
}

void loop() {
  updateButtons();

  uint32_t now = millis();
  if (now - lastFrame < 16) { delay(1); return; }
  lastFrame = now;

  // Tick
  switch (screen) {
    case SCR_PRESPLASH:  tickPreSplash(); break;
    case SCR_SPLASH:     tickSplash(); break;
    case SCR_MENU:       tickMenu(); break;
    case SCR_SETTINGS:   tickSettings(); break;
    case SCR_SET_POWERS: tickSetPowers(); break;
    case SCR_HISCORES:   tickHighScores(); break;
    case SCR_POWERUPS:   tickPowerUps(); break;
    case SCR_ABOUT:      tickAbout(); break;
    case SCR_PLAY:       tickPlay(); break;
    case SCR_GAMEOVER:   tickGameOver(); break;
    case SCR_NAMEENTRY:  tickNameEntry(); break;
    default: break;
  }

  // Render
  switch (screen) {
    case SCR_PRESPLASH:  renderPreSplash(); break;
    case SCR_SPLASH:     renderSplash(); break;
    case SCR_MENU:       renderMenu(); break;
    case SCR_SETTINGS:   renderSettings(); break;
    case SCR_SET_POWERS: renderSetPowers(); break;
    case SCR_HISCORES:   renderHighScores(); break;
    case SCR_POWERUPS:   renderPowerUps(); break;
    case SCR_ABOUT:      renderAbout(); break;
    case SCR_PLAY:       renderPlay(); break;
    case SCR_GAMEOVER:   renderGameOver(); break;
    case SCR_NAMEENTRY:  renderNameEntry(); break;
    default: break;
  }
}
