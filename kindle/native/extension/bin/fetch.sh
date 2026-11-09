#!/bin/sh
# Wrapper so rm2manga can use KOReader's modern TLS stack for HTTPS.
cd /mnt/us/koreader || exit 3
exec ./luajit /mnt/us/extensions/rm2manga/bin/fetch.lua "$@" >/dev/null
