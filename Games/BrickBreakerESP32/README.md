# 🧱 Brick Breaker Arcade  
### (for QPAD — Button Edition + XIAO ESP32-C6)

Welcome to **Brick Breaker Arcade** — a tiny pocket brick-smashing machine running on a **128×64 SSD1306 OLED** and powered by a **Seeed Studio XIAO ESP32-C6**.

This build is designed for a **sibling / fork of the QPAD platform** that uses **physical buttons instead of capacitive touch** and runs on an **ESP32-based XIAO**.

⚠️ IMPORTANT  
This version is specifically written for:
- XIAO ESP32-C6
- Physical buttons (active-LOW with internal pullups)
- The exact pin assignments listed below

If you change pins or board type, update the `#define` values in the sketch.

---

## 🎮 What’s Inside?

### 🧱 Brick Breaker

A full-featured mini arcade experience:

- 8 handcrafted level layouts (looping progression)
- Increasing difficulty on later loops
- 1–5 lives (configurable)
- Up to 3 balls simultaneously (multi-ball)
- Sub-stepped physics (prevents brick tunneling)
- Anti-stuck watchdog for ceiling/corner stalls
- Pause menu (Resume / Restart / Quit)
- Clean HUD with:
  - Score display
  - Heart-based life indicator
  - Active power-up icons

Scoring includes:
- Per-brick scoring by type
- Tough and metal bricks with multiple HP
- Level clear bonus (+250)

---

## ⚡ Power-Ups System

Bricks can drop falling power-ups. Most timed effects last ~9 seconds.

Included power-ups:

- Expand — Wider paddle  
- Shrink — Narrower paddle  
- Slow — Slower ball  
- Fast — Faster ball  
- Sticky — Ball sticks to paddle until A is pressed  
- Invert — Reverses left/right controls  
- Life — +1 life (up to max)  
- Points — Instant score bonus  
- Triple — Splits into up to 3 balls  
- Nail — Ball pierces through bricks  
- Shield — Saves you once from losing all balls  

You can enable/disable individual power-ups in:

Settings → Power-Ups

The firmware prevents disabling the last remaining enabled power-up.

---

## 🧠 UI & Navigation

This is a complete mini-console style firmware:

- Pre-splash screen
- Animated splash screen (ball + paddle + breakable bricks)
- Main menu
- Settings menu
- Power-up configuration screen
- High scores screen
- About screen
- Pause overlay during gameplay
- Wipe + shutter screen transitions
- Blinking selector indicators
- Subtle “alive” pulse indicator on idle screens

---

## 🏆 Persistent High Scores

Stored using ESP32 Preferences (NVS).

- Top 10 scores
- Each entry stores:
  - score (int32)
  - initials (3 chars)
- New high scores trigger:
  - Notification screen
  - 3-letter name entry UI

Name Entry Controls:
- UP / DOWN → Change letter
- A → Next letter / Confirm
- B → Previous letter / Back

---

# 🔌 Hardware Configuration

## 🧠 Microcontroller

- Seeed Studio XIAO ESP32-C6

---

## 🖥️ OLED Display

- SSD1306 128×64
- I2C address: 0x3C (also attempts 0x3D)

### I2C Pins (defined in code)

```cpp
#define I2C_SDA D3
#define I2C_SCL D4
```

Physical Wiring:
- SDA → D3
- SCL → D4

Wire speed:
- 100 kHz during detection
- 400 kHz after successful initialization

---

## 🎛️ Button Wiring (Active-LOW)

All buttons:
- Use INPUT_PULLUP
- Are active-LOW
- Include debounce and hold-repeat logic

### Pin Assignments (exactly as used in code)

```cpp
#define BTN_UP     D0
#define BTN_DOWN   D8
#define BTN_LEFT   D1
#define BTN_RIGHT  D7
#define BTN_A      D10
#define BTN_B      D9
```

### Full Pin Map

| Function | Pin |
|----------|-----|
| UP       | D0  |
| DOWN     | D8  |
| LEFT     | D1  |
| RIGHT    | D7  |
| A        | D10 |
| B        | D9  |
| OLED SDA | D3  |
| OLED SCL | D4  |

If your hardware differs, update these defines in the sketch.

---

# 🎮 Controls

## Gameplay

- LEFT / RIGHT → Move paddle  
- A → Launch ball (when stuck / before serve)  
- B → Pause  

## Pause Menu

- UP / DOWN → Navigate  
- A → Select  
- B → Exit pause  

## Menus

- UP / DOWN → Navigate  
- A → Select  
- B → Back  

## Settings

- LEFT / RIGHT → Adjust values  
- Hold LEFT / RIGHT → Repeat adjust  
- A → Enter Power-Ups editor  
- B → Back  

---

# 📦 Dependencies

Required libraries:

- Wire
- Adafruit_GFX
- Adafruit_SSD1306
- Preferences (ESP32 NVS storage)

Board support:
- ESP32 core with XIAO ESP32-C6 selected

---

# 🛠️ Flashing

1. Install ESP32 board support in Arduino IDE
2. Select "XIAO ESP32-C6"
3. Wire OLED and buttons exactly as specified
4. Upload the sketch
5. Press A on the splash screen
6. Break bricks

---

# 🧪 Technical Notes

- Sub-step physics prevents high-speed collision skipping
- Ceiling/corner watchdog unsticks trapped balls
- Custom 7×7 pixel glyph icons for power-ups
- Ball speed normalization per difficulty
- Preferences-based storage for:
  - Settings
  - Power-up mask
  - High scores

---

# 📜 License

Do whatever you want with it — remix it, fork it, improve it, ship it to space.

Just don’t blame the paddle.