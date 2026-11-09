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
    RECENT,
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
    int selected_tab_ = 0; // 0: Library, 1: Recent, 2: Browse, 3: Downloads, 4: Settings

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
    enum class KeyboardTarget {
        SEARCH = 0,
        REPO_URL = 1
    };
    KeyboardTarget kb_target_ = KeyboardTarget::SEARCH;
    bool show_keyboard_ = false;
    bool kb_shift_active_ = false;
    bool kb_symbols_mode_ = false;
    std::string search_query_;
    std::string status_banner_;

    // Sources and Extension Catalog state
    enum class SourcesSubTab {
        INSTALLED = 0,
        AVAILABLE = 1,
        REPOSITORIES = 2
    };
    SourcesSubTab sources_sub_tab_ = SourcesSubTab::INSTALLED;
    std::vector<SourceInfo> available_aidoku_sources_;
    bool available_sources_loaded_ = false;
    int available_sources_page_ = 0;
    static constexpr int AVAILABLE_PER_PAGE = 7;
    std::string repo_url_input_;

    // Browse pagination state
    static constexpr int BROWSE_PER_PAGE = 8;
    int browse_page_ = 0;

    // Touch gesture tracking for swipes
    int touch_down_x_ = 0;
    int touch_down_y_ = 0;
    bool touch_is_down_ = false;

    // Live Download Progress
    bool download_active_ = false;
    std::string download_status_;
    int download_progress_pct_ = 0;

    // Library Removal Modal state
    bool show_remove_modal_ = false;
    UnifiedManga manga_to_remove_;
    int remove_modal_dl_count_ = 0;

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
    int countDownloadedChapters(const std::string& mangaTitle) {
        if (mangaTitle.empty()) return 0;
        std::string dirPath = "/mnt/us/manga/" + mangaTitle;
        std::error_code ec;
        if (!fs::exists(dirPath, ec)) return 0;
        int count = 0;
        for (const auto& f : fs::directory_iterator(dirPath, ec)) {
            if (f.path().extension() == ".cbz") count++;
        }
        return count;
    }

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
                    if (dirName.empty() || dirName[0] == '.' || dirName == "data") continue;
                    if (state_.isUnpinned(dirName)) continue;

                    int count = 0;
                    for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
                        if (f.path().extension() == ".cbz") count++;
                    }

                    // If not pinned in library and has 0 cbz files, skip
                    if (count == 0 && merged.find(dirName) == merged.end()) continue;

                    UnifiedManga& m = merged[dirName];
                    if (m.title.empty()) {
                        m.title = dirName;
                        m.id = dirName;
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
            if (current_view_ == ViewState::SOURCES && sources_sub_tab_ == SourcesSubTab::AVAILABLE) {
                int totalItems = static_cast<int>(available_aidoku_sources_.size());
                int totalPages = (totalItems + AVAILABLE_PER_PAGE - 1) / AVAILABLE_PER_PAGE;
                if (available_sources_page_ + 1 < totalPages) {
                    available_sources_page_++;
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                }
                return;
            }
            if (current_view_ == ViewState::READER) {
                if (state_.settings().button_inverted) prevPage(); else nextPage();
            } else if (current_view_ == ViewState::DETAIL) {
                nextChapterPage();
            } else if (current_view_ == ViewState::BROWSE) {
                nextBrowsePage();
            } else if (current_view_ == ViewState::RECENT || current_view_ == ViewState::LIBRARY ||
                       current_view_ == ViewState::DOWNLOADS) {
                scroll_offset_ += 350;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
            }
            return;
        }
        if (ev.type == EventType::KEY_PREV_PAGE) {
            if (current_view_ == ViewState::SOURCES && sources_sub_tab_ == SourcesSubTab::AVAILABLE) {
                if (available_sources_page_ > 0) {
                    available_sources_page_--;
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                }
                return;
            }
            if (current_view_ == ViewState::READER) {
                if (state_.settings().button_inverted) nextPage(); else prevPage();
            } else if (current_view_ == ViewState::DETAIL) {
                prevChapterPage();
            } else if (current_view_ == ViewState::BROWSE) {
                prevBrowsePage();
            } else if (current_view_ == ViewState::RECENT || current_view_ == ViewState::LIBRARY ||
                       current_view_ == ViewState::DOWNLOADS) {
                scroll_offset_ = std::max(0, scroll_offset_ - 350);
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
            }
            return;
        }

        if (ev.type == EventType::TOUCH_UP) {
            handleTouch(ev.x, ev.y);
        }
    }

    void handleTouch(int x, int y) {
        // 0. Remove Manga Confirmation Modal touch if active
        if (show_remove_modal_) {
            handleRemoveModalTouch(x, y);
            return;
        }

        // 1. Bottom Navigation Bar touch (only outside Reader)
        if (y >= SCREEN_H - NAV_H && current_view_ != ViewState::READER) {
            int tab_w = SCREEN_W / 5;
            int tapped_tab = x / tab_w;
            if (tapped_tab >= 0 && tapped_tab < 5) {
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
                    current_view_ = ViewState::RECENT;
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                } else if (selected_tab_ == 2) {
                    current_view_ = ViewState::BROWSE;
                    if (browse_mangas_.empty()) {
                        status_banner_ = "Loading popular manga from " + sources_.getActiveSourceId() + "...";
                        renderAll();
                        display_.refreshFull(WAVEFORM_GC16);
                        fetchPopular();
                    }
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                } else if (selected_tab_ == 3) {
                    current_view_ = ViewState::DOWNLOADS;
                    loadLibraryFromDisk();
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                } else if (selected_tab_ == 4) {
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
        } else if (current_view_ == ViewState::RECENT) {
            handleRecentTouch(x, y);
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

    void handleRemoveModalTouch(int x, int y) {
        int dialogW = 860;
        int dialogH = (remove_modal_dl_count_ > 0) ? 460 : 370;
        int dialogX = (SCREEN_W - dialogW) / 2;
        int dialogY = (SCREEN_H - dialogH) / 2;

        if (remove_modal_dl_count_ > 0) {
            // Button A: [ Keep Files & Remove ] at dialogX + 45, dialogY + 265, w=370, h=60
            if (x >= dialogX + 45 && x <= dialogX + 415 && y >= dialogY + 265 && y <= dialogY + 325) {
                state_.removeFromLibrary(manga_to_remove_);
                show_remove_modal_ = false;
                status_banner_ = "Removed \"" + manga_to_remove_.title + "\" from Library (files kept)";
                loadLibraryFromDisk();
                if (current_view_ == ViewState::DETAIL) {
                    loadMangaChapters(selected_manga_);
                }
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }

            // Button B: [ Delete Files & Remove ] at dialogX + 445, dialogY + 265, w=370, h=60
            if (x >= dialogX + 445 && x <= dialogX + 815 && y >= dialogY + 265 && y <= dialogY + 325) {
                state_.removeFromLibrary(manga_to_remove_);
                state_.clearProgress(manga_to_remove_.title, manga_to_remove_.source_id, manga_to_remove_.id);
                std::string dirPath = "/mnt/us/manga/" + manga_to_remove_.title;
                std::error_code ec;
                fs::remove_all(dirPath, ec);

                show_remove_modal_ = false;
                status_banner_ = "Deleted files and removed \"" + manga_to_remove_.title + "\"";
                loadLibraryFromDisk();
                if (current_view_ == ViewState::DETAIL) {
                    loadMangaChapters(selected_manga_);
                }
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }

            // Button C: [ Cancel ] at dialogX + 245, dialogY + 355, w=370, h=60
            if (x >= dialogX + 245 && x <= dialogX + 615 && y >= dialogY + 355 && y <= dialogY + 415) {
                show_remove_modal_ = false;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
        } else {
            // Button A: [ Remove ] at dialogX + 70, dialogY + 250, w=320, h=60
            if (x >= dialogX + 70 && x <= dialogX + 390 && y >= dialogY + 250 && y <= dialogY + 310) {
                state_.removeFromLibrary(manga_to_remove_);
                std::string dirPath = "/mnt/us/manga/" + manga_to_remove_.title;
                std::error_code ec;
                if (fs::exists(dirPath, ec)) {
                    fs::remove_all(dirPath, ec);
                }

                show_remove_modal_ = false;
                status_banner_ = "Removed \"" + manga_to_remove_.title + "\" from Library";
                loadLibraryFromDisk();
                if (current_view_ == ViewState::DETAIL) {
                    loadMangaChapters(selected_manga_);
                }
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }

            // Button B: [ Cancel ] at dialogX + 470, dialogY + 250, w=320, h=60
            if (x >= dialogX + 470 && x <= dialogX + 790 && y >= dialogY + 250 && y <= dialogY + 310) {
                show_remove_modal_ = false;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
        }

        // Tap outside dialog bounds dismisses dialog
        if (x < dialogX || x > dialogX + dialogW || y < dialogY || y > dialogY + dialogH) {
            show_remove_modal_ = false;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
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

        bool hasHero = state_.getHeroManga(hero);
        int listStartY = hasHero ? (TOP_H + 15 + 150 + 25) : (TOP_H + 20);
        int y_start = listStartY + 30;
        int item_h = 135;
        int item_gap = 14;
        int row_stride = item_h + item_gap;

        for (size_t i = 0; i < library_mangas_.size(); ++i) {
            int item_y = y_start + static_cast<int>(i) * row_stride - scroll_offset_;
            if (y >= item_y && y < item_y + item_h) {
                if (x >= SCREEN_W - 340 && x < SCREEN_W - 180) {
                    // Tap on [ Remove ] button
                    manga_to_remove_ = library_mangas_[i];
                    remove_modal_dl_count_ = countDownloadedChapters(manga_to_remove_.title);
                    show_remove_modal_ = true;
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }

                // Tap on [ Open ] or card body
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

    void handleRecentTouch(int x, int y) {
        // Exit App button in header
        if (x >= SCREEN_W - 190 && y >= 10 && y <= TOP_H) {
            quit();
            return;
        }

        // Clear History button in header
        auto recent = state_.getRecentHistory();
        if (!recent.empty() && x >= SCREEN_W - 350 && x < SCREEN_W - 190 && y >= 10 && y <= TOP_H) {
            state_.clearAllHistory();
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        if (recent.empty()) {
            int midY = 600;
            int btnW = 280;
            int btnH = 65;
            int btnX = (SCREEN_W - btnW) / 2;
            int btnY = midY + 110;
            if (x >= btnX && x <= btnX + btnW && y >= btnY && y <= btnY + btnH) {
                selected_tab_ = 2;
                current_view_ = ViewState::BROWSE;
                scroll_offset_ = 0;
                status_banner_.clear();
                if (browse_mangas_.empty()) {
                    status_banner_ = "Loading popular manga from " + sources_.getActiveSourceId() + "...";
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                    fetchPopular();
                }
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
            }
            return;
        }

        int listStartY = TOP_H + 20;
        int y_start = listStartY + 35;
        int item_h = 135;
        int item_gap = 14;
        int row_stride = item_h + item_gap;

        for (size_t i = 0; i < recent.size(); ++i) {
            int item_y = y_start + static_cast<int>(i) * row_stride - scroll_offset_;
            if (y >= item_y && y < item_y + item_h) {
                const auto& prog = recent[i];
                int btnW = 160;
                int btnH = 50;
                int btnX = SCREEN_W - btnW - 45;

                // Tap on [ Details ] button
                if (x >= btnX && x <= btnX + btnW && y >= item_y + 72 && y <= item_y + 72 + btnH) {
                    UnifiedManga m;
                    m.title = prog.manga_title;
                    m.id = prog.manga_id.empty() ? prog.manga_title : prog.manga_id;
                    m.source_id = prog.source_id;
                    m.cover_url = prog.cover_url;
                    m.cover_path = prog.cover_path;
                    for (const auto& libM : library_mangas_) {
                        if (libM.title == prog.manga_title) {
                            m = libM;
                            break;
                        }
                    }
                    selected_manga_ = m;
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

                // Tap on [ Resume > ] or card body
                resumeRecent(prog);
                return;
            }
        }
    }

    void resumeRecent(const MangaProgress& prog) {
        UnifiedManga m;
        m.title = prog.manga_title;
        m.id = prog.manga_id.empty() ? prog.manga_title : prog.manga_id;
        m.source_id = prog.source_id;
        m.cover_url = prog.cover_url;
        m.cover_path = prog.cover_path;

        for (const auto& libM : library_mangas_) {
            if ((!prog.source_id.empty() && libM.source_id == prog.source_id && libM.id == prog.manga_id) ||
                libM.title == prog.manga_title) {
                m = libM;
                break;
            }
        }
        selected_manga_ = m;
        if (!selected_manga_.source_id.empty() && selected_manga_.source_id != "local") {
            sources_.setActiveSourceId(selected_manga_.source_id);
        }
        loadMangaChapters(selected_manga_);

        double heroNum = prog.chapter_num >= 0 ? prog.chapter_num : parseChapterNumber(prog.chapter_title);
        for (size_t i = 0; i < selected_chapters_.size(); ++i) {
            double chNum = parseChapterNumber(selected_chapters_[i].chapterNum.empty() ? selected_chapters_[i].title : selected_chapters_[i].chapterNum);
            if ((!prog.chapter_id.empty() && selected_chapters_[i].id == prog.chapter_id) ||
                selected_chapters_[i].title == prog.chapter_title ||
                selected_chapters_[i].chapterNum == prog.chapter_title ||
                (chNum >= 0 && heroNum >= 0 && std::abs(chNum - heroNum) < 0.0001)) {
                openChapter(static_cast<int>(i), true, prog.page_index);
                return;
            }
        }
        if (!selected_chapters_.empty()) {
            openChapter(0);
        }
    }

    void resumeHero(const MangaProgress& hero) {
        resumeRecent(hero);
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
                if (pill_x + pill_w > SCREEN_W - 25) break;
                if (x >= pill_x && x <= pill_x + pill_w) {
                    show_keyboard_ = false;
                    sources_.setActiveSourceId(s.id);
                    status_banner_ = "Switched source to " + s.name;
                    browse_page_ = 0;
                    fetchPopular();
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }
                pill_x += 210;
            }
        }

        // Pagination buttons: y between SCREEN_H - NAV_H - 80 and SCREEN_H - NAV_H - 10
        if (y >= SCREEN_H - NAV_H - 80 && y <= SCREEN_H - NAV_H - 10) {
            int total_pages = std::max(1, (static_cast<int>(browse_mangas_.size()) + BROWSE_PER_PAGE - 1) / BROWSE_PER_PAGE);
            if (total_pages > 1) {
                int btnW = 250;
                if (x >= 20 && x <= btnW + 20) {
                    prevBrowsePage();
                    return;
                }
                if (x >= SCREEN_W - btnW - 20 && x <= SCREEN_W - 20) {
                    nextBrowsePage();
                    return;
                }
            }
        }

        // Browse list item clicked
        int pill_y = TOP_H + 15;
        int listStartY = pill_y + 60;
        int item_h = 135;
        int item_gap = 12;
        int row_stride = item_h + item_gap;

        int totalItems = static_cast<int>(browse_mangas_.size());
        int startIdx = browse_page_ * BROWSE_PER_PAGE;
        int endIdx = std::min(startIdx + BROWSE_PER_PAGE, totalItems);

        for (int i = startIdx; i < endIdx; ++i) {
            int relIdx = i - startIdx;
            int item_y = listStartY + relIdx * row_stride;
            if (y >= item_y && y < item_y + item_h) {
                selected_manga_ = browse_mangas_[i];
                status_banner_ = "Loading chapters for " + selected_manga_.title + "...";
                renderAll();
                display_.refreshUI(0, 0, SCREEN_W, TOP_H + 80);

                fetchChapters(selected_manga_.id);
                current_view_ = ViewState::DETAIL;
                chapter_page_ = 0;
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
            if (selected_tab_ == 0) current_view_ = ViewState::LIBRARY;
            else if (selected_tab_ == 1) current_view_ = ViewState::RECENT;
            else if (selected_tab_ == 2) current_view_ = ViewState::BROWSE;
            else if (selected_tab_ == 3) current_view_ = ViewState::DOWNLOADS;
            else current_view_ = ViewState::SETTINGS;
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
            bool inLib = state_.isInLibrary(selected_manga_.source_id, selected_manga_.id, selected_manga_.title);
            if (inLib) {
                manga_to_remove_ = selected_manga_;
                remove_modal_dl_count_ = countDownloadedChapters(selected_manga_.title);
                show_remove_modal_ = true;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            } else {
                state_.toggleLibrary(selected_manga_);
                selected_manga_.in_library = true;
                status_banner_ = "✓ Added \"" + selected_manga_.title + "\" to Library";
                loadLibraryFromDisk();
                renderDetail();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
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

    struct OfflineCbz {
        std::string manga_title;
        std::string chapter_stem;
        std::string full_path;
        double mb = 0.0;
    };

    std::vector<OfflineCbz> getOfflineCbzList() {
        std::vector<OfflineCbz> list;
        std::string manga_root = "/mnt/us/manga";
        std::error_code ec;
        if (!fs::exists(manga_root, ec)) return list;

        for (const auto& entry : fs::directory_iterator(manga_root, ec)) {
            if (!entry.is_directory()) continue;
            std::string title = entry.path().filename().string();
            if (title.empty() || title[0] == '.' || title == "data") continue;

            for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
                if (f.path().extension() == ".cbz") {
                    OfflineCbz item;
                    item.manga_title = title;
                    item.chapter_stem = f.path().stem().string();
                    item.full_path = f.path().string();
                    item.mb = static_cast<double>(fs::file_size(f.path(), ec)) / (1024.0 * 1024.0);
                    list.push_back(item);
                }
            }
        }
        std::sort(list.begin(), list.end(), [](const OfflineCbz& a, const OfflineCbz& b) {
            if (a.manga_title != b.manga_title) return util::naturalLess(a.manga_title, b.manga_title);
            return util::naturalLess(a.chapter_stem, b.chapter_stem);
        });
        return list;
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
        auto offlineList = getOfflineCbzList();

        for (size_t i = 0; i < offlineList.size(); ++i) {
            const auto& item = offlineList[i];
            int item_y = start_y + static_cast<int>(i) * row_stride - scroll_offset_;
            if (y >= item_y && y < item_y + item_h) {
                UnifiedManga m;
                m.title = item.manga_title;
                m.id = item.manga_title;
                for (const auto& libM : library_mangas_) {
                    if (libM.title == item.manga_title) {
                        m = libM;
                        break;
                    }
                }
                selected_manga_ = m;
                loadMangaChapters(selected_manga_);
                for (size_t c = 0; c < selected_chapters_.size(); ++c) {
                    if (selected_chapters_[c].localPath == item.full_path ||
                        selected_chapters_[c].title == item.chapter_stem ||
                        selected_chapters_[c].chapterNum == item.chapter_stem) {
                        openChapter(static_cast<int>(c));
                        return;
                    }
                }
                openChapter(0);
                return;
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
            show_keyboard_ = false;
            current_view_ = ViewState::BROWSE;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // Sub-navigation tabs: Y = 75 to 135
        if (y >= 75 && y <= 135) {
            show_keyboard_ = false;
            if (x < 420) {
                sources_sub_tab_ = SourcesSubTab::INSTALLED;
                sources_.refreshSources();
            } else if (x >= 420 && x < 835) {
                sources_sub_tab_ = SourcesSubTab::AVAILABLE;
                if (!available_sources_loaded_) {
                    status_banner_ = "Fetching extensions from repository...";
                    renderSources();
                    display_.refreshFull(WAVEFORM_GC16);
                    std::string err;
                    available_aidoku_sources_ = sources_.getAvailableAidokuSources(&err);
                    available_sources_loaded_ = true;
                    if (!err.empty()) status_banner_ = "Repository error: " + err;
                    else status_banner_.clear();
                }
            } else {
                sources_sub_tab_ = SourcesSubTab::REPOSITORIES;
            }
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        int contentY = 75 + 55 + 48; // ~178

        if (sources_sub_tab_ == SourcesSubTab::INSTALLED) {
            handleInstalledSourcesTouch(x, y, contentY);
        } else if (sources_sub_tab_ == SourcesSubTab::AVAILABLE) {
            handleAvailableSourcesTouch(x, y, contentY);
        } else if (sources_sub_tab_ == SourcesSubTab::REPOSITORIES) {
            handleRepositoriesTouch(x, y, contentY);
        }
    }

    void handleInstalledSourcesTouch(int x, int y, int startY) {
        const auto& srcList = sources_.getSources();
        int cardH = 120;
        int maxVisible = 6;
        for (size_t i = 0; i < srcList.size() && static_cast<int>(i) < maxVisible; ++i) {
            int cy = startY + static_cast<int>(i) * (cardH + 12);
            if (y >= cy && y <= cy + cardH) {
                // If tapped Uninstall button
                if (srcList[i].is_aidoku && x >= SCREEN_W - 365 && x < SCREEN_W - 210) {
                    status_banner_ = "Uninstalling " + srcList[i].name + "...";
                    renderSources();
                    display_.refreshFull(WAVEFORM_GC16);
                    std::string err;
                    if (sources_.uninstallAidokuSource(srcList[i].id, &err)) {
                        status_banner_ = "✓ Uninstalled " + srcList[i].name;
                        available_sources_loaded_ = false;
                    } else {
                        status_banner_ = "Failed: " + err;
                    }
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }

                // If tapped Select / Active button or row
                if (x >= SCREEN_W - 200 || x < SCREEN_W - 365) {
                    sources_.setActiveSourceId(srcList[i].id);
                    status_banner_ = "Active source: " + srcList[i].name;
                    fetchPopular();
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }
            }
        }

        // Bottom button "+ Browse Available Extensions"
        int btnY = startY + std::min(static_cast<int>(srcList.size()), maxVisible) * (cardH + 12) + 20;
        if (y >= btnY && y <= btnY + 70 && x >= 35 && x <= SCREEN_W - 35) {
            sources_sub_tab_ = SourcesSubTab::AVAILABLE;
            if (!available_sources_loaded_) {
                status_banner_ = "Fetching extensions from repository...";
                renderSources();
                display_.refreshFull(WAVEFORM_GC16);
                std::string err;
                available_aidoku_sources_ = sources_.getAvailableAidokuSources(&err);
                available_sources_loaded_ = true;
                if (!err.empty()) status_banner_ = "Repository error: " + err;
                else status_banner_.clear();
            }
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }
    }

    void handleAvailableSourcesTouch(int x, int y, int startY) {
        if (!available_sources_loaded_ || available_aidoku_sources_.empty()) {
            if (y >= startY + 90 && y <= startY + 175) {
                status_banner_ = "Fetching extensions from repository...";
                renderSources();
                display_.refreshFull(WAVEFORM_GC16);
                std::string err;
                available_aidoku_sources_ = sources_.getAvailableAidokuSources(&err);
                available_sources_loaded_ = true;
                if (!err.empty()) status_banner_ = "Repository error: " + err;
                else status_banner_ = "✓ Loaded " + std::to_string(available_aidoku_sources_.size()) + " available extensions";
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
            }
            return;
        }

        int totalItems = static_cast<int>(available_aidoku_sources_.size());
        int totalPages = (totalItems + AVAILABLE_PER_PAGE - 1) / AVAILABLE_PER_PAGE;
        int headerY = startY;

        // Refresh list button
        if (x >= 25 && x <= 285 && y >= headerY && y <= headerY + 55) {
            status_banner_ = "Refreshing extensions from repository...";
            renderSources();
            display_.refreshFull(WAVEFORM_GC16);
            std::string err;
            available_aidoku_sources_ = sources_.getAvailableAidokuSources(&err);
            available_sources_loaded_ = true;
            if (!err.empty()) status_banner_ = "Repository error: " + err;
            else status_banner_ = "✓ Catalog refreshed (" + std::to_string(available_aidoku_sources_.size()) + " extensions)";
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // Prev page button
        if (totalPages > 1 && x >= SCREEN_W - 320 && x <= SCREEN_W - 180 && y >= headerY && y <= headerY + 55) {
            if (available_sources_page_ > 0) {
                available_sources_page_--;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
            }
            return;
        }

        // Next page button
        if (totalPages > 1 && x >= SCREEN_W - 165 && x <= SCREEN_W - 25 && y >= headerY && y <= headerY + 55) {
            if (available_sources_page_ + 1 < totalPages) {
                available_sources_page_++;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
            }
            return;
        }

        // Available items list
        int listStartY = headerY + 65;
        int cardH = 115;
        int startIdx = available_sources_page_ * AVAILABLE_PER_PAGE;
        int endIdx = std::min(totalItems, startIdx + AVAILABLE_PER_PAGE);

        for (int i = startIdx; i < endIdx; ++i) {
            int cy = listStartY + (i - startIdx) * (cardH + 10);
            if (y >= cy && y <= cy + cardH) {
                const auto& s = available_aidoku_sources_[i];
                if (!s.is_installed && x >= SCREEN_W - 240) {
                    printf("handleAvailableSourcesTouch: Installing source '%s' (%s)\n", s.id.c_str(), s.name.c_str());
                    status_banner_ = "Installing " + s.name + "...";
                    renderSources();
                    display_.refreshFull(WAVEFORM_GC16);
                    std::string err;
                    if (sources_.installAidokuSource(s.id, &err)) {
                        status_banner_ = "✓ Successfully installed " + s.name;
                        available_aidoku_sources_[i].is_installed = true;
                    } else {
                        status_banner_ = "Install error: " + (err.empty() ? "Installation failed" : err);
                    }
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }
            }
        }
    }

    void handleRepositoriesTouch(int x, int y, int startY) {
        Json::Value cfg = sources_.getSourceConfig();
        const auto& sl = cfg["source_lists"].asArray();
        int curY = startY + 45;

        for (size_t i = 0; i < sl.size(); ++i) {
            std::string url = sl[i].str();
            if (y >= curY && y <= curY + 75) {
                // Tapped remove button
                if (x >= SCREEN_W - 190 && x <= SCREEN_W - 40) {
                    status_banner_ = "Removing repository...";
                    renderSources();
                    display_.refreshFull(WAVEFORM_GC16);
                    std::string err;
                    sources_.removeSourceListUrl(url, &err);
                    available_sources_loaded_ = false;
                    status_banner_ = "Removed repository URL.";
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }
            }
            curY += 88;
            if (curY > SCREEN_H - NAV_H - 400) break;
        }

        int inputY = curY + 15;
        int boxY = inputY + 30;
        int boxW = SCREEN_W - 270;
        int boxH = 65;

        // Tapped input box -> open keyboard
        if (x >= 25 && x <= 25 + boxW && y >= boxY && y <= boxY + boxH) {
            kb_target_ = KeyboardTarget::REPO_URL;
            show_keyboard_ = true;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // Tapped "+ Add URL" button
        if (x >= SCREEN_W - 225 && x <= SCREEN_W - 40 && y >= boxY && y <= boxY + boxH) {
            if (!repo_url_input_.empty()) {
                status_banner_ = "Adding repository URL...";
                show_keyboard_ = false;
                renderSources();
                display_.refreshFull(WAVEFORM_GC16);
                std::string err;
                if (sources_.addSourceListUrl(repo_url_input_, &err)) {
                    status_banner_ = "✓ Added repository! Fetching catalog...";
                    repo_url_input_.clear();
                    available_sources_loaded_ = false;
                    available_aidoku_sources_ = sources_.getAvailableAidokuSources();
                    available_sources_loaded_ = true;
                } else {
                    status_banner_ = "Failed: " + err;
                }
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
            }
            return;
        }

        // Quick Add Aidoku Community button
        int btnRowY = boxY + boxH + 20;
        if (x >= 25 && x <= SCREEN_W - 25 && y >= btnRowY && y <= btnRowY + 65) {
            status_banner_ = "Adding Aidoku Community repository...";
            renderSources();
            display_.refreshFull(WAVEFORM_GC16);
            std::string err;
            std::string officialUrl = "https://raw.githubusercontent.com/Aidoku-Community/legacy-sources/refs/heads/gh-pages/index.min.json";
            if (sources_.addSourceListUrl(officialUrl, &err)) {
                status_banner_ = "✓ Added Aidoku Community repository!";
                available_sources_loaded_ = false;
                available_aidoku_sources_ = sources_.getAvailableAidokuSources();
                available_sources_loaded_ = true;
            } else {
                status_banner_ = "Failed: " + err;
            }
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

        bool alreadyCbz = false;
        std::string safeCh = ch.chapterNum.empty() ? "chapter" : ("chapter_" + ch.chapterNum);
        fs::path cbzFile = destDir / (safeCh + ".cbz");

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
        } else if (sources_.isAidokuSource(srcId)) {
            preload_pct_ = 25;
            double num = -1.0;
            bool hasNum = false;
            if (!ch.chapterNum.empty()) {
                char* end = nullptr;
                num = std::strtod(ch.chapterNum.c_str(), &end);
                hasNum = (end != ch.chapterNum.c_str());
            }
            std::string downloadedCbz = sources_.downloadAidokuChapter(
                srcId, selected_manga_.id, ch.id, num, hasNum, &shutting_down_,
                [this](const std::string& msg) {
                    preload_pct_ = 50;
                }, &err);
            if (!downloadedCbz.empty() && fs::exists(downloadedCbz, ec)) {
                fs::create_directories(destDir, ec);
                fs::copy_file(downloadedCbz, cbzFile, fs::copy_options::overwrite_existing, ec);
                ok = true;
                alreadyCbz = true;
            } else {
                ok = false;
            }
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

            if (!alreadyCbz) {
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
                }
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
            const auto& ch = selected_chapters_[current_chapter_idx_];
            double chNum = parseChapterNumber(ch.chapterNum.empty() ? ch.title : ch.chapterNum);
            state_.updateProgress(selected_manga_.source_id,
                                 selected_manga_.id,
                                 selected_manga_.title,
                                 ch.id,
                                 ch.title.empty() ? ("Chapter " + ch.chapterNum) : ch.title,
                                 chNum,
                                 reader_current_page_,
                                 reader_total_pages_,
                                 selected_manga_.cover_url,
                                 selected_manga_.cover_path);
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
        const bool firstUpdate = !download_active_;
        download_active_ = true;
        download_status_ = status;
        download_progress_pct_ = pct;
        if (current_view_ == ViewState::READER) {
            renderReaderDownloadProgress(status, pct);
        } else {
            renderDetail();
            if (firstUpdate) {
                // Clear stale pixels from whatever Kindle screen was visible before
                // the detail view, before the synchronous network request begins.
                display_.refreshFull(WAVEFORM_GC16);
            } else {
                display_.refreshUI(25, 290, SCREEN_W - 50, 85);
            }
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
        bool alreadyCbz = false;

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
        } else if (sources_.isAidokuSource(ch.source_id)) {
            updateDownloadProgress("Downloading from " + ch.source_id + "...", 15);
            double num = -1.0;
            bool hasNum = false;
            if (!ch.chapterNum.empty()) {
                char* end = nullptr;
                num = std::strtod(ch.chapterNum.c_str(), &end);
                hasNum = (end != ch.chapterNum.c_str());
            }
            std::string downloadedCbz = sources_.downloadAidokuChapter(
                ch.source_id, selected_manga_.id, ch.id, num, hasNum, &cancel,
                [this](const std::string& msg) {
                    updateDownloadProgress(msg, 50);
                }, &err);
            std::string safeCh = ch.chapterNum.empty() ? "chapter" : ("chapter_" + ch.chapterNum);
            fs::path cbzFile = destDir / (safeCh + ".cbz");
            if (!downloadedCbz.empty() && fs::exists(downloadedCbz, ec)) {
                fs::create_directories(destDir, ec);
                fs::copy_file(downloadedCbz, cbzFile, fs::copy_options::overwrite_existing, ec);
                ok = true;
                alreadyCbz = true;
            } else {
                ok = false;
                if (err.empty()) err = "Aidoku download failed";
            }
        } else {
            // MangaDex
            ok = sources_.mangadexClient().downloadChapterPages(ch.id, tmpExtract, &cancel,
                [this](const std::string& msg, int pct) {
                    updateDownloadProgress(msg + " (" + std::to_string(pct) + "%)", pct);
                }, &err);
        }

        if (ok) {
            std::string safeCh = ch.chapterNum.empty() ? "chapter" : ("chapter_" + ch.chapterNum);
            fs::path cbzFile = destDir / (safeCh + ".cbz");
            bool archiveOk = true;

            if (!alreadyCbz) {
                updateDownloadProgress("Packing CBZ archive...", 90);

                std::error_code ec;
                fs::create_directories(destDir, ec);

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

                    // Also save first page as cover if missing
                    fs::path coverFile = destDir / "cover.jpg";
                    if (!fs::exists(coverFile, ec) && !pageFiles.empty()) {
                        fs::copy_file(pageFiles[0], coverFile, ec);
                    }
                } else {
                    archiveOk = false;
                }
            }
            fs::remove_all(tmpExtract, ec);

            if (archiveOk) {
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
        renderAll();
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
        browse_page_ = 0;
        renderAll();
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

    void nextBrowsePage() {
        int total_pages = std::max(1, (static_cast<int>(browse_mangas_.size()) + BROWSE_PER_PAGE - 1) / BROWSE_PER_PAGE);
        if (browse_page_ < total_pages - 1) {
            browse_page_++;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
        }
    }

    void prevBrowsePage() {
        if (browse_page_ > 0) {
            browse_page_--;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
        }
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

    std::string& getActiveInputText() {
        return (kb_target_ == KeyboardTarget::REPO_URL) ? repo_url_input_ : search_query_;
    }

    void renderSearchBarInput() {
        if (kb_target_ == KeyboardTarget::REPO_URL) {
            renderSources();
            display_.update(0, 0, SCREEN_W, KB_Y, WAVEFORM_DU, UPDATE_PARTIAL);
            return;
        }

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
        std::string& activeInput = getActiveInputText();

        if (row == 0) {
            // Row 0 (10 keys: 26 + k * 122)
            int col = (x - 26) / KB_KEY_STEP;
            int rel = (x - 26) % KB_KEY_STEP;
            if (col >= 0 && col < 10 && rel < KB_KEY_W) {
                if (kb_symbols_mode_) {
                    const char sym0[] = "1234567890";
                    activeInput += sym0[col];
                } else {
                    const char low0[] = "qwertyuiop";
                    const char up0[] = "QWERTYUIOP";
                    activeInput += (kb_shift_active_ ? up0[col] : low0[col]);
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
                    activeInput += sym1[col];
                } else {
                    const char low1[] = "asdfghjkl";
                    const char up1[] = "ASDFGHJKL";
                    activeInput += (kb_shift_active_ ? up1[col] : low1[col]);
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
                if (!activeInput.empty()) {
                    activeInput.pop_back();
                    renderSearchBarInput();
                }
                return;
            } else if (x >= 209 && x < 1063) {
                int col = (x - 209) / KB_KEY_STEP;
                int rel = (x - 209) % KB_KEY_STEP;
                if (col >= 0 && col < 7 && rel < KB_KEY_W) {
                    if (kb_symbols_mode_) {
                        const char sym2[] = ".,?!'\"_";
                        activeInput += sym2[col];
                    } else {
                        const char low2[] = "zxcvbnm";
                        const char up2[] = "ZXCVBNM";
                        activeInput += (kb_shift_active_ ? up2[col] : low2[col]);
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
            // Row 3: ?123 / ABC (26..201), Close (209..359), Space (367..1048), Search/Done (1056..1238)
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
                activeInput += " ";
                renderSearchBarInput();
                return;
            } else if (x >= 1056 && x < 1238) {
                // Done or Search
                show_keyboard_ = false;
                if (kb_target_ == KeyboardTarget::REPO_URL) {
                    if (!repo_url_input_.empty()) {
                        status_banner_ = "Adding repository URL...";
                        renderSources();
                        display_.refreshFull(WAVEFORM_GC16);
                        std::string err;
                        if (sources_.addSourceListUrl(repo_url_input_, &err)) {
                            status_banner_ = "✓ Added repository! Fetching catalog...";
                            repo_url_input_.clear();
                            available_sources_loaded_ = false;
                            available_aidoku_sources_ = sources_.getAvailableAidokuSources();
                            available_sources_loaded_ = true;
                        } else {
                            status_banner_ = "Failed: " + err;
                        }
                    }
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }

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
        renderAll();
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
        browse_page_ = 0;
        renderAll();
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
        } else if (current_view_ == ViewState::RECENT) {
            renderRecent();
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

        if (show_remove_modal_) {
            renderRemoveModal();
        }
    }

    void renderRemoveModal() {
        int dialogW = 860;
        int dialogH = (remove_modal_dl_count_ > 0) ? 460 : 370;
        int dialogX = (SCREEN_W - dialogW) / 2;
        int dialogY = (SCREEN_H - dialogH) / 2;

        // Shadow & dialog background
        canvas_.fillRect(dialogX + 12, dialogY + 12, dialogW, dialogH, 0x66);
        canvas_.fillRect(dialogX, dialogY, dialogW, dialogH, 0xFF);
        canvas_.drawRect(dialogX, dialogY, dialogW, dialogH, 0x00, 3);

        // Header Title
        font_.drawText(canvas_, dialogX + 45, dialogY + 45, "Remove from Library?", 32, 0x00, true);
        canvas_.drawHLine(dialogX + 45, dialogY + 80, dialogW - 90, 0x00, 2);

        // Manga Title
        std::string dispTitle = manga_to_remove_.title;
        if (dispTitle.length() > 38) dispTitle = dispTitle.substr(0, 35) + "...";
        font_.drawText(canvas_, dialogX + 45, dialogY + 125, dispTitle, 28, 0x00, true);

        if (remove_modal_dl_count_ > 0) {
            std::string msg = "This manga has " + std::to_string(remove_modal_dl_count_) +
                              (remove_modal_dl_count_ == 1 ? " chapter" : " chapters") +
                              " downloaded on device.";
            font_.drawText(canvas_, dialogX + 45, dialogY + 170, msg, 22, 0x40, false);
            font_.drawText(canvas_, dialogX + 45, dialogY + 205, "Choose whether to keep offline files or delete them to free space.", 21, 0x60, false);

            // Row 1: Keep Files & Remove | Delete Files & Remove
            font_.drawButton(canvas_, dialogX + 45, dialogY + 265, 370, 60, "Keep Files & Remove", false, 21);
            font_.drawButton(canvas_, dialogX + 445, dialogY + 265, 370, 60, "Delete Files & Remove", true, 21);

            // Row 2: Cancel
            font_.drawButton(canvas_, dialogX + 245, dialogY + 355, 370, 60, "Cancel", false, 22);
        } else {
            font_.drawText(canvas_, dialogX + 45, dialogY + 175, "Are you sure you want to remove this manga from your library?", 22, 0x40, false);

            // Row 1: Remove | Cancel
            font_.drawButton(canvas_, dialogX + 70, dialogY + 250, 320, 60, "Remove", true, 22);
            font_.drawButton(canvas_, dialogX + 470, dialogY + 250, 320, 60, "Cancel", false, 22);
        }
    }

    void renderLibrary() {
        canvas_.clear(0xFF);
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

        int item_h = 135;
        int item_gap = 14;
        int row_stride = item_h + item_gap;

        for (size_t i = 0; i < library_mangas_.size(); ++i) {
            const auto& manga = library_mangas_[i];
            int item_y = listStartY + 30 + static_cast<int>(i) * row_stride - scroll_offset_;
            if (item_y + item_h < TOP_H || item_y > SCREEN_H - NAV_H) continue;

            canvas_.fillRect(25, item_y, SCREEN_W - 50, item_h, 0xFF);
            canvas_.drawRect(25, item_y, SCREEN_W - 50, item_h, 0x00, 2);
            drawCoverThumbnail(35, item_y + 10, 80, 115, manga.cover_url, manga.title, manga.id);

            font_.drawText(canvas_, 135, item_y + 25, manga.title, 28, 0x00, true);

            std::string sub;
            if (manga.downloaded_count > 0) {
                sub = std::to_string(manga.downloaded_count) + (manga.downloaded_count == 1 ? " chapter offline" : " chapters offline");
            } else if (manga.in_library) {
                sub = manga.source_id.empty() ? "Pinned in Library" : ("Pinned · " + manga.source_id);
            } else {
                sub = "Ready to read";
            }
            font_.drawText(canvas_, 135, item_y + 75, sub, 22, 0x60, false);

            font_.drawButton(canvas_, SCREEN_W - 330, item_y + 35, 135, 55, "Remove", false, 22);
            font_.drawButton(canvas_, SCREEN_W - 175, item_y + 35, 135, 55, "Open", false, 22);
        }
    }

    void renderRecent() {
        canvas_.clear(0xFF);
        // Top Header
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 30, 22, "rm2manga", 34, 0x00, true);
        font_.drawText(canvas_, 220, 27, "Recent History", 26, 0x60, false);

        auto recent = state_.getRecentHistory();
        if (!recent.empty()) {
            font_.drawButton(canvas_, SCREEN_W - 350, 15, 140, 50, "Clear", false, 20);
        }
        font_.drawButton(canvas_, SCREEN_W - 190, 15, 160, 50, "Exit App", false, 22);

        int listStartY = TOP_H + 20;

        if (recent.empty()) {
            int midY = 600;
            font_.drawText(canvas_, SCREEN_W / 2 - 160, midY, "No reading history yet.", 30, 0x50, true);
            font_.drawText(canvas_, SCREEN_W / 2 - 280, midY + 45, "Manga you read will appear here with your latest chapter & page progress.", 22, 0x80, false);

            int btnW = 280;
            int btnH = 65;
            int btnX = (SCREEN_W - btnW) / 2;
            int btnY = midY + 110;
            font_.drawButton(canvas_, btnX, btnY, btnW, btnH, "Browse Manga", true, 26);
            return;
        }

        font_.drawText(canvas_, 30, listStartY + 5, "Recent History (" + std::to_string(recent.size()) + ")", 24, 0x50, true);

        int y_start = listStartY + 35;
        int item_h = 135;
        int item_gap = 14;
        int row_stride = item_h + item_gap;

        for (size_t i = 0; i < recent.size(); ++i) {
            int item_y = y_start + static_cast<int>(i) * row_stride - scroll_offset_;
            if (item_y + item_h < TOP_H || item_y > SCREEN_H - NAV_H) continue;

            const auto& prog = recent[i];

            // Card outline
            canvas_.fillRect(25, item_y, SCREEN_W - 50, item_h, 0xFF);
            canvas_.drawRect(25, item_y, SCREEN_W - 50, item_h, 0x00, 2);

            // Cover thumbnail
            drawCoverThumbnail(35, item_y + 10, 80, 115, prog.cover_url, prog.manga_title, prog.manga_id);

            // Title
            std::string dispTitle = prog.manga_title;
            if (dispTitle.length() > 32) dispTitle = dispTitle.substr(0, 30) + "...";
            font_.drawText(canvas_, 135, item_y + 20, dispTitle, 28, 0x00, true);

            // Chapter line
            std::string chText = prog.chapter_title.empty() ? "In progress" : prog.chapter_title;
            if (chText.length() > 38) chText = chText.substr(0, 36) + "...";
            font_.drawText(canvas_, 135, item_y + 55, chText, 22, 0x40, false);

            // Page info
            int pct = 0;
            if (prog.total_pages > 0) {
                pct = std::min(100, std::max(0, (prog.page_index + 1) * 100 / prog.total_pages));
            }
            std::string pgStr = "Page " + std::to_string(prog.page_index + 1);
            if (prog.total_pages > 0) {
                pgStr += " of " + std::to_string(prog.total_pages) + " (" + std::to_string(pct) + "%)";
            }
            std::string timeStr = util::formatTimeAgo(prog.last_read);
            if (!timeStr.empty()) {
                pgStr += "  •  " + timeStr;
            }
            font_.drawText(canvas_, 135, item_y + 85, pgStr, 20, 0x60, false);

            // Mini progress bar
            int pbar_w = 220;
            int pbar_h = 8;
            int pbar_x = 135;
            int pbar_y = item_y + 112;
            canvas_.drawRect(pbar_x, pbar_y, pbar_w, pbar_h, 0x80, 1);
            int filled_w = (pbar_w - 2) * pct / 100;
            if (filled_w > 0) {
                canvas_.fillRect(pbar_x + 1, pbar_y + 1, filled_w, pbar_h - 2, 0x00);
            }

            // Action buttons on the right
            int btnW = 160;
            int btnH = 50;
            int btnX = SCREEN_W - btnW - 45;
            font_.drawButton(canvas_, btnX, item_y + 12, btnW, btnH, "Resume >", true, 22);
            font_.drawButton(canvas_, btnX, item_y + 72, btnW, btnH, "Details", false, 20);
        }
    }

    void renderBrowse() {
        canvas_.clear(0xFF);
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
            if (pill_x + 195 > SCREEN_W - 25) break;
            bool active = (s.id == sources_.getActiveSourceId());
            font_.drawButton(canvas_, pill_x, pill_y, 195, 45, s.name, active, 20);
            pill_x += 210;
        }

        if (!status_banner_.empty()) {
            font_.drawText(canvas_, 30, pill_y + 60, status_banner_, 22, 0x00, true);
        }

        int totalItems = static_cast<int>(browse_mangas_.size());
        int totalPages = std::max(1, (totalItems + BROWSE_PER_PAGE - 1) / BROWSE_PER_PAGE);
        if (browse_page_ >= totalPages) browse_page_ = totalPages - 1;
        if (browse_page_ < 0) browse_page_ = 0;

        int listStartY = pill_y + 60;
        int item_h = 135;
        int item_gap = 12;
        int row_stride = item_h + item_gap;

        if (browse_mangas_.empty()) {
            font_.drawText(canvas_, 40, listStartY + 50, "No manga loaded. Tap a source pill or tap Search above.", 26, 0x60, false);
        } else {
            int startIdx = browse_page_ * BROWSE_PER_PAGE;
            int endIdx = std::min(startIdx + BROWSE_PER_PAGE, totalItems);

            for (int i = startIdx; i < endIdx; ++i) {
                const auto& manga = browse_mangas_[i];
                int relIdx = i - startIdx;
                int cardY = listStartY + relIdx * row_stride;

                canvas_.fillRect(25, cardY, SCREEN_W - 50, item_h, 0xFF);
                canvas_.drawRect(25, cardY, SCREEN_W - 50, item_h, 0x00, 2);
                drawCoverThumbnail(35, cardY + 10, 85, 115, manga.cover_url, manga.title, manga.id);

                std::string t = manga.title;
                if (t.length() > 32) t = t.substr(0, 30) + "...";
                font_.drawText(canvas_, 140, cardY + 22, t, 28, 0x00, true);

                std::string sub = (manga.author.empty() ? sources_.getActiveSourceId() : manga.author) + " | " + manga.status;
                if (sub.length() > 42) sub = sub.substr(0, 39) + "...";
                font_.drawText(canvas_, 140, cardY + 70, sub, 22, 0x60, false);

                font_.drawButton(canvas_, SCREEN_W - 170, cardY + 35, 130, 55, "View", false, 22);
            }
        }

        // Pagination controls at bottom (above bottom nav bar)
        if (totalPages > 1) {
            int pagY = SCREEN_H - NAV_H - 65; // 1525
            int pagH = 52;
            int btnW = 220;

            bool hasPrev = (browse_page_ > 0);
            bool hasNext = (browse_page_ + 1 < totalPages);

            font_.drawButton(canvas_, 25, pagY, btnW, pagH, "< Prev Page", hasPrev, 22);

            std::string pagInfo = "Page " + std::to_string(browse_page_ + 1) + " of " + std::to_string(totalPages) +
                                  " (" + std::to_string(totalItems) + " titles)";
            int textW = static_cast<int>(pagInfo.length()) * 11;
            font_.drawText(canvas_, SCREEN_W / 2 - textW / 2, pagY + 15, pagInfo, 22, 0x00, true);

            font_.drawButton(canvas_, SCREEN_W - btnW - 25, pagY, btnW, pagH, "Next Page >", hasNext, 22);
        }
    }

    void renderSources() {
        canvas_.clear(0xFF);
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 30, 22, "Manga Sources & Extensions", 32, 0x00, true);
        font_.drawButton(canvas_, SCREEN_W - 320, 15, 150, 50, "< Browse", true, 22);
        font_.drawButton(canvas_, SCREEN_W - 155, 15, 135, 50, "✕ Exit", false, 22);

        // Sub-navigation tabs: Y = 75 to 135
        int tabY = 75;
        int tabH = 55;
        const auto& srcList = sources_.getSources();

        std::string instTabTitle = "Installed (" + std::to_string(srcList.size()) + ")";
        std::string availTabTitle = available_sources_loaded_
            ? ("Available (" + std::to_string(available_aidoku_sources_.size()) + ")")
            : "Available Repos";
        std::string repoTabTitle = "Source Lists";

        bool isInstTab = (sources_sub_tab_ == SourcesSubTab::INSTALLED);
        bool isAvailTab = (sources_sub_tab_ == SourcesSubTab::AVAILABLE);
        bool isRepoTab = (sources_sub_tab_ == SourcesSubTab::REPOSITORIES);

        font_.drawButton(canvas_, 25, tabY, 390, tabH, instTabTitle, isInstTab, 22);
        font_.drawButton(canvas_, 435, tabY, 390, tabH, availTabTitle, isAvailTab, 22);
        font_.drawButton(canvas_, 845, tabY, 390, tabH, repoTabTitle, isRepoTab, 22);

        canvas_.drawHLine(0, tabY + tabH + 10, SCREEN_W, 0x00, 2);

        if (!status_banner_.empty()) {
            font_.drawText(canvas_, 35, tabY + tabH + 18, status_banner_, 22, 0x00, true);
        }

        int contentY = tabY + tabH + 48;

        if (sources_sub_tab_ == SourcesSubTab::INSTALLED) {
            renderInstalledSourcesTab(contentY);
        } else if (sources_sub_tab_ == SourcesSubTab::AVAILABLE) {
            renderAvailableSourcesTab(contentY);
        } else if (sources_sub_tab_ == SourcesSubTab::REPOSITORIES) {
            renderRepositoriesTab(contentY);
        }
    }

    void renderInstalledSourcesTab(int startY) {
        const auto& srcList = sources_.getSources();
        int cardH = 120;
        int maxVisible = 6;
        for (size_t i = 0; i < srcList.size() && static_cast<int>(i) < maxVisible; ++i) {
            int cy = startY + static_cast<int>(i) * (cardH + 12);
            bool isActive = (srcList[i].id == sources_.getActiveSourceId());

            canvas_.drawRect(25, cy, SCREEN_W - 50, cardH, 0x00, 2);
            if (isActive) {
                canvas_.fillRect(27, cy + 2, 8, cardH - 4, 0x00);
            }

            font_.drawText(canvas_, 45, cy + 18, srcList[i].name, 28, 0x00, true);
            std::string meta = srcList[i].id + " | v" + srcList[i].version + " | Lang: " + srcList[i].language +
                               " | " + (srcList[i].is_aidoku ? "Aidoku WASM" : "Native C++");
            font_.drawText(canvas_, 45, cy + 54, meta, 20, 0x60, false);
            std::string desc = srcList[i].description;
            if (desc.size() > 60) desc = desc.substr(0, 58) + "...";
            font_.drawText(canvas_, 45, cy + 82, desc.empty() ? "Online source" : desc, 19, 0x80, false);

            if (isActive) {
                font_.drawButton(canvas_, SCREEN_W - 200, cy + 30, 150, 58, "✓ Active", true, 22);
            } else {
                font_.drawButton(canvas_, SCREEN_W - 200, cy + 30, 150, 58, "Select", false, 22);
            }

            if (srcList[i].is_aidoku) {
                font_.drawButton(canvas_, SCREEN_W - 365, cy + 30, 150, 58, "Uninstall", false, 20);
            }
        }

        int btnY = startY + std::min(static_cast<int>(srcList.size()), maxVisible) * (cardH + 12) + 20;
        if (btnY < SCREEN_H - NAV_H - 85) {
            font_.drawButton(canvas_, 35, btnY, SCREEN_W - 70, 65, "+ Browse & Install Community Extensions", false, 24);
        }
    }

    void renderAvailableSourcesTab(int startY) {
        if (!available_sources_loaded_) {
            font_.drawText(canvas_, 45, startY + 40, "Loading available extensions from configured repositories...", 24, 0x40, false);
            font_.drawButton(canvas_, 35, startY + 100, SCREEN_W - 70, 65, "↻ Fetch Extensions Catalog", true, 24);
            return;
        }

        if (available_aidoku_sources_.empty()) {
            font_.drawText(canvas_, 45, startY + 40, "No extensions found in the configured repositories.", 24, 0x00, true);
            font_.drawText(canvas_, 45, startY + 80, "Check 'Source Lists' tab to add or verify repository index URLs.", 20, 0x60, false);
            font_.drawButton(canvas_, 35, startY + 140, SCREEN_W - 70, 65, "↻ Refresh Extensions Catalog", true, 24);
            return;
        }

        // Action header row: Refresh button and page controls
        int totalItems = static_cast<int>(available_aidoku_sources_.size());
        int totalPages = (totalItems + AVAILABLE_PER_PAGE - 1) / AVAILABLE_PER_PAGE;
        if (available_sources_page_ >= totalPages) available_sources_page_ = std::max(0, totalPages - 1);

        int headerY = startY;
        font_.drawButton(canvas_, 25, headerY, 260, 50, "↻ Refresh List", false, 20);

        std::string pageInfo = "Page " + std::to_string(available_sources_page_ + 1) + " of " +
                               std::to_string(totalPages) + " (" + std::to_string(totalItems) + " extensions)";
        font_.drawText(canvas_, 310, headerY + 15, pageInfo, 22, 0x30, true);

        if (totalPages > 1) {
            font_.drawButton(canvas_, SCREEN_W - 320, headerY, 140, 50, "< Prev", false, 20);
            font_.drawButton(canvas_, SCREEN_W - 165, headerY, 140, 50, "Next >", false, 20);
        }

        int listStartY = headerY + 65;
        int cardH = 115;
        int startIdx = available_sources_page_ * AVAILABLE_PER_PAGE;
        int endIdx = std::min(totalItems, startIdx + AVAILABLE_PER_PAGE);

        for (int i = startIdx; i < endIdx; ++i) {
            const auto& s = available_aidoku_sources_[i];
            int cy = listStartY + (i - startIdx) * (cardH + 10);

            canvas_.drawRect(25, cy, SCREEN_W - 50, cardH, 0x00, 2);
            font_.drawText(canvas_, 45, cy + 18, s.name, 28, 0x00, true);
            std::string meta = s.id + " | v" + s.version + " | Lang: " + s.language;
            font_.drawText(canvas_, 45, cy + 54, meta, 20, 0x60, false);
            std::string desc = s.description;
            if (desc.size() > 60) desc = desc.substr(0, 58) + "...";
            font_.drawText(canvas_, 45, cy + 82, desc.empty() ? "Community Aidoku extension" : desc, 19, 0x80, false);

            if (s.is_installed) {
                font_.drawButton(canvas_, SCREEN_W - 200, cy + 28, 150, 58, "✓ Installed", false, 20);
            } else {
                font_.drawButton(canvas_, SCREEN_W - 200, cy + 28, 150, 58, "+ Install", true, 22);
            }
        }
    }

    void renderRepositoriesTab(int startY) {
        font_.drawText(canvas_, 35, startY, "AIDOKU REPOSITORY SOURCE LISTS (settings.json)", 24, 0x00, true);
        canvas_.drawHLine(35, startY + 30, SCREEN_W - 70, 0x00, 2);

        Json::Value cfg = sources_.getSourceConfig();
        const auto& sl = cfg["source_lists"].asArray();

        int curY = startY + 45;
        if (sl.empty()) {
            canvas_.drawRect(25, curY, SCREEN_W - 50, 80, 0x00, 2);
            canvas_.fillRect(26, curY + 1, SCREEN_W - 52, 78, 0xF9);
            font_.drawText(canvas_, 45, curY + 28, "No repository URLs configured yet.", 22, 0x40, false);
            curY += 95;
        } else {
            for (size_t i = 0; i < sl.size(); ++i) {
                std::string url = sl[i].str();
                canvas_.drawRect(25, curY, SCREEN_W - 50, 75, 0x00, 2);
                canvas_.fillRect(26, curY + 1, SCREEN_W - 52, 73, 0xF9);

                std::string dispUrl = url;
                if (dispUrl.size() > 65) dispUrl = dispUrl.substr(0, 62) + "...";
                font_.drawText(canvas_, 45, curY + 26, dispUrl, 20, 0x00, false);

                font_.drawButton(canvas_, SCREEN_W - 190, curY + 12, 140, 50, "Remove", false, 20);
                curY += 88;
                if (curY > SCREEN_H - NAV_H - 400) break;
            }
        }

        // Add URL Input Row
        int inputY = curY + 15;
        font_.drawText(canvas_, 35, inputY, "Add New Repository Index URL:", 22, 0x00, true);

        int boxY = inputY + 30;
        int boxW = SCREEN_W - 270;
        int boxH = 65;
        canvas_.drawRect(25, boxY, boxW, boxH, 0x00, (kb_target_ == KeyboardTarget::REPO_URL && show_keyboard_) ? 3 : 2);
        canvas_.fillRect(26, boxY + 1, boxW - 2, boxH - 2, 0xFF);

        std::string inputDisp = repo_url_input_.empty()
            ? ((kb_target_ == KeyboardTarget::REPO_URL && show_keyboard_) ? "_" : "Tap here to enter repository URL (e.g. index.min.json)...")
            : (repo_url_input_ + ((kb_target_ == KeyboardTarget::REPO_URL && show_keyboard_) ? "_" : ""));
        font_.drawText(canvas_, 45, boxY + 20, inputDisp, 20, (repo_url_input_.empty() && !show_keyboard_) ? 0x80 : 0x00, false);

        font_.drawButton(canvas_, SCREEN_W - 225, boxY, 175, boxH, "+ Add URL", true, 22);

        // Pre-set button for Aidoku Community
        int btnRowY = boxY + boxH + 20;
        font_.drawButton(canvas_, 25, btnRowY, SCREEN_W - 50, 60, "+ Quick Add Official Aidoku-Community Repository", false, 22);

        // Tip note box
        int tipY = btnRowY + 80;
        canvas_.drawRect(25, tipY, SCREEN_W - 50, 110, 0x00, 1);
        canvas_.fillRect(26, tipY + 1, SCREEN_W - 52, 108, 0xF5);
        font_.drawText(canvas_, 45, tipY + 20, "Tip: You can also edit settings.json directly over SSH or USB at:", 20, 0x00, true);
        font_.drawText(canvas_, 45, tipY + 50, "/mnt/us/manga/data/aidoku/settings.json", 20, 0x50, false);
        font_.drawText(canvas_, 45, tipY + 75, "Languages: [\"en\"] | Schema: https://github.com/hanatsumi/rakuyomi", 18, 0x70, false);
    }

    void renderDetail() {
        // Some callers redraw this view directly (outside renderAll), so it must
        // initialize every pixel before repainting the detail screen.
        canvas_.clear(0xFF);
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
        canvas_.clear(0xFF);
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
        int item_gap = 12;
        int row_stride = item_h + item_gap;
        auto offlineList = getOfflineCbzList();

        for (size_t i = 0; i < offlineList.size(); ++i) {
            const auto& item = offlineList[i];
            int item_y = curY + static_cast<int>(i) * row_stride - scroll_offset_;
            if (item_y + item_h < TOP_H || item_y > SCREEN_H - NAV_H) continue;

            canvas_.drawRect(25, item_y, SCREEN_W - 50, item_h, 0x00, 2);
            font_.drawText(canvas_, 45, item_y + 18, item.manga_title, 26, 0x00, true);

            char szBuf[64];
            snprintf(szBuf, sizeof(szBuf), "Ch. %s  ·  %.1f MB", item.chapter_stem.c_str(), item.mb);
            font_.drawText(canvas_, 45, item_y + 55, szBuf, 22, 0x50, false);

            font_.drawButton(canvas_, SCREEN_W - 190, item_y + 20, 130, 52, "Read", true, 22);
        }

        if (offlineList.empty() && !download_active_) {
            font_.drawText(canvas_, SCREEN_W / 2 - 180, 500, "No downloaded chapters found.", 28, 0x60, false);
            font_.drawText(canvas_, SCREEN_W / 2 - 250, 550, "Open any manga and tap 'Download' to save offline.", 22, 0x80, false);
        }
    }

    void renderSettings() {
        canvas_.clear(0xFF);
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
        std::string actionLabel = (kb_target_ == KeyboardTarget::REPO_URL) ? "Done" : "Search";
        renderKey(1056, y3, 182, KB_KEY_H, actionLabel, true, false, 26);
    }

    void renderBottomNav() {
        int nav_y = SCREEN_H - NAV_H;
        canvas_.fillRect(0, nav_y, SCREEN_W, NAV_H, 0xFF);
        canvas_.drawHLine(0, nav_y, SCREEN_W, 0x00, 2);

        const char* tabs[] = { "Library", "Recent", "Browse", "Downloads", "Settings" };
        int tab_w = SCREEN_W / 5;

        for (int i = 0; i < 5; ++i) {
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
