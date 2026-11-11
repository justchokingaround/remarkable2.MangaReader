#!/bin/bash
# XOVI + AppLoad (rm2manga) boot hook.
#
# The reMarkable OS lives on an A/B root partition that is reflashed on every update,
# so /etc (and therefore systemd units) does not survive updates. /home/root does.
# This script is therefore the only durable piece: run after boot and after an OS
# update, it rebuilds the Qt resource hashtable when xochitl changed and re-applies XOVI.
set -uo pipefail

XOVI=/home/root/xovi
QRR="$XOVI/exthome/qt-resource-rebuilder"
LOG="$XOVI/autostart.log"
SHAFILE="$QRR/xochitl.md5"

log() { echo "$(date -u +%FT%TZ) $*" >>"$LOG"; }

# Never run twice at the same time (boot hook + manual run)
exec 9>/tmp/xovi-autostart.lock
if ! flock -n 9; then
    log "another instance is running, skipping"
    exit 0
fi

if [ ! -x "$XOVI/start" ]; then
    log "ERROR: $XOVI/start missing"
    exit 1
fi

# Wait for xochitl so the restart inside xovi/start is not racing the boot
for _ in $(seq 1 60); do
    pidof xochitl >/dev/null 2>&1 && break
    sleep 2
done

cur=$(md5sum /usr/bin/xochitl 2>/dev/null | cut -d' ' -f1)
prev=$(cat "$SHAFILE" 2>/dev/null || echo none)

if [ -z "$cur" ]; then
    log "ERROR: /usr/bin/xochitl not readable"
elif [ "$cur" != "$prev" ]; then
    log "xochitl changed ($prev -> $cur): rebuilding qt-resource-rebuilder hashtable"
    [ -f "$QRR/hashtab" ] && cp -a "$QRR/hashtab" "$QRR/hashtab.pre-$cur"
    # rebuild_hashtable prints progress and needs one empty line on stdin
    echo "" | timeout 600 "$XOVI/rebuild_hashtable" >>"$LOG" 2>&1
    if [ -f "$QRR/hashtab" ]; then
        echo "$cur" >"$SHAFILE"
        log "hashtable rebuilt for $cur"
    else
        log "ERROR: hashtable rebuild failed, leaving xochitl stock"
    fi
fi

log "starting xovi"
"$XOVI/start" >>"$LOG" 2>&1
log "xovi start finished (exit $?)"
