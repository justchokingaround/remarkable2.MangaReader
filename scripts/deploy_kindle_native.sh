#!/usr/bin/env bash
set -euo pipefail

KINDLE_IP="${KINDLE_IP:-192.168.1.190}"
KINDLE_PORT="${KINDLE_PORT:-2222}"
DEST="/mnt/us/extensions/rm2manga"
KPM_DEST="/mnt/us/kmc/kpm/packages/rm2manga"
SCRIPTLET_DEST="/mnt/us/kmc/kpm/packages/rm2manga/scriptlets"

echo "==> Deploying rm2manga Native App & KPM Package to Kindle Oasis (${KINDLE_IP}:${KINDLE_PORT})..."

export DISPLAY=:0
export SSH_ASKPASS_REQUIRE=force
export SSH_ASKPASS=/bin/echo

ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p "${KINDLE_PORT}" "root@${KINDLE_IP}" \
    "killall -9 rm2manga 2>/dev/null || true; rm -f ${DEST}/bin/rm2manga; mkdir -p ${DEST}/bin ${KPM_DEST} ${SCRIPTLET_DEST} /mnt/us/manga"

scp -O -P "${KINDLE_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    kindle/native/extension/config.xml kindle/native/extension/menu.json "root@${KINDLE_IP}:${DEST}/"

scp -O -P "${KINDLE_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    kindle/native/extension/bin/* "root@${KINDLE_IP}:${DEST}/bin/"

scp -O -P "${KINDLE_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    kindle/native/kpm/* "root@${KINDLE_IP}:${KPM_DEST}/"

scp -O -P "${KINDLE_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    kindle/native/scriptlet/rm2manga.sh "root@${KINDLE_IP}:${SCRIPTLET_DEST}/"

scp -O -P "${KINDLE_PORT}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    kindle/native/scriptlet/rm2manga.sh "root@${KINDLE_IP}:/mnt/us/documents/rm2manga.sh"

ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p "${KINDLE_PORT}" "root@${KINDLE_IP}" \
    "chmod +x ${DEST}/bin/rm2manga.sh ${DEST}/bin/rm2manga ${DEST}/bin/fetch.sh ${KPM_DEST}/launch.sh ${SCRIPTLET_DEST}/rm2manga.sh /mnt/us/documents/rm2manga.sh"

echo "==> rm2manga Native App deployed successfully!"
echo "==> An rm2manga icon will now appear in your Kindle Home Library!"
