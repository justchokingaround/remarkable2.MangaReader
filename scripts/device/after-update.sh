#!/bin/bash
# Run this once after a reMarkable OS update.
#
# An OS update reflashes the root partition: systemd units and the Qt resource hashtable
# are gone, XOVI is not loaded and "My Apps" disappears. /home/root is untouched, so this
# script re-installs the boot hook, rebuilds the hashtable if xochitl changed and starts
# XOVI again. It is safe to run any time.
set -uo pipefail

XOVI=/home/root/xovi
UNIT_DIR=/etc/systemd/system
UNIT=xovi-autostart.service

echo "==> Installing boot hook ($UNIT)"
cp -f "$XOVI/systemd/xovi-autostart.service" "$UNIT_DIR/$UNIT"
chmod 644 "$UNIT_DIR/$UNIT"
chmod 755 "$XOVI/autostart.sh" "$XOVI/os-updates.sh" "$XOVI/after-update.sh"
systemctl daemon-reload
systemctl enable "$UNIT" >/dev/null 2>&1
echo "    enabled"

echo "==> Applying XOVI (rebuilds the Qt hashtable if the firmware changed; takes ~2 min)"
"$XOVI/autostart.sh"

echo
echo "==> Done. Open My Apps: XOVI is loaded and rm2manga should be listed."
echo "    Log: $XOVI/autostart.log"
"$XOVI/os-updates.sh" status
