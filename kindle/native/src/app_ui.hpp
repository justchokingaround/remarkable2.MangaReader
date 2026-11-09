#pragma once

#include "canvas.hpp"
#include "display_zelda.hpp"
#include "input_kindle.hpp"
#include "zip_writer.hpp"
#include "../third_party/stb_image.h"
#include "../../../backend/src/mangadex_client.hpp"
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

struct ChapterItem {
    std::string id;
    std::string chapterNum;
    std::string title;
    int pages = 0;
    std::string scanlator;
    bool isDownloaded = false;
    std::string localPath;
};

struct MangaItem {
    std::string id;
    std::string title;
    std::string author;
    std::string description;
    std::string status;
    std::string coverUrl;
    std::vector<ChapterItem> chapters;
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
    mangadex::Client mangadex_client_;

    std::atomic<bool> is_running_{true};
    ViewState current_view_ = ViewState::LIBRARY;
    int selected_tab_ = 0; // 0: Library, 1: Browse, 2: Recent, 3: Settings

    // Manga lists
    std::vector<MangaItem> library_mangas_;
    std::vector<MangaItem> browse_mangas_;
    MangaItem selected_manga_;

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
                usleep(15000); // 15ms sleep to prevent CPU busy loop
            }
        }
    }

    void quit() {
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
                    MangaItem m;
                    m.title = entry.path().filename().string();
                    m.id = m.title;

                    // Scan chapters
                    for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
                        if (f.path().extension() == ".cbz") {
                            ChapterItem ch;
                            ch.title = f.path().stem().string();
                            ch.localPath = f.path().string();
                            ch.isDownloaded = true;
                            m.chapters.push_back(ch);
                        }
                    }
                    std::sort(m.chapters.begin(), m.chapters.end(), [](const ChapterItem& a, const ChapterItem& b) {
                        return util::naturalLess(a.title, b.title);
                    });
                    library_mangas_.push_back(m);
                }
            }
        }
        std::sort(library_mangas_.begin(), library_mangas_.end(), [](const MangaItem& a, const MangaItem& b) {
            return util::naturalLess(a.title, b.title);
        });
    }

    void handleInput(const InputEvent& ev) {
        if (ev.type == EventType::KEY_NEXT_PAGE) {
            if (current_view_ == ViewState::READER) {
                nextPage();
            }
            return;
        }
        if (ev.type == EventType::KEY_PREV_PAGE) {
            if (current_view_ == ViewState::READER) {
                prevPage();
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
                        status_banner_ = "Loading popular manga from MangaDex...";
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
        if (show_keyboard_ && y >= 1100 && y < SCREEN_H - NAV_H) {
            handleKeyboardTouch(x, y);
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

        int start_y = TOP_H + 20;
        int item_h = 140;

        for (size_t i = 0; i < library_mangas_.size(); ++i) {
            int item_y = start_y + static_cast<int>(i) * item_h - scroll_offset_;
            if (y >= item_y && y < item_y + item_h) {
                selected_manga_ = library_mangas_[i];
                current_view_ = ViewState::DETAIL;
                scroll_offset_ = 0;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
        }
    }

    void handleBrowseTouch(int x, int y) {
        // Search bar clicked
        if (y >= 10 && y <= TOP_H - 10) {
            show_keyboard_ = true;
            renderAll();
            display_.refreshUI(0, 1100, SCREEN_W, SCREEN_H - 1100);
            return;
        }

        // Browse list item clicked
        int start_y = TOP_H + 70;
        int item_h = 140;

        for (size_t i = 0; i < browse_mangas_.size(); ++i) {
            int item_y = start_y + static_cast<int>(i) * item_h - scroll_offset_;
            if (y >= item_y && y < item_y + item_h) {
                selected_manga_ = browse_mangas_[i];
                status_banner_ = "Loading chapters for " + selected_manga_.title + "...";
                renderBrowse();
                display_.refreshUI(0, 0, SCREEN_W, TOP_H + 60);

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

        for (size_t i = 0; i < selected_manga_.chapters.size(); ++i) {
            int ch_y = start_y + static_cast<int>(i) * item_h - scroll_offset_;
            if (y >= ch_y && y < ch_y + item_h) {
                // If downloaded or right-side tap: open chapter
                if (selected_manga_.chapters[i].isDownloaded) {
                    openChapter(static_cast<int>(i));
                } else {
                    // Tap download button or chapter row to download
                    downloadChapter(static_cast<int>(i));
                }
                return;
            }
        }
    }

    void handleDownloadsTouch(int x, int y) {
        // List recent chapters
        int start_y = TOP_H + 20;
        int item_h = 110;
        int idx = 0;

        for (const auto& manga : library_mangas_) {
            for (size_t c = 0; c < manga.chapters.size(); ++c) {
                int item_y = start_y + idx * item_h - scroll_offset_;
                if (y >= item_y && y < item_y + item_h) {
                    selected_manga_ = manga;
                    openChapter(static_cast<int>(c));
                    return;
                }
                idx++;
            }
        }
    }

    void handleSettingsTouch(int x, int y) {
        // Exit to Kindle Home button: x: 100..SCREEN_W-100, y: 350..430
        if (x >= 100 && x <= SCREEN_W - 100 && y >= 350 && y <= 430) {
            quit();
            return;
        }

        // Clear Cache button: x: 100..SCREEN_W-100, y: 470..550
        if (x >= 100 && x <= SCREEN_W - 100 && y >= 470 && y <= 550) {
            page_cache_.clearAll();
            status_banner_ = "Reader cache cleared successfully!";
            renderSettings();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // Refresh Library button: x: 100..SCREEN_W-100, y: 590..670
        if (x >= 100 && x <= SCREEN_W - 100 && y >= 590 && y <= 670) {
            loadLibraryFromDisk();
            status_banner_ = "Library refreshed!";
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

            // Tap in middle dismisses overlay
            reader_show_overlay_ = false;
            renderReader();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // 2. Normal Fullscreen Reading:
        // Top 10% of screen OR Center screen tap: Invoke overlay UI
        if (y <= 120 || (x >= SCREEN_W * 0.3 && x <= SCREEN_W * 0.7 && y >= SCREEN_H * 0.25 && y <= SCREEN_H * 0.75)) {
            reader_show_overlay_ = true;
            renderReader();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // 3. Page Turn: Left tap = next page (standard RTL manga), Right tap = prev page
        if (x < SCREEN_W * 0.45) {
            nextPage();
        } else if (x > SCREEN_W * 0.55) {
            prevPage();
        }
    }

    void nextPage() {
        if (reader_current_page_ + 1 < reader_total_pages_) {
            reader_current_page_++;
            renderReader();
            reader_page_turn_counter_++;
            if (reader_page_turn_counter_ % 8 == 0) {
                display_.refreshFull(WAVEFORM_GC16);
            } else {
                display_.refreshPage();
            }
        }
    }

    void prevPage() {
        if (reader_current_page_ > 0) {
            reader_current_page_--;
            renderReader();
            reader_page_turn_counter_++;
            if (reader_page_turn_counter_ % 8 == 0) {
                display_.refreshFull(WAVEFORM_GC16);
            } else {
                display_.refreshPage();
            }
        }
    }

    void openChapter(int chIdx) {
        if (chIdx < 0 || chIdx >= static_cast<int>(selected_manga_.chapters.size())) return;
        current_chapter_idx_ = chIdx;
        const auto& ch = selected_manga_.chapters[chIdx];

        if (ch.isDownloaded && fs::exists(ch.localPath)) {
            std::string err;
            std::set<std::string> pinned;
            reader_page_files_ = page_cache_.extractCbz(ch.id.empty() ? ch.title : ch.id, ch.localPath, pinned, &err);
            reader_total_pages_ = static_cast<int>(reader_page_files_.size());
            reader_current_page_ = 0;
            reader_show_overlay_ = false;
            current_view_ = ViewState::READER;
            renderReader();
            display_.refreshFull(WAVEFORM_GC16);
        } else {
            downloadChapter(chIdx);
        }
    }

    void downloadChapter(int chIdx) {
        if (chIdx < 0 || chIdx >= static_cast<int>(selected_manga_.chapters.size())) return;
        auto& ch = selected_manga_.chapters[chIdx];

        status_banner_ = "Downloading Ch. " + ch.chapterNum + "...";
        renderDetail();
        display_.refreshUI(0, 0, SCREEN_W, TOP_H);

        fs::path destDir = "/mnt/us/manga/" + selected_manga_.title;
        fs::path tmpExtract = "/tmp/rm2manga_dl_" + ch.id;
        std::atomic<bool> cancel{false};
        std::string err;

        bool ok = mangadex_client_.downloadChapterPages(ch.id, tmpExtract, &cancel,
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

            // Pack images into CBZ
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

    void fetchPopular() {
        status_banner_ = "Loading popular manga from MangaDex...";
        renderBrowse();
        display_.refreshUI(0, 0, SCREEN_W, TOP_H + 40);

        std::string err;
        auto jsonList = mangadex_client_.queryMangaList("", "popular", &err);
        browse_mangas_.clear();

        if (jsonList.isArray()) {
            for (const auto& item : jsonList.asArray()) {
                MangaItem m;
                m.id = item["id"].str();
                m.title = item["title"].str();
                m.author = item["author"].str();
                m.description = item["description"].str();
                m.status = item["status"].str();
                m.coverUrl = item["cover_url"].str();
                browse_mangas_.push_back(m);
            }
        }
        status_banner_.clear();
    }

    void fetchChapters(const std::string& mangaId) {
        std::string err;
        auto jsonFeed = mangadex_client_.getChapters(mangaId, &err);
        selected_manga_.chapters.clear();

        if (jsonFeed.isArray()) {
            for (const auto& item : jsonFeed.asArray()) {
                ChapterItem ch;
                ch.id = item["id"].str();
                ch.chapterNum = std::to_string(static_cast<int>(item["chapter_num"].num(0.0)));
                ch.title = item["title"].str();
                ch.scanlator = item["scanlator"].str();
                selected_manga_.chapters.push_back(ch);
            }
        }
    }

    void handleKeyboardTouch(int x, int y) {
        int row_h = 100;
        int row = (y - 1100) / row_h;
        if (row < 0 || row > 4) return;

        if (row == 4) {
            if (x < 300) { // Close
                show_keyboard_ = false;
            } else if (x > SCREEN_W - 300) { // Search
                show_keyboard_ = false;
                if (!search_query_.empty()) {
                    performSearch(search_query_);
                }
            } else { // Space
                search_query_ += " ";
            }
        } else {
            const char* rows[] = {
                "QWERTYUIOP",
                "ASDFGHJKL",
                "ZXCVBNM"
            };
            if (row < 3) {
                std::string rStr = rows[row];
                int key_w = SCREEN_W / (static_cast<int>(rStr.length()) + 1);
                int col = x / key_w;
                if (col >= 0 && col < static_cast<int>(rStr.length())) {
                    search_query_ += rStr[col];
                }
            } else if (row == 3) {
                if (!search_query_.empty()) {
                    search_query_.pop_back();
                }
            }
        }

        renderAll();
        display_.refreshUI(0, 0, SCREEN_W, SCREEN_H);
    }

    void performSearch(const std::string& query) {
        status_banner_ = "Searching MangaDex for: " + query;
        renderBrowse();
        display_.refreshUI(0, 0, SCREEN_W, TOP_H + 40);

        std::string err;
        auto jsonList = mangadex_client_.queryMangaList(query, "search", &err);
        browse_mangas_.clear();

        if (jsonList.isArray()) {
            for (const auto& item : jsonList.asArray()) {
                MangaItem m;
                m.id = item["id"].str();
                m.title = item["title"].str();
                m.author = item["author"].str();
                m.description = item["description"].str();
                m.status = item["status"].str();
                m.coverUrl = item["cover_url"].str();
                browse_mangas_.push_back(m);
            }
        }
        status_banner_.clear();
        renderBrowse();
        display_.refreshFull(WAVEFORM_GC16);
    }

    // --- RENDER PASSES ---
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
        canvas_.drawText(30, 25, "rm2manga - Library", 3, 0x00);
        canvas_.drawButton(SCREEN_W - 190, 15, 160, 50, "Exit App", false, 2);

        if (library_mangas_.empty()) {
            canvas_.drawText(SCREEN_W / 2 - 250, 450, "Your library is empty.", 3, 0x50);
            canvas_.drawText(SCREEN_W / 2 - 380, 530, "Tap 'Browse' below to search & download manga!", 2, 0x70);
            return;
        }

        int y = TOP_H + 20;
        int item_h = 130;

        for (const auto& manga : library_mangas_) {
            canvas_.drawRect(20, y, SCREEN_W - 40, item_h, 0x00, 2);
            canvas_.fillRect(30, y + 10, 80, 110, 0xD0);
            canvas_.drawRect(30, y + 10, 80, 110, 0x00, 2);

            canvas_.drawText(130, y + 25, manga.title, 2, 0x00);
            std::string sub = std::to_string(manga.chapters.size()) + " chapters downloaded";
            canvas_.drawText(130, y + 75, sub, 2, 0x60);

            y += item_h + 15;
            if (y > SCREEN_H - NAV_H - item_h) break;
        }
    }

    void renderBrowse() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);

        std::string searchDisplay = search_query_.empty() ? "Tap here to search MangaDex..." : search_query_;
        canvas_.drawRect(20, 15, SCREEN_W - 40, 50, 0x00, 2);
        canvas_.drawText(40, 26, searchDisplay, 2, search_query_.empty() ? 0x80 : 0x00);

        if (!status_banner_.empty()) {
            canvas_.drawText(30, TOP_H + 20, status_banner_, 2, 0x00);
        }

        int y = TOP_H + 60;
        int item_h = 130;

        for (const auto& manga : browse_mangas_) {
            canvas_.drawRect(20, y, SCREEN_W - 40, item_h, 0x00, 2);
            canvas_.fillRect(30, y + 10, 80, 110, 0xD0);
            canvas_.drawRect(30, y + 10, 80, 110, 0x00, 2);

            canvas_.drawText(130, y + 25, manga.title, 2, 0x00);
            std::string sub = (manga.author.empty() ? "MangaDex" : manga.author) + " • " + manga.status;
            canvas_.drawText(130, y + 75, sub, 2, 0x60);

            y += item_h + 15;
            if (y > SCREEN_H - NAV_H - item_h) break;
        }
    }

    void renderDetail() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        canvas_.drawButton(20, 15, 140, 50, "< Back", false, 2);
        canvas_.drawText(190, 25, selected_manga_.title, 3, 0x00);

        if (!status_banner_.empty()) {
            canvas_.drawText(30, TOP_H + 10, status_banner_, 2, 0x00);
        }

        canvas_.fillRect(30, TOP_H + 30, 150, 220, 0xD0);
        canvas_.drawRect(30, TOP_H + 30, 150, 220, 0x00, 2);

        canvas_.drawText(210, TOP_H + 40, selected_manga_.title, 2, 0x00);
        canvas_.drawText(210, TOP_H + 85, "Author: " + (selected_manga_.author.empty() ? "Unknown" : selected_manga_.author), 2, 0x40);
        canvas_.drawText(210, TOP_H + 125, "Status: " + selected_manga_.status, 2, 0x40);

        canvas_.drawText(30, 340, "Chapters (" + std::to_string(selected_manga_.chapters.size()) + ")", 3, 0x00);
        canvas_.drawHLine(30, 375, SCREEN_W - 60, 0x00, 2);

        int y = 390;
        int item_h = 80;

        for (const auto& ch : selected_manga_.chapters) {
            canvas_.drawRect(30, y, SCREEN_W - 60, item_h, 0x00, 2);
            std::string label = "Ch. " + ch.chapterNum + (ch.title.empty() ? "" : (" - " + ch.title));
            canvas_.drawText(50, y + 25, label, 2, 0x00);

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
        canvas_.drawText(30, 25, "Recent Chapters", 3, 0x00);

        int y = TOP_H + 20;
        int item_h = 95;
        int count = 0;

        for (const auto& manga : library_mangas_) {
            for (const auto& ch : manga.chapters) {
                canvas_.drawRect(20, y, SCREEN_W - 40, item_h, 0x00, 2);
                canvas_.drawText(40, y + 18, manga.title, 2, 0x00);
                canvas_.drawText(40, y + 55, "Ch. " + ch.title, 2, 0x50);
                canvas_.drawButton(SCREEN_W - 180, y + 20, 120, 50, "Read", true, 2);

                y += item_h + 12;
                count++;
                if (y > SCREEN_H - NAV_H - item_h) break;
            }
            if (y > SCREEN_H - NAV_H - item_h) break;
        }

        if (count == 0) {
            canvas_.drawText(SCREEN_W / 2 - 250, 450, "No downloaded chapters found.", 3, 0x50);
        }
    }

    void renderSettings() {
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        canvas_.drawText(30, 25, "Settings & Device Info", 3, 0x00);

        if (!status_banner_.empty()) {
            canvas_.drawText(40, TOP_H + 15, status_banner_, 2, 0x00);
        }

        int y = TOP_H + 40;
        canvas_.drawText(40, y, "rm2manga for Kindle Oasis", 3, 0x00);
        y += 50;
        canvas_.drawText(40, y, "Screen: 1264x1680 (300 PPI, 8-bit Grayscale)", 2, 0x40);
        y += 40;
        canvas_.drawText(40, y, "EPDC Controller: Zelda MXCFB Native Driver", 2, 0x40);
        y += 40;
        canvas_.drawText(40, y, "Storage Directory: /mnt/us/manga", 2, 0x40);

        y = 350;
        // Big Exit Button
        canvas_.drawButton(100, y, SCREEN_W - 200, 80, "Exit rm2manga to Kindle Home", true, 3);

        y = 470;
        // Clear Cache Button
        canvas_.drawButton(100, y, SCREEN_W - 200, 80, "Clear Reader Image Cache", false, 3);

        y = 590;
        // Refresh Library Button
        canvas_.drawButton(100, y, SCREEN_W - 200, 80, "Rescan Library on Storage", false, 3);
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
            canvas_.drawText(200, 26, titleStr, 2, 0x00);

            std::string pageInfo = std::to_string(reader_current_page_ + 1) + " / " + std::to_string(reader_total_pages_);
            int piW = canvas_.measureTextWidth(pageInfo, 2);
            canvas_.drawText(SCREEN_W - piW - 40, 26, pageInfo, 2, 0x00);

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
            canvas_.drawText(SCREEN_W / 2 - 200, SCREEN_H / 2 - 30, "No page image available", 2, 0x00);
            return;
        }

        std::string rawPath = reader_page_files_[reader_current_page_];
        if (rawPath.rfind("file://", 0) == 0) {
            rawPath = rawPath.substr(7);
        }

        int imgW = 0, imgH = 0, channels = 0;
        // Decode directly as 1-channel 8-bit grayscale
        unsigned char* data = stbi_load(rawPath.c_str(), &imgW, &imgH, &channels, 1);
        if (!data) {
            canvas_.drawText(SCREEN_W / 2 - 220, SCREEN_H / 2 - 30, "Failed to decode page: " + fs::path(rawPath).filename().string(), 2, 0x00);
            return;
        }

        // Fullscreen edge-to-edge aspect ratio fit (1264 x 1680)
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
        int kb_y = 1100;
        int kb_h = SCREEN_H - NAV_H - kb_y;
        canvas_.fillRect(0, kb_y, SCREEN_W, kb_h, 0xF0);
        canvas_.drawHLine(0, kb_y, SCREEN_W, 0x00, 3);

        const char* rows[] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
        int row_h = 95;

        for (int r = 0; r < 3; ++r) {
            std::string rStr = rows[r];
            int num_keys = static_cast<int>(rStr.length());
            int key_w = (SCREEN_W - 40) / num_keys;
            int y = kb_y + 15 + r * row_h;

            for (int k = 0; k < num_keys; ++k) {
                int x = 20 + k * key_w;
                canvas_.drawButton(x + 3, y, key_w - 6, row_h - 10, std::string(1, rStr[k]), false, 2);
            }
        }

        // Bottom row: Close, Space, Backspace, Search
        int bot_y = kb_y + 15 + 3 * row_h;
        canvas_.drawButton(20, bot_y, 160, row_h - 10, "Close", false, 2);
        canvas_.drawButton(190, bot_y, SCREEN_W - 480, row_h - 10, "SPACE", false, 2);
        canvas_.drawButton(SCREEN_W - 280, bot_y, 120, row_h - 10, "<--", false, 2);
        canvas_.drawButton(SCREEN_W - 150, bot_y, 130, row_h - 10, "GO", true, 2);
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
                canvas_.drawText(x + (tab_w - canvas_.measureTextWidth(tabs[i], 2)) / 2, nav_y + 35, tabs[i], 2, 0xFF);
            } else {
                canvas_.drawText(x + (tab_w - canvas_.measureTextWidth(tabs[i], 2)) / 2, nav_y + 35, tabs[i], 2, 0x00);
            }
        }
    }
};

} // namespace kindle
