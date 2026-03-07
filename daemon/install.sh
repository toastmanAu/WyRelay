#!/bin/bash
# WyRelay daemon quick installer
# Usage: sudo bash install.sh --token YOUR_TOKEN --chat YOUR_CHAT_ID

set -e

TOKEN=""
CHAT=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --token) TOKEN="$2"; shift 2 ;;
        --chat)  CHAT="$2";  shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

if [ -z "$TOKEN" ] || [ -z "$CHAT" ]; then
    echo "Usage: sudo bash install.sh --token YOUR_BOT_TOKEN --chat YOUR_CHAT_ID"
    exit 1
fi

echo "Installing WyRelay daemon..."

# Dependencies
pip3 install pyserial requests Pillow --quiet
apt-get install -y scrot xclip 2>/dev/null || true

# Copy files
mkdir -p /opt/wyrelay
cp wyrelay-daemon.py /opt/wyrelay/
chmod +x /opt/wyrelay/wyrelay-daemon.py

# Install service
cat > /etc/systemd/system/wyrelay-daemon.service << EOF
[Unit]
Description=WyRelay Daemon
After=network.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 /opt/wyrelay/wyrelay-daemon.py --port /dev/ttyACM0 --token ${TOKEN} --chat ${CHAT}
Restart=always
RestartSec=5
Environment=DISPLAY=:0

[Install]
WantedBy=multi-user.target
EOF

# Add user to dialout for serial access
usermod -aG dialout $SUDO_USER 2>/dev/null || true

systemctl daemon-reload
systemctl enable wyrelay-daemon
systemctl start wyrelay-daemon

echo "✅ WyRelay daemon installed and running"
echo "   Check status: systemctl status wyrelay-daemon"
echo "   View logs:    journalctl -u wyrelay-daemon -f"
