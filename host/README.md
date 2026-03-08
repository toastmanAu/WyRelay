# WyTerminal Host Setup

## What this does
When you plug WyTerminal into any Linux machine, it automatically tells the board
"SSH to *me*" — no `/target` command needed.

## Install (one-time per machine)

```bash
sudo cp wyterminal-hello.sh /usr/local/bin/wyterminal-hello.sh
sudo chmod +x /usr/local/bin/wyterminal-hello.sh
sudo cp 99-wyterminal.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

## What happens on plug-in

1. udev fires `wyterminal-hello.sh` when `303a:1001` appears
2. Script waits 4s for board to boot
3. Detects current logged-in user + LAN IP
4. Sends `TARGET user@ip` over `/dev/ttyACM0`
5. Board sets that as the active SSH target, shows "auto: user@ip" on display
6. `/shell whoami` will now SSH back to the machine you just plugged into

## Override anytime

Send `/target user@otherhost` via Telegram to switch to a different machine.
Send `/ssh_pass <password>` if key auth isn't set up on the target.

## SSH key auth (recommended)

For passwordless `/shell`, add the board's key to the target's `authorized_keys`.
The board uses `ssh_userauth_publickey_auto()` which reads keys from SPIFFS.
Alternatively just use `/ssh_pass` — it's stored in RAM only (cleared on reboot).

## Troubleshooting

```bash
# Check if udev fired
cat /tmp/wyterminal-hello.log

# Test manually
/usr/local/bin/wyterminal-hello.sh /dev/ttyACM0

# Watch system log
journalctl -t wyterminal -f
```
