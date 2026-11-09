#!/usr/bin/env bash
set -euo pipefail

KINDLE_IP="${KINDLE_IP:-192.168.1.190}"
KINDLE_PORT="${KINDLE_PORT:-2222}"
DEST="/mnt/us/koreader/plugins/rm2manga.koplugin"

echo "==> Deploying rm2manga KOReader plugin to Kindle Oasis (${KINDLE_IP}:${KINDLE_PORT})..."

export SSH_ASKPASS_REQUIRE=force
export SSH_ASKPASS=/bin/echo

ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p "${KINDLE_PORT}" "root@${KINDLE_IP}" \
    "mkdir -p ${DEST} /mnt/us/manga"

scp -O -P "${KINDLE_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    kindle/rm2manga.koplugin/* "root@${KINDLE_IP}:${DEST}/"

echo "==> rm2manga plugin deployed successfully!"
echo "==> Restart KOReader on your Kindle to activate the plugin."
