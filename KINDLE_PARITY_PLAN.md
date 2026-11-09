# Kindle Oasis Feature Parity Plan

## Goal

Bring the native Kindle Oasis app toward the reMarkable app's core product experience while keeping the Kindle implementation native to its framebuffer, input devices, storage, and e-ink refresh behavior. Treat the reMarkable app (`ui/`, `backend/src/`) as the product reference; do not attempt to port Qt/AppLoad or the reMarkable IPC architecture to Kindle.

## Review scope and current shape

- Compared `kindlebranch` with `main` (`git diff main...HEAD`), including the reMarkable QML/backend implementation and the Kindle native C++ implementation.
- `main` has Library, Recent, Browse, Downloads, Manga Detail, and Reader views; unified Aidoku/MangaDex source management; persistent reading progress; search/discover; library filters; download queue controls; and configurable reader fit, zoom, swipe, and reading direction.
- Kindle currently implements a standalone framebuffer UI and `SourceManager`, with Library, Browse, Detail, Reader, Downloads, Settings, and Sources views. It has a basic on-disk library, chapter catalog cache, reading progress, chapter download, e-ink controls, and background next-chapter preload.
- Kindle is not wired to the reMarkable backend. Its source implementations and data model are in `kindle/native/src/source_manager.hpp` and `kindle/native/src/state_manager.hpp`.
- The Kindle tab bar currently exposes Library, Browse, Downloads, and Settings. Although progress is persisted, there is no Recent/history screen. `ViewState::SOURCES` displays hardcoded source descriptions and a rescan message; it does not configure repositories, install extensions, or manage source languages.

## Priority 0 — Make the Kindle foundation safe to extend

### 0.1 Separate UI, catalog, storage, reader, and device responsibilities

**Current:** Most interaction and rendering logic is concentrated in `kindle/native/src/app_ui.hpp`; source HTTP/parsing is in `source_manager.hpp`; state serialization is in `state_manager.hpp`.

**Work:** Establish explicit interfaces for catalog/source operations, local library/download storage, persisted history/settings, and reader navigation. Keep `AppUI` responsible for presentation and input routing. Move source-specific parsing and file naming out of UI handlers. Define stable IDs for manga and chapters using source ID plus source item ID; avoid title-only identity when titles collide.

**Acceptance:** UI screens call the same small interfaces regardless of source; state and downloaded chapters remain addressable across app restarts and title changes; adding a source does not require adding source-specific branches throughout `AppUI`.

### 0.2 Make slow work asynchronous and cancellable

**Current:** Search, popular catalog, chapter listing, and downloads are driven from UI interactions; input backlog is explicitly cleared after blocking work. The preload worker exists, but cancellation and shutdown paths need a focused audit.

**Work:** Run network/catalog and long file operations on workers, expose progress/result/error state to the UI, and provide cancellation where the user can wait on or cancel work. Ensure worker lifetime, synchronization, and UI refreshes are safe during navigation and shutdown. Avoid losing taps or freezing the e-ink UI while requests run.

**Acceptance:** A slow request leaves navigation responsive; loading, empty, error, retry, and completion states are visible; leaving a view or exiting cannot access destroyed state or leave a worker writing into a removed cache.

### 0.3 Verify Kindle target and deployment contract

**Current:** Native build/deployment is separate in `kindle/native/Makefile` and `scripts/deploy_kindle_native.sh`; KPM manifest names `kindlepw2`, while product wording targets Kindle Oasis. Build flags, actual Oasis model/firmware support, paths, framebuffer dimensions, and package metadata need to be treated as a single support matrix.

**Work:** Document supported Oasis generations/firmware and required jailbreak/runtime; validate package platform identifiers and launch paths against those devices; make build dependencies include headers and third-party source; ensure deployment does not rely on stale generated binaries. Keep credentials/device addresses configurable and do not bake developer machine paths into user-facing install instructions.

**Acceptance:** A clean checkout can build the Kindle binary with documented prerequisites; the package metadata and installation guide name the verified devices; deploy/install steps agree with the actual package layout.

## Priority 1 — Reach core navigation and library parity

### 1.1 Add a real Recent view and reliable resume

**Reference:** `ui/RecentView.qml`, `ui/main.qml`, and backend progress/history serialization.

**Current:** Kindle saves a `history` map and an active manga title, but exposes no Recent view. Progress keys and hero resume rely heavily on manga title; resume searches a cached chapter list and can fall back to chapter 0.

**Work:** Persist source/manga/chapter IDs, chapter number/title, page index/count, and timestamp. Add a Recent tab with newest-first ordering, cover/title/chapter/page/time, actions to resume or open details, and a clear empty state. Resume exact chapter/page first; provide a clear recovery path if a chapter was removed or source is unavailable.

**Acceptance:** Several manga appear in correct most-recent order; each resumes to the saved chapter/page after restart; same-title series from different sources remain distinct.

### 1.2 Improve Library controls and state

**Reference:** `ui/LibraryView.qml` and `backend/src/main.cpp` library payload.

**Current:** Kindle library can open a manga and show a continue-reading hero, but has no All/Unread/Downloaded filters, no consistent sort controls, and refreshes only by rescanning folders. Cover handling and library/download counts are split between state and disk scans.

**Work:** Add Library filters and deterministic sorting; make the hero resume exact progress; show unread/read and offline status; allow removal from the library; keep counts and cover metadata synchronized after add/remove/download. Add a robust empty state with a Browse action.

**Acceptance:** Filters produce correct lists and survive view changes; downloaded and unread indicators match persisted state; removing an item does not delete its downloaded files unless the user explicitly chooses that action.

### 1.3 Complete Downloads management

**Reference:** `ui/DownloadsView.qml` and backend download queue operations.

**Current:** Kindle exposes a Downloads screen with a single active-download indicator and downloaded CBZ rows. There is no durable queue listing, per-item cancellation, cancel-all, delete-download action, or cache/storage summary. The download screen is reachable, but its current tab mapping displaces Recent.

**Work:** Add a queue model with per-item status/progress and cancellation; support cancel-all, retry failed, open, and delete offline chapter; show storage used and a carefully scoped temporary-cache clear action. Persist enough queue/download metadata to recover cleanly after process termination, or explicitly mark interrupted work for retry.

**Acceptance:** Queue state is understandable while downloading; cancel/delete affect only the chosen item; deleted files disappear from chapter and library offline indicators; the screen reports offline storage use.

## Priority 2 — Bring source discovery and manga details to parity

### 2.1 Replace placeholder Sources view with source configuration

**Reference:** Sources modal and settings flow in `ui/BrowseView.qml`; Aidoku client/database in `backend/src/aidoku_client.hpp` and `backend/src/aidoku_db.hpp`.

**Current:** Kindle `SourceManager` hardcodes MangaDex, MangaFox, and MangaKatana. `renderSources()` displays hardcoded repository text and `handleSourcesTouch()` reports a rescan without fetching or installing anything. There is no source list/language settings file, install/uninstall lifecycle, or source health state.

**Work:** Define a source-provider interface first. Then choose and document a supported Kindle source strategy: retain maintained native adapters with honest availability/error states, or add a compatible extension/runtime integration if its dependencies and device constraints are feasible. Implement user-controlled source configuration, available/installed/active states, add/remove/refresh flows, and persisted per-source settings. Do not present placeholder repository or install states as functional.

**Acceptance:** Users can see which sources are available, installed, active, and failing; configuration is persisted; source-list failures have retryable messages; removing a source does not silently remove local library/download data.

### 2.2 Improve Browse search and discover

**Reference:** `ui/BrowseView.qml` search, source filters, discovery, keyboard, and result states.

**Current:** Kindle supports popular/search for one active hardcoded source at a time. Search entry has a custom keyboard, but there is no cross-source search, pagination/load-more contract, language filtering, or explicit empty/error/retry experience.

**Work:** Add result pagination or bounded load-more, preserve query/source when returning from detail, show loading/error/empty results, and make source scope obvious. Add cross-source search only after provider interfaces and cancellation are in place. Keep the keyboard usable at Kindle screen size and ensure touch and hardware page buttons do not trigger unintended navigation while typing.

**Acceptance:** Search results identify their source; repeated searches do not append stale results; users can retry failures and load more results without losing the query.

### 2.3 Complete manga detail and chapter actions

**Reference:** `ui/MangaDetailView.qml` and backend chapter metadata.

**Current:** Kindle has detail, add-to-library, read/download, sorting, and chapter pagination. Metadata display is sparse, chapter list controls are limited, and downloads/read state are primarily inferred from local CBZ files.

**Work:** Show available author/description/status/source metadata; expose refresh chapters; mark chapters read/unread; add bulk download where device constraints allow; make sort order and page position clear; represent downloaded/reading state consistently. Preserve full catalog metadata for seamless previous/next navigation, including gaps or special chapters.

**Acceptance:** Chapter state and sort order are stable across reopening; refresh does not lose locally downloaded matches; next/previous chapter navigation follows the displayed order and handles catalog boundaries clearly.

## Priority 3 — Reader controls, resilience, and e-ink polish

### 3.1 Match reader controls available on reMarkable

**Reference:** `ui/ReaderView.qml` and reader settings persisted by `ui/main.qml`.

**Current:** Kindle already has full-screen page display, RTL/LTR, page buttons, frontlight, periodic refresh, and chapter transitions/preload. Its persisted `trim_margins` setting is not exposed in Settings; no full-width mode, 2x zoom/pan, or configurable swipe direction equivalent is present. Reader fit behavior currently uses a single full-screen aspect-fit path.

**Work:** Add Fit Page, Trim Margins, and Full Width modes; add 2x zoom with predictable pan/exit behavior; expose configurable swipe polarity if touch swipes are supported. Keep hardware page-button behavior and RTL/LTR tap zones consistent. Expose refresh interval and ghost-clear controls with sensible defaults for e-ink.

**Acceptance:** Each fit mode behaves as labeled on portrait and landscape manga pages; zoom can be exited without losing the page; reader controls persist across restarts; refresh controls do not cause unnecessary full-screen flashing.

### 3.2 Make chapter transitions and preloading dependable

**Current:** Kindle has a five-page next-chapter preload and automatic boundary transition. Preload/download share chapter state, and the app stores full chapter catalogs.

**Work:** Define state transitions for preload success, failure, cancellation, source errors, already-downloaded chapters, navigation away, and app exit. Avoid duplicate downloads and stale worker results. Keep the manual download path and automatic preload from racing over the same chapter. Confirm cached page order is natural and image cache invalidation handles replaced/corrupt files.

**Acceptance:** Reaching a boundary opens a valid preloaded chapter immediately or shows a useful fallback; failed preload does not block manual download; repeated page turns never start duplicate workers; previous chapter works after app restart.

### 3.3 Accessibility and interaction polish for e-ink

**Work:** Review touch hit-target sizes, long labels, truncated text, keyboard dismissal, button page turns outside Reader, partial/full refresh strategy, ghosting, frontlight control range, and rotation mapping on every supported Oasis model. Provide user-visible feedback for all state-changing actions and confirmations for destructive offline-file deletion.

**Acceptance:** Every visible control responds in its drawn hit area; no essential action depends on color alone; view changes and page turns use appropriate refresh modes; no hidden screen has an inaccessible back path.

## Cross-cutting engineering work

- Add a small host-side test seam for state serialization, chapter identity/sorting, storage reconciliation, and provider response parsing. Add device/manual test notes for touch, buttons, rotation, frontlight, framebuffer refresh, and low-storage behavior.
- Add structured logging with request/source context while keeping credentials and sensitive configuration out of logs.
- Validate untrusted titles/IDs before using them in filesystem paths; use stable encoded directory names and atomic state writes with recoverable corruption behavior.
- Bound memory/disk use for cover/page caches and handle malformed CBZ/ZIP files, partial downloads, and low storage without corrupting the library.
- Keep Kindle-specific setup and feature claims separate from the reMarkable README unless the README is explicitly rewritten for both targets.

## Suggested coding-agent work units

Work units are ordered to minimize conflicts. Each should be implemented and reviewed independently against its acceptance criteria.

1. **State schema and identity migration** — stable IDs, progress/history migration, atomic persistence, and path safety.
2. **Kindle Recent screen** — history model, tab navigation, resume/details actions, and empty state.
3. **Async operation framework** — worker ownership, request IDs/cancellation, progress/error events, and shutdown safety.
4. **Download manager** — queue, cancel/retry/delete, storage summary, and disk/state reconciliation.
5. **Library parity** — filters, sorting, read/offline indicators, remove action, and hero behavior.
6. **Source provider/configuration decision and implementation** — define supported source lifecycle; replace placeholder UI with actual capabilities and honest states.
7. **Browse and detail parity** — pagination, source-aware search, metadata, refresh, and chapter read/download controls.
8. **Reader fit/zoom/settings** — fit modes, zoom/pan, swipe settings, refresh controls, and persistence.
9. **Stability and e-ink QA** — preload races, cache/storage failure cases, supported-device matrix, interaction and refresh audit.

## Completion definition

The Kindle app reaches practical parity when users can configure/use supported sources, search/discover titles, manage a library and reading history, inspect and download chapters, resume exact progress, read chapters with equivalent core controls, and recover from network/storage failures with clear UI feedback. Device-specific implementation details may differ; user-facing capabilities and persisted outcomes should be consistent.
