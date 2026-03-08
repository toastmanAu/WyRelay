#!/bin/bash
# /usr/local/bin/wyterminal-hello.sh
# Runs on USB plug-in via udev. Waits for board CDC to be ready,
# then sends the host's SSH target info. Board replies with its public key,
# which this script installs into ~/.ssh/authorized_keys automatically.
#
# Install:
#   sudo cp wyterminal-hello.sh /usr/local/bin/wyterminal-hello.sh
#   sudo chmod +x /usr/local/bin/wyterminal-hello.sh
#   sudo cp 99-wyterminal.rules /etc/udev/rules.d/
#   sudo udevadm control --reload-rules

DEV="$1"
[ -z "$DEV" ] && DEV="/dev/ttyACM0"

# Wait for the device to be ready (board boots + keygen takes ~5s on first run)
sleep 5

# Make sure device exists
[ -c "$DEV" ] || exit 1

# Detect current user (the one with an active session, not root)
SESS_USER=$(loginctl list-sessions --no-legend 2>/dev/null | awk '{print $3}' | grep -v root | head -1)
[ -z "$SESS_USER" ] && SESS_USER=$(who | awk '{print $1}' | grep -v root | head -1)
[ -z "$SESS_USER" ] && SESS_USER="$(ls /home | head -1)"

# Get the primary LAN IP
HOST_IP=$(ip route get 1.1.1.1 2>/dev/null | grep -oP 'src \K[\d.]+' | head -1)
[ -z "$HOST_IP" ] && HOST_IP=$(hostname -I 2>/dev/null | awk '{print $1}')

TARGET="${SESS_USER}@${HOST_IP}"
USER_HOME=$(eval echo "~${SESS_USER}")
AUTH_KEYS="${USER_HOME}/.ssh/authorized_keys"

# Configure serial port
stty -F "$DEV" 115200 raw -echo 2>/dev/null

# Send TARGET — board will reply with PUBKEY on the same port
echo "TARGET ${TARGET}" > "$DEV"

logger -t wyterminal "Sent TARGET=${TARGET} on ${DEV}"

# Read response from board for up to 8 seconds, looking for PUBKEY line
PUBKEY=""
TIMEOUT=8
START=$(date +%s)
while [ -z "$PUBKEY" ]; do
    NOW=$(date +%s)
    ELAPSED=$((NOW - START))
    [ $ELAPSED -ge $TIMEOUT ] && break
    LINE=$(timeout 1 head -1 "$DEV" 2>/dev/null)
    if echo "$LINE" | grep -q "^PUBKEY "; then
        PUBKEY=$(echo "$LINE" | sed 's/^PUBKEY //')
    fi
done

if [ -z "$PUBKEY" ]; then
    logger -t wyterminal "WARNING: No PUBKEY received from board"
    exit 0
fi

# Install public key into authorized_keys for the session user
mkdir -p "${USER_HOME}/.ssh"
chmod 700 "${USER_HOME}/.ssh"
touch "$AUTH_KEYS"
chmod 600 "$AUTH_KEYS"
chown "${SESS_USER}:${SESS_USER}" "${USER_HOME}/.ssh" "$AUTH_KEYS"

# Only add if not already present (idempotent)
if ! grep -qF "$PUBKEY" "$AUTH_KEYS" 2>/dev/null; then
    echo "$PUBKEY" >> "$AUTH_KEYS"
    logger -t wyterminal "Installed WyTerminal pubkey into ${AUTH_KEYS}"
else
    logger -t wyterminal "WyTerminal pubkey already in ${AUTH_KEYS}"
fi
