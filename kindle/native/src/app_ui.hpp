#pragma once

#include "canvas.hpp"
#include "display_zelda.hpp"
#include "input_kindle.hpp"
#include "../../../backend/src/mangadex_client.hpp"
#include "../../../backend/src/cbz_extractor.hpp"
#include <vector>
#include <string>
#include <memory>
#include <filesystem>

namespace fs = std::filesystem;

namespace kindle {

enum class ViewState {
    LIBRARY = 0,
    BROWSE,
    DETAIL,
    READER,
    DOWNLOADS
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

    // Search and Keyboard state
    bool show_keyboard_ = false;
    std::string search_query_;
    std::string status_banner_;

    // Scroll state
    int scroll_offset_ = 0;

public:
    AppUI() = default;

    bool init() {
        if (!display_.init()) return false;
        canvas_ = Canvas(SCREEN_W, SCREEN_H, DisplayZelda::STRIDE, display_.getBuffer());
        if (!input_.start()) return false;

        loadLibraryFromDisk();
        renderAll();
        display_.refreshFull(WAVEFORM_GC16);
        return true;
    }

    void run() {
        bool running = true;
        InputEvent ev;

        while (running) {
            if (input_.pollEvent(ev)) {
                handleInput(ev);
            } else {
                usleep(15000); // 15ms sleep to prevent CPU busy loop
            }
        }
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
                    library_mangas_.push_back(m);
                }
            }
        }
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
        // 1. Bottom Navigation Bar touch
        if (y >= SCREEN_H - NAV_H && current_view_ != ViewState::READER) {
            int tab_w = SCREEN_W / 4;
            int tapped_tab = x / tab_w;
            if (tapped_tab >= 0 && tapped_tab < 4) {
                selected_tab_ = tapped_tab;
                if (selected_tab_ == 0) {
                    current_view_ = ViewState::LIBRARY;
                    loadLibraryFromDisk();
                } else if (selected_tab_ == 1) {
                    current_view_ = ViewState::BROWSE;
                    if (browse_mangas_.empty()) {
                        fetchPopular();
                    }
                }
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
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
        } else if (current_view_ == ViewState::READER) {
            handleReaderTouch(x, y);
        }
    }

    void handleLibraryTouch(int x, int y) {
        // Manga item clicked
        int start_y = TOP_H + 20;
        int item_h = 140;

        for (size_t i = 0; i < library_mangas_.size(); ++i) {
            int item_y = start_y + i * item_h - scroll_offset_;
            if (y >= item_y && y < item_y + item_h) {
                selected_manga_ = library_mangas_[i];
                current_view_ = ViewState::DETAIL;
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
        int start_y = TOP_H + 20;
        int item_h = 140;

        for (size_t i = 0; i < browse_mangas_.size(); ++i) {
            int item_y = start_y + i * item_h - scroll_offset_;
            if (y >= item_y && y < item_y + item_h) {
                selected_manga_ = browse_mangas_[i];
                fetchChapters(selected_manga_.id);
                current_view_ = ViewState::DETAIL;
                renderAll();
                display_.refreshFull(WAVEFORM_GC16);
                return;
            }
        }
    }

    void handleDetailTouch(int x, int y) {
        // Back button
        if (x >= 20 && x <= 160 && y >= 15 && y <= 65) {
            current_view_ = (selected_tab_ == 0) ? ViewState::LIBRARY : ViewState::BROWSE;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // Chapter tapped
        int start_y = 380;
        int item_h = 90;

        for (size_t i = 0; i < selected_manga_.chapters.size(); ++i) {
            int ch_y = start_y + i * item_h - scroll_offset_;
            if (y >= ch_y && y < ch_y + item_h) {
                openChapter(static_cast<int>(i));
                return;
            }
        }
    }

    void handleReaderTouch(int x, int y) {
        // Center tap: toggle overlay controls
        if (x >= SCREEN_W * 0.3 && x <= SCREEN_W * 0.7 && y >= SCREEN_H * 0.3 && y <= SCREEN_H * 0.7) {
            reader_show_overlay_ = !reader_show_overlay_;
            renderReader();
            display_.refreshUI(0, 0, SCREEN_W, SCREEN_H);
            return;
        }

        // Top bar back button in overlay
        if (reader_show_overlay_ && y <= 80 && x <= 160) {
            current_view_ = ViewState::DETAIL;
            reader_show_overlay_ = false;
            renderAll();
            display_.refreshFull(WAVEFORM_GC16);
            return;
        }

        // RTL Page Turn: Tap left -> next page; Tap right -> prev page
        if (x < SCREEN_W * 0.4) {
            nextPage();
        } else if (x > SCREEN_W * 0.6) {
            prevPage();
        }
    }

    void nextPage() {
        if (reader_current_page_ + 1 < reader_total_pages_) {
            reader_current_page_++;
            renderReader();
            display_.refreshPage();
        }
    }

    void prevPage() {
        if (reader_current_page_ > 0) {
            reader_current_page_--;
            renderReader();
            display_.refreshPage();
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
            current_view_ = ViewState::READER;
            renderReader();
            display_.refreshFull(WAVEFORM_GC16);
        } else {
            status_banner_ = "Downloading chapter...";
            renderDetail();
            display_.refreshUI(0, 0, SCREEN_W, TOP_H);
        }
    }

    void fetchPopular() {
        status_banner_ = "Loading popular manga from MangaDex...";
        renderBrowse();
        display_.refreshUI(0, 0, SCREEN_W, TOP_H);

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
        // Keyboard bounds: y: 1100 to 1590
        int row_h = 100;
        int row = (y - 1100) / row_h;
        if (row < 0 || row > 4) return;

        // Space / Backspace / Search row
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
            // QWERTY rows
            const char* rows[] = {
                "QWERTYUIOP",
                "ASDFGHJKL",
                "ZXCVBNM"
            };
            if (row < 3) {
                std::string rStr = rows[row];
                int key_w = SCREEN_W / (rStr.length() + 1);
                int col = x / key_w;
                if (col >= 0 && col < static_cast<int>(rStr.length())) {
                    search_query_ += rStr[col];
                }
            } else if (row == 3) {
                // Backspace
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
        display_.refreshUI(0, 0, SCREEN_W, TOP_H);

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

        if (library_mangas_.empty()) {
            canvas_.drawText(SCREEN_W / 2 - 250, 400, "Your library is empty.", 3, 0x50);
            canvas_.drawText(SCREEN_W / 2 - 350, 480, "Tap 'Browse' below to search & download manga!", 2, 0x70);
            return;
        }

        // Render manga list items
        int y = TOP_H + 20;
        int item_h = 130;

        for (const auto& manga : library_mangas_) {
            canvas_.drawRect(20, y, SCREEN_W - 40, item_h, 0x00, 2);
            // Thumbnail placeholder
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
        // Top Search Bar
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);

        std::string searchDisplay = search_query_.empty() ? "Tap here to search MangaDex..." : search_query_;
        canvas_.drawRect(20, 15, SCREEN_W - 40, 50, 0x00, 2);
        canvas_.drawText(40, 26, searchDisplay, 2, search_query_.empty() ? 0x80 : 0x00);

        if (!status_banner_.empty()) {
            canvas_.drawText(30, TOP_H + 15, status_banner_, 2, 0x00);
        }

        // Render browse results
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
        // Header with Back button
        canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF5);
        canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
        canvas_.drawButton(20, 15, 140, 50, "< Back", false, 2);
        canvas_.drawText(190, 25, selected_manga_.title, 3, 0x00);

        // Manga Overview
        canvas_.fillRect(30, TOP_H + 20, 150, 220, 0xD0);
        canvas_.drawRect(30, TOP_H + 20, 150, 220, 0x00, 2);

        canvas_.drawText(210, TOP_H + 30, selected_manga_.title, 2, 0x00);
        canvas_.drawText(210, TOP_H + 75, "Author: " + (selected_manga_.author.empty() ? "Unknown" : selected_manga_.author), 2, 0x40);
        canvas_.drawText(210, TOP_H + 115, "Status: " + selected_manga_.status, 2, 0x40);

        // Chapters Section
        canvas_.drawText(30, 340, "Chapters (" + std::to_string(selected_manga_.chapters.size()) + ")", 3, 0x00);
        canvas_.drawHLine(30, 375, SCREEN_W - 60, 0x00, 2);

        int y = 390;
        int item_h = 80;

        for (const auto& ch : selected_manga_.chapters) {
            canvas_.drawRect(30, y, SCREEN_W - 60, item_h, 0x00, 2);
            std::string label = "Ch. " + ch.chapterNum + (ch.title.empty() ? "" : (" - " + ch.title));
            canvas_.drawText(50, y + 25, label, 2, 0x00);

            if (ch.isDownloaded) {
                canvas_.drawText(SCREEN_W - 250, y + 25, "[Downloaded]", 2, 0x00);
            } else {
                canvas_.drawButton(SCREEN_W - 220, y + 15, 140, 50, "Download", false, 2);
            }

            y += item_h + 10;
            if (y > SCREEN_H - NAV_H - item_h) break;
        }
    }

    void renderReader() {
        canvas_.clear(0xFF);

        // Render current manga page
        if (reader_current_page_ < reader_total_pages_ && !reader_page_files_.empty()) {
            canvas_.drawText(SCREEN_W / 2 - 150, SCREEN_H / 2 - 50, "Reading Page " + std::to_string(reader_current_page_ + 1), 3, 0x00);
        }

        // Overlay controls when tapped
        if (reader_show_overlay_) {
            canvas_.fillRect(0, 0, SCREEN_W, TOP_H, 0xF0);
            canvas_.drawHLine(0, TOP_H, SCREEN_W, 0x00, 2);
            canvas_.drawButton(20, 15, 140, 50, "< Close", false, 2);
            canvas_.drawText(190, 25, selected_manga_.title, 2, 0x00);

            // Bottom page scrubber bar
            canvas_.fillRect(0, SCREEN_H - 80, SCREEN_W, 80, 0xF0);
            canvas_.drawHLine(0, SCREEN_H - 80, SCREEN_W, 0x00, 2);
            std::string pageInfo = "Page " + std::to_string(reader_current_page_ + 1) + " / " + std::to_string(reader_total_pages_);
            canvas_.drawText(SCREEN_W / 2 - 100, SCREEN_H - 50, pageInfo, 2, 0x00);
        }
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
