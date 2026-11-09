#pragma once

#include "canvas.hpp"
#include "display_zelda.hpp"
#include "input_kindle.hpp"
#include "zip_writer.hpp"
#include "font_manager.hpp"
#include "state_manager.hpp"
#include "source_manager.hpp"
#include "frontlight.hpp"
#include "../third_party/stb_image.h"
#include "../../../backend/src/cbz_extractor.hpp"
#include <vector>
#include <string>
#include <memory>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>

namespace fs = std::filesystem;

namespace kindle {

enum class ViewState {
    LIBRARY = 0,
    BROWSE,
    DETAIL,
    READER,
    DOWNLOADS,
    SETTINGS,
    SOURCES
};

class AppUI {
public:
    static constexpr int SCREEN_W = DisplayZelda::WIDTH;   // 1264
    static constexpr int SCREEN_H = DisplayZelda::HEIGHT;  // 1680
    static constexpr int NAV_H = 90;
    static constexpr int TOP_H = 80;

private:
    DisplayZelda display_;
    InputKindle input_;
    Canvas canvas_{SCREEN_W, SCREEN_H, DisplayZelda::STRIDE, nullptr};
    FontManager font_;
    StateManager state_;
    SourceManager sources_;

    std::atomic<bool> is_running_{true};
    ViewState current_view_ = ViewState::LIBRARY;
    int selected_tab_ = 0; // 0: Library, 1: Browse, 2: Recent, 3: Settings

    // Manga data
    std::vector<UnifiedManga> library_mangas_;
    std::vector<UnifiedManga> browse_mangas_;
    UnifiedManga selected_manga_;
    std::vector<UnifiedChapter> selected_chapters_;

    // Chapter and reader state
    int current_chapter_idx_ = -1;
    int reader_current_page_ = 0;
    int reader_total_pages_ = 0;
    std::vector<std::string> reader_page_files_;
    extractor::PageCache page_cache_;
    bool reader_show_overlay_ = false;
    int reader_page_turn_counter_ = 0;

    // Chapter list navigation & sorting
    int chapter_page_ = 0;
    bool detail_sort_descending_ = true;
    static constexpr int CHAPTERS_PER_PAGE = 11;

    // Search and Keyboard state
    bool show_keyboard_ = false;
    bool kb_shift_active_ = false;
    bool kb_symbols_mode_ = false;
    std::string search_query_;
    std::string status_banner_;

    // Live Download Progress
    bool download_active_ = false;
    std::string download_status_;
    int download_progress_pct_ = 0;

    // Scroll state
    int scroll_offset_ = 0;

    // Background Next Chapter Preloader (auto-downloads within 5 pages of chapter end)
    std::atomic<bool> preload_in_progress_{false};
    std::string preloading_chapter_id_;
    std::atomic<int> preload_pct_{0};
    std::string preload_status_;
    std::thread preload_thread_;
    std::mutex chapters_mu_;
    std::atomic<bool> shutting_down_{false};

public:
    AppUI() = default;

    ~AppUI() {
        shutting_down_ = true;
        if (preload_thread_.joinable()) {
            preload_thread_.join();
        }
    }

    bool init() {
        if (!std::getenv("RM2MANGA_HTTP_HELPER")) {
            ::setenv("RM2MANGA_HTTP_HELPER", "/mnt/us/extensions/rm2manga/bin/fetch.sh", 1);
        }

        if (!display_.init()) return false;
        canvas_ = Canvas(SCREEN_W, SCREEN_H, DisplayZelda::STRIDE, display_.getBuffer());
        if (!input_.start()) return false;

        font_.init();
        state_.load();
        input_.setRotation(state_.settings().touch_rotation);

        loadLibraryFromDisk();
        renderAll();
        display_.refreshFull(WAVEFORM_GC16);
        return true;
    }

    void run() {
        InputEvent ev;

        while (is_running_) {
            if (input_.pollEvent(ev)) {
                handleInput(ev);
            } else {
                usleep(15000);
            }
        }
    }

    void quit() {
        shutting_down_ = true;
        if (preload_thread_.joinable()) {
            preload_thread_.join();
        }
        state_.save();
        is_running_ = false;
    }

private:
    void loadLibraryFromDisk() {
        library_mangas_.clear();
        std::map<std::string, UnifiedManga> merged;

        // 1. Mangas saved in Library via [+ Add to Library]
        for (const auto& sm : state_.getLibraryMangas()) {
            merged[sm.title] = sm;
        }

        // 2. Scan /mnt/us/manga for offline downloads
        std::string manga_root = "/mnt/us/manga";
        std::error_code ec;

        if (fs::exists(manga_root, ec)) {
            for (const auto& entry : fs::directory_iterator(manga_root, ec)) {
                if (entry.is_directory()) {
                    std::string dirName = entry.path().filename().string();
                    if (dirName.empty() || dirName[0] == '.') continue;

                    UnifiedManga& m = merged[dirName];
                    if (m.title.empty()) {
                        m.title = dirName;
                        m.id = dirName;
                    }

                    int count = 0;
                    for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
                        if (f.path().extension() == ".cbz") count++;
                    }
                    m.downloaded_count = count;

                    // Check for local cover.jpg / cover.png, or extract first image from chapter
                    fs::path localCover = entry.path() / "cover.jpg";
                    fs::path localCoverPng = entry.path() / "cover.png";
                    if (fs::exists(localCoverPng, ec) && fs::file_size(localCoverPng, ec) > 0) {
                        m.cover_url = localCoverPng.string();
                    } else if (fs::exists(localCover, ec) && fs::file_size(localCover, ec) > 0) {
                        m.cover_url = localCover.string();
                    } else {
                        for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
                            if (f.path().extension() == ".cbz") {
                                std::string cmd = "IMG=$(unzip -l \"" + f.path().string() + "\" 2>/dev/null | grep -iE '\\.(jpg|jpeg|png|webp)$' | head -n 1 | awk '{print $NF}'); if [ -n \"$IMG\" ]; then unzip -p \"" + f.path().string() + "\" \"$IMG\" > \"" + localCover.string() + "\" 2>/dev/null; fi";
                                int ret = std::system(cmd.c_str());
                                (void)ret;
                                if (fs::exists(localCover, ec) && fs::file_size(localCover, ec) > 0) {
                                    m.cover_url = localCover.string();
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }

        for (const auto& kv : merged) {
            library_mangas_.push_back(kv.second);
        }

        std::sort(library_mangas_.begin(), library_mangas_.end(), [](const UnifiedManga& a, const UnifiedManga& b) {
            return util::naturalLess(a.title, b.title);
        });
    }

    void handleInput(const InputEvent& ev) {
        if (ev.type == EventType::KEY_NEXT_PAGE) {
            if (current_view_ == ViewState::READER) {
                if (state_.settings().button_inverted) prevPage(); else nextPage();
            } else if (current_view_ == ViewState::DETAIL) {
                nextChapterPage();
            }
            return;
        }
        if (ev.type == EventType::KEY_PREV_PAGE) {
            if (current_view_ == ViewState::READER) {
                if (state_.settings().button_inverted) nextPage(); else prevPage();
            } else if (current_view_ == ViewState::DETAIL) {
                prevChapterPage();
            }
            return;
        }

        if (ev.type == EventType::TOUCH_UP) {
            handleTouch(ev.x, ev.y);
        }
    }

    void handleTouch(int x, int y) {
        // 1. Bottom Navigation Bar touch (only outside Reader)
        if (y >= SCREEN_H - NAV_H && current_view_ != ViewState::READER) {
            int tab_w = SCREEN_W / 4;
            int tapped_tab = x / tab_w;
            if (tapped_tab >= 0 && tapped_tab < 4) {
                input_.clear();
                show_keyboard_ = false;
                selected_tab_ = tapped_tab;
                scroll_offset_ = 0;
                status_banner_.clear();

                if (selected_tab_ == 0) {
                    current_view_ = ViewState::LIBRARY;
                    loadLibraryFromDisk();
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                } else if (selected_tab_ == 1) {
                    current_view_ = ViewState::BROWSE;
                    if (browse_mangas_.empty()) {
                        status_banner_ = "Loading popular manga from " + sources_.getActiveSourceId() + "...";
                        renderAll();
                        display_.refreshFull(WAVEFORM_GC16);
                        fetchPopular();
                    }
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                } else if (selected_tab_ == 2) {
                    current_view_ = ViewState::DOWNLOADS;
                    loadLibraryFromDisk();
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                } else if (selected_tab_ == 3) {
                    current_view_ = ViewState::SETTINGS;
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                }
            }
            return;
        }

        // 2. Keyboard touch if active
        if (show_keyboard_) {
            if (y >= KB_Y && y < SCREEN_H - NAV_H) {
                handleKeyboardTouch(x, y);
                return;
            }
            if (y >= 10 && y <= TOP_H) {
                if (x >= SCREEN_W - 155) {
                    quit();
                    return;
                }
                if (x >= SCREEN_W - 470 && x < SCREEN_W - 325) {
                    show_keyboard_ = false;
                    if (!search_query_.empty()) {
                        performSearch(search_query_);
                    } else {
                        renderAll();
                        display_.refreshFull(WAVEFORM_GC16);
                    }
                    return;
                }
                if (x >= SCREEN_W - 325 && x < SCREEN_W - 155) {
                    show_keyboard_ = false;
                    current_view_ = ViewState::SOURCES;
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }
                if (x < SCREEN_W - 470) {
                    // Tap on search box while keyboard is up: keep keyboard open
                    return;
                }
            }
            // Tap outside keyboard dismisses it
            show_keyboard_ = false;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // 3. View-specific touch
        if (current_view_ == ViewState::LIBRARY) {
            handleLibraryTouch(x, y);
        } else if (current_view_ == ViewState::BROWSE) {
            handleBrowseTouch(x, y);
        } else if (current_view_ == ViewState::DETAIL) {
            handleDetailTouch(x, y);
        } else if (current_view_ == ViewState::DOWNLOADS) {
            handleDownloadsTouch(x, y);
        } else if (current_view_ == ViewState::SETTINGS) {
            handleSettingsTouch(x, y);
        } else if (current_view_ == ViewState::SOURCES) {
            handleSourcesTouch(x, y);
        } else if (current_view_ == ViewState::READER) {
            handleReaderTouch(x, y);
        }
    }

    void handleLibraryTouch(int x, int y) {
        // Exit App button in header
        if (x >= SCREEN_W - 200 && y >= 10 && y <= TOP_H) {
            quit();
            return;
        }

        // Hero Card Resume button: y: 100 to 270
        MangaProgress hero;
        if (state_.getHeroManga(hero) && y >= 100 && y <= 270) {
            resumeHero(hero);
            return;
        }

        int start_y = state_.getHeroManga(hero) ? 310 : (TOP_H + 20);
        int item_h = 140;

        for (size_t i = 0; i < library_mangas_.size(); ++i) {
            int item_y = start_y + static_cast<int>(i) * item_h - scroll_offset_;
            if (y >= item_y && y < item_y + item_h) {
                selected_manga_ = library_mangas_[i];
                if (!selected_manga_.source_id.empty() && selected_manga_.source_id != "local") {
                    sources_.setActiveSourceId(selected_manga_.source_id);
                }
                loadMangaChapters(selected_manga_);
                current_view_ = ViewState::DETAIL;
                scroll_offset_ = 0;
                status_banner_.clear();
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
        }
    }

    void resumeHero(const MangaProgress& hero) {
        selected_manga_.title = hero.manga_title;
        selected_manga_.id = hero.manga_title;
        for (const auto& libM : library_mangas_) {
            if (libM.title == hero.manga_title) {
                selected_manga_ = libM;
                break;
            }
        }
        loadMangaChapters(selected_manga_);

        double heroNum = parseChapterNumber(hero.chapter_title);
        for (size_t i = 0; i < selected_chapters_.size(); ++i) {
            double chNum = parseChapterNumber(selected_chapters_[i].chapterNum.empty() ? selected_chapters_[i].title : selected_chapters_[i].chapterNum);
            if (selected_chapters_[i].title == hero.chapter_title ||
                selected_chapters_[i].chapterNum == hero.chapter_title ||
                (chNum >= 0 && heroNum >= 0 && std::abs(chNum - heroNum) < 0.0001)) {
                openChapter(static_cast<int>(i), true, hero.page_index);
                return;
            }
        }
        if (!selected_chapters_.empty()) {
            openChapter(0);
        }
    }

    void loadMangaChapters(const UnifiedManga& manga) {
        selected_manga_ = manga;
        selected_chapters_.clear();
        chapter_page_ = 0;

        // 1. Try to load cached full chapter catalog
        selected_chapters_ = state_.getCachedChapters(manga.title, manga.source_id, manga.id);

        // 2. If cached chapter catalog is empty and remote source is known, fetch full catalog from source
        if (selected_chapters_.empty() && !manga.id.empty() && !manga.source_id.empty() && manga.source_id != "local") {
            sources_.setActiveSourceId(manga.source_id);
            fetchChapters(manga.id);
            if (!selected_chapters_.empty()) {
                return;
            }
        }

        // 3. Reconcile with local storage for downloaded .cbz files
        std::string dirPath = "/mnt/us/manga/" + manga.title;
        std::error_code ec;

        if (fs::exists(dirPath, ec)) {
            for (const auto& f : fs::directory_iterator(dirPath, ec)) {
                if (f.path().extension() == ".cbz") {
                    std::string stem = f.path().stem().string();
                    double fileNum = parseChapterNumber(stem);
                    bool matched = false;

                    for (auto& ch : selected_chapters_) {
                        double chNum = parseChapterNumber(ch.chapterNum.empty() ? ch.title : ch.chapterNum);
                        std::string safeCh = ch.chapterNum.empty() ? "chapter" : ("chapter_" + ch.chapterNum);
                        if (ch.title == stem || ch.chapterNum == stem || safeCh == stem ||
                            (fileNum >= 0 && chNum >= 0 && std::abs(fileNum - chNum) < 0.0001)) {
                            ch.isDownloaded = true;
                            ch.localPath = f.path().string();
                            matched = true;
                            break;
                        }
                    }

                    if (!matched) {
                        UnifiedChapter localCh;
                        localCh.title = stem;
                        localCh.chapterNum = stem;
                        localCh.localPath = f.path().string();
                        localCh.isDownloaded = true;
                        selected_chapters_.push_back(localCh);
                    }
                }
            }
        }

        sortChapters();
        if (!selected_chapters_.empty()) {
            state_.saveChaptersCache(manga.title, manga.source_id, manga.id, selected_chapters_);
        }
    }

    void handleBrowseTouch(int x, int y) {
        // Search bar, Search button, ⚙ Sources, or ✕ Exit clicked
        if (y >= 10 && y <= TOP_H - 10) {
            if (x >= SCREEN_W - 155) {
                quit();
                return;
            } else if (x >= SCREEN_W - 325) {
                show_keyboard_ = false;
                current_view_ = ViewState::SOURCES;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            } else if (x >= SCREEN_W - 470) {
                if (!search_query_.empty()) {
                    performSearch(search_query_);
                } else {
                    show_keyboard_ = true;
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                }
                return;
            } else {
                show_keyboard_ = true;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
        }

        // Source Switcher Pills: y: 90 to 155
        if (y >= 90 && y <= 155) {
            const auto& srcList = sources_.getSources();
            int pill_w = 195;
            int pill_x = 110;

            for (const auto& s : srcList) {
                if (x >= pill_x && x <= pill_x + pill_w) {
                    show_keyboard_ = false;
                    sources_.setActiveSourceId(s.id);
                    status_banner_ = "Switched source to " + s.name;
                    fetchPopular();
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }
                pill_x += 210;
            }
        }

        // Browse list item clicked
        int start_y = 170;
        int item_h = 140;

        for (size_t i = 0; i < browse_mangas_.size(); ++i) {
            int item_y = start_y + static_cast<int>(i) * item_h - scroll_offset_;
            if (y >= item_y && y < item_y + item_h) {
                selected_manga_ = browse_mangas_[i];
                status_banner_ = "Loading chapters for " + selected_manga_.title + "...";
                renderBrowse();
                display_.refreshUI(0, 0, SCREEN_W, TOP_H + 80);

                fetchChapters(selected_manga_.id);
                current_view_ = ViewState::DETAIL;
                scroll_offset_ = 0;
                status_banner_.clear();
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
        }
    }

    void handleDetailTouch(int x, int y) {
        // Exit App button in top bar
        if (x >= SCREEN_W - 155 && y >= 10 && y <= TOP_H) {
            quit();
            return;
        }

        // Back button (< Back)
        if (x >= 20 && x <= 180 && y >= 15 && y <= 70) {
            current_view_ = (selected_tab_ == 0) ? ViewState::LIBRARY : ViewState::BROWSE;
            scroll_offset_ = 0;
            status_banner_.clear();
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // Primary Read button: x: 180..420, y: TOP_H + 155..TOP_H + 225
        if (x >= 180 && x <= 420 && y >= TOP_H + 155 && y <= TOP_H + 225) {
            MangaProgress prog;
            if (state_.getProgress(selected_manga_.title, prog)) {
                resumeHero(prog);
            } else if (!selected_chapters_.empty()) {
                openChapter(0);
            }
            return;
        }

        // [+ Add to Library] / [✓ In Library] toggle button: x: 425..680, y: TOP_H + 155..TOP_H + 225
        if (x >= 425 && x <= 680 && y >= TOP_H + 155 && y <= TOP_H + 225) {
            bool inLib = state_.toggleLibrary(selected_manga_);
            status_banner_ = inLib ? "✓ Added \"" + selected_manga_.title + "\" to Library" : "Removed from Library";
            renderDetail();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // Sort button toggle: y: 375 to 445, right aligned
        if (x >= SCREEN_W - 280 && x <= SCREEN_W - 20 && y >= 375 && y <= 445) {
            detail_sort_descending_ = !detail_sort_descending_;
            sortChapters();
            chapter_page_ = 0;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // Pagination buttons: y between 1480 and 1580
        if (y >= 1480 && y <= 1580) {
            if (x >= 20 && x <= 230) {
                prevChapterPage();
                return;
            }
            if (x >= SCREEN_W - 230 && x <= SCREEN_W - 20) {
                nextChapterPage();
                return;
            }
        }

        // Chapter tapped: start_y = 455
        int start_y = 455;
        int item_h = 76;
        int item_gap = 10;
        int row_stride = item_h + item_gap;

        if (y >= start_y && y < start_y + CHAPTERS_PER_PAGE * row_stride) {
            int row = (y - start_y) / row_stride;
            int idx = chapter_page_ * CHAPTERS_PER_PAGE + row;
            if (idx >= 0 && idx < static_cast<int>(selected_chapters_.size())) {
                if (selected_chapters_[idx].isDownloaded) {
                    openChapter(idx);
                } else {
                    downloadChapter(idx);
                }
                return;
            }
        }
    }

    void handleDownloadsTouch(int x, int y) {
        // Exit App button in top bar
        if (x >= SCREEN_W - 155 && y >= 10 && y <= TOP_H) {
            quit();
            return;
        }

        int start_y = TOP_H + 20 + (download_active_ ? 95 : 0);
        int item_h = 95;
        int item_gap = 12;
        int row_stride = item_h + item_gap;
        int idx = 0;

        for (const auto& manga : library_mangas_) {
            std::string dirPath = "/mnt/us/manga/" + manga.title;
            std::error_code ec;
            if (fs::exists(dirPath, ec)) {
                for (const auto& f : fs::directory_iterator(dirPath, ec)) {
                    if (f.path().extension() == ".cbz") {
                        int item_y = start_y + idx * row_stride - scroll_offset_;
                        if (y >= item_y && y < item_y + item_h) {
                            selected_manga_ = manga;
                            loadMangaChapters(selected_manga_);
                            for (size_t c = 0; c < selected_chapters_.size(); ++c) {
                                if (selected_chapters_[c].localPath == f.path().string() ||
                                    selected_chapters_[c].title == f.path().stem().string()) {
                                    openChapter(static_cast<int>(c));
                                    return;
                                }
                            }
                            openChapter(0);
                            return;
                        }
                        idx++;
                        if (item_y > SCREEN_H - NAV_H - item_h) break;
                    }
                }
            }
        }
    }

    void handleSettingsTouch(int x, int y) {
        // Exit App button in top bar
        if (x >= SCREEN_W - 155 && y >= 10 && y <= TOP_H) {
            quit();
            return;
        }

        // Frontlight slider [-] [+] : y: 195..275 (buttons drawn at y = TOP_H + 35 + 60 = 205, h=55)
        if (y >= 195 && y <= 275) {
            if (x >= 290 && x <= 380) { // [-] drawn at x=300, w=70
                int cur = state_.settings().frontlight;
                cur = std::max(0, cur - 2);
                state_.settings().frontlight = cur;
                Frontlight::setIntensity(cur);
                state_.save();
                renderSettings();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
            if (x >= 480 && x <= 570) { // [+] drawn at x=490, w=70
                int cur = state_.settings().frontlight;
                cur = std::min(24, cur + 2);
                state_.settings().frontlight = cur;
                Frontlight::setIntensity(cur);
                state_.save();
                renderSettings();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
        }

        // Reading Mode toggle (RTL / LTR): y: 275..355 (drawn at y = 205 + 80 = 285, x=300..560)
        if (x >= 290 && x <= 570 && y >= 275 && y <= 355) {
            if (state_.settings().reading_direction == "RTL") {
                state_.settings().reading_direction = "LTR";
            } else {
                state_.settings().reading_direction = "RTL";
            }
            state_.save();
            renderSettings();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // Grip / Touch Orientation toggle: y: 355..435
        if (x >= 290 && x <= 630 && y >= 355 && y <= 435) {
            int newRot = (state_.settings().touch_rotation == 180 ? 0 : 180);
            state_.settings().touch_rotation = newRot;
            input_.setRotation(newRot);
            state_.save();
            status_banner_ = "Touch orientation: " + std::string(newRot == 180 ? "Buttons Left (180°)" : "Buttons Right (0°)");
            renderSettings();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // Exit to Kindle Home button: x: 100..SCREEN_W-100, y: 470..550
        if (x >= 100 && x <= SCREEN_W - 100 && y >= 470 && y <= 550) {
            quit();
            return;
        }

        // Clear Cache button: x: 100..SCREEN_W-100, y: 565..645
        if (x >= 100 && x <= SCREEN_W - 100 && y >= 565 && y <= 645) {
            page_cache_.clearAll();
            std::error_code ec;
            fs::remove_all("/mnt/us/manga/.cache", ec);
            status_banner_ = "Reader & cover cache cleared successfully!";
            renderSettings();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // Refresh Library button: x: 100..SCREEN_W-100, y: 660..740
        if (x >= 100 && x <= SCREEN_W - 100 && y >= 660 && y <= 740) {
            loadLibraryFromDisk();
            status_banner_ = "Library refreshed (" + std::to_string(library_mangas_.size()) + " manga)!";
            renderSettings();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }
    }

    void handleSourcesTouch(int x, int y) {
        // Exit App button in top bar
        if (x >= SCREEN_W - 155 && y >= 10 && y <= TOP_H) {
            quit();
            return;
        }

        // Back button to Browse
        if (x >= SCREEN_W - 330 && x < SCREEN_W - 155 && y >= 10 && y <= TOP_H) {
            current_view_ = ViewState::BROWSE;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        int start_y = TOP_H + 35;
        int card_h = 130;
        const auto& srcList = sources_.getSources();

        for (size_t i = 0; i < srcList.size(); ++i) {
            int cy = start_y + static_cast<int>(i) * (card_h + 15);
            if (y >= cy && y <= cy + card_h) {
                sources_.setActiveSourceId(srcList[i].id);
                status_banner_ = "Active source: " + srcList[i].name;
                fetchPopular();
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
        }

        // Action button: Rescan Extension Catalog
        int infoY = start_y + static_cast<int>(srcList.size()) * (card_h + 15) + 20;
        int boxY = infoY + 45;
        int btnY = boxY + 180;
        if (x >= 40 && x <= SCREEN_W - 40 && y >= btnY && y <= btnY + 70) {
            status_banner_ = "Extension catalog up to date (MangaDex, MangaFox, MangaKatana).";
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }
    }

    void handleReaderTouch(int x, int y) {
        // 1. If overlay is active
        if (reader_show_overlay_) {
            // Top bar
            if (y <= TOP_H + 15) {
                if (x <= 250) {
                    // < Back button
                    current_view_ = ViewState::DETAIL;
                    reader_show_overlay_ = false;
                    input_.clear();
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }
                // Tap elsewhere on top bar: close overlay
                reader_show_overlay_ = false;
                renderReader();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }

            // Middle Hardware Light Slider & Mode: y: 810 to 900
            if (y >= 810 && y <= 900) {
                if (x >= SCREEN_W / 2 - 140 && x <= SCREEN_W / 2 - 60) { // Light [-]
                    int cur = state_.settings().frontlight;
                    cur = std::max(0, cur - 2);
                    state_.settings().frontlight = cur;
                    Frontlight::setIntensity(cur);
                    renderReader();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }
                if (x >= SCREEN_W / 2 && x <= SCREEN_W / 2 + 80) { // Light [+]
                    int cur = state_.settings().frontlight;
                    cur = std::min(24, cur + 2);
                    state_.settings().frontlight = cur;
                    Frontlight::setIntensity(cur);
                    renderReader();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }
                if (x >= SCREEN_W / 2 + 85 && x <= SCREEN_W / 2 + 240) { // Mode
                    if (state_.settings().reading_direction == "RTL") {
                        state_.settings().reading_direction = "LTR";
                    } else {
                        state_.settings().reading_direction = "RTL";
                    }
                    state_.save();
                    renderReader();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }
            }

            // Bottom bar
            if (y >= SCREEN_H - 100) {
                if (x < SCREEN_W * 0.35) {
                    prevPage();
                } else if (x > SCREEN_W * 0.65) {
                    nextPage();
                } else {
                    reader_show_overlay_ = false;
                    renderReader();
                    display_.refreshFull(WAVEFORM_GC16);
                }
                return;
            }

            // Tap elsewhere dismisses overlay
            reader_show_overlay_ = false;
            renderReader();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // 2. Fullscreen Reading:
        // Top 10% or Center tap invokes menu
        if (y <= 120 || (x >= SCREEN_W * 0.3 && x <= SCREEN_W * 0.7 && y >= SCREEN_H * 0.25 && y <= SCREEN_H * 0.75)) {
            reader_show_overlay_ = true;
            renderReader();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // 3. Page Turn (Respects RTL / LTR setting)
        bool isRTL = (state_.settings().reading_direction == "RTL");
        if (x < SCREEN_W * 0.45) {
            if (isRTL) nextPage(); else prevPage();
        } else if (x > SCREEN_W * 0.55) {
            if (isRTL) prevPage(); else nextPage();
        }
    }

    int getNextChapterIndex() const {
        if (current_chapter_idx_ < 0 || selected_chapters_.empty()) return -1;
        const auto& curCh = selected_chapters_[current_chapter_idx_];
        double curNum = parseChapterNumber(curCh.chapterNum.empty() ? curCh.title : curCh.chapterNum);

        // 1. If chapter number is known, find the chapter with the smallest number strictly greater than curNum
        if (curNum >= 0) {
            int bestIdx = -1;
            double bestNum = 1e9;
            for (int i = 0; i < static_cast<int>(selected_chapters_.size()); ++i) {
                double n = parseChapterNumber(selected_chapters_[i].chapterNum.empty() ? selected_chapters_[i].title : selected_chapters_[i].chapterNum);
                if (n > curNum + 0.0001 && n < bestNum) {
                    bestNum = n;
                    bestIdx = i;
                }
            }
            return bestIdx;
        }

        // 2. Unnumbered chapters fallback based on list ordering
        if (detail_sort_descending_) {
            if (current_chapter_idx_ > 0) return current_chapter_idx_ - 1;
        } else {
            if (current_chapter_idx_ + 1 < static_cast<int>(selected_chapters_.size())) return current_chapter_idx_ + 1;
        }

        return -1;
    }

    int getPrevChapterIndex() const {
        if (current_chapter_idx_ < 0 || selected_chapters_.empty()) return -1;
        const auto& curCh = selected_chapters_[current_chapter_idx_];
        double curNum = parseChapterNumber(curCh.chapterNum.empty() ? curCh.title : curCh.chapterNum);

        // 1. If chapter number is known, find the chapter with the largest number strictly less than curNum
        if (curNum >= 0) {
            int bestIdx = -1;
            double bestNum = -1.0;
            for (int i = 0; i < static_cast<int>(selected_chapters_.size()); ++i) {
                double n = parseChapterNumber(selected_chapters_[i].chapterNum.empty() ? selected_chapters_[i].title : selected_chapters_[i].chapterNum);
                if (n >= 0 && n < curNum - 0.0001 && n > bestNum) {
                    bestNum = n;
                    bestIdx = i;
                }
            }
            return bestIdx;
        }

        // 2. Unnumbered chapters fallback based on list ordering
        if (detail_sort_descending_) {
            if (current_chapter_idx_ + 1 < static_cast<int>(selected_chapters_.size())) return current_chapter_idx_ + 1;
        } else {
            if (current_chapter_idx_ > 0) return current_chapter_idx_ - 1;
        }

        return -1;
    }

    void renderReaderToast(const std::string& msg) {
        int w = 560;
        int h = 80;
        int x = (SCREEN_W - w) / 2;
        int y = SCREEN_H / 2 - 40;
        canvas_.fillRect(x, y, w, h, 0x00);
        canvas_.drawRect(x, y, w, h, 0xFF, 2);
        int tw = font_.measureText(msg, 26, true);
        font_.drawText(canvas_, x + (w - tw) / 2, y + 25, msg, 26, 0xFF, true);
        display_.update(x, y, w, h, WAVEFORM_DU, UPDATE_PARTIAL);
    }

    void renderReaderDownloadProgress(const std::string& status, int pct) {
        int w = 620;
        int h = 170;
        int x = (SCREEN_W - w) / 2;
        int y = (SCREEN_H - h) / 2;

        canvas_.fillRect(x, y, w, h, 0xFF);
        canvas_.drawRect(x, y, w, h, 0x00, 3);

        std::string s = status;
        if (s.length() > 38) s = s.substr(0, 36) + "...";
        int tw = font_.measureText(s, 24, true);
        font_.drawText(canvas_, x + (w - tw) / 2, y + 25, s, 24, 0x00, true);

        // Progress bar
        int bar_w = w - 80;
        int bar_h = 24;
        int bar_x = x + 40;
        int bar_y = y + 75;
        canvas_.drawRect(bar_x, bar_y, bar_w, bar_h, 0x00, 2);
        int filled_w = (bar_w - 4) * std::max(0, std::min(100, pct)) / 100;
        if (filled_w > 0) {
            canvas_.fillRect(bar_x + 2, bar_y + 2, filled_w, bar_h - 4, 0x00);
        }

        std::string pctStr = std::to_string(pct) + "%";
        int ptw = font_.measureText(pctStr, 22, true);
        font_.drawText(canvas_, x + (w - ptw) / 2, y + 120, pctStr, 22, 0x50, true);

        display_.update(x, y, w, h, WAVEFORM_DU, UPDATE_PARTIAL);
    }

    void openChapterAtEnd(int chIdx) {
        if (chIdx < 0 || chIdx >= static_cast<int>(selected_chapters_.size())) return;
        openChapter(chIdx, true, 999999);
    }

    void triggerBackgroundPreload() {
        if (preload_in_progress_.load() || shutting_down_.load()) return;

        int nextIdx = getNextChapterIndex();
        if (nextIdx < 0 || nextIdx >= static_cast<int>(selected_chapters_.size())) return;

        std::string safeManga = selected_manga_.title;
        std::string safeCh;
        std::string nextCid;
        UnifiedChapter nextCh;
        {
            std::lock_guard<std::mutex> lock(chapters_mu_);
            nextCh = selected_chapters_[nextIdx];
            safeCh = nextCh.chapterNum.empty() ? "chapter" : ("chapter_" + nextCh.chapterNum);
            nextCid = nextCh.id.empty() ? nextCh.title : nextCh.id;
        }

        fs::path destDir = "/mnt/us/manga/" + safeManga;
        fs::path cbzFile = destDir / (safeCh + ".cbz");
        std::error_code ec;

        if (fs::exists(cbzFile, ec)) {
            // Already downloaded on disk! Ensure in-memory status is marked
            std::lock_guard<std::mutex> lock(chapters_mu_);
            selected_chapters_[nextIdx].isDownloaded = true;
            selected_chapters_[nextIdx].localPath = cbzFile.string();
            return;
        } else {
            double chNum = parseChapterNumber(nextCh.chapterNum.empty() ? nextCh.title : nextCh.chapterNum);
            if (chNum >= 0 && fs::exists(destDir, ec)) {
                for (const auto& f : fs::directory_iterator(destDir, ec)) {
                    if (f.path().extension() == ".cbz") {
                        double fNum = parseChapterNumber(f.path().stem().string());
                        if (fNum >= 0 && std::abs(fNum - chNum) < 0.0001) {
                            std::lock_guard<std::mutex> lock(chapters_mu_);
                            selected_chapters_[nextIdx].isDownloaded = true;
                            selected_chapters_[nextIdx].localPath = f.path().string();
                            return;
                        }
                    }
                }
            }
        }

        // Start background preloading
        preload_in_progress_ = true;
        preloading_chapter_id_ = nextCid;
        preload_pct_ = 5;

        if (preload_thread_.joinable()) {
            preload_thread_.join();
        }

        preload_thread_ = std::thread([this, nextIdx]() {
            downloadChapterInBackground(nextIdx);
            preload_in_progress_ = false;
        });
    }

    void downloadChapterInBackground(int chIdx) {
        UnifiedChapter ch;
        std::string mangaTitle;
        {
            std::lock_guard<std::mutex> lock(chapters_mu_);
            if (chIdx < 0 || chIdx >= static_cast<int>(selected_chapters_.size())) return;
            ch = selected_chapters_[chIdx];
            mangaTitle = selected_manga_.title;
        }

        if (ch.isDownloaded) return;

        fs::path destDir = "/mnt/us/manga/" + mangaTitle;
        std::string safeId = ch.id.empty() ? ch.title : ch.id;
        for (char& c : safeId) if (c == '/' || c == ':' || c == ' ') c = '_';
        fs::path tmpExtract = "/tmp/rm2manga_dl_" + safeId;

        std::error_code ec;
        fs::create_directories(tmpExtract, ec);

        std::atomic<bool> cancel{false};
        std::string err;
        bool ok = false;

        preload_pct_ = 10;

        std::string srcId = ch.source_id.empty() ? selected_manga_.source_id : ch.source_id;
        ch.source_id = srcId;

        if (srcId == "mangafox" || srcId == "mangakatana") {
            std::vector<std::string> pageUrls = sources_.getChapterPageUrls(ch, &err);
            if (pageUrls.empty() || shutting_down_.load()) {
                fs::remove_all(tmpExtract, ec);
                return;
            }

            int total = static_cast<int>(pageUrls.size());
            int downloaded = 0;
            for (int p = 0; p < total; ++p) {
                if (shutting_down_.load()) break;
                char fname[32];
                snprintf(fname, sizeof(fname), "page_%03d.jpg", p + 1);
                fs::path pageFile = tmpExtract / fname;

                int pct = 10 + ((p + 1) * 75) / total;
                preload_pct_ = pct;

                if (util::httpsDownloadFile(pageUrls[p], pageFile.string(), 30)) {
                    downloaded++;
                }
            }
            ok = (downloaded > 0);
        } else {
            // MangaDex
            ok = sources_.mangadexClient().downloadChapterPages(ch.id, tmpExtract, &cancel,
                [this](const std::string& msg, int pct) {
                    preload_pct_ = pct;
                }, &err);
        }

        if (ok && !shutting_down_.load()) {
            preload_pct_ = 90;
            fs::create_directories(destDir, ec);
            std::string safeCh = ch.chapterNum.empty() ? "chapter" : ("chapter_" + ch.chapterNum);
            fs::path cbzFile = destDir / (safeCh + ".cbz");

            ZipStoreWriter zip;
            if (zip.open(cbzFile.string())) {
                std::vector<fs::path> pageFiles;
                for (const auto& entry : fs::directory_iterator(tmpExtract, ec)) {
                    if (entry.is_regular_file(ec)) {
                        pageFiles.push_back(entry.path());
                    }
                }
                std::sort(pageFiles.begin(), pageFiles.end(), [](const fs::path& a, const fs::path& b) {
                    return util::naturalLess(a.filename().string(), b.filename().string());
                });

                for (const auto& pf : pageFiles) {
                    std::string bytes;
                    if (util::readFileToString(pf, bytes)) {
                        std::vector<uint8_t> vec(bytes.begin(), bytes.end());
                        zip.add(pf.filename().string(), vec);
                    }
                }
                zip.close();
                fs::remove_all(tmpExtract, ec);

                // Save first page as cover if missing
                fs::path coverFile = destDir / "cover.jpg";
                if (!fs::exists(coverFile, ec) && !pageFiles.empty()) {
                    fs::copy_file(pageFiles[0], coverFile, ec);
                }

                // Update in-memory chapter state
                ch.isDownloaded = true;
                ch.localPath = cbzFile.string();
                {
                    std::lock_guard<std::mutex> lock(chapters_mu_);
                    if (chIdx < static_cast<int>(selected_chapters_.size())) {
                        selected_chapters_[chIdx] = ch;
                    }
                }

                // Pre-extract into page cache so it's instantly decoded
                std::set<std::string> pinned;
                page_cache_.extractCbz(ch.id.empty() ? ch.title : ch.id, cbzFile, pinned, &err);
                preload_pct_ = 100;
            }
        }
        fs::remove_all(tmpExtract, ec);
    }

    void transitionToNextChapter(int nextIdx) {
        if (nextIdx < 0 || nextIdx >= static_cast<int>(selected_chapters_.size())) return;

        // If background preloader is currently fetching this chapter, display progress and await completion
        if (preload_in_progress_.load()) {
            std::string chNum = selected_chapters_[nextIdx].chapterNum;
            renderReaderDownloadProgress("Downloading next chapter (Ch. " + chNum + ")...", preload_pct_.load());

            while (preload_in_progress_.load() && !shutting_down_.load()) {
                renderReaderDownloadProgress("Downloading next chapter (Ch. " + chNum + ")...", preload_pct_.load());
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }

            if (preload_thread_.joinable()) {
                preload_thread_.join();
            }
        }

        openChapter(nextIdx);
    }

    void nextPage() {
        if (reader_current_page_ + 1 < reader_total_pages_) {
            reader_current_page_++;
            recordProgress();
            renderReader();
            reader_page_turn_counter_++;
            if (reader_page_turn_counter_ % state_.settings().refresh_interval == 0) {
                display_.refreshFull(WAVEFORM_GC16);
            } else {
                display_.refreshPage();
            }

            // Background pre-fetch next chapter when within 5 pages of chapter end
            if (reader_total_pages_ > 0 && reader_current_page_ >= reader_total_pages_ - 5) {
                triggerBackgroundPreload();
            }
        } else {
            // Reached the end of the chapter: transition to next chapter
            int nextIdx = getNextChapterIndex();
            if (nextIdx >= 0) {
                transitionToNextChapter(nextIdx);
            } else {
                renderReaderToast("End of manga reached");
            }
        }
    }

    void prevPage() {
        if (reader_current_page_ > 0) {
            reader_current_page_--;
            recordProgress();
            renderReader();
            reader_page_turn_counter_++;
            if (reader_page_turn_counter_ % state_.settings().refresh_interval == 0) {
                display_.refreshFull(WAVEFORM_GC16);
            } else {
                display_.refreshPage();
            }
        } else {
            // Reached start of chapter: go to end of previous chapter
            int prevIdx = getPrevChapterIndex();
            if (prevIdx >= 0) {
                openChapterAtEnd(prevIdx);
            } else {
                renderReaderToast("Beginning of manga");
            }
        }
    }

    void recordProgress() {
        if (current_chapter_idx_ >= 0 && current_chapter_idx_ < static_cast<int>(selected_chapters_.size())) {
            std::string chName = selected_chapters_[current_chapter_idx_].chapterNum;
            state_.updateProgress(selected_manga_.title, chName, reader_current_page_, reader_total_pages_);
        }
    }

    void openChapter(int chIdx, bool autoOpenAfterDownload = true, int startPage = 0) {
        if (chIdx < 0 || chIdx >= static_cast<int>(selected_chapters_.size())) return;
        current_chapter_idx_ = chIdx;
        auto& ch = selected_chapters_[chIdx];

        fs::path destDir = "/mnt/us/manga/" + selected_manga_.title;
        std::string safeCh = ch.chapterNum.empty() ? "chapter" : ("chapter_" + ch.chapterNum);
        fs::path cbzFile = destDir / (safeCh + ".cbz");
        std::error_code ec;

        if (fs::exists(cbzFile, ec)) {
            ch.isDownloaded = true;
            ch.localPath = cbzFile.string();
        } else {
            double chNum = parseChapterNumber(ch.chapterNum.empty() ? ch.title : ch.chapterNum);
            if (chNum >= 0 && fs::exists(destDir, ec)) {
                for (const auto& f : fs::directory_iterator(destDir, ec)) {
                    if (f.path().extension() == ".cbz") {
                        double fNum = parseChapterNumber(f.path().stem().string());
                        if (fNum >= 0 && std::abs(fNum - chNum) < 0.0001) {
                            ch.isDownloaded = true;
                            ch.localPath = f.path().string();
                            break;
                        }
                    }
                }
            }
        }

        if (ch.isDownloaded && fs::exists(ch.localPath, ec)) {
            std::string err;
            std::set<std::string> pinned;
            reader_page_files_ = page_cache_.extractCbz(ch.id.empty() ? ch.title : ch.id, ch.localPath, pinned, &err);
            reader_total_pages_ = static_cast<int>(reader_page_files_.size());
            reader_current_page_ = std::min(startPage, std::max(0, reader_total_pages_ - 1));
            reader_show_overlay_ = false;
            current_view_ = ViewState::READER;
            recordProgress();
            renderReader();
            display_.refreshFull(WAVEFORM_GC16);

            // If within 5 pages of chapter end, start preloading next chapter right away
            if (reader_total_pages_ > 0 && reader_current_page_ >= reader_total_pages_ - 5) {
                triggerBackgroundPreload();
            }
        } else {
            downloadChapter(chIdx, autoOpenAfterDownload);
        }
    }

    void updateDownloadProgress(const std::string& status, int pct) {
        download_active_ = true;
        download_status_ = status;
        download_progress_pct_ = pct;
        if (current_view_ == ViewState::READER) {
            renderReaderDownloadProgress(status, pct);
        } else {
            renderDetail();
            display_.refreshUI(25, 290, SCREEN_W - 50, 85);
        }
    }

    void downloadChapter(int chIdx, bool autoOpen = false) {
        if (chIdx < 0 || chIdx >= static_cast<int>(selected_chapters_.size())) return;
        auto& ch = selected_chapters_[chIdx];

        if (ch.source_id.empty()) {
            ch.source_id = selected_manga_.source_id;
        }

        updateDownloadProgress("Connecting to " + ch.source_id + " for Ch. " + ch.chapterNum + "...", 5);

        fs::path destDir = "/mnt/us/manga/" + selected_manga_.title;
        std::string safeId = ch.id.empty() ? ch.title : ch.id;
        for (char& c : safeId) if (c == '/' || c == ':' || c == ' ') c = '_';
        fs::path tmpExtract = "/tmp/rm2manga_dl_" + safeId;

        std::error_code ec;
        fs::create_directories(tmpExtract, ec);

        std::atomic<bool> cancel{false};
        std::string err;
        bool ok = false;

        if (ch.source_id == "mangafox" || ch.source_id == "mangakatana") {
            updateDownloadProgress("Fetching page list from " + ch.source_id + "...", 10);

            std::vector<std::string> pageUrls = sources_.getChapterPageUrls(ch, &err);
            if (pageUrls.empty()) {
                download_active_ = false;
                status_banner_ = "Failed fetching pages: " + (err.empty() ? "Empty page list" : err);
                renderDetail();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }

            int total = static_cast<int>(pageUrls.size());
            int downloaded = 0;
            for (int p = 0; p < total; ++p) {
                if (cancel.load()) break;
                char fname[32];
                snprintf(fname, sizeof(fname), "page_%03d.jpg", p + 1);
                fs::path pageFile = tmpExtract / fname;

                int pct = 10 + ((p + 1) * 75) / total;
                updateDownloadProgress("Downloading page " + std::to_string(p + 1) + "/" + std::to_string(total) + " (" + std::to_string(pct) + "%)", pct);

                if (util::httpsDownloadFile(pageUrls[p], pageFile.string(), 30)) {
                    downloaded++;
                }
            }
            ok = (downloaded > 0);
            if (!ok && err.empty()) err = "Failed downloading pages";
        } else {
            // MangaDex
            ok = sources_.mangadexClient().downloadChapterPages(ch.id, tmpExtract, &cancel,
                [this](const std::string& msg, int pct) {
                    updateDownloadProgress(msg + " (" + std::to_string(pct) + "%)", pct);
                }, &err);
        }

        if (ok) {
            updateDownloadProgress("Packing CBZ archive...", 90);

            std::error_code ec;
            fs::create_directories(destDir, ec);
            std::string safeCh = ch.chapterNum.empty() ? "chapter" : ("chapter_" + ch.chapterNum);
            fs::path cbzFile = destDir / (safeCh + ".cbz");

            ZipStoreWriter zip;
            if (zip.open(cbzFile.string())) {
                std::vector<fs::path> pageFiles;
                for (const auto& entry : fs::directory_iterator(tmpExtract, ec)) {
                    if (entry.is_regular_file(ec)) {
                        pageFiles.push_back(entry.path());
                    }
                }
                std::sort(pageFiles.begin(), pageFiles.end(), [](const fs::path& a, const fs::path& b) {
                    return util::naturalLess(a.filename().string(), b.filename().string());
                });

                for (const auto& pf : pageFiles) {
                    std::string bytes;
                    if (util::readFileToString(pf, bytes)) {
                        std::vector<uint8_t> vec(bytes.begin(), bytes.end());
                        zip.add(pf.filename().string(), vec);
                    }
                }
                zip.close();
                fs::remove_all(tmpExtract, ec);

                // Also save first page as cover if missing
                fs::path coverFile = destDir / "cover.jpg";
                if (!fs::exists(coverFile, ec) && !pageFiles.empty()) {
                    fs::copy_file(pageFiles[0], coverFile, ec);
                }

                download_active_ = false;
                ch.isDownloaded = true;
                ch.localPath = cbzFile.string();
                status_banner_ = "✓ Chapter " + ch.chapterNum + " saved!";
            } else {
                download_active_ = false;
                status_banner_ = "Failed creating CBZ archive";
            }
        } else {
            download_active_ = false;
            status_banner_ = "Download failed: " + err;
        }

        if (current_view_ == ViewState::READER || autoOpen) {
            if (ch.isDownloaded) {
                openChapter(chIdx, false);
                return;
            } else {
                renderReader();
                renderReaderToast("Failed to download Ch. " + ch.chapterNum);
                return;
            }
        }

        renderDetail();
        display_.refreshFull(WAVEFORM_GC16);
    }

    void cacheCoversForBrowse() {
        std::error_code ec;
        fs::create_directories("/mnt/us/manga/.cache/covers", ec);
        std::string batchFile = "/tmp/rm2manga_cover_batch.txt";
        std::ofstream out(batchFile);
        int count = 0;
        for (const auto& m : browse_mangas_) {
            if (count++ >= 8) break;
            if (m.cover_url.rfind("http://", 0) == 0 || m.cover_url.rfind("https://", 0) == 0) {
                std::string safeId = m.id;
                for (char& c : safeId) if (c == '/' || c == ':' || c == ' ' || c == '\\') c = '_';
                std::string localPath = "/mnt/us/manga/.cache/covers/" + safeId + ".jpg";
                if (!fs::exists(localPath, ec)) {
                    out << m.cover_url << "\t" << localPath << "\n";
                }
            }
        }
        out.close();
        if (const char* helper = util::httpHelper()) {
            std::vector<std::string> args = {helper, "batch", batchFile};
            util::runProcess(args, 15);
        }
        ::unlink(batchFile.c_str());
    }

    void fetchPopular() {
        status_banner_ = "Loading popular manga from " + sources_.getActiveSourceId() + "...";
        renderBrowse();
        display_.refreshFull(WAVEFORM_GC16);

        std::string err;
        browse_mangas_ = sources_.getPopular(&err);
        if (!err.empty()) {
            status_banner_ = "Error: " + err;
        } else {
            status_banner_.clear();
            cacheCoversForBrowse();
        }
        scroll_offset_ = 0;
        renderBrowse();
        display_.refreshFull(WAVEFORM_GC16);
    }

    static double parseChapterNumber(const std::string& s) {
        if (s.empty()) return -1.0;
        try {
            size_t i = 0;
            while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i])) && s[i] != '.') {
                i++;
            }
            if (i < s.size()) {
                return std::stod(s.substr(i));
            }
        } catch (...) {}
        return -1.0;
    }

    void sortChapters() {
        if (selected_chapters_.empty()) return;
        std::sort(selected_chapters_.begin(), selected_chapters_.end(), [this](const UnifiedChapter& a, const UnifiedChapter& b) {
            double na = parseChapterNumber(a.chapterNum.empty() ? a.title : a.chapterNum);
            double nb = parseChapterNumber(b.chapterNum.empty() ? b.title : b.chapterNum);
            if (na >= 0 && nb >= 0 && std::abs(na - nb) > 0.0001) {
                return detail_sort_descending_ ? (na > nb) : (na < nb);
            }
            if (na >= 0 && nb < 0) return true;
            if (na < 0 && nb >= 0) return false;
            return detail_sort_descending_ ? util::naturalLess(b.title, a.title) : util::naturalLess(a.title, b.title);
        });
    }

    void nextChapterPage() {
        int total_pages = std::max(1, (static_cast<int>(selected_chapters_.size()) + CHAPTERS_PER_PAGE - 1) / CHAPTERS_PER_PAGE);
        if (chapter_page_ < total_pages - 1) {
            chapter_page_++;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
        }
    }

    void prevChapterPage() {
        if (chapter_page_ > 0) {
            chapter_page_--;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
        }
    }

    void fetchChapters(const std::string& mangaId) {
        std::string err;
        selected_chapters_ = sources_.getChapters(mangaId, &err);
        chapter_page_ = 0;

        // Check if any chapters are already downloaded locally
        fs::path destDir = "/mnt/us/manga/" + selected_manga_.title;
        std::error_code ec;
        if (fs::exists(destDir, ec)) {
            for (auto& ch : selected_chapters_) {
                std::string safeCh = ch.chapterNum.empty() ? "chapter" : ("chapter_" + ch.chapterNum);
                fs::path cbzFile = destDir / (safeCh + ".cbz");
                double chNum = parseChapterNumber(ch.chapterNum.empty() ? ch.title : ch.chapterNum);

                if (fs::exists(cbzFile, ec)) {
                    ch.isDownloaded = true;
                    ch.localPath = cbzFile.string();
                } else {
                    for (const auto& f : fs::directory_iterator(destDir, ec)) {
                        if (f.path().extension() == ".cbz") {
                            double fNum = parseChapterNumber(f.path().stem().string());
                            if (fNum >= 0 && chNum >= 0 && std::abs(fNum - chNum) < 0.0001) {
                                ch.isDownloaded = true;
                                ch.localPath = f.path().string();
                                break;
                            }
                        }
                    }
                }
            }
        }
        sortChapters();
        if (!selected_chapters_.empty()) {
            state_.saveChaptersCache(selected_manga_.title, selected_manga_.source_id, selected_manga_.id, selected_chapters_);
        }
    }

    static constexpr int KB_Y = 1130;
    static constexpr int KB_ROW_H = 105;
    static constexpr int KB_START_Y = KB_Y + 15;
    static constexpr int KB_KEY_H = 92;
    static constexpr int KB_KEY_W = 114;
    static constexpr int KB_KEY_GAP = 8;
    static constexpr int KB_KEY_STEP = 122; // KB_KEY_W + KB_KEY_GAP

    void renderKey(int x, int y, int w, int h, const std::string& label, bool inverted = false, bool funcKey = false, int fontSize = 30) {
        uint8_t bg = inverted ? 0x00 : (funcKey ? 0xE6 : 0xFF);
        uint8_t fg = inverted ? 0xFF : 0x00;
        canvas_.fillRect(x, y, w, h, bg);
        canvas_.drawRect(x, y, w, h, 0x00, 2);

        int tw = font_.measureText(label, fontSize, true);
        int tx = x + (w - tw) / 2;
        int ty = y + (h - fontSize) / 2;
        font_.drawText(canvas_, tx, ty, label, fontSize, fg, true);
    }

    void renderSearchBarInput() {
        int sb_x = 20;
        int sb_y = 15;
        int sb_w = SCREEN_W - 500;
        int sb_h = 50;

        // Clear only the search bar text area to prevent any screen flash or memory corruption
        canvas_.fillRect(sb_x + 2, sb_y + 2, sb_w - 4, sb_h - 4, 0xFF);

        std::string searchDisplay = search_query_.empty()
            ? (show_keyboard_ ? "_" : "Search manga...")
            : (show_keyboard_ ? search_query_ + "_" : search_query_);

        font_.drawText(canvas_, sb_x + 15, sb_y + 12, searchDisplay, 24, (search_query_.empty() && !show_keyboard_) ? 0x80 : 0x00, false);

        // Hardware update: only the search bar rectangle in fast DU mode
        display_.update(sb_x, sb_y, sb_w, sb_h, WAVEFORM_DU, UPDATE_PARTIAL);
    }

    void handleKeyboardTouch(int x, int y) {
        if (y < KB_START_Y || y >= KB_START_Y + 4 * KB_ROW_H) return;
        int row = (y - KB_START_Y) / KB_ROW_H;
        if (row < 0 || row > 3) return;

        bool updated_keyboard = false;

        if (row == 0) {
            // Row 0 (10 keys: 26 + k * 122)
            int col = (x - 26) / KB_KEY_STEP;
            int rel = (x - 26) % KB_KEY_STEP;
            if (col >= 0 && col < 10 && rel < KB_KEY_W) {
                if (kb_symbols_mode_) {
                    const char sym0[] = "1234567890";
                    search_query_ += sym0[col];
                } else {
                    const char low0[] = "qwertyuiop";
                    const char up0[] = "QWERTYUIOP";
                    search_query_ += (kb_shift_active_ ? up0[col] : low0[col]);
                    if (kb_shift_active_) {
                        kb_shift_active_ = false;
                        updated_keyboard = true;
                    }
                }
                renderSearchBarInput();
                if (updated_keyboard) {
                    renderKeyboard();
                    display_.update(0, KB_Y, SCREEN_W, SCREEN_H - NAV_H - KB_Y, WAVEFORM_DU, UPDATE_PARTIAL);
                }
                return;
            }
        } else if (row == 1) {
            // Row 1 (9 keys: 87 + k * 122)
            int col = (x - 87) / KB_KEY_STEP;
            int rel = (x - 87) % KB_KEY_STEP;
            if (col >= 0 && col < 9 && rel >= 0 && rel < KB_KEY_W) {
                if (kb_symbols_mode_) {
                    const char sym1[] = "-/:;()$&@";
                    search_query_ += sym1[col];
                } else {
                    const char low1[] = "asdfghjkl";
                    const char up1[] = "ASDFGHJKL";
                    search_query_ += (kb_shift_active_ ? up1[col] : low1[col]);
                    if (kb_shift_active_) {
                        kb_shift_active_ = false;
                        updated_keyboard = true;
                    }
                }
                renderSearchBarInput();
                if (updated_keyboard) {
                    renderKeyboard();
                    display_.update(0, KB_Y, SCREEN_W, SCREEN_H - NAV_H - KB_Y, WAVEFORM_DU, UPDATE_PARTIAL);
                }
                return;
            }
        } else if (row == 2) {
            // Row 2: Shift (26..201), 7 keys (209..1055), Bksp (1063..1238)
            if (x >= 26 && x < 201) {
                // Shift toggle
                kb_shift_active_ = !kb_shift_active_;
                renderKeyboard();
                display_.update(0, KB_Y, SCREEN_W, SCREEN_H - NAV_H - KB_Y, WAVEFORM_DU, UPDATE_PARTIAL);
                return;
            } else if (x >= 1063 && x < 1238) {
                // Backspace
                if (!search_query_.empty()) {
                    search_query_.pop_back();
                    renderSearchBarInput();
                }
                return;
            } else if (x >= 209 && x < 1063) {
                int col = (x - 209) / KB_KEY_STEP;
                int rel = (x - 209) % KB_KEY_STEP;
                if (col >= 0 && col < 7 && rel < KB_KEY_W) {
                    if (kb_symbols_mode_) {
                        const char sym2[] = ".,?!'\"_";
                        search_query_ += sym2[col];
                    } else {
                        const char low2[] = "zxcvbnm";
                        const char up2[] = "ZXCVBNM";
                        search_query_ += (kb_shift_active_ ? up2[col] : low2[col]);
                        if (kb_shift_active_) {
                            kb_shift_active_ = false;
                            updated_keyboard = true;
                        }
                    }
                    renderSearchBarInput();
                    if (updated_keyboard) {
                        renderKeyboard();
                        display_.update(0, KB_Y, SCREEN_W, SCREEN_H - NAV_H - KB_Y, WAVEFORM_DU, UPDATE_PARTIAL);
                    }
                    return;
                }
            }
        } else if (row == 3) {
            // Row 3: ?123 / ABC (26..201), Close (209..359), Space (367..1048), Search (1056..1238)
            if (x >= 26 && x < 201) {
                // Symbols / ABC toggle
                kb_symbols_mode_ = !kb_symbols_mode_;
                renderKeyboard();
                display_.update(0, KB_Y, SCREEN_W, SCREEN_H - NAV_H - KB_Y, WAVEFORM_DU, UPDATE_PARTIAL);
                return;
            } else if (x >= 209 && x < 359) {
                // Close / Hide
                show_keyboard_ = false;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            } else if (x >= 367 && x < 1048) {
                // Space
                search_query_ += " ";
                renderSearchBarInput();
                return;
            } else if (x >= 1056 && x < 1238) {
                // Search
                show_keyboard_ = false;
                if (!search_query_.empty()) {
                    performSearch(search_query_);
                } else {
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                }
                return;
            }
        }
    }

    void performSearch(const std::string& query) {
        show_keyboard_ = false;
        status_banner_ = "Searching " + sources_.getActiveSourceId() + " for: " + query;
        renderBrowse();
        display_.refreshFull(WAVEFORM_GC16);

        std::string err;
        browse_mangas_ = sources_.search(query, &err);
        if (!err.empty()) {
            status_banner_ = "Search error: " + err;
        } else if (browse_mangas_.empty()) {
            status_banner_ = "No results found for \"" + query + "\"";
        } else {
            status_banner_.clear();
            cacheCoversForBrowse();
        }
        scroll_offset_ = 0;
        renderBrowse();
        display_.refreshFull(WAVEFORM_GC16);
    }

    void drawCoverThumbnail(int x, int y, int w, int h, const std::string& pathOrUrl, const std::string& fallbackTitle, const std::string& mangaId = "") {
        std::string localPath = pathOrUrl;
        std::error_code ec;
        if (localPath.rfind("http://", 0) == 0 || localPath.rfind("https://", 0) == 0) {
            std::string safeId = mangaId.empty() ? fallbackTitle : mangaId;
            for (char& c : safeId) if (c == '/' || c == ':' || c == ' ' || c == '\\') c = '_';
            std::string cached = "/mnt/us/manga/.cache/covers/" + safeId + ".jpg";
            if (fs::exists(cached, ec)) {
                localPath = cached;
            } else {
                if (util::httpsDownloadFile(pathOrUrl, cached, 6)) {
                    localPath = cached;
                }
            }
        }

        if (!localPath.empty() && fs::exists(localPath, ec)) {
            int cw = 0, ch = 0, channels = 0;
            unsigned char* data = stbi_load(localPath.c_str(), &cw, &ch, &channels, 1);
            if (data) {
                canvas_.blitImage(x, y, w, h, data, cw, ch);
                stbi_image_free(data);
                canvas_.drawRect(x, y, w, h, 0x00, 2);
                return;
            }
        }

        // Fallback: draw neat bordered card with first letter of title
        canvas_.fillRect(x, y, w, h, 0xE8);
        canvas_.drawRect(x, y, w, h, 0x00, 2);
        std::string initial = fallbackTitle.empty() ? "?" : fallbackTitle.substr(0, 1);
        font_.drawText(canvas_, x + (w - 20) / 2, y + (h - 28) / 2, initial, 30, 0x40, true);
    }

    // --- RENDER PASSES WITH EMBER TYPOGRAPHY ---
    void renderAll() {
        canvas_.clear(0xFF);

        if (current_view_ == ViewState::LIBRARY) {
            renderLibrary();
        } else if (current_view_ == ViewState::BROWSE) {
            renderBrowse();
        } else if (current_view_ == ViewState::DETAIL) {
            renderDetail();
        } else if (current_view_ == ViewState::DOWNLOADS) {
            renderDownloads();
        } else if (current_view_ == ViewState::SETTINGS) {
            renderSettings();
        } else if (current_view_ == ViewState::SOURCES) {
            renderSources();
        } else if (current_view_ == ViewState::READER) {
            renderReader();
            return;
        }

        if (show_keyboard_) {
            renderKeyboard();
        }

        renderBottomNav();
    }

    void renderLibrary() {
        // Top Header
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 30, 22, "rm2manga", 34, 0x00, true);
        font_.drawText(canvas_, 220, 27, "Library", 26, 0x60, false);
        font_.drawButton(canvas_, SCREEN_W - 190, 15, 160, 50, "Exit App", false, 22);

        // Hero Card: Continue Reading
        MangaProgress hero;
        int listStartY = TOP_H + 20;

        if (state_.getHeroManga(hero)) {
            int heroY = TOP_H + 15;
            int heroH = 150;
            canvas_.drawRect(25, heroY, SCREEN_W - 50, heroH, 0x00, 2);
            canvas_.fillRect(26, heroY + 1, SCREEN_W - 52, heroH - 2, 0xFA);

            // Hero Thumbnail
            std::string heroCover = "/mnt/us/manga/" + hero.manga_title + "/cover.jpg";
            if (!fs::exists(heroCover)) {
                heroCover = "/mnt/us/manga/" + hero.manga_title + "/cover.png";
            }
            drawCoverThumbnail(40, heroY + 15, 80, 120, heroCover, hero.manga_title, hero.manga_title);

            font_.drawText(canvas_, 140, heroY + 20, "CONTINUE READING", 20, 0x60, true);
            font_.drawText(canvas_, 140, heroY + 50, hero.manga_title, 30, 0x00, true);

            std::string sub = "Ch. " + hero.chapter_title + " | Page " + std::to_string(hero.page_index + 1) + " of " + std::to_string(hero.total_pages);
            font_.drawText(canvas_, 140, heroY + 95, sub, 24, 0x40, false);

            font_.drawButton(canvas_, SCREEN_W - 220, heroY + 45, 170, 60, "Resume >", true, 24);
            listStartY = heroY + heroH + 25;
        }

        if (library_mangas_.empty()) {
            font_.drawText(canvas_, SCREEN_W / 2 - 160, 600, "Your library is empty.", 30, 0x60, true);
            font_.drawText(canvas_, SCREEN_W / 2 - 280, 650, "Browse manga and tap [+ Add to Library] to pin titles here!", 22, 0x80, false);
            return;
        }

        font_.drawText(canvas_, 30, listStartY - 5, "My Library (" + std::to_string(library_mangas_.size()) + ")", 24, 0x50, true);

        int y = listStartY + 30;
        int item_h = 130;

        for (const auto& manga : library_mangas_) {
            canvas_.drawRect(25, y, SCREEN_W - 50, item_h, 0x00, 2);
            drawCoverThumbnail(35, y + 10, 80, 110, manga.cover_url, manga.title, manga.id);

            font_.drawText(canvas_, 135, y + 25, manga.title, 28, 0x00, true);

            std::string sub;
            if (manga.downloaded_count > 0) {
                sub = std::to_string(manga.downloaded_count) + (manga.downloaded_count == 1 ? " chapter offline" : " chapters offline");
            } else if (manga.in_library) {
                sub = manga.source_id.empty() ? "Pinned in Library" : ("Pinned · " + manga.source_id);
            } else {
                sub = "Ready to read";
            }
            font_.drawText(canvas_, 135, y + 75, sub, 22, 0x60, false);

            font_.drawButton(canvas_, SCREEN_W - 180, y + 35, 130, 55, "Open", false, 22);

            y += item_h + 15;
            if (y > SCREEN_H - NAV_H - item_h) break;
        }
    }

    void renderBrowse() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);

        std::string searchDisplay = search_query_.empty()
            ? (show_keyboard_ ? "_" : "Search manga...")
            : (show_keyboard_ ? search_query_ + "_" : search_query_);
        canvas_.drawRect(20, 15, SCREEN_W - 500, 50, 0x00, 2);
        font_.drawText(canvas_, 35, 27, searchDisplay, 24, (search_query_.empty() && !show_keyboard_) ? 0x80 : 0x00, false);

        font_.drawButton(canvas_, SCREEN_W - 470, 15, 130, 50, "Search", true, 22);
        font_.drawButton(canvas_, SCREEN_W - 325, 15, 155, 50, "⚙ Sources", false, 20);
        font_.drawButton(canvas_, SCREEN_W - 155, 15, 135, 50, "✕ Exit", false, 22);

        // Source Switcher Pills
        int pill_y = TOP_H + 15;
        font_.drawText(canvas_, 25, pill_y + 10, "Source:", 22, 0x50, true);

        int pill_x = 110;
        for (const auto& s : sources_.getSources()) {
            bool active = (s.id == sources_.getActiveSourceId());
            font_.drawButton(canvas_, pill_x, pill_y, 195, 45, s.name, active, 20);
            pill_x += 210;
        }

        if (!status_banner_.empty()) {
            font_.drawText(canvas_, 30, pill_y + 65, status_banner_, 22, 0x00, true);
        }

        int y = pill_y + 60;
        int item_h = 130;

        for (const auto& manga : browse_mangas_) {
            canvas_.drawRect(25, y, SCREEN_W - 50, item_h, 0x00, 2);
            drawCoverThumbnail(35, y + 10, 80, 110, manga.cover_url, manga.title, manga.id);

            std::string t = manga.title;
            if (t.length() > 32) t = t.substr(0, 30) + "...";
            font_.drawText(canvas_, 135, y + 22, t, 28, 0x00, true);

            std::string sub = (manga.author.empty() ? sources_.getActiveSourceId() : manga.author) + " | " + manga.status;
            font_.drawText(canvas_, 135, y + 70, sub, 22, 0x60, false);

            font_.drawButton(canvas_, SCREEN_W - 160, y + 35, 120, 55, "View", false, 22);

            y += item_h + 15;
            if (y > SCREEN_H - NAV_H - item_h) break;
        }
    }

    void renderSources() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 30, 22, "Manga Sources & Extensions", 32, 0x00, true);
        font_.drawButton(canvas_, SCREEN_W - 320, 15, 150, 50, "< Browse", true, 22);
        font_.drawButton(canvas_, SCREEN_W - 155, 15, 135, 50, "✕ Exit", false, 22);

        if (!status_banner_.empty()) {
            font_.drawText(canvas_, 35, TOP_H + 12, status_banner_, 22, 0x00, true);
        }

        int start_y = TOP_H + 35;
        int card_h = 130;
        const auto& srcList = sources_.getSources();

        for (size_t i = 0; i < srcList.size(); ++i) {
            int cy = start_y + static_cast<int>(i) * (card_h + 15);
            bool isActive = (srcList[i].id == sources_.getActiveSourceId());

            canvas_.drawRect(25, cy, SCREEN_W - 50, card_h, 0x00, 2);
            if (isActive) {
                canvas_.fillRect(27, cy + 2, 8, card_h - 4, 0x00);
            }

            font_.drawText(canvas_, 45, cy + 22, srcList[i].name, 30, 0x00, true);
            font_.drawText(canvas_, 45, cy + 62, srcList[i].description, 22, 0x60, false);
            font_.drawText(canvas_, 45, cy + 92, "Status: Online | Protocol: HTTPS Modern TLS", 20, 0x80, false);

            if (isActive) {
                font_.drawButton(canvas_, SCREEN_W - 230, cy + 35, 180, 60, "✓ Active", true, 22);
            } else {
                font_.drawButton(canvas_, SCREEN_W - 230, cy + 35, 180, 60, "Select", false, 22);
            }
        }

        int infoY = start_y + static_cast<int>(srcList.size()) * (card_h + 15) + 20;
        font_.drawText(canvas_, 35, infoY, "CUSTOM EXTENSIONS & SOURCE REPOSITORIES", 22, 0x50, true);
        canvas_.drawHLine(35, infoY + 30, SCREEN_W - 70, 0x00, 2);

        int boxY = infoY + 45;
        canvas_.drawRect(25, boxY, SCREEN_W - 50, 160, 0x00, 2);
        canvas_.fillRect(26, boxY + 1, SCREEN_W - 52, 158, 0xF9);

        font_.drawText(canvas_, 45, boxY + 20, "Active Index: Aidoku Community & rm2manga Official", 24, 0x00, true);
        font_.drawText(canvas_, 45, boxY + 55, "Repository: https://raw.githubusercontent.com/Aidoku-Community/", 20, 0x50, false);
        font_.drawText(canvas_, 45, boxY + 85, "Sources Loaded: MangaDex v5, MangaFox (FanFox/MangaHere), MangaKatana", 20, 0x50, false);
        font_.drawText(canvas_, 45, boxY + 115, "Languages: English (en) [Default]", 20, 0x50, false);

        int btnY = boxY + 180;
        font_.drawButton(canvas_, 40, btnY, SCREEN_W - 80, 65, "↻ Rescan & Refresh Extension Catalog", false, 24);
    }

    void renderDetail() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawButton(canvas_, 20, 15, 140, 50, "< Back", false, 22);
        font_.drawButton(canvas_, SCREEN_W - 155, 15, 135, 50, "✕ Exit", false, 22);

        std::string titleStr = selected_manga_.title;
        if (titleStr.length() > 24) titleStr = titleStr.substr(0, 22) + "...";
        font_.drawText(canvas_, 180, 24, titleStr, 30, 0x00, true);

        // Cover thumbnail (130x185)
        drawCoverThumbnail(30, TOP_H + 20, 130, 185, selected_manga_.cover_url, selected_manga_.title, selected_manga_.id);

        std::string displayTitle = selected_manga_.title;
        if (displayTitle.length() > 36) displayTitle = displayTitle.substr(0, 34) + "...";
        font_.drawText(canvas_, 180, TOP_H + 20, displayTitle, 28, 0x00, true);
        font_.drawText(canvas_, 180, TOP_H + 58, "Author: " + (selected_manga_.author.empty() ? sources_.getActiveSourceId() : selected_manga_.author), 22, 0x40, false);
        font_.drawText(canvas_, 180, TOP_H + 92, "Status: " + selected_manga_.status + " | Source: " + sources_.getActiveSourceId(), 22, 0x40, false);
        font_.drawText(canvas_, 180, TOP_H + 125, "Total: " + std::to_string(selected_chapters_.size()) + " chapters", 22, 0x00, true);

        // Action Buttons Row: [ ▶ Read ] and [ + Add to Library ] / [ ✓ In Library ]
        int btnY = TOP_H + 160;
        std::string readLabel = "▶ Read First";
        if (!selected_chapters_.empty()) {
            readLabel = "▶ Read Ch. " + selected_chapters_[0].chapterNum;
        }
        font_.drawButton(canvas_, 180, btnY, 230, 52, readLabel, true, 22);

        bool inLib = state_.isInLibrary(selected_manga_.source_id, selected_manga_.id, selected_manga_.title);
        if (inLib) {
            font_.drawButton(canvas_, 430, btnY, 240, 52, "✓ In Library", true, 22);
        } else {
            font_.drawButton(canvas_, 430, btnY, 240, 52, "+ Add to Library", false, 22);
        }

        // Active Download / Status Progress Card
        int cardY = 295;
        int cardH = 75;
        if (download_active_) {
            canvas_.fillRect(25, cardY, SCREEN_W - 50, cardH, 0xF2);
            canvas_.drawRect(25, cardY, SCREEN_W - 50, cardH, 0x00, 2);
            font_.drawText(canvas_, 45, cardY + 14, download_status_, 22, 0x00, true);
            int pbW = SCREEN_W - 90;
            int pbH = 16;
            int pbY = cardY + 45;
            canvas_.drawRect(45, pbY, pbW, pbH, 0x00, 2);
            int fillW = std::max(0, std::min(pbW - 2, (pbW - 2) * download_progress_pct_ / 100));
            canvas_.fillRect(46, pbY + 1, fillW, pbH - 2, 0x00);
        } else if (!status_banner_.empty()) {
            canvas_.fillRect(25, cardY, SCREEN_W - 50, 50, 0xF5);
            canvas_.drawRect(25, cardY, SCREEN_W - 50, 50, 0x00, 2);
            font_.drawText(canvas_, 45, cardY + 14, status_banner_, 22, 0x00, true);
        }

        // Section header & Sort control strip
        int chHeadY = 385;
        font_.drawText(canvas_, 30, chHeadY + 10, "Chapters (" + std::to_string(selected_chapters_.size()) + ")", 26, 0x00, true);

        // Sort toggle button
        std::string sortLabel = detail_sort_descending_ ? "⇅ Sort: Newest" : "⇅ Sort: Oldest";
        font_.drawButton(canvas_, SCREEN_W - 260, chHeadY, 230, 48, sortLabel, false, 20);

        canvas_.drawHLine(30, chHeadY + 58, SCREEN_W - 60, 0x00, 2);

        // Chapter list page items
        int total_chapters = static_cast<int>(selected_chapters_.size());
        int total_pages = std::max(1, (total_chapters + CHAPTERS_PER_PAGE - 1) / CHAPTERS_PER_PAGE);
        if (chapter_page_ >= total_pages) chapter_page_ = total_pages - 1;
        if (chapter_page_ < 0) chapter_page_ = 0;

        int start_idx = chapter_page_ * CHAPTERS_PER_PAGE;
        int end_idx = std::min(start_idx + CHAPTERS_PER_PAGE, total_chapters);

        int start_y = chHeadY + 70;
        int item_h = 76;
        int item_gap = 10;
        int row_stride = item_h + item_gap;

        for (int i = start_idx; i < end_idx; ++i) {
            int row = i - start_idx;
            int y = start_y + row * row_stride;
            const auto& ch = selected_chapters_[i];

            canvas_.drawRect(30, y, SCREEN_W - 60, item_h, 0x00, 2);
            std::string label = "Ch. " + ch.chapterNum + (ch.title.empty() ? "" : (" - " + ch.title));
            if (label.length() > 45) label = label.substr(0, 43) + "...";
            font_.drawText(canvas_, 50, y + 25, label, 24, 0x00, false);

            if (ch.isDownloaded) {
                font_.drawButton(canvas_, SCREEN_W - 200, y + 13, 140, 50, "Read", true, 22);
            } else {
                font_.drawButton(canvas_, SCREEN_W - 220, y + 13, 160, 50, "Download", false, 20);
            }
        }

        // Pagination controls at bottom
        int pag_y = 1500;
        canvas_.drawHLine(30, pag_y - 10, SCREEN_W - 60, 0xC0, 1);

        // Prev page button
        if (chapter_page_ > 0) {
            font_.drawButton(canvas_, 30, pag_y, 180, 55, "◄ Prev", false, 22);
        } else {
            canvas_.drawRect(30, pag_y, 180, 55, 0xC0, 1);
            font_.drawText(canvas_, 30 + (180 - font_.measureText("◄ Prev", 22, false)) / 2, pag_y + 16, "◄ Prev", 22, 0xC0, false);
        }

        // Page info text (e.g. "Page 1 of 12  (120 .. 109)")
        std::string rangeStr = "";
        if (start_idx < end_idx) {
            rangeStr = "  (" + selected_chapters_[start_idx].chapterNum + " .. " + selected_chapters_[end_idx - 1].chapterNum + ")";
        }
        std::string pageInfo = "Page " + std::to_string(chapter_page_ + 1) + " of " + std::to_string(total_pages) + rangeStr;
        int textW = font_.measureText(pageInfo, 22, true);
        font_.drawText(canvas_, (SCREEN_W - textW) / 2, pag_y + 16, pageInfo, 22, 0x00, true);

        // Next page button
        if (chapter_page_ < total_pages - 1) {
            font_.drawButton(canvas_, SCREEN_W - 210, pag_y, 180, 55, "Next ►", false, 22);
        } else {
            canvas_.drawRect(SCREEN_W - 210, pag_y, 180, 55, 0xC0, 1);
            font_.drawText(canvas_, SCREEN_W - 210 + (180 - font_.measureText("Next ►", 22, false)) / 2, pag_y + 16, "Next ►", 22, 0xC0, false);
        }
    }

    void renderDownloads() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 30, 24, "Downloads & Offline Chapters", 32, 0x00, true);
        font_.drawButton(canvas_, SCREEN_W - 155, 15, 135, 50, "✕ Exit", false, 22);

        int curY = TOP_H + 20;

        // Active download card if in progress
        if (download_active_) {
            canvas_.fillRect(25, curY, SCREEN_W - 50, 80, 0xF2);
            canvas_.drawRect(25, curY, SCREEN_W - 50, 80, 0x00, 2);
            font_.drawText(canvas_, 45, curY + 15, download_status_, 22, 0x00, true);
            int pbW = SCREEN_W - 90;
            int pbH = 16;
            int pbY = curY + 48;
            canvas_.drawRect(45, pbY, pbW, pbH, 0x00, 2);
            int fillW = std::max(0, std::min(pbW - 2, (pbW - 2) * download_progress_pct_ / 100));
            canvas_.fillRect(46, pbY + 1, fillW, pbH - 2, 0x00);
            curY += 95;
        }

        int item_h = 95;
        int count = 0;

        for (const auto& manga : library_mangas_) {
            std::string dirPath = "/mnt/us/manga/" + manga.title;
            std::error_code ec;
            if (fs::exists(dirPath, ec)) {
                for (const auto& f : fs::directory_iterator(dirPath, ec)) {
                    if (f.path().extension() == ".cbz") {
                        canvas_.drawRect(25, curY, SCREEN_W - 50, item_h, 0x00, 2);
                        font_.drawText(canvas_, 45, curY + 18, manga.title, 26, 0x00, true);

                        double mb = static_cast<double>(fs::file_size(f.path(), ec)) / (1024.0 * 1024.0);
                        char szBuf[64];
                        snprintf(szBuf, sizeof(szBuf), "Ch. %s  ·  %.1f MB", f.path().stem().string().c_str(), mb);
                        font_.drawText(canvas_, 45, curY + 55, szBuf, 22, 0x50, false);

                        font_.drawButton(canvas_, SCREEN_W - 190, curY + 20, 130, 52, "Read", true, 22);

                        curY += item_h + 12;
                        count++;
                        if (curY > SCREEN_H - NAV_H - item_h) break;
                    }
                }
            }
        }

        if (count == 0 && !download_active_) {
            font_.drawText(canvas_, SCREEN_W / 2 - 180, 500, "No downloaded chapters found.", 28, 0x60, false);
            font_.drawText(canvas_, SCREEN_W / 2 - 250, 550, "Open any manga and tap 'Download' to save offline.", 22, 0x80, false);
        }
    }

    void renderSettings() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 30, 24, "Settings & Device Controls", 32, 0x00, true);
        font_.drawButton(canvas_, SCREEN_W - 155, 15, 135, 50, "✕ Exit", false, 22);

        if (!status_banner_.empty()) {
            font_.drawText(canvas_, 40, TOP_H + 15, status_banner_, 22, 0x00, true);
        }

        int y = TOP_H + 35;
        font_.drawText(canvas_, 40, y, "Kindle Oasis (Zelda Platform, 1264x1680 @ 300 PPI)", 24, 0x40, false);

        // Frontlight Control
        y += 60;
        font_.drawText(canvas_, 40, y + 10, "Frontlight Intensity:", 26, 0x00, true);
        font_.drawButton(canvas_, 300, y, 70, 55, "-", false, 28);
        std::string flVal = std::to_string(state_.settings().frontlight) + " / 24";
        font_.drawText(canvas_, 395, y + 12, flVal, 26, 0x00, true);
        font_.drawButton(canvas_, 490, y, 70, 55, "+", false, 28);

        // Reading Mode Control
        y += 80;
        font_.drawText(canvas_, 40, y + 10, "Reading Mode:", 26, 0x00, true);
        std::string modeLabel = (state_.settings().reading_direction == "RTL") ? "Manga (RTL <-)" : "Comic (LTR ->)";
        font_.drawButton(canvas_, 300, y, 260, 55, modeLabel, true, 22);

        // Grip / Touch Orientation Control
        y += 80;
        font_.drawText(canvas_, 40, y + 10, "Grip / Touch:", 26, 0x00, true);
        std::string rotLabel = (state_.settings().touch_rotation == 180) ? "Buttons Left (180°)" : "Buttons Right (0°)";
        font_.drawButton(canvas_, 300, y, 320, 55, rotLabel, true, 22);

        y = 470;
        // Big Exit Button
        font_.drawButton(canvas_, 100, y, SCREEN_W - 200, 75, "Exit rm2manga to Kindle Home", true, 24);

        y = 565;
        // Clear Cache Button
        font_.drawButton(canvas_, 100, y, SCREEN_W - 200, 75, "Clear Reader Image Cache", false, 24);

        y = 660;
        // Refresh Library Button
        font_.drawButton(canvas_, 100, y, SCREEN_W - 200, 75, "Rescan Manga Storage", false, 24);
    }

    void renderReader() {
        canvas_.clear(0xFF);

        // 1. Render actual manga page full screen (edge-to-edge 1264x1680)
        renderReaderPage();

        // 2. Overlay controls (only drawn when invoked)
        if (reader_show_overlay_) {
            // Top Bar Overlay
            canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
            canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
            font_.drawButton(canvas_, 20, 15, 160, 50, "< Back", false, 22);

            std::string titleStr = selected_manga_.title;
            if (current_chapter_idx_ >= 0 && current_chapter_idx_ < static_cast<int>(selected_chapters_.size())) {
                const auto& ch = selected_chapters_[current_chapter_idx_];
                titleStr += " - Ch. " + (ch.chapterNum.empty() ? ch.title : ch.chapterNum);
            }
            if (titleStr.length() > 32) titleStr = titleStr.substr(0, 30) + "...";
            font_.drawText(canvas_, 200, 26, titleStr, 28, 0x00, true);

            std::string pageInfo = std::to_string(reader_current_page_ + 1) + " / " + std::to_string(reader_total_pages_);
            font_.drawText(canvas_, SCREEN_W - 200, 26, pageInfo, 26, 0x00, false);

            // Middle Light Control Overlay
            int midY = 820;
            canvas_.fillRect(SCREEN_W / 2 - 250, midY, 500, 70, 0xF5);
            canvas_.drawRect(SCREEN_W / 2 - 250, midY, 500, 70, 0x00, 2);
            font_.drawText(canvas_, SCREEN_W / 2 - 230, midY + 20, "Light", 24, 0x00, true);
            font_.drawButton(canvas_, SCREEN_W / 2 - 130, midY + 10, 60, 50, "-", false, 26);
            std::string curFl = std::to_string(state_.settings().frontlight);
            font_.drawText(canvas_, SCREEN_W / 2 - 40, midY + 20, curFl, 24, 0x00, true);
            font_.drawButton(canvas_, SCREEN_W / 2 + 10, midY + 10, 60, 50, "+", false, 26);
            font_.drawButton(canvas_, SCREEN_W / 2 + 90, midY + 10, 140, 50, state_.settings().reading_direction, false, 22);

            // Bottom Bar Overlay
            int bot_y = SCREEN_H - 90;
            canvas_.fillRect(0, bot_y, SCREEN_W, 90, 0xF0);
            canvas_.drawHLine(0, bot_y, SCREEN_W, 0x00, 2);
            font_.drawButton(canvas_, 30, bot_y + 15, 180, 60, "< Prev", false, 22);
            font_.drawButton(canvas_, SCREEN_W / 2 - 100, bot_y + 15, 200, 60, "Close Menu", false, 22);
            font_.drawButton(canvas_, SCREEN_W - 210, bot_y + 15, 180, 60, "Next >", false, 22);
        }
    }

    void renderReaderPage() {
        if (reader_page_files_.empty() || reader_current_page_ < 0 ||
            reader_current_page_ >= static_cast<int>(reader_page_files_.size())) {
            font_.drawText(canvas_, SCREEN_W / 2 - 140, SCREEN_H / 2 - 20, "No page image available", 28, 0x00, false);
            return;
        }

        std::string rawPath = reader_page_files_[reader_current_page_];
        if (rawPath.rfind("file://", 0) == 0) {
            rawPath = rawPath.substr(7);
        }

        int imgW = 0, imgH = 0, channels = 0;
        unsigned char* data = stbi_load(rawPath.c_str(), &imgW, &imgH, &channels, 1);
        if (!data) {
            font_.drawText(canvas_, SCREEN_W / 2 - 180, SCREEN_H / 2 - 20, "Failed to decode page", 28, 0x00, false);
            return;
        }

        // Fullscreen edge-to-edge aspect ratio fit
        double scaleW = static_cast<double>(SCREEN_W) / imgW;
        double scaleH = static_cast<double>(SCREEN_H) / imgH;
        double scale = std::min(scaleW, scaleH);

        int targetW = static_cast<int>(imgW * scale);
        int targetH = static_cast<int>(imgH * scale);
        if (targetW > SCREEN_W) targetW = SCREEN_W;
        if (targetH > SCREEN_H) targetH = SCREEN_H;

        int offsetX = (SCREEN_W - targetW) / 2;
        int offsetY = (SCREEN_H - targetH) / 2;

        canvas_.blitImage(offsetX, offsetY, targetW, targetH, data, imgW, imgH);
        stbi_image_free(data);
    }

    void renderKeyboard() {
        int kb_h = SCREEN_H - NAV_H - KB_Y;
        canvas_.fillRect(0, KB_Y, SCREEN_W, kb_h, 0xEE);
        canvas_.drawHLine(0, KB_Y, SCREEN_W, 0x80, 2);

        // Row 0 (10 keys: 26 + k * 122)
        int y0 = KB_START_Y;
        if (kb_symbols_mode_) {
            const char* syms[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" };
            for (int k = 0; k < 10; ++k) {
                renderKey(26 + k * KB_KEY_STEP, y0, KB_KEY_W, KB_KEY_H, syms[k], false, false, 32);
            }
        } else {
            const char* letters_up[] = { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" };
            const char* letters_low[] = { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" };
            for (int k = 0; k < 10; ++k) {
                renderKey(26 + k * KB_KEY_STEP, y0, KB_KEY_W, KB_KEY_H, kb_shift_active_ ? letters_up[k] : letters_low[k], false, false, 32);
            }
        }

        // Row 1 (9 keys: 87 + k * 122)
        int y1 = KB_START_Y + KB_ROW_H;
        if (kb_symbols_mode_) {
            const char* syms[] = { "-", "/", ":", ";", "(", ")", "$", "&", "@" };
            for (int k = 0; k < 9; ++k) {
                renderKey(87 + k * KB_KEY_STEP, y1, KB_KEY_W, KB_KEY_H, syms[k], false, false, 32);
            }
        } else {
            const char* letters_up[] = { "A", "S", "D", "F", "G", "H", "J", "K", "L" };
            const char* letters_low[] = { "a", "s", "d", "f", "g", "h", "j", "k", "l" };
            for (int k = 0; k < 9; ++k) {
                renderKey(87 + k * KB_KEY_STEP, y1, KB_KEY_W, KB_KEY_H, kb_shift_active_ ? letters_up[k] : letters_low[k], false, false, 32);
            }
        }

        // Row 2 (Shift + 7 keys + Bksp)
        int y2 = KB_START_Y + 2 * KB_ROW_H;
        // Shift key
        renderKey(26, y2, 175, KB_KEY_H, kb_shift_active_ ? "^ Shift" : "Shift", kb_shift_active_, !kb_shift_active_, 26);

        // Middle 7 keys (209 + k * 122)
        if (kb_symbols_mode_) {
            const char* syms[] = { ".", ",", "?", "!", "'", "\"", "_" };
            for (int k = 0; k < 7; ++k) {
                renderKey(209 + k * KB_KEY_STEP, y2, KB_KEY_W, KB_KEY_H, syms[k], false, false, 32);
            }
        } else {
            const char* letters_up[] = { "Z", "X", "C", "V", "B", "N", "M" };
            const char* letters_low[] = { "z", "x", "c", "v", "b", "n", "m" };
            for (int k = 0; k < 7; ++k) {
                renderKey(209 + k * KB_KEY_STEP, y2, KB_KEY_W, KB_KEY_H, kb_shift_active_ ? letters_up[k] : letters_low[k], false, false, 32);
            }
        }

        // Backspace key
        renderKey(1063, y2, 175, KB_KEY_H, "Bksp", false, true, 26);

        // Row 3 (Bottom row: ?123 / ABC, Close, Space, Search)
        int y3 = KB_START_Y + 3 * KB_ROW_H;
        renderKey(26, y3, 175, KB_KEY_H, kb_symbols_mode_ ? "ABC" : "?123", kb_symbols_mode_, !kb_symbols_mode_, 26);
        renderKey(209, y3, 150, KB_KEY_H, "Close", false, true, 24);
        renderKey(367, y3, 681, KB_KEY_H, "space", false, false, 24);
        renderKey(1056, y3, 182, KB_KEY_H, "Search", true, false, 26);
    }

    void renderBottomNav() {
        int nav_y = SCREEN_H - NAV_H;
        canvas_.fillRect(0, nav_y, SCREEN_W, NAV_H, 0xFF);
        canvas_.drawHLine(0, nav_y, SCREEN_W, 0x00, 2);

        const char* tabs[] = { "Library", "Browse", "Downloads", "Settings" };
        int tab_w = SCREEN_W / 4;

        for (int i = 0; i < 4; ++i) {
            int x = i * tab_w;
            bool is_active = (selected_tab_ == i);
            if (is_active) {
                canvas_.fillRect(x + 5, nav_y + 5, tab_w - 10, NAV_H - 10, 0x00);
                font_.drawText(canvas_, x + (tab_w - font_.measureText(tabs[i], 24, true)) / 2, nav_y + 35, tabs[i], 24, 0xFF, true);
            } else {
                font_.drawText(canvas_, x + (tab_w - font_.measureText(tabs[i], 24, false)) / 2, nav_y + 35, tabs[i], 24, 0x00, false);
            }
        }
    }
};

} // namespace kindle
