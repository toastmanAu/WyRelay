# Recommended Hardware

## Primary Target — LilyGO T-Dongle S3

The ideal WyRelay hardware. USB-A plug form factor — looks like a flash drive.

- **SoC:** ESP32-S3 (native USB HID)
- **Form factor:** USB-A plug — plug directly into any machine
- **Display:** 0.96" TFT (shows WiFi status, connection state)
- **Flash:** 16MB
- **Buy:** ~$12-15 AUD on AliExpress

### T-Dongle S3 Pinout (for display)
Display is ST7735 80x160, connected internally. No extra wiring needed.

---

## Development/Testing — ESP32-S3 DevKitC-1 N16R8

What we built and tested WyRelay on first.

- **SoC:** ESP32-S3 (native USB HID)
- **Flash:** 16MB, 8MB PSRAM
- **Two USB-C ports:** Left=UART (flash), Right=Native USB (HID output)
- **Buy:** ~$8-12 AUD on AliExpress

### Wiring
No extra wiring needed — just two USB-C cables.

---

## Other Compatible Boards

Any ESP32-S2 or ESP32-S3 board with native USB support:

| Board | USB HID | Form Factor | Notes |
|---|---|---|---|
| T-Dongle S3 | ✅ | USB-A dongle | Best for production |
| ESP32-S3 DevKitC-1 | ✅ | Dev board | Best for development |
| Waveshare ESP32-S2-USB | ✅ | USB-A dongle | S2, no BLE |
| LilyGO T-QT C6 | ❌ | Tiny dev board | C6 HID support limited |
| ESP32 (original) | ❌ | — | No native USB HID |

---

## Arduino IDE Settings

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| USB Mode | **USB-OTG (TinyUSB)** ← critical |
| USB CDC On Boot | Enabled |
| Flash Size | 16MB (N16R8) or 8MB (T-Dongle) |
| PSRAM | OPI PSRAM (N16R8) / disabled (T-Dongle) |
