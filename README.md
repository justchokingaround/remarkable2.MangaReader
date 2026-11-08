# rm2manga

A native **Mihon-style** manga reader and downloader for the **reMarkable 2**, built for **AppLoad (`rm-appload` / XOVI)** with a Qt 6 QML E-Ink interface (`1404 x 1872`) and a multithreaded C++20 ARMv7 backend.

## Highlights

- **Zero Built-In Sources (Bring Your Own Source Lists)**:
  - Ships with **no hardcoded manga websites, APIs, or source repositories**.
  - Users supply their own Aidoku `settings.json` (`source_lists` URLs + `languages` array) directly inside **`Browse → ⚙ Sources → Source Lists & Languages (JSON)`** (via the Visual Builder, Raw JSON editor, or SSH file edit), then install and discover/search manga per installed `.aix` source.
- **1-Tap Launcher App**: Appears directly inside AppLoad as **rm2manga** with instant startup.
- **Mihon-Style Home & Navigation**:
  - **Continue Reading Hero Card** at the top of `Library` — 1 tap jumps straight back to your exact series, chapter, and page.
  - **4 Bottom Tabs**: `Library` | `Recent` | `Browse` | `Downloads`.
  - **Built-in Touch QWERTY + URL/JSON Keyboard** (plus hardware / Type Folio keyboard support).
- **Purpose-Built E-Ink Reader**:
  - Fullscreen `1404 x 1872` canvas using direct Qt 6 `MultiPointTouchArea` raw finger-touch + Wacom stylus handling so horizontal swipes and single taps never get stolen or delayed.
  - **Configurable Direction & Swipe Polarity**: Toggle `Manga (RTL ←)` / `Comic (LTR →)` tap zones and `Swipe: → is Next` / `Swipe: ← is Next` directly from the Reader HUD.
  - `Full Page`, `Trim Margins`, `Full Width` (1:1 E-Ink vertical drag), and `2x Zoom` modes.
  - Manual **Clear Ghost** button + periodic full-black/white E-Ink ghost-clearing waveform flash.
  - **Seamless Chapter Transitions**: Automatically preloads and extracts the next chapter in the background when you reach the final 3 pages of the current chapter.

## Configuring User Source Lists

Inside **Browse → ⚙ Sources → Source Lists & Languages (JSON)** (or at `/home/root/xovi/exthome/appload/rm2manga/data/aidoku/settings.json` on the tablet), enter your desired Aidoku source index JSON:

```json
{
  "source_lists": [
    "https://example.org/path/to/index.min.json"
  ],
  "languages": ["en"]
}
```

Tap **Save & Fetch Sources**, install any source(s) you want, and tap **Browse →** (or select its pill in `Browse`) to discover and search manga from that source.

## Project Layout

```text
rm2manga/
├── manifest.json            # AppLoad app manifest
├── icon.png                 # 256x256 grayscale manga-panel launcher icon
├── application.qrc          # Qt resource descriptor
├── ui/                      # Qt 6 QML E-Ink frontend
│   ├── main.qml
│   ├── LibraryView.qml
│   ├── RecentView.qml
│   ├── BrowseView.qml
│   ├── DownloadsView.qml
│   ├── MangaDetailView.qml
│   ├── ReaderView.qml
│   └── TouchKeyboard.qml
├── backend/src/             # C++20 ARMv7 backend (AppLoad SOCK_SEQPACKET IPC)
│   ├── main.cpp
│   ├── appload_ipc.hpp
│   ├── aidoku_client.hpp
│   ├── cbz_extractor.hpp
│   ├── state_store.hpp
│   ├── json.hpp
│   └── util.hpp
└── scripts/
    ├── build.sh             # Cross-compiles ARMv7 backend + resources.rcc
    ├── deploy.sh            # Deploys dist/rm2manga to root@10.11.99.1
    ├── test_backend.py      # End-to-end IPC + Aidoku integration test
    └── generate_icon.py     # Generates icon.png
```

## Build & Test

```bash
# Build dist/rm2manga/ (ARMv7 backend + resources.rcc) and host test binary
./scripts/build.sh --with-host /tmp/rm2manga_backend_host

# Run end-to-end local backend integration test
python3 ./scripts/test_backend.py /tmp/rm2manga_backend_host
```

## Deploy to reMarkable 2

Connect your reMarkable 2 over USB (`10.11.99.1`) or Wi-Fi and run:

```bash
./scripts/deploy.sh
# Or specify a custom SSH host:
RM_HOST=root@192.168.1.50 ./scripts/deploy.sh
```
