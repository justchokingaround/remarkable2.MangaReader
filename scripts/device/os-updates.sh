#!/bin/bash
# Turns the reMarkable's automatic OS updates on/off.
#
# SWUpdate polls reMarkable's server every 30 minutes and installs whatever it finds
# (suricatta, confirm = 0). An OS update reflashes the root partition, which removes the
# XOVI/AppLoad hooks ("My Apps" disappears) until /home/root/xovi/autostart.sh runs again.
set -uo pipefail

UNITS="swupdate.service swupdate.socket"

status() {
    echo "Automatic OS updates:"
    for u in $UNITS; do
        state=$(systemctl is-enabled "$u" 2>&1)
        active=$(systemctl is-active "$u" 2>&1)
        echo "  $u: $state / $active"
    done
    echo
    echo "Firmware: $(cat /etc/version 2>/dev/null)   xochitl md5: $(md5sum /usr/bin/xochitl | cut -d' ' -f1)"
}

case "${1:-status}" in
    disable)
        for u in $UNITS; do systemctl stop "$u" 2>/dev/null; done
        for u in $UNITS; do systemctl mask "$u" >/dev/null 2>&1 && echo "masked $u"; done
        echo "Automatic updates are OFF. Use '$0 enable' (or Settings > Software update"
        echo "after running '$0 enable') to update on your own terms."
        ;;
    enable)
        for u in $UNITS; do systemctl unmask "$u" >/dev/null 2>&1 && echo "unmasked $u"; done
        systemctl start swupdate.socket 2>/dev/null
        systemctl start swupdate.service 2>/dev/null
        echo "Automatic updates are ON again."
        ;;
    status) status ;;
    *)
        echo "usage: $0 {disable|enable|status}" >&2
        exit 1
        ;;
esac
