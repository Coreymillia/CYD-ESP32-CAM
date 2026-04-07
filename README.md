# StickVCam — Wireless Camera System

A multi-device wireless camera system built around the M5Stack StickV and an ESP32-CAM, displayed on an ESP32-2432S028R (CYD) touchscreen.

---

## System Overview

```
┌─────────────┐  Grove UART   ┌─────────────┐
│  M5StickV   │──────────────▶│ ESP32-C3    │  WiFi ─┐
│  K210+OV7740│  115200 baud  │ Relay       │        │
└─────────────┘               └─────────────┘        ▼
  Manual snapshots              Serves JPEG    ┌─────────────┐
  Button A = capture             /latest.jpg   │ CYD Display │
  Saved to SD card                             │ ESP32 ILI9341│
                                               └─────────────┘
┌─────────────┐  WiFi                               ▲
│ ESP32-CAM   │─────────────────────────────────────┘
│ AI Thinker  │  Connects to CYD SoftAP
│ OV2640      │  Serves /latest.jpg on demand
└─────────────┘
  Live feed source
```

The CYD creates a WiFi SoftAP (`StickVCam`). Both the ESP32-C3 relay and ESP32-CAM connect to it. The CYD auto-discovers them and operates in two modes:

- **LIVE mode** — Polls the ESP32-CAM every 10 seconds, shows full-screen live feed
- **GALLERY mode** — Browse the last 24 saved photos from the StickV (SPIFFS ring buffer)

---

## Hardware

| Device | Board | Role |
|---|---|---|
| Camera | M5Stack StickV (K210 + OV7740) | Manual snapshots via button press |
| Relay | ESP32-C3 Super Mini | Receives JPEG over UART, serves via WiFi |
| Live cam | AI Thinker ESP32-CAM (OV2640) | Continuous live feed, serves JPEG on demand |
| Display | ESP32-2432S028R (CYD) | WiFi AP, displays live feed + gallery, touchscreen UI |

---

## CYD Touch Controls

| Zone | LIVE mode | GALLERY mode |
|---|---|---|
| **Centre (upper)** | Toggle → GALLERY | Toggle → LIVE |
| **Bottom-centre** | ◉ CAPTURE (save to gallery + SD) | — |
| **Left edge** | — | Older photo |
| **Right edge** | — | Newer photo |
| **Status bar (bottom)** | Cycle filter | Cycle filter |

**Filters:** None → Grayscale → Invert → Sepia (applied at draw time, no device changes needed)

---

## Status Bar

```
[HH:MM:SS]   [LIVE / filter name]   [cam IP / photo index]
```

---

## Boot Settings Menu

Hold the **centre of the screen** during the 3-second boot countdown to enter the settings menu.

**Page 1 — Set Time:** Adjust HH and MM with +/− buttons. Time is saved to NVS flash and restored on next boot (approximate — no hardware RTC).

**Pages 2–4 — Schedules 1–3:** Enable/disable each slot and set a daily HH:MM auto-capture time. When the clock reaches a scheduled time, the CYD automatically captures a frame from the ESP32-CAM and saves it to gallery + SD card.

Press **SAVE** on the last page to commit all changes.

> **Clock accuracy:** The software clock drifts slightly (~1–2 min/day). It restores from the last saved time on reboot. For precision timing, add a DS3231 RTC module on I2C.

---

## SD Card (CYD)

Insert a FAT32-formatted microSD card into the CYD's SD slot. Manual captures (shutter button) and scheduled captures are saved as:

```
/photos/cap00001.jpg
/photos/cap00002.jpg
...
```

The counter persists across reboots via NVS. The live feed auto-poll does **not** write to SD — only intentional captures (manual or scheduled) do.

---

## Project Structure

```
StickV/
├── main.py                     ← M5StickV camera firmware (upload as /flash/boot.py)
├── maixpy_v0.6.3_m5stickv.bin  ← MaixPy firmware binary
│
├── ESP32Cam/                   ← AI Thinker ESP32-CAM PlatformIO project
│   ├── platformio.ini
│   └── src/main.cpp
│
├── ESP32Cam_CYD/               ← CYD display PlatformIO project
│   ├── platformio.ini
│   └── src/main.cpp
│
└── StickVRelay_C3/             ← ESP32-C3 relay PlatformIO project
    ├── platformio.ini
    └── src/main.cpp
```

---

## Flash / Setup

### 1 — M5StickV (MaixPy firmware)
```bash
pip install kflash
python3 kflash.py -p /dev/ttyUSB0 -b 1500000 maixpy_v0.6.3_m5stickv.bin
```
Then upload `main.py` as `/flash/boot.py` via mpremote or ampy.

### 2 — ESP32-CAM
```bash
cd ESP32Cam
pio run -t upload --upload-port /dev/ttyUSB0
```
> GPIO0 must be pulled LOW during boot to enter flash mode.

### 3 — ESP32-C3 Relay
```bash
cd StickVRelay_C3
pio run -t upload --upload-port /dev/ttyUSB0
```

### 4 — CYD Display
```bash
cd ESP32Cam_CYD
pio run -t upload --upload-port /dev/ttyUSB0
```

### Boot order
1. Power CYD first (creates the `StickVCam` AP)
2. Power ESP32-CAM and/or C3 relay (auto-connect to AP)
3. CYD discovers devices within ~10 seconds and starts the live feed

---

## StickV Wiring (Grove → C3 Relay)

| Grove wire | StickV pin | C3 GPIO |
|---|---|---|
| White | CONNEXT_A / pin 35 (TX) | GPIO20 (RX) |
| Black | GND | GND |
| Red (3.3V) | — | Leave unconnected |

---

## Known Issues

### StickV LCD is blank
The StickV LCD does not show a live preview. Photos save and transmit correctly — the blank screen does not affect functionality.

### Clock resets on power loss
Without a hardware RTC, the clock is restored from the last NVS-saved time. This is approximate; if the board was off for a long time the time will be off. Set it via the boot settings menu.


A manual snapshot camera built on the [M5Stack StickV](https://docs.m5stack.com/en/core/stickv), a tiny K210 RISC-V AI module with a built-in OV7740 camera sensor.

---

## What It Does

- **Press Button A** (front button) → takes a photo, white LED flashes as a shutter indicator
- **Press Button B** (side button) → toggles preview mode on/off *(see Known Issues)*
- Photos are saved automatically to the MicroSD card
- Can snap photos rapidly, back to back
- On boot: plays a chime, then enters the camera loop ready to shoot

---

## Hardware

| Component | Details |
|---|---|
| Board | M5Stack StickV |
| Chip | Kendryte K210 (RISC-V dual-core, 400 MHz) |
| Camera | OV7740 (640×480 max) |
| Display | 1.14" ST7789 LCD, 240×135 px |
| Storage | MicroSD card (FAT32 formatted) |
| Firmware | MaixPy v0.6.3 (MicroPython for K210) |

---

## Photo Output

- **Location:** `/sd/photos/img_0000.jpg`, `img_0001.jpg`, ...
- **Resolution:** QVGA (320×240)
- **Format:** JPEG, quality 85
- Filenames auto-increment — no duplicates across sessions

---

## Setup

### Requirements
- MicroSD card formatted as FAT32
- MaixPy v0.6.3 firmware flashed to the device
- `kflash` or `kflash_gui` for firmware flashing

### Flash Firmware
```bash
pip install kflash
python3 kflash.py -p /dev/ttyUSB0 -b 1500000 maixpy_v0.6.3_m5stickv.bin
```

### Upload Camera Script
The camera runs from `/flash/boot.py` on the device. Use any MaixPy-compatible uploader (e.g., `mpremote`, `ampy`, or the raw REPL serial method).

---

## Known Issues

### Screen is blank
The LCD does not display a live preview or any UI. `lcd.display()` is called without error but produces no visible output. `lcd.clear()` also has no visible effect after sensor initialization.

**What was tried:**
- Multiple LCD init orderings (before sensor, after sensor, re-init after sensor)
- Various frame sizes (QVGA 320×240, resized to LCD native 240×135)
- `lcd.display(img)`, `lcd.display(img.resize(240,135))`, `lcd.clear(color)`
- Exec'd `main.py` vs code directly in `boot.py`
- Plain `lcd.init()`, `lcd.init(freq=15000000)`

**Current theory:** The M5StickV is primarily designed as an AI inference *module* (meant to be embedded in larger projects), not a standalone camera. The LCD driver in this MaixPy build may require a specific initialization sequence or firmware version that differs from what is currently flashed. The original factory firmware showed a static error screen, but live sensor-to-LCD display has not been achieved.

**Photos are unaffected** — the blank screen does not impact photo capture or saving.

---

## File Structure

```
/flash/
  boot.py         ← camera app (runs on every boot)
  boot_orig.py    ← original factory firmware (backup)
  startup.jpg     ← M5Stack boot screen image
  ding.wav        ← boot chime audio

/sd/
  photos/
    img_0000.jpg
    img_0001.jpg
    ...
```

---

## How It Was Built

1. Flashed open-source [MaixPy](https://github.com/sipeed/MaixPy) firmware (v0.6.3) via `kflash`
2. Reverse-engineered the original `boot.py` factory script for correct GPIO pin numbers, audio init, and sensor retry loop
3. Replaced the KPU face-detection demo loop with a manual snapshot camera loop
4. Debugged serial output via raw REPL to confirm SD saves and identify the LCD issue

---

## Future Ideas

- [ ] Fix LCD preview (try a different MaixPy build or ST7789 init sequence)
- [ ] Timelapse mode (auto-snap every N seconds)
- [ ] Use as an AI module: color blob tracking, face detection via KPU
- [ ] WiFi transfer if paired with an ESP32 companion board
