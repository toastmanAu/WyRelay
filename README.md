# WyRelay

**ESP32-S3 USB HID keyboard relay + shell bridge — controlled via Telegram.**

Plug into any Linux machine like a USB flash drive. Get a full interactive terminal in your Telegram chat. No SSH. No open ports. No configuration on the target machine (optional daemon for full shell output).

Built by [Wyltek Industries](https://wyltekindustries.com).

---

## What It Does

**Without daemon (v1 — HID only):**
- Appears as a USB keyboard to the host machine
- Send `/run <command>` in Telegram → types it + presses Enter
- Send `/key ctrl+alt+t` → opens terminal
- Send `/key ctrl+c` → kills process
- Works on ANY machine (Linux, Windows, macOS) with zero setup

**With daemon (v2 — full shell bridge):**
- Two-way terminal in Telegram chat
- `/shell <cmd>` → runs command, sends output back to Telegram
- `/screenshot` → captures screen, sends as Telegram photo
- `/upload <path>` → sends file to Telegram
- `/clipboard` → reads clipboard content
- `/sysinfo` → system stats

---

## Hardware

**Recommended:** [LilyGO T-Dongle S3](https://lilygo.cc/products/t-dongle-s3) — ESP32-S3 in a USB-A dongle form factor. Plug directly into any machine. Tiny TFT shows connection status.

**Development:** ESP32-S3 DevKitC-1 N16R8 (two USB-C ports — one for flashing, one for HID output).

See [hardware/HARDWARE.md](hardware/HARDWARE.md) for full hardware guide.

---

## Quick Start

### 1. Flash the firmware

```bash
# Clone repo
git clone https://github.com/toastmanAu/WyRelay
cd WyRelay/firmware
```

Edit `WyRelay.ino`:
```cpp
#define WIFI_SSID       "your-wifi"
#define WIFI_PASSWORD   "your-password"
#define BOT_TOKEN       "your-bot-token"    // from @BotFather
#define ALLOWED_CHAT_ID  123456789          // your Telegram user ID
```

Flash via Arduino IDE:
- Board: `ESP32S3 Dev Module`
- USB Mode: `USB-OTG (TinyUSB)` ← **critical**
- USB CDC On Boot: `Enabled`

### 2. Plug in and test

- Flash via **UART port** (left/top USB-C)
- Plug **native USB port** (right/bottom USB-C) into target machine
- Send `/status` to your bot → should reply with IP + uptime

### 3. (Optional) Install daemon for full shell

On the target Linux machine:
```bash
cd WyRelay/daemon
sudo bash install.sh --token YOUR_BOT_TOKEN --chat YOUR_CHAT_ID
```

---

## Commands

### Always available (HID — no daemon needed)

| Command | Action |
|---|---|
| `/run <cmd>` | Type command + press Enter |
| `/type <text>` | Type text only (no Enter) |
| `/enter` | Press Enter |
| `/paste` | Retype last `/type` text |
| `/key <combo>` | Send key combination |
| `/status` | WyRelay IP + uptime |
| `/help` | Command list |

### Key combos (`/key`)

```
/key ctrl+alt+t     → open terminal (Linux)
/key ctrl+c         → interrupt process
/key ctrl+v         → paste
/key ctrl+z         → undo / suspend
/key super          → open launcher
/key alt+f4         → close window
/key ctrl+shift+v   → terminal paste
/key tab            → tab completion
/key esc            → escape
/key up / down      → arrow keys (command history)
```

### With daemon installed

| Command | Action |
|---|---|
| `/shell <cmd>` | Run command, get output in Telegram |
| `/screenshot` | Capture screen → Telegram photo |
| `/upload <path>` | Send file to Telegram |
| `/clipboard` | Read clipboard content |
| `/sysinfo` | CPU, RAM, disk, IP |
| `/ps` | Top processes by CPU |

---

## Example Session

```
You: /key ctrl+alt+t
[terminal opens on target machine]

You: /shell whoami
Bot: ✅ whoami
     phill

You: /shell df -h /
Bot: ✅ df -h /
     Filesystem      Size  Used Avail Use%
     /dev/mmcblk0p2   58G   12G   43G  22%

You: /screenshot
Bot: [sends photo of target machine's screen]

You: /run sudo systemctl restart ckb
[types command + Enter in the open terminal]

You: /shell journalctl -u ckb -n 10
Bot: ✅ journalctl -u ckb -n 10
     Mar 08 02:40 ckbnode ckb[1234]: INFO syncing...
```

---

## Security Model

- **Only your chat ID works** — all other senders silently ignored (hardcoded in firmware)
- **Physical access required** — device must be physically plugged in to function
- **No open ports** — works behind any firewall, NAT, or corporate network
- **No SSH keys** — the USB stick IS the authentication factor
- **HID-only mode** — without daemon, device can type but cannot read output

---

## Repository Structure

```
WyRelay/
├── firmware/
│   └── WyRelay.ino          # ESP32-S3 firmware (HID keyboard + WiFi)
├── daemon/
│   ├── wyrelay-daemon.py    # Companion daemon (shell bridge + screenshot)
│   ├── wyrelay-daemon.service  # systemd service file
│   └── install.sh           # One-line installer
├── hardware/
│   └── HARDWARE.md          # Supported boards + wiring guide
├── library.properties       # Arduino Library Manager metadata
└── README.md
```

---

## Roadmap

- [x] USB HID keyboard relay
- [x] `/key` arbitrary key combos
- [x] Companion daemon with shell output
- [x] Screenshot to Telegram
- [x] File upload to Telegram
- [ ] T-Dongle S3 status display (WiFi strength, connected/idle)
- [ ] `/macro` — save and replay command sequences
- [ ] `/mouse` — USB HID mouse control
- [ ] Windows support in daemon
- [ ] Arduino Library Manager submission (pending v1 hardware test)

---

## Dependencies

- [ArduinoJson](https://arduinojson.org/) by Benoit Blanchon (firmware)
- ESP32 Arduino core ≥ 3.x (Espressif)
- `pyserial`, `requests`, `Pillow` (daemon)
- `scrot` or `import` (daemon, screenshot)

---

*Part of the [Wyltek Embedded Builder](https://wyltekindustries.com) ecosystem.*
