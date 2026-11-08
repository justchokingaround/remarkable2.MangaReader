<p align="center">
  <img src="icon.png" width="128" height="128" alt="rm2manga icon" />
</p>

<h1 align="center">rm2manga</h1>

<p align="center">
  <strong>A native, high-performance Mihon-style manga reader and downloader for the reMarkable 2</strong><br />
  Built for <strong>AppLoad (XOVI)</strong> with a custom Qt 6 QML E-Ink canvas and a multi-threaded C++20 backend.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-reMarkable%202-black?style=flat-square&logo=linux" alt="Platform" />
  <img src="https://img.shields.io/badge/Runtime-AppLoad%20%2F%20XOVI-black?style=flat-square" alt="Runtime" />
  <img src="https://img.shields.io/badge/UI-Qt%206%20QML%20E--Ink-black?style=flat-square&logo=qt" alt="Qt 6" />
  <img src="https://img.shields.io/badge/Backend-C%2B%2B20%20(ARMv7%20Cortex--A7)-black?style=flat-square&logo=c%2B%2B" alt="C++20" />
  <img src="https://img.shields.io/badge/License-GPLv3-black?style=flat-square" alt="License" />
</p>

---

## ✨ Features

- ⚡ **Native 1-Tap Launcher**: Integrates directly into reMarkable's stock launcher via AppLoad / XOVI with near-instant cold startup (<30ms UI paint).
- 📱 **Mihon / Tachiyomi Experience**:
  - **Hero Card**: Quick 1-tap "Continue Reading" right at the top of your Library.
  - **4 Navigation Views**: `Library`, `Recent`, `Browse`, and `Downloads`.
  - **Unified Search & Discover**: Browse trending manga or search across all sources simultaneously.
  - **Integrated On-Screen Keyboard**: Custom E-Ink touch QWERTY keyboard with URL/JSON quick keys and Type Folio hardware keyboard support.
- 📖 **Purpose-Built E-Ink Reader**:
  - Fullscreen `1404 × 1872` hardware-accelerated canvas.
  - Raw `MultiPointTouchArea` finger and Wacom pen handling for zero-latency page turns.
  - Configurable reading modes: `Manga (RTL ←)` or `Comic (LTR →)`.
  - Configurable swipe polarity: toggle whether swiping left or right navigates forward.
  - Display modes: `Fit Page`, `Trim Margins`, `Full Width` (1:1 vertical drag), and `2x Zoom`.
  - Ghosting reduction: on-demand **Clear Ghost** button and periodic e-paper waveform refresh.
  - **Zero-Wait Chapter Preloading**: Automatically downloads and unpacks the next chapter in the background when approaching the end of the current one.
- 🔌 **Dual-Engine Source Architecture**:
  - **Native MangaDex v5 Engine**: High-resolution covers, metadata, authors, and chapter feeds with DDoS-compliant API negotiation.
  - **Aidoku WASM Plugin Support**: Install and browse external community sources (e.g. MangaFox, MangaKatana, Toonily) directly from user-supplied source lists.
- 🖼️ **Intelligent Cover Pipeline**:
  - SQLite metadata cache directly queries the on-device Aidoku runtime database.
  - Cross-source title fallback automatically queries MangaDex for official high-resolution covers when community plugins omit cover thumbnails.
  - Dynamic User-Agent and Referer headers protect against 403 Forbidden CDN blocks.

---

## 🏛️ Architecture

```
┌──────────────────────────────────────────────────────────┐
│                   Qt 6 QML E-Ink GUI                     │
│    Library  •  Recent  •  Browse  •  Reader  •  Keyboard │
└────────────────────────────┬─────────────────────────────┘
                             │ AppLoad IPC (SOCK_SEQPACKET)
┌────────────────────────────▼─────────────────────────────┐
│                 rm2manga C++20 Backend                   │
│                                                          │
│  ┌───────────────────────┐    ┌──────────────────────┐  │
│  │   MangaDex Engine     │    │   Aidoku WASM Host   │  │
│  │  - Native REST API v5 │    │  - Source Extractor  │  │
│  │  - At-Home downloader │    │  - SQLite Database   │  │
│  └───────────┬───────────┘    └──────────┬───────────┘  │
│              │                           │              │
│  ┌───────────▼───────────────────────────▼───────────┐  │
│  │   State Store & LRU Cache Engine                  │  │
│  │   - state.json persistence                        │  │
│  │   - CBZ & folder extraction                       │  │
│  │   - Natural filename page sort                    │  │
│  │   - Background preloader & cover worker           │  │
│  └───────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

---

## ⚙️ Adding Manga Sources

rm2manga ships with **zero hardcoded pirate sources** out of the box, allowing users full control over their extension catalog.

### Option 1: In-App UI (Recommended)
1. Open rm2manga on your reMarkable tablet.
2. Tap the **Browse** tab at the bottom.
3. Tap **⚙ Sources** in the top right.
4. Tap **Source Lists & Languages (JSON)**.
5. Enter your Aidoku source index URL in the JSON editor:

```json
{
  "source_lists": [
    "https://raw.githubusercontent.com/Aidoku-Community/legacy-sources/refs/heads/gh-pages/index.min.json"
  ],
  "languages": ["en"]
}
```

6. Tap **Save & Fetch Sources**.
7. Tap **Available Sources**, find your preferred extensions (e.g. *MangaFox*), and tap **Install**.
8. Return to **Browse** and start reading!

### Option 2: Over SSH
You can also directly edit the settings file on your reMarkable 2:

```bash
ssh root@10.11.99.1
nano /home/root/xovi/exthome/appload/rm2manga/data/aidoku/settings.json
```

---

## 📁 Repository Structure

```text
rm2manga/
├── manifest.json            # AppLoad application manifest
├── icon.png                 # 256x256 launcher icon
├── application.qrc          # Qt resource bundle descriptor
├── ui/                      # Qt 6 QML user interface
│   ├── main.qml             # Root navigation & AppLoad IPC glue
│   ├── LibraryView.qml      # Library grid & Continue Reading hero card
│   ├── BrowseView.qml       # Unified discover, source filter pills & search
│   ├── MangaDetailView.qml  # Series info, cover art & chapter list
│   ├── ReaderView.qml       # Touch/stylus reader canvas & HUD controls
│   ├── RecentView.qml       # Reading history
│   ├── DownloadsView.qml    # Active download queue & cached storage stats
│   └── TouchKeyboard.qml    # Custom on-screen E-Ink touch keyboard
├── backend/src/             # C++20 ARMv7 multi-threaded backend
│   ├── main.cpp             # App coordination, IPC message loop, download queue
│   ├── appload_ipc.hpp      # Zero-copy binary framing protocol
│   ├── mangadex_client.hpp  # MangaDex REST v5 & At-Home downloader
│   ├── aidoku_client.hpp    # Aidoku daemon HTTP/UDS client
│   ├── aidoku_db.hpp        # SQLite dynamic cover metadata loader
│   ├── cbz_extractor.hpp    # Multi-format image extractor & natural sort
│   ├── state_store.hpp      # Thread-safe JSON state store with atomic file I/O
│   ├── util.hpp             # HTTP client with anti-scraping CDN headers
│   └── json.hpp             # Lightweight header-only JSON parser
└── scripts/
    ├── build.sh             # Cross-compiles ARMv7 binary via Zig & builds RCC
    ├── deploy.sh            # One-click SSH deployment to reMarkable tablet
    ├── test_backend.py      # End-to-end integration test harness
    └── generate_icon.py     # Standalone launcher icon generator
```

---

## 🛠️ Building & Developing

### Prerequisites
- macOS or Linux
- Python 3.10+
- Qt 6 (`rcc` tool for QML resource bundling)
- [Zig](https://ziglang.org/) (used as a cross-compiler for `arm-linux-gnueabihf` targeting ARMv7 Cortex-A7)

### Cross-Compiling the AppLoad Bundle
```bash
# Compile ARMv7 backend, generate resources.rcc, and assemble dist/rm2manga
./scripts/build.sh

# Also build the host binary for local testing:
./scripts/build.sh --with-host /tmp/rm2manga_backend_host
```

### Running the Test Suite
The repository includes an end-to-end integration harness that simulates the AppLoad coordinator IPC and Aidoku daemon locally:

```bash
python3 ./scripts/test_backend.py /tmp/rm2manga_backend_host
```

---

## 🚀 Deployment to reMarkable 2

Make sure your tablet is connected via USB (`10.11.99.1`) or Wi-Fi, and that [AppLoad](https://github.com/asivery/rm-appload) is installed.

```bash
# Deploy to USB-connected reMarkable (default 10.11.99.1):
./scripts/deploy.sh

# Or deploy to a custom IP over Wi-Fi:
RM_HOST=root@192.168.1.50 ./scripts/deploy.sh
```

The script stops any running instance, copies the backend binary and compiled resources, fixes permissions, and restarts `xochitl`.

---

## 📜 License

Distributed under the **GPL-3.0 License**. See `LICENSE` for more information.

---

<p align="center">
  <sub>Built with ❤️ for the reMarkable e-paper community.</sub>
</p>
