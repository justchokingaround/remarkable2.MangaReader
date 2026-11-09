#!/usr/bin/env bash
set -euo pipefail

KINDLE_IP="${KINDLE_IP:-192.168.1.190}"
KINDLE_PORT="${KINDLE_PORT:-2222}"
DEST="/mnt/us/extensions/rm2manga"

echo "==> Deploying rm2manga Native App (KUAL extension) to Kindle Oasis (${KINDLE_IP}:${KINDLE_PORT})..."

export SSH_ASKPASS_REQUIRE=force
export SSH_ASKPASS=/bin/echo

ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p "${KINDLE_PORT}" "root@${KINDLE_IP}" \
    "mkdir -p ${DEST}/bin /mnt/us/manga"

scp -O -P "${KINDLE_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    kindle/native/extension/config.xml kindle/native/extension/menu.json "root@${KINDLE_IP}:${DEST}/"

scp -O -P "${KINDLE_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    kindle/native/extension/bin/* "root@${KINDLE_IP}:${DEST}/bin/"

ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p "${KINDLE_PORT}" "root@${KINDLE_IP}" \
    "chmod +x ${DEST}/bin/rm2manga.sh ${DEST}/bin/rm2manga"

echo "==> rm2manga Native App deployed successfully to ${DEST}!"
echo "==> You can now launch rm2manga directly from KUAL on your Kindle Oasis."
