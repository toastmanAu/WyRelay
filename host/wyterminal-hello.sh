#!/bin/bash
# /usr/local/bin/wyterminal-hello.sh
# Runs on USB plug-in via udev. Waits for board CDC to be ready,
# then sends the host's SSH target info so WyTerminal auto-discovers itself.
#
# Install:
#   sudo cp wyterminal-hello.sh /usr/local/bin/wyterminal-hello.sh
#   sudo chmod +x /usr/local/bin/wyterminal-hello.sh
#   sudo cp 99-wyterminal.rules /etc/udev/rules.d/
#   sudo udevadm control --reload-rules

DEV="$1"
[ -z "$DEV" ] && DEV="/dev/ttyACM0"

# Wait for the device to be ready (board boots ~3s after USB)
sleep 4

# Make sure device exists
[ -c "$DEV" ] || exit 1

# Detect current user (the one with an active session, not root)
SESS_USER=$(loginctl list-sessions --no-legend 2>/dev/null | awk '{print $3}' | grep -v root | head -1)
[ -z "$SESS_USER" ] && SESS_USER=$(who | awk '{print $1}' | grep -v root | head -1)
[ -z "$SESS_USER" ] && SESS_USER="$(ls /home | head -1)"

# Get the primary LAN IP (not loopback, prefer 192.168/10.)
HOST_IP=$(ip route get 1.1.1.1 2>/dev/null | grep -oP 'src \K[\d.]+' | head -1)
[ -z "$HOST_IP" ] && HOST_IP=$(hostname -I 2>/dev/null | awk '{print $1}')

# Build the TARGET string
TARGET="${SESS_USER}@${HOST_IP}"

# Configure serial port (115200, raw)
stty -F "$DEV" 115200 raw -echo 2>/dev/null

# Wait briefly for WYTERMINAL_HELLO from board (optional handshake)
timeout 3 cat "$DEV" 2>/dev/null | grep -q "WYTERMINAL_HELLO" &
sleep 1

# Send target
echo "TARGET ${TARGET}" > "$DEV"

# Log it
logger -t wyterminal "Auto-configured: TARGET=${TARGET} on ${DEV}"
