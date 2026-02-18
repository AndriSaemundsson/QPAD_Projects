# 📡 WiFi Tool Console  
### (for QPAD — Button Edition + XIAO ESP32-C6)

Welcome to **WiFi Tool Console** — a tiny **WiFi utility dashboard** for your QPAD-style handheld, running on a **128×64 SSD1306 OLED** and powered by a **Seeed Studio XIAO ESP32-C6**.

It’s built for a **button-based QPAD sibling/fork** (active-LOW buttons with pullups), and keeps everything readable on the small OLED with clean **header + footer bars**.

⚠️ IMPORTANT  
This version is specifically written for:
- **XIAO ESP32-C6**
- **Physical buttons (active-LOW with internal pullups)**
- **The exact pin assignments listed below**
- **SSD1306 128×64 OLED over I2C**

If you change pins or board type, update the `#define` values in the sketch.

---

## ✅ What This App Does

This firmware is a compact “network toolbox” you can carry around:

### 📶 1) Scan WiFi Networks
- Scans nearby SSIDs
- Shows **signal strength (RSSI bars)** and **lock/unlock** icon for security
- Scrollable list (fits the OLED without overlap)
- Hold **A** to rescan quickly

### 🔑 2) Connect to WiFi (with Password Editor)
- Select an SSID and connect
- Open networks skip password entry automatically
- Secured networks open a password editor:
  - Cursor movement
  - Character cycling with **hold repeat**
  - Mask/visible toggle
- Connection flow includes:
  - Cancel option
  - Timeout handling
  - Best-effort failure hints based on disconnect reason
- Successful connects can optionally store credentials in NVS

### 💾 3) Save + Forget WiFi
- Saves last connected SSID/password in **ESP32 Preferences (NVS)**
- “Forget WiFi” clears stored credentials
- On boot, if a saved SSID is nearby, it offers a reconnect prompt ✅

### 🕒 4) Clock (NTP) + Time Zone
- NTP sync using:
  - `pool.ntp.org`
  - `time.google.com`
  - `time.cloudflare.com`
- Time zone picker is a simple **UTC offset** selector (−12 … +14)
- Time appears in the title bar when online/time synced

### 🌍 5) Status + Real Internet Check
- Shows connection status, SSID, RSSI, channel, IP info
- Performs a real “is the internet reachable?” check using:
  - `https://connectivitycheck.gstatic.com/generate_204`
- Detects common captive portal behavior (redirect/200 instead of 204)

### 🧠 6) WiFi Info (Link + Network Details)
Shows useful “what am I connected to?” facts:
- **SSID**
- **BSSID** (AP MAC address)
- Channel / RSSI
- **STA MAC** (your ESP32’s WiFi MAC)
- IP / Gateway / DNS / Subnet mask

### 🌐 7) Public IP (WhatsMyIP style)
- Fetches your **public IP** via HTTPS:
  - `https://api.ipify.org`
- Displays public + local IP on-screen
- Press **A** to refresh

### 🔎 8) Discovery (mDNS Advertisers)
- Press **A** to run an mDNS discovery scan
- Shows devices/services that **actively advertise via mDNS**
- Queries a few common service types:
  - HTTP (`_http._tcp`)
  - Workstation (`_workstation._tcp`)
  - Printing (`_ipp._tcp`)
  - AirPlay (`_airplay._tcp`)
  - Chromecast (`_googlecast._tcp`)

⚠️ IMPORTANT (Discovery)  
This is **not a full list of every device connected to your WiFi**.  
It only shows devices that broadcast **mDNS services**. Many devices stay silent and won’t appear.

---

## 🧼 UI & Navigation

This is a clean mini-console UI designed for 128×64:

- Top header bar with a separator line
- Bottom footer bar for button hints (prevents text overlap)
- List screens are limited to **3 visible rows** when header+footer are present
- “Alive” pulse dot indicator
- Spinners for scanning/connecting/fetching states
- Toast-style message screens for feedback and errors

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

### Full Display Wiring
- SDA → **D3**
- SCL → **D4**
- VCC → 3V3
- GND → GND

Wire speed:
- 400 kHz I2C clock after init

---

## 🎛️ Button Wiring (Active-LOW)

All buttons:
- Use `INPUT_PULLUP`
- Are **active-LOW**
- Have debounce + hold detection
- Password editor uses hold-repeat for UP/DOWN

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

## Global Navigation
- **UP / DOWN** → Move selection / switch pages (some screens)
- **A** → Select / confirm / refresh / scan (depends on screen)
- **B** → Back / cancel

## WiFi Scan List
- **UP / DOWN** → Move through networks  
- **A** → Select network  
- **B** → Back  
- **Hold A** → Rescan  

## Password Editor
- **LEFT / RIGHT** → Move cursor  
- **UP / DOWN** → Change character (hold repeats)  
- **A** → Advance / append character  
- **B** → Delete (hold B = back)  
- **Hold RIGHT** → Connect  

## Clock
- **A** → NTP sync  
- **RIGHT** → Time zone picker  
- **B** → Back  

## Status
- **A** → Refresh internet check  
- **UP / DOWN** → Switch status page  
- **B** → Back  

## Public IP
- **A** → Fetch/refresh public IP  
- **B** → Back  

## Discovery (mDNS)
- **A** → Scan/refresh mDNS advertisers  
- **UP / DOWN** → Move selection  
- **B** → Back  

---

# 📦 Dependencies

## Required Libraries
- `Wire`
- `WiFi`
- `Preferences`
- `time.h`
- `WiFiClientSecure`
- `HTTPClient`
- `ESPmDNS`
- `Adafruit_GFX`
- `Adafruit_SSD1306`

Install Adafruit libs via Library Manager:
- **Adafruit GFX Library**
- **Adafruit SSD1306**

## Board Support
- Arduino IDE / PlatformIO with **ESP32 core** installed
- Select board: **Seeed Studio XIAO ESP32-C6**

---

# 🔐 Safety / Legal Notes

This tool is designed to stay on the safe side:
- It scans and connects like a normal WiFi client
- It shows your device’s network information and public IP
- Discovery is limited to **mDNS advertisements** (what devices voluntarily broadcast)

It does **not**:
- Capture packets
- Deauth devices
- Break passwords
- Enumerate all clients on the network
- Perform intrusive scanning

Use it on networks you own or have permission to test. ✅

---

# 🛠️ Flashing

1. Install ESP32 board support in Arduino IDE
2. Select **Seeed Studio XIAO ESP32-C6**
3. Wire OLED + buttons exactly as specified
4. Install Adafruit SSD1306 + GFX libraries
5. Upload the sketch
6. Boot into the menu
7. Scan → Connect → Explore tools 😄

---

# 🧪 Technical Notes

- Credentials stored with **Preferences (NVS)** under namespace `wifiscn`
- Time zone stored as signed UTC hour offset
- TLS uses ESP32 Arduino CA bundle:
  - `_binary_x509_crt_bundle_start`
  - `_binary_x509_crt_bundle_end`
- Internet check uses HTTPS `generate_204`
- OLED layout:
  - Header (title + time/online status)
  - Content area
  - Footer (button hints) to prevent overlap

---

# 📜 License

Do whatever you want with it — remix it, fork it, improve it, ship it to space. 🚀  

Just don’t blame the WiFi when the router is in the basement.