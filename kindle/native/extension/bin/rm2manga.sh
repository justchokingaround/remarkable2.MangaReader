#!/bin/sh

# Set working directory
BASE_DIR="/mnt/us/extensions/rm2manga/bin"
cd "${BASE_DIR}" || exit 1

# Ensure executable permissions
chmod +x rm2manga fetch.sh

# Export modern TLS helper for MangaDex HTTP(S) requests
export RM2MANGA_HTTP_HELPER="${BASE_DIR}/fetch.sh"

# Kill any stale instance
killall -9 rm2manga 2>/dev/null || true
sleep 1

# Cleanup trap to guarantee Kindle framework is always restored
cleanup() {
    killall -CONT awesome 2>/dev/null
    lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
    lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home 2>/dev/null
}
trap cleanup EXIT INT TERM

# Inhibit Amazon framework
lipc-set-prop com.lab126.pillow disableEnablePillow disable 2>/dev/null
killall -STOP awesome 2>/dev/null

# Execute rm2manga native binary
./rm2manga >> /mnt/us/manga/rm2manga_native.log 2>&1

# Restore Amazon framework on exit
killall -CONT awesome 2>/dev/null
lipc-set-prop com.lab126.pillow disableEnablePillow enable 2>/dev/null
lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home 2>/dev/null
