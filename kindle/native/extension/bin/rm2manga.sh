#!/bin/sh

# Set working directory
BASE_DIR="/mnt/us/extensions/rm2manga/bin"
cd "${BASE_DIR}" || exit 1

# Ensure executable permissions
chmod +x rm2manga fetch.sh aidoku-server 2>/dev/null || true

# Export modern TLS helper for MangaDex HTTP(S) requests
export RM2MANGA_HTTP_HELPER="${BASE_DIR}/fetch.sh"
export RM2MANGA_AIDOKU_BIN="${BASE_DIR}/aidoku-server"

# Ensure Aidoku daemon is running
mkdir -p /mnt/us/manga/data/aidoku/sources /mnt/us/manga/data/aidoku/downloads
if ! pgrep -f "aidoku-server" >/dev/null 2>&1; then
    "${BASE_DIR}/aidoku-server" /mnt/us/manga/data/aidoku >/dev/null 2>&1 &
fi

# Kill any stale instance
killall -9 rm2manga 2>/dev/null || true
sleep 1

# List of resource-hogging Amazon services to suspend for maximum performance
TOGGLED_SERVICES="stored webreader kfxreader kfxview todo tmd rcm archive scanner otav3 otaupd"

# Cleanup trap to guarantee Kindle framework is always restored
cleanup() {
    # Restore CPU governor
    echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null

    # Resume background services
    for svc in ${TOGGLED_SERVICES}; do
        start "${svc}" 2>/dev/null
    done

    killall -CONT awesome 2>/dev/null
    lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
    lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home 2>/dev/null
}
trap cleanup EXIT INT TERM

# Boost CPU governor to performance for snappier, stutter-free performance
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null

# Suspend background services to reclaim ~300MB RAM and 100% CPU time
for svc in ${TOGGLED_SERVICES}; do
    stop "${svc}" 2>/dev/null
done

# Inhibit Amazon framework
lipc-set-prop com.lab126.pillow disableEnablePillow disable 2>/dev/null
killall -STOP awesome 2>/dev/null

# Execute rm2manga native binary
./rm2manga >> /mnt/us/manga/rm2manga_native.log 2>&1

# Restore Amazon framework on exit
killall -CONT awesome 2>/dev/null
lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home 2>/dev/null
