#!/bin/bash
# WyTerminal v3 — Pi host setup for USB NCM
# Run once after flashing the board and plugging it in

set -e

echo "=== WyTerminal v3 USB NCM Setup ==="

# Wait for usb0 to appear
echo "Waiting for usb0..."
for i in $(seq 1 30); do
    if ip link show usb0 &>/dev/null; then
        echo "  Found usb0"
        break
    fi
    sleep 1
done

if ! ip link show usb0 &>/dev/null; then
    echo "ERROR: usb0 not found. Is the board plugged in and flashed with v3?"
    exit 1
fi

# Assign host IP
echo "Configuring 192.168.100.1/24 on usb0..."
sudo ip addr flush dev usb0 2>/dev/null || true
sudo ip addr add 192.168.100.1/24 dev usb0
sudo ip link set usb0 up

# Verify
echo ""
echo "Interface:"
ip addr show usb0

# Test
echo ""
echo "Testing connection to board..."
sleep 2
if curl -sf http://192.168.100.2/status; then
    echo ""
    echo "✅ WyTerminal v3 connected!"
    echo ""
    echo "Usage:"
    echo "  curl 'http://192.168.100.2/run?cmd=whoami'"
    echo "  curl 'http://192.168.100.2/type?text=hello'"
    echo "  curl 'http://192.168.100.2/key?combo=ctrl+c'"
    echo "  curl 'http://192.168.100.2/enter'"
    echo "  curl 'http://192.168.100.2/status'"
else
    echo "⚠️  Board not responding yet — may still be booting. Try:"
    echo "  curl http://192.168.100.2/status"
fi

# Persist across reboots (optional)
echo ""
read -p "Make persistent across reboots? (y/N): " persist
if [[ "$persist" == "y" || "$persist" == "Y" ]]; then
    UDEV_RULE='/etc/udev/rules.d/99-wyterminal-ncm.rules'
    cat << 'EOF' | sudo tee "$UDEV_RULE"
# WyTerminal v3 USB NCM — auto-configure usb0 on plug
ACTION=="add", SUBSYSTEM=="net", KERNEL=="usb0", RUN+="/bin/sh -c 'ip addr flush dev usb0; ip addr add 192.168.100.1/24 dev usb0; ip link set usb0 up'"
EOF
    sudo udevadm control --reload-rules
    echo "✅ udev rule installed: $UDEV_RULE"
    echo "   usb0 will auto-configure on every plug-in"
fi
