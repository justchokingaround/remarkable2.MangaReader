# rm2manga — Native Mihon-Style Manga Reader for reMarkable 2 (AppLoad)

## 1. Product & UX Spec
- **Launcher Integration**: Native `rm-appload` app (`/home/root/xovi/exthome/appload/rm2manga/`), 1-tap fullscreen launch (`1404 x 1872`).
- **Home & Navigation (Mihon-Style)**:
  - **Continue Reading Hero Banner** at top of Library (1-tap resume right to exact chapter & page).
  - **Bottom Tab Bar**: `Library` | `Recent` | `Browse` | `Downloads`.
  - **Built-in E-Ink Touch QWERTY + URL/JSON Keyboard** (plus hardware/Type Folio keyboard support).
- **Bring-Your-Own Manga Sources (Zero Hardcoded Sources)**:
  - Ships with **zero hardcoded sources and zero hardcoded repository URLs** (`data/aidoku/settings.json` starts with `{"source_lists": [], "languages": ["en"]}`).
  - Users configure `source_lists` and `languages` via the in-app Visual Builder, Raw JSON editor, or SSH file edit, then install `.aix` WASM plugins into `data/aidoku/sources/` and discover/search per source.
  - Uses the local Aidoku WASM runtime (`aidoku-server` / `rakuyomi.koplugin/server` over `/tmp/rakuyomi.sock`) pointed at `rm2manga`'s isolated `data/aidoku/` home directory.
- **Reader UX**:
  - Extract `.cbz` pages to local `/tmp/rm2manga_pages` cache and render full-bleed in QtQuick (`1404 x 1872`).
  - Direct `MultiPointTouchArea` handling raw `QTouchEvent`s + stylus events without `Flickable` touch-stealing or `onDoubleClicked` tap delay.
  - Configurable `Manga (RTL)` / `Comic (LTR)` tap zones + configurable `invertSwipe` toggle (`Swipe: → is Next` / `Swipe: ← is Next`).
  - Seamless chapter transitions with background next-chapter preloading near the end of the current chapter.

## 2. Verified Device & SDK Details (`root@10.11.99.1`)
- **Hardware / OS**: `armv7l` (`Linux 5.4.70-v1.6.4-rm11x`), `glibc 2.43`, **Qt 6.10.3**.
- **AppLoad Bundle Layout** (`/home/root/xovi/exthome/appload/rm2manga/`):
  - `manifest.json`, `icon.png`, `resources.rcc`, `backend/entry`, and `data/aidoku/settings.json`.
- **AppLoad IPC Wire Protocol**:
  - `socket(AF_UNIX, SOCK_SEQPACKET, 0)` connected to `argv[1]`.
  - `struct PacketHeader { int32_t type; int32_t messageLength; }` (8 bytes little-endian).
  - System message types: `-1` (`TERMINATE`), `-2` (`NEW_COORDINATOR`), `-3` (`LOST_COORDINATOR`).

## 3. Aidoku Runtime HTTP-over-UDS API
- **Launch**: `<server_bin> /home/root/xovi/exthome/appload/rm2manga/data/aidoku` listening on `/tmp/rakuyomi.sock`.
- **Key Endpoints**:
  - `GET /health-check`
  - `GET /installed-sources`, `GET /available-sources`, `POST /available-sources/<source_id>/install`, `DELETE /installed-sources/<source_id>`
  - `GET /mangas?q=<urlencoded>` (empty `q=` invokes each installed `.aix` plugin's default/popular manga listing for Discover)
  - `GET /library`, `POST /mangas/<source_id>/<manga_id>/add-to-library`, `POST /mangas/<source_id>/<manga_id>/remove-from-library`
  - `GET /mangas/<source_id>/<manga_id>/chapters`, `POST /mangas/<source_id>/<manga_id>/refresh-chapters`
  - `POST /jobs/download-chapter`, `GET /jobs/<uuid>`, `POST /mangas/<source_id>/<manga_id>/chapters/<chapter_id>/mark-as-read`
