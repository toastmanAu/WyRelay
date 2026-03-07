# WyRelay

**ESP32-S3 USB HID keyboard relay — controlled via Telegram bot.**

Plug a ESP32-S3 into any Raspberry Pi or PC via its native USB port. It appears as a USB keyboard. Send commands from Telegram and they get typed on the target machine — no typing errors, works on any headless device.

Built by [Wyltek Industries](https://wyltekindustries.com) as part of the Wyltek Embedded Builder stack.

---

## Hardware Required

- ESP32-S3 board with **two USB-C ports** (UART + native USB)
  - e.g. ESP32-S3 DevKitC-1 N16R8, LILYGO T-QT S3, etc.
- USB-C cable for flashing (UART port)
- USB-C cable for HID output (native USB port → target machine)

## Dependencies

- [ArduinoJson](https://arduinojson.org/) by Benoit Blanchon

## Arduino IDE Settings

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| USB Mode | USB-OTG (TinyUSB) |
| USB CDC On Boot | Enabled |
| Flash Size | 16MB (or match your board) |
| PSRAM | OPI PSRAM (for N16R8) |

## Installation

1. Install via Arduino Library Manager (search **WyRelay**)
2. Install dependency: **ArduinoJson** by Benoit Blanchon
3. Open **Examples → WyRelay → WyRelay**
4. Set your config (see below)
5. Flash via UART port
6. Plug native USB into target machine

## Configuration

Edit the top of `WyRelay.ino`:

```cpp
#define WIFI_SSID       "your-wifi-ssid"
#define WIFI_PASSWORD   "your-wifi-password"
#define BOT_TOKEN       "your-telegram-bot-token"
#define ALLOWED_CHAT_ID  123456789   // Your Telegram user ID only
```

**Get your bot token:** Message [@BotFather](https://t.me/BotFather) on Telegram → `/newbot`

**Get your chat ID:** Message [@userinfobot](https://t.me/userinfobot) on Telegram

## Usage

Send commands to your bot:

| Command | Action |
|---|---|
| `/run <command>` | Type command + press Enter |
| `/type <text>` | Type text only (no Enter) |
| `/enter` | Press Enter only |
| `/paste` | Retype last `/type` text |
| `/status` | Reply with IP + uptime |
| `/help` | Show command list |

### Examples

```
/run sudo systemctl restart ckb
/run ssh phill@192.168.68.87
/type hunter2
/enter
/status
```

## Security

- **Only your chat ID can send commands** — all other senders are silently ignored
- TLS used for Telegram API (certificate verification optional)
- No commands are executed locally on the ESP32 — it only types on the target machine

## Use Cases

- Headless Pi management without a keyboard
- Sending commands to a machine mid-session from your phone
- Pasting long commands without typos
- Remote terminal access when SSH isn't available

## License

MIT — see [LICENSE](LICENSE)

---

*Part of the [Wyltek Embedded Builder](https://wyltekindustries.com) ecosystem.*
