# Overview

This project turns mechanical split-flap hardware into a smart, internet-connected clock. It uses an ESP32 to manage NTP time synchronization, precise stepper motor control, sensor-based calibration, and a comprehensive web-based dashboard for configuration and maintenance.

## Key Features

### 🕒 Timekeeping & Display
* **NTP Synchronization:** Keeps precise time via the internet using `pool.ntp.org`.
* **Global Timezone Support:** Features a curated list of major global timezones with automatic Daylight Saving Time (DST) adjustments.
* **12/24 Hour Modes:** Easily toggle between formats via the web UI.
* **Alternating Date Display:** Optionally cycles the display to show the current date (Month/Day) at configurable intervals.

### ⚙️ Mechanics & Calibration
* **Sensor-Based Homing:** Uses Hall effect sensors and magnets to automatically find the `00:00` position.
* **Precision Motor Calibration:** A specialized "Full Calibration" routine spins the motors for two complete revolutions to accurately calculate the exact steps per revolution, compensating for mechanical variations.
* **Smart Power Save:** Disables stepper motor outputs when idle to reduce heat and power consumption. Includes a "snap-to-grid" software fix to prevent mechanical drift when motors de-energize.
* **Auto-Home Maintenance:** Configurable interval to automatically re-home the clock (e.g., every 24 hours) to correct any long-term drift.

### 💡 Lighting Control
* **Four-Channel LED Support:** Individual controls for:
  * Internal Status LED
  * Blinking Colon (PWM dimmable)
  * AM/PM Indicator
  * Auxiliary/Backlight strip
* **Web UI Dimming:** Individual brightness sliders and on/off toggles for each LED channel.

### 🌐 Web Interface & Connectivity
* **Responsive Dashboard:** A modern, mobile-friendly web UI hosted directly on the ESP32.
* **Live Status:** Real-time display of Time, Date, WiFi signal strength, sensor readings, and calibration accuracy.
* **WiFiManager:** Easy initial setup via a captive portal—no hardcoding WiFi credentials.
* **Night Mode:** Automatically disables motor movements and turns off displays during user-defined sleeping hours.
* **OTA Updates:** Upload new firmware binaries wirelessly directly through the web browser.

## Hardware Requirements
* **MCU:** ESP32 Development Board
* **Drivers:** 2x ULN2003 Stepper Drivers
* **Motors:** 2x 28BYJ-48 Stepper Motors (5V) 
* **Sensors:** 2x Hall Effect Sensors (analog output recommended, e.g., A3144 or KY-003/KY-024 modules) & 2x small magnets mounted on the flap spools.
* **Power Supply:** 5V DC PSU (min 2A recommended).

## Setup & First Run
1. **Flash Firmware:** Upload the code to your ESP32 using the Arduino IDE.
2. **Connect WiFi:** On first boot, connect to the WiFi access point named `SplitFlapClockSetup`. A captive portal should appear allowing you to select your home WiFi network.
3. **Access Dashboard:** Once connected, find the ESP32's IP address on your router, or try navigating to `http://splitflap.local`.
4. **Initial Calibration:**
   * Go to the "Sensor Tuning" section.
   * Click the orange **"Calibrate Sensors (Home)"** button. The clock will spin to find the magnets and define the `00:00` point.
   * For best accuracy, perform a **"Calibrate Motors (Full)"** run. This will spin the flaps multiple times to count the exact steps for your specific hardware.

## License
This project is open-source. Feel free to modify and share.
Based on the origional project from Adam-Simon1

## 🔌 Hardware Pinout

### ⚙️ Stepper Motors (ULN2003 Driver)
*Note: The sequence IN1-IN3-IN2-IN4 is standard for the AccelStepper library with 28BYJ-48 motors.*

| Motor (Spool) | Driver Pin | ESP32 Pin | Notes |
| :--- | :--- | :--- | :--- |
| **HOURS** (Left) | IN1 | **GPIO 26** | |
| | IN2 | **GPIO 33** | |
| | IN3 | **GPIO 25** | |
| | IN4 | **GPIO 32** | |
| **MINUTES** (Right) | IN1 | **GPIO 27** | |
| | IN2 | **GPIO 12** | |
| | IN3 | **GPIO 14** | |
| | IN4 | **GPIO 13** | |

### 🧲 Sensors & Peripherals

| Component | ESP32 Pin | Type | Notes |
| :--- | :--- | :--- | :--- |
| **Sensor (Hours)** | **GPIO 35** | Analog Input | *Input Only Pin* |
| **Sensor (Minutes)** | **GPIO 34** | Analog Input | *Input Only Pin* |
| **Status LED** | **GPIO 2** | Output | On-board Blue LED |
| **Colon (:) LEDs** | **GPIO 4** | PWM Output | Supports Dimming |
| **AM/PM Indicator** | **GPIO 18** | Output | |
| **Aux / Backlight** | **GPIO 23** | Output | |

---

### ⚠️ Wiring Notes
1.  **Power Supply:** Do **not** power the stepper motors directly from the ESP32's 5V/VIN pin if you are powering the ESP32 via USB. The motors draw too much current. Use a dedicated 5V power supply (min 2A) and split the power to the ESP32 and the Motor Drivers.
2.  **Common Ground:** Ensure the **GND** of your external power supply is connected to the **GND** of the ESP32.
3.  **Sensors:** If using generic Hall modules (like KY-003 or KY-024), connect VCC to 3.3V or 5V and Output to the assigned GPIO.
