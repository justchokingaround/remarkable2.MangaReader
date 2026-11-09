#pragma once

#include "types.hpp"
#include "../../../backend/src/json.hpp"
#include "../../../backend/src/util.hpp"
#include <string>
#include <map>
#include <set>
#include <vector>
#include <ctime>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace kindle {

struct AppSettings {
    int frontlight = 12; // 0 to 24
    std::string reading_direction = "RTL"; // "RTL" or "LTR"
    bool trim_margins = true;
    bool button_inverted = false;
    int refresh_interval = 8;
    int touch_rotation = 0; // 0 (buttons right) or 180 (buttons left)
};

class StateManager {
private:
    std::string state_file_ = "/mnt/us/manga/state.json";
    AppSettings settings_;
    std::string active_manga_;
    std::map<std::string, MangaProgress> progress_map_;
    std::map<std::string, UnifiedManga> library_map_;
    std::map<std::string, std::vector<UnifiedChapter>> chapters_cache_map_;

    static std::string makeKey(const std::string& sourceId, const std::string& mangaId, const std::string& title) {
        if (!sourceId.empty() && !mangaId.empty()) {
            return sourceId + ":" + mangaId;
        }
        return title;
    }

public:
    StateManager() = default;

    AppSettings& settings() { return settings_; }
    const AppSettings& settings() const { return settings_; }

    const std::string& activeManga() const { return active_manga_; }

    void load(const std::string& path = "/mnt/us/manga/state.json") {
        state_file_ = path;
        std::string raw;
        if (!util::readFileToString(state_file_, raw)) return;

        bool ok = false;
        Json::Value root = Json::parse(raw, &ok);
        if (!ok || !root.isObject()) return;

        active_manga_ = root["active_manga"].str();

        if (root.has("settings") && root["settings"].isObject()) {
            const auto& s = root["settings"];
            settings_.frontlight = static_cast<int>(s["frontlight"].integer(12));
            settings_.reading_direction = s["reading_direction"].str("RTL");
            settings_.trim_margins = s["trim_margins"].boolean(true);
            settings_.button_inverted = s["button_inverted"].boolean(false);
            settings_.refresh_interval = static_cast<int>(s["refresh_interval"].integer(8));
            settings_.touch_rotation = static_cast<int>(s["touch_rotation"].integer(0));
        }

        if (root.has("history") && root["history"].isObject()) {
            progress_map_.clear();
            for (const auto& kv : root["history"].asObject()) {
                MangaProgress p;
                const auto& item = kv.second;
                p.manga_title = item.has("manga_title") ? item["manga_title"].str() : kv.first;
                p.source_id = item["source_id"].str();
                p.manga_id = item["manga_id"].str();
                p.cover_url = item["cover_url"].str();
                p.cover_path = item["cover_path"].str();
                p.chapter_id = item["chapter_id"].str();
                p.chapter_title = item["chapter_title"].str();
                p.chapter_num = item["chapter_num"].num(-1.0);
                p.page_index = static_cast<int>(item["page_index"].integer(0));
                p.total_pages = static_cast<int>(item["total_pages"].integer(0));
                p.last_read = static_cast<time_t>(item["last_read"].integer(0));

                // Backwards compatibility migration: if source_id or manga_id missing,
                // try to populate from library
                if (p.source_id.empty() || p.manga_id.empty()) {
                    for (const auto& libItem : root["library"].asArray()) {
                        if (libItem["title"].str() == p.manga_title) {
                            p.source_id = libItem["source_id"].str();
                            p.manga_id = libItem["id"].str();
                            if (p.cover_url.empty()) p.cover_url = libItem["cover_url"].str();
                            break;
                        }
                    }
                }

                std::string k = makeKey(p.source_id, p.manga_id, p.manga_title);
                progress_map_[k] = p;
                if (!p.manga_title.empty()) {
                    progress_map_[p.manga_title] = p;
                }
            }
        }

        if (root.has("library") && root["library"].isArray()) {
            library_map_.clear();
            for (const auto& item : root["library"].asArray()) {
                UnifiedManga m;
                m.id = item["id"].str();
                m.title = item["title"].str();
                m.author = item["author"].str();
                m.description = item["description"].str();
                m.status = item["status"].str();
                m.cover_url = item["cover_url"].str();
                m.source_id = item["source_id"].str();
                m.in_library = true;
                std::string k = makeKey(m.source_id, m.id, m.title);
                library_map_[k] = m;
            }
        }

        if (root.has("chapters_cache") && root["chapters_cache"].isObject()) {
            chapters_cache_map_.clear();
            for (const auto& kv : root["chapters_cache"].asObject()) {
                if (kv.second.isArray()) {
                    std::vector<UnifiedChapter> list;
                    for (const auto& item : kv.second.asArray()) {
                        UnifiedChapter ch;
                        ch.id = item["id"].str();
                        ch.chapterNum = item["chapterNum"].str();
                        ch.title = item["title"].str();
                        ch.scanlator = item["scanlator"].str();
                        ch.url = item["url"].str();
                        ch.source_id = item["source_id"].str();
                        list.push_back(ch);
                    }
                    chapters_cache_map_[kv.first] = list;
                }
            }
        }
    }

    void save() {
        Json::Value root = Json::Value::object();
        root["active_manga"] = active_manga_;

        Json::Value s = Json::Value::object();
        s["frontlight"] = static_cast<double>(settings_.frontlight);
        s["reading_direction"] = settings_.reading_direction;
        s["trim_margins"] = settings_.trim_margins;
        s["button_inverted"] = settings_.button_inverted;
        s["refresh_interval"] = static_cast<double>(settings_.refresh_interval);
        s["touch_rotation"] = static_cast<double>(settings_.touch_rotation);
        root["settings"] = s;

        Json::Value h = Json::Value::object();
        std::set<std::string> savedKeys;
        for (const auto& kv : progress_map_) {
            const auto& p = kv.second;
            std::string canonicalKey = makeKey(p.source_id, p.manga_id, p.manga_title);
            if (savedKeys.find(canonicalKey) != savedKeys.end()) continue;
            savedKeys.insert(canonicalKey);

            Json::Value item = Json::Value::object();
            item["source_id"] = p.source_id;
            item["manga_id"] = p.manga_id;
            item["manga_title"] = p.manga_title;
            item["cover_url"] = p.cover_url;
            item["cover_path"] = p.cover_path;
            item["chapter_id"] = p.chapter_id;
            item["chapter_title"] = p.chapter_title;
            if (p.chapter_num >= 0) item["chapter_num"] = p.chapter_num;
            item["page_index"] = static_cast<double>(p.page_index);
            item["total_pages"] = static_cast<double>(p.total_pages);
            item["last_read"] = static_cast<double>(p.last_read);

            h[canonicalKey] = item;
            if (!p.manga_title.empty() && p.manga_title != canonicalKey) {
                h[p.manga_title] = item;
            }
        }
        root["history"] = h;

        Json::Value libArr = Json::Value::array();
        for (const auto& kv : library_map_) {
            Json::Value m = Json::Value::object();
            m["id"] = kv.second.id;
            m["title"] = kv.second.title;
            m["author"] = kv.second.author;
            m["description"] = kv.second.description;
            m["status"] = kv.second.status;
            m["cover_url"] = kv.second.cover_url;
            m["source_id"] = kv.second.source_id;
            libArr.push(m);
        }
        root["library"] = libArr;

        Json::Value cacheObj = Json::Value::object();
        for (const auto& kv : chapters_cache_map_) {
            Json::Value arr = Json::Value::array();
            for (const auto& ch : kv.second) {
                Json::Value item = Json::Value::object();
                item["id"] = ch.id;
                item["chapterNum"] = ch.chapterNum;
                item["title"] = ch.title;
                item["scanlator"] = ch.scanlator;
                item["url"] = ch.url;
                item["source_id"] = ch.source_id;
                arr.push(item);
            }
            cacheObj[kv.first] = arr;
        }
        root["chapters_cache"] = cacheObj;

        std::string jsonStr = Json::stringify(root);
        util::atomicWriteFile(state_file_, jsonStr);
    }

    void saveChaptersCache(const std::string& mangaTitle, const std::string& sourceId, const std::string& mangaId, const std::vector<UnifiedChapter>& chapters) {
        if (chapters.empty()) return;
        std::string k = makeKey(sourceId, mangaId, mangaTitle);
        chapters_cache_map_[k] = chapters;
        if (!mangaTitle.empty()) {
            chapters_cache_map_[mangaTitle] = chapters;
        }
        save();
    }

    std::vector<UnifiedChapter> getCachedChapters(const std::string& mangaTitle, const std::string& sourceId = "", const std::string& mangaId = "") const {
        std::string k = makeKey(sourceId, mangaId, mangaTitle);
        auto it = chapters_cache_map_.find(k);
        if (it != chapters_cache_map_.end() && !it->second.empty()) {
            return it->second;
        }
        if (!mangaTitle.empty()) {
            auto it2 = chapters_cache_map_.find(mangaTitle);
            if (it2 != chapters_cache_map_.end() && !it2->second.empty()) {
                return it2->second;
            }
        }
        return {};
    }

    bool isInLibrary(const std::string& sourceId, const std::string& mangaId, const std::string& title = "") const {
        std::string k1 = makeKey(sourceId, mangaId, title);
        if (library_map_.find(k1) != library_map_.end()) return true;
        if (!title.empty()) {
            for (const auto& kv : library_map_) {
                if (kv.second.title == title) return true;
            }
        }
        return false;
    }

    bool toggleLibrary(const UnifiedManga& manga) {
        std::string k = makeKey(manga.source_id, manga.id, manga.title);
        auto it = library_map_.find(k);
        if (it == library_map_.end() && !manga.title.empty()) {
            for (auto iter = library_map_.begin(); iter != library_map_.end(); ++iter) {
                if (iter->second.title == manga.title) {
                    it = iter;
                    break;
                }
            }
        }

        if (it != library_map_.end()) {
            library_map_.erase(it);
            save();
            return false; // Removed from library
        } else {
            UnifiedManga copy = manga;
            copy.in_library = true;
            library_map_[k] = copy;
            save();
            return true; // Added to library
        }
    }

    std::vector<UnifiedManga> getLibraryMangas() const {
        std::vector<UnifiedManga> list;
        list.reserve(library_map_.size());
        for (const auto& kv : library_map_) {
            list.push_back(kv.second);
        }
        return list;
    }

    void updateProgress(const std::string& sourceId,
                        const std::string& mangaId,
                        const std::string& mangaTitle,
                        const std::string& chapterId,
                        const std::string& chapterTitle,
                        double chapterNum,
                        int page,
                        int total,
                        const std::string& coverUrl = "",
                        const std::string& coverPath = "") {
        if (mangaTitle.empty()) return;
        active_manga_ = mangaTitle;

        std::string canonicalKey = makeKey(sourceId, mangaId, mangaTitle);
        MangaProgress p;
        auto it = progress_map_.find(canonicalKey);
        if (it != progress_map_.end()) {
            p = it->second;
        }
        p.source_id = sourceId;
        p.manga_id = mangaId;
        p.manga_title = mangaTitle;
        if (!coverUrl.empty()) p.cover_url = coverUrl;
        if (!coverPath.empty()) p.cover_path = coverPath;
        p.chapter_id = chapterId;
        p.chapter_title = chapterTitle;
        p.chapter_num = chapterNum;
        p.page_index = page;
        p.total_pages = total;
        p.last_read = std::time(nullptr);

        progress_map_[canonicalKey] = p;
        if (!mangaTitle.empty()) {
            progress_map_[mangaTitle] = p;
        }

        save();
    }

    void updateProgress(const std::string& manga, const std::string& chapter, int page, int total) {
        updateProgress("", "", manga, "", chapter, -1.0, page, total);
    }

    bool getProgress(const std::string& sourceId, const std::string& mangaId, const std::string& mangaTitle, MangaProgress& out) const {
        std::string canonicalKey = makeKey(sourceId, mangaId, mangaTitle);
        auto it = progress_map_.find(canonicalKey);
        if (it != progress_map_.end()) {
            out = it->second;
            return true;
        }
        if (!mangaTitle.empty()) {
            auto it2 = progress_map_.find(mangaTitle);
            if (it2 != progress_map_.end()) {
                out = it2->second;
                return true;
            }
        }
        return false;
    }

    bool getProgress(const std::string& manga, MangaProgress& out) const {
        return getProgress("", "", manga, out);
    }

    std::vector<MangaProgress> getRecentHistory(int limit = 50) const {
        std::vector<MangaProgress> list;
        std::set<std::string> seen;
        for (const auto& kv : progress_map_) {
            const auto& p = kv.second;
            if (p.last_read <= 0 || p.manga_title.empty()) continue;
            std::string canonicalKey = makeKey(p.source_id, p.manga_id, p.manga_title);
            if (seen.find(canonicalKey) != seen.end()) continue;
            seen.insert(canonicalKey);
            list.push_back(p);
        }
        std::sort(list.begin(), list.end(), [](const MangaProgress& a, const MangaProgress& b) {
            return a.last_read > b.last_read;
        });
        if (static_cast<int>(list.size()) > limit) {
            list.resize(limit);
        }
        return list;
    }

    bool clearHistoryItem(const std::string& sourceId, const std::string& mangaId, const std::string& mangaTitle) {
        std::string canonicalKey = makeKey(sourceId, mangaId, mangaTitle);
        bool erased = false;
        auto it = progress_map_.find(canonicalKey);
        if (it != progress_map_.end()) {
            progress_map_.erase(it);
            erased = true;
        }
        if (!mangaTitle.empty()) {
            auto it2 = progress_map_.find(mangaTitle);
            if (it2 != progress_map_.end()) {
                progress_map_.erase(it2);
                erased = true;
            }
        }
        if (erased) {
            save();
        }
        return erased;
    }

    void clearAllHistory() {
        progress_map_.clear();
        save();
    }

    bool getHeroManga(MangaProgress& out) const {
        auto recent = getRecentHistory(1);
        if (!recent.empty()) {
            out = recent[0];
            return true;
        }
        return false;
    }
};

} // namespace kindle
