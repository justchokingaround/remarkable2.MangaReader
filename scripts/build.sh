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

# Bundled ARMv7 helper binaries (downloaded once into build/vendor, checksum-pinned):
#  - curl: reMarkable OS only has BusyBox wget, which can't talk TLS 1.3 (api.mangadex.org)
#  - aidoku-run: runs new-format Aidoku sources (MangaLib, ReManga, MangaDex v14, ...)
VENDOR_DIR="${ROOT_DIR}/build/vendor"
CURL_VER="8.23.0-1"
CURL_TXZ="curl-linux-armv7-musl-${CURL_VER}.tar.xz"
CURL_SHA256="10245cccca6e81b654ec7454c0397ab26e4a31910440548625a3802ce0971b3b"
ARUN_VER="v0.1.22"
ARUN_ZIP="aidoku-koplugin-armv7-${ARUN_VER}.zip"
ARUN_SHA256="4ad2859a87b120166c1cf0b6469ce82da47501de650ecdb93a321d5133561d7f"

fetch_pinned() {  # url dest sha256
    if [[ -f "$2" ]] && echo "$3  $2" | sha256sum -c --status 2>/dev/null; then return 0; fi
    echo "    downloading $(basename "$2")..."
    curl -fsSL -o "$2.part" "$1"
    echo "$3  $2.part" | sha256sum -c --status || { echo "Checksum mismatch for $1" >&2; rm -f "$2.part"; exit 1; }
    mv "$2.part" "$2"
}

if [[ "${SKIP_VENDOR:-0}" != "1" ]]; then
    mkdir -p "${VENDOR_DIR}"
    fetch_pinned "https://github.com/stunnel/static-curl/releases/download/${CURL_VER}/${CURL_TXZ}" \
        "${VENDOR_DIR}/${CURL_TXZ}" "${CURL_SHA256}"
    fetch_pinned "https://github.com/ohaiibuzzle/aidoku-backend-0.7/releases/download/${ARUN_VER}/${ARUN_ZIP}" \
        "${VENDOR_DIR}/${ARUN_ZIP}" "${ARUN_SHA256}"
    tar -xJf "${VENDOR_DIR}/${CURL_TXZ}" -C "${DIST_DIR}/backend" curl

    # Prefer building aidoku-run from the pinned tag + our URL-encoding patch (fixes searches
    # containing spaces or Cyrillic, which otherwise time out / get HTTP 400). Falls back to
    # the upstream prebuilt binary when Go is not installed.
    ARUN_SRC="${VENDOR_DIR}/aidoku-backend-0.7-${ARUN_VER}"
    if command -v go >/dev/null 2>&1; then
        if [[ ! -d "${ARUN_SRC}" ]]; then
            git clone -q --depth 1 --branch "${ARUN_VER}" \
                https://github.com/ohaiibuzzle/aidoku-backend-0.7 "${ARUN_SRC}"
            git -C "${ARUN_SRC}" apply "${ROOT_DIR}/scripts/patches/aidoku-run-url-encoding.patch"
        fi
        echo "    building patched aidoku-run ${ARUN_VER} (linux/arm, GOARM=7)..."
        (cd "${ARUN_SRC}" && CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 \
            go build -trimpath -ldflags="-s -w" -o "${DIST_DIR}/backend/aidoku-run" ./cmd/aidoku-run)
    else
        echo "    WARNING: go not found, using unpatched prebuilt aidoku-run (multi-word/Cyrillic searches may fail)" >&2
        unzip -p "${VENDOR_DIR}/${ARUN_ZIP}" aidoku.koplugin/bin/armv7/aidoku-run > "${DIST_DIR}/backend/aidoku-run"
    fi
    chmod +x "${DIST_DIR}/backend/curl" "${DIST_DIR}/backend/aidoku-run"
fi

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
ls -lh "${DIST_DIR}" "${DIST_DIR}/backend"
