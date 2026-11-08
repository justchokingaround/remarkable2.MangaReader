#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="${ROOT_DIR}/dist/rm2manga"

# Locate Zig toolchain (for ARMv7 Cortex-A7 glibc cross-compilation)
if [[ -n "${ZIG_BIN:-}" ]]; then
    ZIG="${ZIG_BIN}"
elif command -v zig >/dev/null 2>&1; then
    ZIG="$(command -v zig)"
elif [[ -x "/tmp/toolchain/zig-aarch64-macos-0.15.1/zig" ]]; then
    ZIG="/tmp/toolchain/zig-aarch64-macos-0.15.1/zig"
else
    echo "Error: zig compiler not found in PATH or /tmp/toolchain/zig-aarch64-macos-0.15.1/zig" >&2
    exit 1
fi

# Locate Qt 6 rcc binary
if [[ -n "${RCC_BIN:-}" ]]; then
    RCC="${RCC_BIN}"
elif command -v rcc >/dev/null 2>&1; then
    RCC="$(command -v rcc)"
elif [[ -x "/tmp/toolchain/PySide6/Qt/libexec/rcc" ]]; then
    RCC="/tmp/toolchain/PySide6/Qt/libexec/rcc"
else
    echo "Error: Qt rcc not found in PATH or /tmp/toolchain/PySide6/Qt/libexec/rcc" >&2
    exit 1
fi

mkdir -p "${DIST_DIR}/backend"

echo "[1/4] Generating launcher icon if missing..."
if [[ ! -f "${ROOT_DIR}/icon.png" ]]; then
    python3 "${ROOT_DIR}/scripts/generate_icon.py"
fi

echo "[2/4] Cross-compiling ARMv7 backend (arm-linux-gnueabihf.2.35, Cortex-A7)..."
"${ZIG}" c++ \
    -target arm-linux-gnueabihf.2.35 \
    -mcpu=cortex_a7 \
    -std=c++20 \
    -O2 \
    -s \
    -Wall -Wextra \
    -pthread \
    -ldl \
    -I"${ROOT_DIR}/backend/src" \
    "${ROOT_DIR}/backend/src/main.cpp" \
    -o "${DIST_DIR}/backend/entry"
chmod +x "${DIST_DIR}/backend/entry"

echo "[3/4] Compiling Qt 6 QML resource bundle (resources.rcc)..."
"${RCC}" \
    --binary \
    --no-compress \
    --format-version 2 \
    -o "${DIST_DIR}/resources.rcc" \
    "${ROOT_DIR}/application.qrc"

echo "[4/4] Assembling AppLoad bundle in ${DIST_DIR}..."
cp "${ROOT_DIR}/manifest.json" "${DIST_DIR}/manifest.json"
cp "${ROOT_DIR}/icon.png" "${DIST_DIR}/icon.png"

if [[ "${1:-}" == "--with-host" ]]; then
    HOST_OUT="${2:-/tmp/rm2manga_backend_host}"
    echo "[+] Compiling host test binary -> ${HOST_OUT}..."
    c++ \
        -std=c++20 \
        -O2 \
        -Wall -Wextra \
        -pthread \
        -I"${ROOT_DIR}/backend/src" \
        "${ROOT_DIR}/backend/src/main.cpp" \
        -o "${HOST_OUT}"
fi

echo "Build complete:"
ls -lh "${DIST_DIR}" "${DIST_DIR}/backend/entry"
