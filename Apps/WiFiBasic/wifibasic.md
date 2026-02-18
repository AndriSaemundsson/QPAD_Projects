# 📶 WiFi Basics  
### (for QPAD — Button Edition + XIAO ESP32-C6)

Welcome to **WiFi Basics** — a small, reusable **network “module app”** for QPAD-style handheld projects.

It’s built for a **128×64 SSD1306 OLED**, **active-LOW buttons**, and a **Seeed Studio XIAO ESP32-C6**.  
The goal is simple: **connect to WiFi when you want**, **disconnect when you want**, and **know if you’re truly online** (via an NTP “RTC-like” clock).

⚠️ IMPORTANT  
This version is specifically written for:
- **XIAO ESP32-C6**
- **Physical buttons (active-LOW with internal pullups)**
- **The exact pin assignments listed below**
- **SSD1306 128×64 OLED over I2C**

If your pins or board differ, update the `#define` values in the sketch.

---

## ✅ What This App Does

This firmware is a clean “Network” tab you can reuse across many projects.

---

### 🚀 1) Splash Screen
- Shows **“WiFi Basics”** on boot
- Stays for **10 seconds** or skips early if **any button** is pressed  
- No hints, no clutter — just a proper boot splash

---

### 🧭 2) Network Main Menu (3-item layout)
Designed for 128×64 with **header + footer**, so the content area stays clean.

**Menu items:**
1. **Connect to WiFi**
2. **WiFi Status**
3. **Action (dynamic):**
   - **Disconnect WiFi** (when connected)
   - **Reconnect Saved** (when offline but saved creds exist)
   - **Scan Networks** (when offline and nothing saved)

---

### 🔑 3) Connect to WiFi (no auto-connect)
This is important: **it never assumes** you want to connect.

- If saved credentials exist, it asks first:
  - **A: Connect**
  - **B: Scan**
- If no saved creds exist:
  - Goes straight to **Scan → Select → Connect**

---

### 📡 4) Scan Nearby Networks
- Scans nearby SSIDs and shows a **scrollable list**
- List view respects the UI rules (header + footer + **3 visible rows max**)
- Shows a tiny indicator:
  - `O` = Open network
  - `L` = Locked network

---

### 🔠 5) Password Entry (simple + fast)
For secured networks, you get a compact password editor:
- **UP/DOWN** changes the current character
- **LEFT/RIGHT** moves cursor
- **A** appends/advances
- **B** deletes (backspace)
- **RIGHT** begins connection attempt

This is meant to be “good enough” without heavy UI overhead.

---

### 🌐 6) Online Verification via NTP (RTC-like clock)
This project uses **NTP time sync** as a practical “online verification”.

- If NTP time is synced, the header shows a live clock: **HH:MM:SS**
- If not synced, header shows: **OFFLINE**
- This acts like a lightweight “RTC”: once synced, time continues to update while powered.

---

### 📟 7) WiFi Status Screen (minimal + readable)
Status screen is intentionally short and uncluttered:

- **WiFi:** connected / not connected  
- **SSID:** current SSID (trimmed)  
- **IP:** local IP address  

Press **A** to re-sync time (re-verify online), **B** to go back.

---

## 🧼 UI & Layout Rules

This app is strict about being readable on 128×64:

- Header bar with title + right-aligned status/time
- Footer bar reserved for button hints
- Content area never overlaps footer
- List views show **3 items max** when header/footer are present

This is meant to feel **shippable**, not “debuggy”.

---

# 🔌 Hardware Configuration

## 🧠 Microcontroller
- **Seeed Studio XIAO ESP32-C6**

---

## 🖥️ OLED Display
- **SSD1306 128×64**
- I2C address: **0x3C** (also attempts **0x3D**)

### I2C Pins (defined in code)

```cpp
#define I2C_SDA D3
#define I2C_SCL D4
```

---

## 🎛️ Button Wiring (Active-LOW)

All buttons:
- Use `INPUT_PULLUP`
- Are **active-LOW**
- Debounced in software

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

---

# 🎮 Controls

## Global
- **UP / DOWN** → Move selection
- **A** → Select / confirm
- **B** → Back / cancel

## Network Main Menu
- **UP / DOWN** → Select item
- **A** → Enter selected item

## Saved WiFi Prompt
- **A** → Connect to saved WiFi
- **B** → Scan networks instead

## WiFi Scan List
- **UP / DOWN** → Move through networks  
- **A** → Select network  
- **B** → Back  

## Password Entry
- **LEFT / RIGHT** → Move cursor  
- **UP / DOWN** → Change character  
- **A** → Append/advance  
- **B** → Delete  
- **RIGHT** → Connect  

## WiFi Status
- **A** → Sync time (NTP) / verify online  
- **B** → Back  

---

# 📦 Dependencies

## Required Libraries
- `Wire`
- `WiFi`
- `Preferences`
- `time.h`
- `Adafruit_GFX`
- `Adafruit_SSD1306`

Install Adafruit libs via Library Manager:
- **Adafruit GFX Library**
- **Adafruit SSD1306**

## Board Support
- Arduino IDE / PlatformIO with **ESP32 core** installed
- Select board: **Seeed Studio XIAO ESP32-C6**

---

# 🧪 Technical Notes

- Credentials stored using **Preferences (NVS)** under namespace: `wifibasics`
  - keys: `ssid`, `pass`
- “Online” is determined by:
  - WiFi connected **and**
  - NTP time synced (clock available)
- OLED layout:
  - Header (title + clock/Offline)
  - Content
  - Footer (button hints)

---

# 🛠️ Flashing

1. Install ESP32 board support in Arduino IDE
2. Select **Seeed Studio XIAO ESP32-C6**
3. Wire OLED + buttons exactly as specified
4. Install Adafruit SSD1306 + GFX libraries
5. Upload the sketch
6. Boot → Network → Connect / Status / Disconnect ✅

---

# 📜 License

Do whatever you want with it — fork it, remix it, ship it, embed it into every project forever. 🚀  
If it says OFFLINE, that’s between you and your router.