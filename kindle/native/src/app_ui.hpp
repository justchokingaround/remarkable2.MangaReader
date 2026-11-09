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

namespace fs = std::filesystem;

namespace kindle {

enum class ViewState {
    LIBRARY = 0,
    BROWSE,
    DETAIL,
    READER,
    DOWNLOADS,
    SETTINGS
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

    // Search and Keyboard state
    bool show_keyboard_ = false;
    std::string search_query_;
    std::string status_banner_;

    // Scroll state
    int scroll_offset_ = 0;

public:
    AppUI() = default;

    bool init() {
        if (!std::getenv("RM2MANGA_HTTP_HELPER")) {
            ::setenv("RM2MANGA_HTTP_HELPER", "/mnt/us/extensions/rm2manga/bin/fetch.sh", 1);
        }

        if (!display_.init()) return false;
        canvas_ = Canvas(SCREEN_W, SCREEN_H, DisplayZelda::STRIDE, display_.getBuffer());
        if (!input_.start()) return false;

        font_.init();
        state_.load();

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
        state_.save();
        is_running_ = false;
    }

private:
    void loadLibraryFromDisk() {
        library_mangas_.clear();
        std::string manga_root = "/mnt/us/manga";
        std::error_code ec;

        if (fs::exists(manga_root, ec)) {
            for (const auto& entry : fs::directory_iterator(manga_root, ec)) {
                if (entry.is_directory()) {
                    std::string dirName = entry.path().filename().string();
                    if (dirName.empty() || dirName[0] == '.') continue;

                    UnifiedManga m;
                    m.title = dirName;
                    m.id = dirName;

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

                    library_mangas_.push_back(m);
                }
            }
        }
        std::sort(library_mangas_.begin(), library_mangas_.end(), [](const UnifiedManga& a, const UnifiedManga& b) {
            return util::naturalLess(a.title, b.title);
        });
    }

    void handleInput(const InputEvent& ev) {
        if (ev.type == EventType::KEY_NEXT_PAGE) {
            if (current_view_ == ViewState::READER) {
                if (state_.settings().button_inverted) prevPage(); else nextPage();
            }
            return;
        }
        if (ev.type == EventType::KEY_PREV_PAGE) {
            if (current_view_ == ViewState::READER) {
                if (state_.settings().button_inverted) nextPage(); else prevPage();
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
            if (y >= 1100 && y < SCREEN_H - NAV_H) {
                handleKeyboardTouch(x, y);
                return;
            }
            if (y >= 10 && y <= TOP_H && x >= SCREEN_W - 160) {
                show_keyboard_ = false;
                if (!search_query_.empty()) {
                    performSearch(search_query_);
                } else {
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                }
                return;
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
                loadLocalChapters(selected_manga_.title);
                current_view_ = ViewState::DETAIL;
                scroll_offset_ = 0;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
        }
    }

    void resumeHero(const MangaProgress& hero) {
        selected_manga_.title = hero.manga_title;
        selected_manga_.id = hero.manga_title;
        loadLocalChapters(hero.manga_title);

        for (size_t i = 0; i < selected_chapters_.size(); ++i) {
            if (selected_chapters_[i].title == hero.chapter_title ||
                selected_chapters_[i].chapterNum == hero.chapter_title) {
                openChapter(static_cast<int>(i));
                reader_current_page_ = std::min(hero.page_index, std::max(0, reader_total_pages_ - 1));
                renderReader();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
        }
        if (!selected_chapters_.empty()) {
            openChapter(0);
        }
    }

    void loadLocalChapters(const std::string& mangaTitle) {
        selected_chapters_.clear();
        std::string dirPath = "/mnt/us/manga/" + mangaTitle;
        std::error_code ec;

        if (fs::exists(dirPath, ec)) {
            for (const auto& f : fs::directory_iterator(dirPath, ec)) {
                if (f.path().extension() == ".cbz") {
                    UnifiedChapter ch;
                    ch.title = f.path().stem().string();
                    ch.chapterNum = ch.title;
                    ch.localPath = f.path().string();
                    ch.isDownloaded = true;
                    selected_chapters_.push_back(ch);
                }
            }
        }
        std::sort(selected_chapters_.begin(), selected_chapters_.end(), [](const UnifiedChapter& a, const UnifiedChapter& b) {
            return util::naturalLess(a.title, b.title);
        });
    }

    void handleBrowseTouch(int x, int y) {
        // Search bar or button clicked
        if (y >= 10 && y <= TOP_H - 10) {
            if (x >= SCREEN_W - 160) {
                if (!search_query_.empty()) {
                    performSearch(search_query_);
                } else {
                    show_keyboard_ = true;
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                }
            } else {
                show_keyboard_ = true;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
            }
            return;
        }

        // Source Switcher Pills: y: 90 to 150
        if (y >= 90 && y <= 150) {
            const auto& srcList = sources_.getSources();
            int pill_w = 240;
            int pill_x = 20;

            for (const auto& s : srcList) {
                if (x >= pill_x && x <= pill_x + pill_w) {
                    sources_.setActiveSourceId(s.id);
                    status_banner_ = "Switched source to " + s.name;
                    fetchPopular();
                    renderAll();
                    display_.refreshFull(WAVEFORM_GC16);
                    return;
                }
                pill_x += pill_w + 15;
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
        // Back button (< Back)
        if (x >= 20 && x <= 180 && y >= 15 && y <= 70) {
            current_view_ = (selected_tab_ == 0) ? ViewState::LIBRARY : ViewState::BROWSE;
            scroll_offset_ = 0;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // Chapter tapped
        int start_y = 390;
        int item_h = 90;

        for (size_t i = 0; i < selected_chapters_.size(); ++i) {
            int ch_y = start_y + static_cast<int>(i) * item_h - scroll_offset_;
            if (y >= ch_y && y < ch_y + item_h) {
                if (selected_chapters_[i].isDownloaded) {
                    openChapter(static_cast<int>(i));
                } else {
                    downloadChapter(static_cast<int>(i));
                }
                return;
            }
        }
    }

    void handleDownloadsTouch(int x, int y) {
        int start_y = TOP_H + 20;
        int item_h = 107;
        int idx = 0;

        for (const auto& manga : library_mangas_) {
            std::string dirPath = "/mnt/us/manga/" + manga.title;
            std::error_code ec;
            if (fs::exists(dirPath, ec)) {
                for (const auto& f : fs::directory_iterator(dirPath, ec)) {
                    if (f.path().extension() == ".cbz") {
                        int item_y = start_y + idx * item_h - scroll_offset_;
                        if (y >= item_y && y < item_y + item_h) {
                            selected_manga_ = manga;
                            loadLocalChapters(manga.title);
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

        // Exit to Kindle Home button: x: 100..SCREEN_W-100, y: 440..520
        if (x >= 100 && x <= SCREEN_W - 100 && y >= 440 && y <= 520) {
            quit();
            return;
        }

        // Clear Cache button: x: 100..SCREEN_W-100, y: 550..630
        if (x >= 100 && x <= SCREEN_W - 100 && y >= 550 && y <= 630) {
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
        }
    }

    void recordProgress() {
        if (current_chapter_idx_ >= 0 && current_chapter_idx_ < static_cast<int>(selected_chapters_.size())) {
            std::string chName = selected_chapters_[current_chapter_idx_].chapterNum;
            state_.updateProgress(selected_manga_.title, chName, reader_current_page_, reader_total_pages_);
        }
    }

    void openChapter(int chIdx) {
        if (chIdx < 0 || chIdx >= static_cast<int>(selected_chapters_.size())) return;
        current_chapter_idx_ = chIdx;
        const auto& ch = selected_chapters_[chIdx];

        if (ch.isDownloaded && fs::exists(ch.localPath)) {
            std::string err;
            std::set<std::string> pinned;
            reader_page_files_ = page_cache_.extractCbz(ch.id.empty() ? ch.title : ch.id, ch.localPath, pinned, &err);
            reader_total_pages_ = static_cast<int>(reader_page_files_.size());
            reader_current_page_ = 0;
            reader_show_overlay_ = false;
            current_view_ = ViewState::READER;
            recordProgress();
            renderReader();
            display_.refreshFull(WAVEFORM_GC16);
        } else {
            downloadChapter(chIdx);
        }
    }

    void downloadChapter(int chIdx) {
        if (chIdx < 0 || chIdx >= static_cast<int>(selected_chapters_.size())) return;
        auto& ch = selected_chapters_[chIdx];

        status_banner_ = "Downloading Ch. " + ch.chapterNum + "...";
        renderDetail();
        display_.refreshUI(0, 0, SCREEN_W, TOP_H);

        fs::path destDir = "/mnt/us/manga/" + selected_manga_.title;
        fs::path tmpExtract = "/tmp/rm2manga_dl_" + ch.id;
        std::atomic<bool> cancel{false};
        std::string err;

        bool ok = sources_.mangadexClient().downloadChapterPages(ch.id, tmpExtract, &cancel,
            [this](const std::string& msg, int pct) {
                status_banner_ = msg + " (" + std::to_string(pct) + "%)";
                renderDetail();
                display_.refreshUI(0, 0, SCREEN_W, TOP_H);
            }, &err);

        if (ok) {
            status_banner_ = "Packing CBZ archive...";
            renderDetail();
            display_.refreshUI(0, 0, SCREEN_W, TOP_H);

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

                ch.isDownloaded = true;
                ch.localPath = cbzFile.string();
                status_banner_ = "Download complete!";
            } else {
                status_banner_ = "Failed creating CBZ archive";
            }
        } else {
            status_banner_ = "Download failed: " + err;
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

    void fetchChapters(const std::string& mangaId) {
        std::string err;
        selected_chapters_ = sources_.getChapters(mangaId, &err);
    }

    static constexpr int KB_Y = 1100;
    static constexpr int KB_ROW_H = 95;
    static constexpr int KB_START_Y = KB_Y + 15;

    void handleKeyboardTouch(int x, int y) {
        if (y < KB_START_Y) return;
        int row = (y - KB_START_Y) / KB_ROW_H;
        if (row < 0 || row > 3) return;

        if (row < 3) {
            const char* rows[] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
            std::string rStr = rows[row];
            int num_keys = static_cast<int>(rStr.length());
            int key_w = (SCREEN_W - 40) / num_keys;
            int col = (x - 20) / key_w;
            if (col >= 0 && col < num_keys) {
                search_query_ += rStr[col];
            }
        } else if (row == 3) {
            if (x < 200) {
                // Close button
                show_keyboard_ = false;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            } else if (x >= 205 && x < SCREEN_W - 345) {
                // SPACE
                search_query_ += " ";
            } else if (x >= SCREEN_W - 345 && x < SCREEN_W - 180) {
                // Backspace
                if (!search_query_.empty()) {
                    search_query_.pop_back();
                }
            } else if (x >= SCREEN_W - 180) {
                // GO / Submit Search
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

        // Fast responsive update of search bar without screen flash or driver lockup
        renderBrowse();
        display_.update(0, 0, SCREEN_W, TOP_H + 20, WAVEFORM_GC16, UPDATE_PARTIAL);
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
        canvas_.drawButton(SCREEN_W - 190, 15, 160, 50, "Exit App", false, 2);

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

            std::string sub = "Ch. " + hero.chapter_title + " • Page " + std::to_string(hero.page_index + 1) + " of " + std::to_string(hero.total_pages);
            font_.drawText(canvas_, 140, heroY + 95, sub, 24, 0x40, false);

            canvas_.drawButton(SCREEN_W - 220, heroY + 45, 170, 60, "Resume >", true, 2);
            listStartY = heroY + heroH + 25;
        }

        if (library_mangas_.empty()) {
            font_.drawText(canvas_, SCREEN_W / 2 - 140, 600, "Your library is empty.", 30, 0x60, true);
            font_.drawText(canvas_, SCREEN_W / 2 - 250, 650, "Tap 'Browse' below to search & download manga!", 24, 0x80, false);
            return;
        }

        font_.drawText(canvas_, 30, listStartY - 5, "Downloaded Manga (" + std::to_string(library_mangas_.size()) + ")", 24, 0x50, true);

        int y = listStartY + 30;
        int item_h = 130;

        for (const auto& manga : library_mangas_) {
            canvas_.drawRect(25, y, SCREEN_W - 50, item_h, 0x00, 2);
            drawCoverThumbnail(35, y + 10, 80, 110, manga.cover_url, manga.title);

            font_.drawText(canvas_, 135, y + 25, manga.title, 28, 0x00, true);
            font_.drawText(canvas_, 135, y + 75, "Ready to read", 22, 0x70, false);

            canvas_.drawButton(SCREEN_W - 180, y + 35, 130, 55, "Open", false, 2);

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
        canvas_.drawRect(20, 15, SCREEN_W - 200, 50, 0x00, 2);
        font_.drawText(canvas_, 40, 27, searchDisplay, 24, (search_query_.empty() && !show_keyboard_) ? 0x80 : 0x00, false);

        canvas_.drawButton(SCREEN_W - 160, 15, 140, 50, "Search", true, 2);

        // Source Switcher Pills
        int pill_y = TOP_H + 15;
        font_.drawText(canvas_, 25, pill_y + 10, "Source:", 22, 0x50, true);

        int pill_x = 120;
        for (const auto& s : sources_.getSources()) {
            bool active = (s.id == sources_.getActiveSourceId());
            canvas_.drawButton(pill_x, pill_y, 200, 45, s.name, active, 2);
            pill_x += 215;
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

            std::string sub = (manga.author.empty() ? sources_.getActiveSourceId() : manga.author) + " • " + manga.status;
            font_.drawText(canvas_, 135, y + 70, sub, 22, 0x60, false);

            canvas_.drawButton(SCREEN_W - 160, y + 35, 120, 55, "View", false, 2);

            y += item_h + 15;
            if (y > SCREEN_H - NAV_H - item_h) break;
        }
    }

    void renderDetail() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        canvas_.drawButton(20, 15, 140, 50, "< Back", false, 2);

        std::string titleStr = selected_manga_.title;
        if (titleStr.length() > 28) titleStr = titleStr.substr(0, 26) + "...";
        font_.drawText(canvas_, 190, 24, titleStr, 32, 0x00, true);

        if (!status_banner_.empty()) {
            font_.drawText(canvas_, 30, TOP_H + 12, status_banner_, 22, 0x00, true);
        }

        drawCoverThumbnail(30, TOP_H + 35, 150, 220, selected_manga_.cover_url, selected_manga_.title, selected_manga_.id);

        font_.drawText(canvas_, 210, TOP_H + 45, selected_manga_.title, 30, 0x00, true);
        font_.drawText(canvas_, 210, TOP_H + 95, "Author: " + (selected_manga_.author.empty() ? "MangaDex" : selected_manga_.author), 24, 0x40, false);
        font_.drawText(canvas_, 210, TOP_H + 135, "Status: " + selected_manga_.status, 24, 0x40, false);

        font_.drawText(canvas_, 30, 345, "Chapters (" + std::to_string(selected_chapters_.size()) + ")", 28, 0x00, true);
        canvas_.drawHLine(30, 380, SCREEN_W - 60, 0x00, 2);

        int y = 395;
        int item_h = 80;

        for (const auto& ch : selected_chapters_) {
            canvas_.drawRect(30, y, SCREEN_W - 60, item_h, 0x00, 2);
            std::string label = "Ch. " + ch.chapterNum + (ch.title.empty() ? "" : (" - " + ch.title));
            font_.drawText(canvas_, 50, y + 25, label, 24, 0x00, false);

            if (ch.isDownloaded) {
                canvas_.drawButton(SCREEN_W - 200, y + 15, 120, 50, "Read", true, 2);
            } else {
                canvas_.drawButton(SCREEN_W - 220, y + 15, 140, 50, "Download", false, 2);
            }

            y += item_h + 10;
            if (y > SCREEN_H - NAV_H - item_h) break;
        }
    }

    void renderDownloads() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 30, 24, "Downloaded Chapters", 32, 0x00, true);

        int y = TOP_H + 20;
        int item_h = 95;
        int count = 0;

        for (const auto& manga : library_mangas_) {
            std::string dirPath = "/mnt/us/manga/" + manga.title;
            std::error_code ec;
            if (fs::exists(dirPath, ec)) {
                for (const auto& f : fs::directory_iterator(dirPath, ec)) {
                    if (f.path().extension() == ".cbz") {
                        canvas_.drawRect(20, y, SCREEN_W - 40, item_h, 0x00, 2);
                        font_.drawText(canvas_, 40, y + 18, manga.title, 26, 0x00, true);
                        font_.drawText(canvas_, 40, y + 55, "Ch. " + f.path().stem().string(), 22, 0x50, false);
                        canvas_.drawButton(SCREEN_W - 180, y + 20, 120, 50, "Read", true, 2);

                        y += item_h + 12;
                        count++;
                        if (y > SCREEN_H - NAV_H - item_h) break;
                    }
                }
            }
        }

        if (count == 0) {
            font_.drawText(canvas_, SCREEN_W / 2 - 180, 500, "No downloaded chapters found.", 28, 0x60, false);
        }
    }

    void renderSettings() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        font_.drawText(canvas_, 30, 24, "Settings & Device Controls", 32, 0x00, true);

        if (!status_banner_.empty()) {
            font_.drawText(canvas_, 40, TOP_H + 15, status_banner_, 22, 0x00, true);
        }

        int y = TOP_H + 35;
        font_.drawText(canvas_, 40, y, "Kindle Oasis (Zelda Platform, 1264x1680 @ 300 PPI)", 24, 0x40, false);

        // Frontlight Control
        y += 60;
        font_.drawText(canvas_, 40, y + 10, "Frontlight Intensity:", 26, 0x00, true);
        canvas_.drawButton(300, y, 70, 55, "-", false, 2);
        std::string flVal = std::to_string(state_.settings().frontlight) + " / 24";
        font_.drawText(canvas_, 395, y + 12, flVal, 26, 0x00, true);
        canvas_.drawButton(490, y, 70, 55, "+", false, 2);

        // Reading Mode Control
        y += 80;
        font_.drawText(canvas_, 40, y + 10, "Reading Mode:", 26, 0x00, true);
        std::string modeLabel = (state_.settings().reading_direction == "RTL") ? "Manga (RTL <-)" : "Comic (LTR ->)";
        canvas_.drawButton(300, y, 260, 55, modeLabel, true, 2);

        y = 440;
        // Big Exit Button
        canvas_.drawButton(100, y, SCREEN_W - 200, 80, "Exit rm2manga to Kindle Home", true, 3);

        y = 550;
        // Clear Cache Button
        canvas_.drawButton(100, y, SCREEN_W - 200, 80, "Clear Reader Image Cache", false, 3);

        y = 660;
        // Refresh Library Button
        canvas_.drawButton(100, y, SCREEN_W - 200, 80, "Rescan Manga Storage", false, 3);
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
            canvas_.drawButton(20, 15, 160, 50, "< Back", false, 2);

            std::string titleStr = selected_manga_.title;
            if (titleStr.length() > 24) titleStr = titleStr.substr(0, 22) + "...";
            font_.drawText(canvas_, 200, 26, titleStr, 28, 0x00, true);

            std::string pageInfo = std::to_string(reader_current_page_ + 1) + " / " + std::to_string(reader_total_pages_);
            font_.drawText(canvas_, SCREEN_W - 200, 26, pageInfo, 26, 0x00, false);

            // Middle Light Control Overlay
            int midY = 820;
            canvas_.fillRect(SCREEN_W / 2 - 250, midY, 500, 70, 0xF5);
            canvas_.drawRect(SCREEN_W / 2 - 250, midY, 500, 70, 0x00, 2);
            font_.drawText(canvas_, SCREEN_W / 2 - 230, midY + 20, "Light", 24, 0x00, true);
            canvas_.drawButton(SCREEN_W / 2 - 130, midY + 10, 60, 50, "-", false, 2);
            std::string curFl = std::to_string(state_.settings().frontlight);
            font_.drawText(canvas_, SCREEN_W / 2 - 40, midY + 20, curFl, 24, 0x00, true);
            canvas_.drawButton(SCREEN_W / 2 + 10, midY + 10, 60, 50, "+", false, 2);
            canvas_.drawButton(SCREEN_W / 2 + 90, midY + 10, 140, 50, state_.settings().reading_direction, false, 2);

            // Bottom Bar Overlay
            int bot_y = SCREEN_H - 90;
            canvas_.fillRect(0, bot_y, SCREEN_W, 90, 0xF0);
            canvas_.drawHLine(0, bot_y, SCREEN_W, 0x00, 2);
            canvas_.drawButton(30, bot_y + 15, 180, 60, "< Prev", false, 2);
            canvas_.drawButton(SCREEN_W / 2 - 100, bot_y + 15, 200, 60, "Close Menu", false, 2);
            canvas_.drawButton(SCREEN_W - 210, bot_y + 15, 180, 60, "Next >", false, 2);
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
        canvas_.fillRect(0, KB_Y, SCREEN_W, kb_h, 0xF0);
        canvas_.drawHLine(0, KB_Y, SCREEN_W, 0x00, 3);

        const char* rows[] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };

        for (int r = 0; r < 3; ++r) {
            std::string rStr = rows[r];
            int num_keys = static_cast<int>(rStr.length());
            int key_w = (SCREEN_W - 40) / num_keys;
            int y = KB_START_Y + r * KB_ROW_H;

            for (int k = 0; k < num_keys; ++k) {
                int x = 20 + k * key_w;
                canvas_.drawButton(x + 3, y, key_w - 6, KB_ROW_H - 10, std::string(1, rStr[k]), false, 2);
            }
        }

        int bot_y = KB_START_Y + 3 * KB_ROW_H;
        int close_w = 170;
        int go_w = 160;
        int back_w = 150;
        canvas_.drawButton(20, bot_y, close_w, KB_ROW_H - 10, "Close", false, 2);
        int space_x = 20 + close_w + 15;
        int right_ctrl_w = back_w + 15 + go_w;
        int space_w = SCREEN_W - 20 - right_ctrl_w - 15 - space_x;
        canvas_.drawButton(space_x, bot_y, space_w, KB_ROW_H - 10, "SPACE", false, 2);
        int back_x = space_x + space_w + 15;
        canvas_.drawButton(back_x, bot_y, back_w, KB_ROW_H - 10, "<--", false, 2);
        int go_x = back_x + back_w + 15;
        canvas_.drawButton(go_x, bot_y, go_w, KB_ROW_H - 10, "GO", true, 2);
    }

    void renderBottomNav() {
        int nav_y = SCREEN_H - NAV_H;
        canvas_.fillRect(0, nav_y, SCREEN_W, NAV_H, 0xFF);
        canvas_.drawHLine(0, nav_y, SCREEN_W, 0x00, 2);

        const char* tabs[] = { "Library", "Browse", "Recent", "Settings" };
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
