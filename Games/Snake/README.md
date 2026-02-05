# 🐍 Snake Arcade Deluxe (for QPAD + XIAO RP2040)

Welcome to **Snake Arcade Deluxe** — a tiny pocket-sized arcade cabinet living inside a **128×64 OLED** and powered by pure microcontroller chaos. 😈✨  
Two game modes, snappy touch controls, persistent high scores, and just enough UI polish to make it feel like a *real* mini console.

Built for the **QPAD** (touch-pad controller + XIAO RP2040) and designed to be fun, fast, and delightfully over-engineered. 🕹️

---

## 🎮 What’s inside?

### 🐍 Classic Snake
The classic you know and love — but with a few spicy upgrades:

- **Smooth grid-based snake movement** on a 32×13 playfield  
- **Pulsing food animation** 🍎✨  
- **Difficulty levels** (Easy / Normal / Hard) that adjust game tick speed  
- **Walls setting**
  - **Solid walls** = crash and burn 💥
  - **Wrap mode** = wormhole vibes 🌀
- **Pause overlay** so you can breathe for once 😮‍💨

Score system is simple and honest:
- **+1 point per food** on *all* difficulties (no funny business).

---

### 🧱 Maze Trial
A survival mode where the goal is not to win… it’s to **last longer than your previous self**. 😤

- Generates a **static maze** with configurable density:
  - **Low / Medium / High** maze clutter
- **Timer-based score** ⏱️ (your score = seconds survived)
- **Hard mode hazards**: subtle edge chaos that occasionally toggles extra blocked tiles 👀  
- Solid boundaries (no wrap) because this mode is *serious*. 🗿

---

## 🧠 Menu & UI polish
This isn’t just “press reset and pray” firmware — it’s a whole tiny UX:

- **Splash screen** with animated intro + “Press A to start”  
- A **scrollable menu** with:
  - Classic Snake  
  - Maze Trial  
  - Settings  
  - High Scores  
  - About
- **Attract-mode snake** slithering around the menu screen like it owns the place 🐍💅  
- A satisfying **wipe transition** between screens for maximum arcade vibes 🌈

---

## 🏆 High scores that actually persist
High scores are stored in **EEPROM**, so they survive resets and power cycles like a true leaderboard should. 💾

- **Top 5** scores per:
  - Mode (**Classic / Maze**)
  - Difficulty (**Easy / Normal / Hard**)
- Each entry stores:
  - `score (uint16)`
  - `initials (3 chars)`
- If you earn a spot:
  - 🎉 **Celebration screen** with particle sparks  
  - ✍️ **Initials entry UI** (UP/DOWN changes character, LEFT/RIGHT moves cursor)

---

## 👆 Controls (Touch Pads)
This project uses capacitive-ish touch timing (charge/discharge measurement) for a satisfying “buttonless” controller feel.

- **D-Pad**: UP / DOWN / LEFT / RIGHT  
- **A**
  - Start / select
  - Pause toggle during gameplay
- **B**
  - Back / return (context-sensitive)
- **A + B (hold)**
  - **Emergency exit to menu** 🚪🏃‍♂️ (because life happens)

Touch pads are auto-calibrated on boot with a baseline + threshold margin so it behaves nicely across different environments and fingers. 🤌

---

## 🧩 Hardware / Dependencies
- **Seeed XIAO RP2040**
- **SSD1306 128×64 OLED** (I2C @ `0x3C`)
- Libraries:
  - `Adafruit_GFX`
  - `Adafruit_SSD1306`
  - `EEPROM`

---

## 🛠️ How to use
1. Flash the sketch to your XIAO RP2040
2. Boot it up
3. Touch the pads
4. Get humbled by a snake 🐍

---

## 🙌 Shoutout / Credits
Huge shoutout to **Quentin Bolsee** — creator of the **QPAD** — for the awesome platform that makes projects like this ridiculously fun to build on. 💛🔥

QPAD project page:
https://flex.cba.mit.edu/quentinbolsee/qpad-xiao/

(Seriously — go check it out.)

---

## 🧪 Fun details (for the curious)
- The playfield is rendered as a **4×4 pixel grid cell system** for fast drawing
- Uses a lightweight XORSHIFT RNG for food placement, maze generation, and spark effects
- The menu snake avoids scribbling over your menu text because it has ✨manners✨

---

## 📜 License
Do whatever you want with it — remix it, fork it, break it, improve it, ship it to space. 🚀  
(Just don’t blame the snake.)
