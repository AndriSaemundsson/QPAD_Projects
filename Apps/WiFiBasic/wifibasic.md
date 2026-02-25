# 📶 WiFi Basics  
### QPAD Network Module — XIAO ESP32-C6 + SSD1306

**WiFi Basics** is a compact, reusable WiFi management application designed for handheld projects using a **Seeed Studio XIAO ESP32-C6**, a **128×64 SSD1306 OLED**, and **active-LOW push buttons**.

It provides a clean user interface for scanning networks, connecting, disconnecting, forgetting saved credentials, and verifying real internet connectivity using NTP time synchronization.

---

## 🚀 Features

### 🔄 Boot & Scan
- Splash screen on startup
- Automatic WiFi scan with animated WiFi logo
- Displays number of networks found
- Waits for user confirmation before continuing
- If a saved SSID is detected, user can connect immediately or view the network list

---

### 🧭 Main Menu
Two simple items:

1. **WiFi Status**
2. **Connect / Disconnect** (dynamic based on current connection state)

---

### 🔌 Connect & Disconnect

When **not connected**:
- **Scan**
- **Connect to saved** (if credentials exist)

When **connected**:
- **Disconnect**
- **Disconnect & Forget** (clears saved SSID and password)

WiFi credentials are stored using the ESP32 `Preferences` (NVS) system under the namespace `wifibasics`.

---

### 📡 Network List
- Displays up to 3 networks at a time
- Scroll arrows indicate additional items
- `O` = Open network  
- `L` = Locked network  
- Long SSIDs scroll smoothly (no wrapping)

---

### 🔐 Password Editor
Character-based password editor supporting full printable ASCII.

Controls:
- **UP/DOWN** → Change character
- **LEFT/RIGHT** → Move cursor
- **A** → Append/advance
- **B** → Delete
- **RIGHT** → Connect

Supports standard WPA/WPA2 passphrases (8–63 printable ASCII characters).

---

### 🌐 Online Verification
Uses NTP time synchronization to confirm real internet access.

- Connected + time synced → Header shows live clock (HH:MM:SS)
- Otherwise → Header shows `OFFLINE`

The WiFi Status screen displays:
- Connection state
- SSID (scrolls if long)
- Local IP address

---

## 🖥 Hardware Requirements
- Seeed Studio XIAO ESP32-C6
- SSD1306 128×64 OLED (I2C)
- 6 active-LOW push buttons

---

## 🔌 Pin Configuration

### Buttons (active-LOW using `INPUT_PULLUP`)

| Function | Pin |
|----------|------|
| UP       | D0   |
| DOWN     | D8   |
| LEFT     | D1   |
| RIGHT    | D7   |
| A        | D10  |
| B        | D9   |

### OLED (I2C)

#define I2C_SDA D3  
#define I2C_SCL D4  

OLED address: 0x3C (fallback: 0x3D)

---

## 📦 Dependencies
- Wire
- WiFi
- Preferences
- time.h
- Adafruit_GFX
- Adafruit_SSD1306

Install Adafruit libraries via Arduino Library Manager.  
Board target: **Seeed Studio XIAO ESP32-C6**

---

## 🎯 Design Principles
- Clean 128×64 layout (header + content + footer)
- No text wrapping
- Smooth scrolling for long text
- Explicit user control (no forced auto-connect)
- Reliable online detection via NTP

WiFi Basics is intended as a reusable, polished WiFi management module for embedded ESP32 projects.