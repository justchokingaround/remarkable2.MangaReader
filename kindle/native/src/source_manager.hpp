#pragma once

#include "../../../backend/src/mangadex_client.hpp"
#include "../../../backend/src/json.hpp"
#include "../../../backend/src/util.hpp"
#include <string>
#include <vector>
#include <regex>

namespace kindle {

struct SourceInfo {
    std::string id;
    std::string name;
    std::string description;
};

struct UnifiedManga {
    std::string id;
    std::string title;
    std::string author;
    std::string description;
    std::string status;
    std::string cover_url;
    std::string source_id;
};

struct UnifiedChapter {
    std::string id;
    std::string chapterNum;
    std::string title;
    std::string scanlator;
    std::string url;
    std::string source_id;
    bool isDownloaded = false;
    std::string localPath;
};

class SourceManager {
private:
    mangadex::Client mangadex_client_;
    std::string active_source_id_ = "mangadex";
    std::vector<SourceInfo> available_sources_;

public:
    SourceManager() {
        available_sources_.push_back({"mangadex", "MangaDex", "Official API (Multi-language)"});
        available_sources_.push_back({"mangakatana", "MangaKatana", "Fast Scans (English)"});
    }

    const std::vector<SourceInfo>& getSources() const { return available_sources_; }
    const std::string& getActiveSourceId() const { return active_source_id_; }
    void setActiveSourceId(const std::string& id) { active_source_id_ = id; }

    std::vector<UnifiedManga> getPopular(std::string* errOut = nullptr) {
        if (active_source_id_ == "mangakatana") {
            return fetchMangaKatanaPopular(errOut);
        }
        // Default to MangaDex
        return fetchMangaDexPopular(errOut);
    }

    std::vector<UnifiedManga> search(const std::string& query, std::string* errOut = nullptr) {
        if (active_source_id_ == "mangakatana") {
            return searchMangaKatana(query, errOut);
        }
        return searchMangaDex(query, errOut);
    }

    std::vector<UnifiedChapter> getChapters(const std::string& mangaId, std::string* errOut = nullptr) {
        if (active_source_id_ == "mangakatana") {
            return fetchMangaKatanaChapters(mangaId, errOut);
        }
        return fetchMangaDexChapters(mangaId, errOut);
    }

    mangadex::Client& mangadexClient() { return mangadex_client_; }

private:
    std::vector<UnifiedManga> fetchMangaDexPopular(std::string* errOut) {
        std::vector<UnifiedManga> results;
        auto jsonList = mangadex_client_.queryMangaList("", "popular", errOut);
        if (jsonList.isArray()) {
            for (const auto& item : jsonList.asArray()) {
                UnifiedManga m;
                m.id = item["id"].str();
                m.title = item["title"].str();
                m.author = item["author"].str();
                m.description = item["description"].str();
                m.status = item["status"].str();
                m.cover_url = item["cover_url"].str();
                m.source_id = "mangadex";
                results.push_back(m);
            }
        }
        return results;
    }

    std::vector<UnifiedManga> searchMangaDex(const std::string& query, std::string* errOut) {
        std::vector<UnifiedManga> results;
        auto jsonList = mangadex_client_.queryMangaList(query, "search", errOut);
        if (jsonList.isArray()) {
            for (const auto& item : jsonList.asArray()) {
                UnifiedManga m;
                m.id = item["id"].str();
                m.title = item["title"].str();
                m.author = item["author"].str();
                m.description = item["description"].str();
                m.status = item["status"].str();
                m.cover_url = item["cover_url"].str();
                m.source_id = "mangadex";
                results.push_back(m);
            }
        }
        return results;
    }

    std::vector<UnifiedChapter> fetchMangaDexChapters(const std::string& mangaId, std::string* errOut) {
        std::vector<UnifiedChapter> results;
        auto jsonFeed = mangadex_client_.getChapters(mangaId, errOut);
        if (jsonFeed.isArray()) {
            for (const auto& item : jsonFeed.asArray()) {
                UnifiedChapter ch;
                ch.id = item["id"].str();
                ch.chapterNum = std::to_string(static_cast<int>(item["chapter_num"].num(0.0)));
                ch.title = item["title"].str();
                ch.scanlator = item["scanlator"].str();
                ch.source_id = "mangadex";
                results.push_back(ch);
            }
        }
        return results;
    }

    std::vector<UnifiedManga> fetchMangaKatanaPopular(std::string* errOut) {
        std::string html;
        if (!util::httpsGetString("https://mangakatana.com", html, 25)) {
            if (errOut) *errOut = "Failed connecting to MangaKatana";
            return {};
        }
        return parseKatanaMangaList(html);
    }

    std::vector<UnifiedManga> searchMangaKatana(const std::string& query, std::string* errOut) {
        std::string url = "https://mangakatana.com/?search=" + util::urlEncode(query);
        std::string html;
        if (!util::httpsGetString(url, html, 25)) {
            if (errOut) *errOut = "Search failed on MangaKatana";
            return {};
        }
        return parseKatanaMangaList(html);
    }

    std::vector<UnifiedManga> parseKatanaMangaList(const std::string& html) {
        std::vector<UnifiedManga> list;
        // Regex to parse MangaKatana items:
        // <a href="https://mangakatana.com/manga/(slug\.[0-9]+)"><img src="([^"]+)"
        // <h3 class="title"><a href="[^"]+">([^<]+)</a></h3>
        std::regex itemRegex(R"raw(<a href="https://mangakatana\.com/manga/([^"]+)"><img src="([^"]+)"[\s\S]*?<h3 class="title">\s*<a[^>]*>([^<]+)</a>)raw");
        auto words_begin = std::sregex_iterator(html.begin(), html.end(), itemRegex);
        auto words_end = std::sregex_iterator();

        for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
            std::smatch match = *i;
            UnifiedManga m;
            m.id = match[1].str();
            m.cover_url = match[2].str();
            m.title = match[3].str();
            m.source_id = "mangakatana";
            m.status = "MangaKatana";
            list.push_back(m);
            if (list.size() >= 24) break;
        }
        return list;
    }

    std::vector<UnifiedChapter> fetchMangaKatanaChapters(const std::string& mangaSlug, std::string* errOut) {
        std::vector<UnifiedChapter> chapters;
        std::string url = "https://mangakatana.com/manga/" + mangaSlug;
        std::string html;
        if (!util::httpsGetString(url, html, 25)) {
            if (errOut) *errOut = "Failed loading chapters from MangaKatana";
            return {};
        }

        // Parse chapters: <a href="([^"]+/c([0-9\.]+))">Chapter ([^<]+)</a>
        std::regex chRegex(R"raw(<a href="(https://mangakatana\.com/manga/[^"]+/c([0-9\.]+))"[^>]*>([^<]+)</a>)raw");
        auto words_begin = std::sregex_iterator(html.begin(), html.end(), chRegex);
        auto words_end = std::sregex_iterator();

        for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
            std::smatch match = *i;
            UnifiedChapter ch;
            ch.url = match[1].str();
            ch.id = match[1].str();
            ch.chapterNum = match[2].str();
            ch.title = match[3].str();
            ch.source_id = "mangakatana";
            chapters.push_back(ch);
        }
        std::reverse(chapters.begin(), chapters.end());
        return chapters;
    }
};

} // namespace kindle
