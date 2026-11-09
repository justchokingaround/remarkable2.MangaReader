#pragma once

#include <string>
#include <vector>
#include <ctime>

namespace kindle {

struct UnifiedManga {
    std::string id;
    std::string title;
    std::string author;
    std::string description;
    std::string status;
    std::string cover_url;
    std::string cover_path;
    std::string source_id;
    bool in_library = false;
    int downloaded_count = 0;
    int unread_count = 0;
};

struct UnifiedChapter {
    std::string id;
    std::string chapterNum;
    std::string title;
    std::string scanlator;
    std::string url;
    std::string source_id;
    bool isDownloaded = false;
    bool isRead = false;
    std::string localPath;
};

struct SourceInfo {
    std::string id;
    std::string name;
    std::string description;
};

struct MangaProgress {
    std::string source_id;
    std::string manga_id;
    std::string manga_title;
    std::string cover_url;
    std::string cover_path;
    std::string chapter_id;
    std::string chapter_title;
    double chapter_num = -1.0;
    int page_index = 0;
    int total_pages = 0;
    time_t last_read = 0;
};

} // namespace kindle
