#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="${ROOT_DIR}/dist/rm2manga"
RM_HOST="${RM_HOST:-root@10.11.99.1}"
REMOTE_DIR="/home/root/xovi/exthome/appload/rm2manga"
RESTART_XOCHITL=1

for arg in "$@"; do
    case "$arg" in
        --no-restart)
            RESTART_XOCHITL=0
            ;;
        *)
            RM_HOST="$arg"
            ;;
    esac
done

if [[ ! -x "${DIST_DIR}/backend/entry" || ! -f "${DIST_DIR}/resources.rcc" ]]; then
    echo "Bundle missing; running scripts/build.sh first..."
    "${ROOT_DIR}/scripts/build.sh"
fi

SSH_ARGS=()
if [[ -n "${RM_SSH_KEY:-}" ]]; then
    SSH_ARGS+=(-i "${RM_SSH_KEY}")
fi

REMOTE_CMD="set -e
mkdir -p '${REMOTE_DIR}/backend' '${REMOTE_DIR}/data'
pkill -f '${REMOTE_DIR}/backend/entry' 2>/dev/null || true
tar --no-same-owner -xf - -C '${REMOTE_DIR}'
chmod 755 '${REMOTE_DIR}/backend/entry'
chmod 755 '${REMOTE_DIR}/backend/curl' '${REMOTE_DIR}/backend/aidoku-run' 2>/dev/null || true
chmod 755 /home/root/xovi/exthome/appload/koreader/plugins/rakuyomi.koplugin/server 2>/dev/null || true"

if [[ "${RESTART_XOCHITL}" -eq 1 ]]; then
    REMOTE_CMD="${REMOTE_CMD}
systemctl restart xochitl"
fi

echo "==> Deploying AppLoad bundle to ${RM_HOST}:${REMOTE_DIR}..."
# --no-mac-metadata only exists in macOS bsdtar; GNU tar (Linux) rejects it
TAR_ARGS=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    TAR_ARGS+=(--no-mac-metadata)
fi
tar ${TAR_ARGS[@]+"${TAR_ARGS[@]}"} -C "${DIST_DIR}" -cf - manifest.json icon.png resources.rcc backend/entry \
    | ssh "${SSH_ARGS[@]}" "${RM_HOST}" "${REMOTE_CMD}"

if [[ "${RESTART_XOCHITL}" -eq 1 ]]; then
    echo "==> Deployed rm2manga and restarted xochitl! Open AppLoad on your reMarkable 2 and tap 'Manga'."
else
    echo "==> Deployed rm2manga (without restarting xochitl)."
fi
