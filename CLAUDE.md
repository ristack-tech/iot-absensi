# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an **IoT-based Fingerprint Attendance System** built with PlatformIO on an ESP32 microcontroller. The system uses a fingerprint sensor (AS608) and I2C LCD display for standalone attendance tracking.

## Build Commands

```bash
# Build the project
pio run

# Upload to device (ttyUSB0)
pio run --target upload

# Monitor serial output (115200 baud)
pio device monitor

# Build and upload in one command
pio run --target upload --monitor
```

## Hardware Configuration

- **Microcontroller:** ESP32 (board: esp32dev)
- **Fingerprint Sensor:** AS608 via UART2 (GPIO 16=RX, 17=TX, baud 57600)
- **LCD Display:** I2C LCD 16x2 (GPIO 21=SDA, 22=SCL, address 0x3F)
- **Serial Monitor:** 115200 baud

## Architecture

```
src/
  main.cpp          # Main application (setup, loop, fingerprint logic)
include/            # Header files directory
lib/                # Private libraries (unused in current project)
test/               # Unit tests directory
platformio.ini      # Build configuration and dependencies
```

**Core flow in main.cpp:**
1. `setup()` - Initialize LCD, fingerprint sensor, show ready screen
2. `loop()` - Continuously scan fingerprint, apply 3-second cooldown between scans
3. `getFingerprintID()` - Capture and match fingerprint, return ID or error code
4. `showReadyScreen()` - Display "Tempel Jari Anda" on LCD

**Dependencies (platformio.ini):**
- Adafruit SSD1306 (OLED display)
- Adafruit GFX Library (graphics)
- Adafruit Fingerprint Sensor Library

## Key Behaviors

- **Double-scan prevention:** 3-second cooldown between successful scans
- **Sensor halt on failure:** If fingerprint sensor not detected, system halts
- **Fingerprint states:** Returns -1 (no finger), -2 (not matched), or positive ID (matched)
