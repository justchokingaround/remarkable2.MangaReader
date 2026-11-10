#pragma once

#include "json.hpp"
#include "util.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace store {

namespace fs = std::filesystem;

inline std::string makeMangaKey(const std::string& sourceId, const std::string& mangaId) {
    return sourceId + "::" + mangaId;
}

inline std::string makeChapterKey(const std::string& sourceId,
                                  const std::string& mangaId,
                                  const std::string& chapterId) {
    return sourceId + "::" + mangaId + "::" + chapterId;
}

class StateStore {
    fs::path dataDir_;
    fs::path stateFile_;
    mutable std::mutex mu_;
    Json::Value root_;

    void ensureDefaultsLocked() {
        if (!root_.isObject()) root_ = Json::Value::object();
        if (!root_["settings"].isObject()) {
            Json::Value s = Json::Value::object();
            s["readingMode"] = "rtl";
            s["fitMode"] = "contain";
            s["invertSwipe"] = false;
            s["autoPreloadNext"] = true;
            s["ghostRefreshPages"] = 20;
            root_["settings"] = std::move(s);
        } else {
            if (!root_["settings"].has("invertSwipe")) {
                root_["settings"]["invertSwipe"] = false;
            }
        }
        if (!root_["mangas"].isObject()) root_["mangas"] = Json::Value::object();
        if (!root_["chapters_cache"].isObject()) root_["chapters_cache"] = Json::Value::object();
        if (!root_["downloads"].isObject()) root_["downloads"] = Json::Value::object();
    }

    void saveLocked() {
        util::atomicWriteFile(stateFile_, Json::stringify(root_));
    }

public:
    explicit StateStore(fs::path dataDir)
        : dataDir_(std::move(dataDir)), stateFile_(dataDir_ / "state.json") {
        std::error_code ec;
        fs::create_directories(dataDir_ / "covers", ec);
        fs::create_directories(dataDir_ / "chapters", ec);

        std::string raw;
        if (util::readFileToString(stateFile_, raw) && !raw.empty()) {
            bool ok = false;
            Json::Value parsed = Json::parse(raw, &ok);
            if (ok && parsed.isObject()) {
                root_ = std::move(parsed);
            }
        }
        ensureDefaultsLocked();
    }

    fs::path dataDir() const { return dataDir_; }
    fs::path coversDir() const { return dataDir_ / "covers"; }
    fs::path localChaptersDir() const { return dataDir_ / "chapters"; }

    Json::Value getSettings() const {
        std::lock_guard<std::mutex> lock(mu_);
        return root_["settings"];
    }

    Json::Value updateSettings(const Json::Value& patch) {
        std::lock_guard<std::mutex> lock(mu_);
        Json::Value& s = root_["settings"];
        if (patch.has("readingMode")) s["readingMode"] = patch["readingMode"].str("rtl");
        if (patch.has("fitMode")) s["fitMode"] = patch["fitMode"].str("contain");
        if (patch.has("invertSwipe")) s["invertSwipe"] = patch["invertSwipe"].boolean(false);
        if (patch.has("autoPreloadNext")) s["autoPreloadNext"] = patch["autoPreloadNext"].boolean(true);
        if (patch.has("ghostRefreshPages")) s["ghostRefreshPages"] = patch["ghostRefreshPages"].integer(20);
        saveLocked();
        return s;
    }

    void upsertMangaMeta(const std::string& sourceId,
                         const std::string& sourceName,
                         const std::string& mangaId,
                         const std::string& title,
                         const std::string& coverUrl = "",
                         const std::string& author = "",
                         const std::string& description = "",
                         const std::string& status = "",
                         int inLibraryTriState = -1,
                         int unreadCount = -1) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string key = makeMangaKey(sourceId, mangaId);
        Json::Value& m = root_["mangas"][key];
        if (!m.isObject()) m = Json::Value::object();

        m["key"] = key;
        m["source_id"] = sourceId;
        if (!sourceName.empty()) m["source_name"] = sourceName;
        else if (!m.has("source_name")) m["source_name"] = sourceId;
        m["manga_id"] = mangaId;
        if (!title.empty()) m["title"] = title;
        if (!coverUrl.empty()) m["cover_url"] = coverUrl;
        if (!author.empty()) m["author"] = author;
        if (!description.empty()) m["description"] = description;
        if (!status.empty()) m["status"] = status;
        if (inLibraryTriState >= 0) m["in_library"] = (inLibraryTriState > 0);
        if (unreadCount >= 0) m["unread_count"] = unreadCount;
        if (!m["read_chapters"].isObject()) m["read_chapters"] = Json::Value::object();
        if (!m["chapter_pages"].isObject()) m["chapter_pages"] = Json::Value::object();
        saveLocked();
    }

    void setCoverPath(const std::string& sourceId, const std::string& mangaId, const std::string& fileUri) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string key = makeMangaKey(sourceId, mangaId);
        Json::Value& m = root_["mangas"][key];
        if (!m.isObject()) m = Json::Value::object();
        m["source_id"] = sourceId;
        m["manga_id"] = mangaId;
        m["cover_path"] = fileUri;
        saveLocked();
    }

    Json::Value getManga(const std::string& sourceId, const std::string& mangaId) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::string key = makeMangaKey(sourceId, mangaId);
        return root_["mangas"][key];
    }

    // Syncs Aidoku /library list with local state while preserving cover_path and reading progress
    void syncAidokuLibraryList(const Json::Value& aidokuLibArray) {
        std::lock_guard<std::mutex> lock(mu_);
        // Only rakuyomi-managed mangas mirror the rakuyomi library; built-in MangaDex and
        // aidoku-run ("v2.") sources keep their library flag locally.
        for (auto& kv : root_["mangas"].asObjectMut()) {
            std::string sid = kv.second["source_id"].str();
            if (sid == "mangadex" || sid.rfind("v2.", 0) == 0) continue;
            kv.second["in_library"] = false;
        }
        if (aidokuLibArray.isArray()) {
            for (const auto& item : aidokuLibArray.asArray()) {
                std::string mangaId = item["id"].str();
                std::string sourceId = item["source"]["id"].str();
                std::string sourceName = item["source"]["name"].str(sourceId);
                std::string title = item["title"].str(mangaId);
                if (sourceId.empty() || mangaId.empty()) continue;

                std::string key = makeMangaKey(sourceId, mangaId);
                Json::Value& m = root_["mangas"][key];
                if (!m.isObject()) m = Json::Value::object();
                m["key"] = key;
                m["source_id"] = sourceId;
                m["source_name"] = sourceName;
                m["manga_id"] = mangaId;
                if (!title.empty()) m["title"] = title;
                m["in_library"] = true;
                if (item.has("unread_chapters_count")) {
                    m["unread_count"] = item["unread_chapters_count"].integer(0);
                }
                if (!m["read_chapters"].isObject()) m["read_chapters"] = Json::Value::object();
                if (!m["chapter_pages"].isObject()) m["chapter_pages"] = Json::Value::object();
            }
        }
        saveLocked();
    }

    // Merges remote chapters with local read_chapters, page bookmarks, and downloads
    Json::Value mergeAndCacheChapters(const std::string& sourceId,
                                      const std::string& sourceName,
                                      const std::string& mangaId,
                                      const std::string& mangaTitle,
                                      const Json::Value& rawChapters) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string mkey = makeMangaKey(sourceId, mangaId);
        Json::Value& m = root_["mangas"][mkey];
        if (!m.isObject()) {
            m = Json::Value::object();
            m["key"] = mkey;
            m["source_id"] = sourceId;
            m["source_name"] = sourceName.empty() ? sourceId : sourceName;
            m["manga_id"] = mangaId;
            m["title"] = mangaTitle.empty() ? mangaId : mangaTitle;
            m["read_chapters"] = Json::Value::object();
            m["chapter_pages"] = Json::Value::object();
        }

        Json::Value& readMap = m["read_chapters"];
        Json::Value& pageMap = m["chapter_pages"];
        Json::Value& dlMap = root_["downloads"];

        Json::Value merged = Json::Value::array();
        int unreadCount = 0;

        if (rawChapters.isArray()) {
            for (const auto& ch : rawChapters.asArray()) {
                std::string cid = ch["id"].str();
                if (cid.empty()) continue;

                Json::Value item = ch;
                item["source_id"] = sourceId;
                item["manga_id"] = mangaId;

                bool remoteRead = ch["read"].boolean(false);
                bool localRead = readMap[cid].boolean(false);
                bool isRead = remoteRead || localRead;
                if (remoteRead && !localRead) {
                    readMap[cid] = true;
                }
                item["read"] = isRead;
                if (!isRead) ++unreadCount;

                std::string ckey = makeChapterKey(sourceId, mangaId, cid);
                bool remoteDl = ch["downloaded"].boolean(false);
                bool localDl = dlMap.has(ckey);
                item["downloaded"] = remoteDl || localDl;

                if (remoteDl && !localDl) {
                    // Register existing Aidoku-downloaded chapter so Downloads tab shows it immediately
                    Json::Value d = Json::Value::object();
                    d["key"] = ckey;
                    d["source_id"] = sourceId;
                    d["source_name"] = m["source_name"].str(sourceId);
                    d["manga_id"] = mangaId;
                    d["manga_title"] = m["title"].str(mangaId);
                    d["chapter_id"] = cid;
                    if (ch.has("chapter_num")) d["chapter_num"] = ch["chapter_num"].num();
                    d["chapter_title"] = ch["title"].str();
                    d["aidoku_managed"] = true;
                    d["downloaded_at"] = util::nowSeconds();
                    dlMap[ckey] = std::move(d);
                }

                if (pageMap[cid].isObject()) {
                    item["saved_page"] = pageMap[cid]["page"].integer(0);
                    item["page_count"] = pageMap[cid]["total"].integer(0);
                } else {
                    item["saved_page"] = 0;
                    item["page_count"] = 0;
                }
                merged.push(std::move(item));
            }
        }

        m["unread_count"] = unreadCount;
        root_["chapters_cache"][mkey] = merged;
        saveLocked();
        return merged;
    }

    Json::Value getCachedChapters(const std::string& sourceId, const std::string& mangaId) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::string mkey = makeMangaKey(sourceId, mangaId);
        const Json::Value& cached = root_["chapters_cache"][mkey];
        if (!cached.isArray()) return Json::Value::array();

        const Json::Value& m = root_["mangas"][mkey];
        const Json::Value& readMap = m["read_chapters"];
        const Json::Value& pageMap = m["chapter_pages"];
        const Json::Value& dlMap = root_["downloads"];

        Json::Value out = Json::Value::array();
        for (const auto& ch : cached.asArray()) {
            Json::Value item = ch;
            std::string cid = ch["id"].str();
            std::string ckey = makeChapterKey(sourceId, mangaId, cid);
            if (readMap[cid].boolean(false)) item["read"] = true;
            if (dlMap.has(ckey)) item["downloaded"] = true;
            if (pageMap[cid].isObject()) {
                item["saved_page"] = pageMap[cid]["page"].integer(0);
                item["page_count"] = pageMap[cid]["total"].integer(0);
            }
            out.push(std::move(item));
        }
        return out;
    }

    void saveProgress(const std::string& sourceId,
                      const std::string& sourceName,
                      const std::string& mangaId,
                      const std::string& mangaTitle,
                      const std::string& chapterId,
                      double chapterNum,
                      bool hasChapterNum,
                      const std::string& chapterTitle,
                      int pageIndex,
                      int pageCount,
                      bool completed) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string mkey = makeMangaKey(sourceId, mangaId);
        Json::Value& m = root_["mangas"][mkey];
        if (!m.isObject()) m = Json::Value::object();

        m["key"] = mkey;
        m["source_id"] = sourceId;
        if (!sourceName.empty()) m["source_name"] = sourceName;
        m["manga_id"] = mangaId;
        if (!mangaTitle.empty()) m["title"] = mangaTitle;

        m["last_chapter_id"] = chapterId;
        if (hasChapterNum) m["last_chapter_num"] = chapterNum;
        m["last_chapter_title"] = chapterTitle;
        m["last_page_index"] = pageIndex;
        m["last_page_count"] = pageCount;
        m["last_read_at"] = util::nowSeconds();

        if (!m["read_chapters"].isObject()) m["read_chapters"] = Json::Value::object();
        if (!m["chapter_pages"].isObject()) m["chapter_pages"] = Json::Value::object();

        Json::Value pg = Json::Value::object();
        pg["page"] = completed ? 0 : pageIndex;
        pg["total"] = pageCount;
        m["chapter_pages"][chapterId] = std::move(pg);

        if (completed) {
            m["read_chapters"][chapterId] = true;
            // Recompute unread_count from cached chapters if present
            const Json::Value& cached = root_["chapters_cache"][mkey];
            if (cached.isArray()) {
                int unread = 0;
                for (const auto& c : cached.asArray()) {
                    std::string cid = c["id"].str();
                    if (!m["read_chapters"][cid].boolean(c["read"].boolean(false))) {
                        ++unread;
                    }
                }
                m["unread_count"] = unread;
            }
        }
        saveLocked();
    }

    void markChapterRead(const std::string& sourceId,
                         const std::string& mangaId,
                         const std::string& chapterId,
                         bool read) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string mkey = makeMangaKey(sourceId, mangaId);
        Json::Value& m = root_["mangas"][mkey];
        if (!m.isObject()) m = Json::Value::object();
        if (!m["read_chapters"].isObject()) m["read_chapters"] = Json::Value::object();

        if (read) {
            m["read_chapters"][chapterId] = true;
        } else {
            m["read_chapters"].erase(chapterId);
        }

        Json::Value& cached = root_["chapters_cache"][mkey];
        if (cached.isArray()) {
            int unread = 0;
            for (auto& c : cached.asArrayMut()) {
                if (c["id"].str() == chapterId) c["read"] = read;
                std::string cid = c["id"].str();
                if (!m["read_chapters"][cid].boolean(c["read"].boolean(false))) {
                    ++unread;
                }
            }
            m["unread_count"] = unread;
        }
        saveLocked();
    }

    void recordDownload(const std::string& sourceId,
                        const std::string& sourceName,
                        const std::string& mangaId,
                        const std::string& mangaTitle,
                        const std::string& chapterId,
                        double chapterNum,
                        bool hasChapterNum,
                        const std::string& chapterTitle,
                        const std::string& storagePath,
                        bool isFolder) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string ckey = makeChapterKey(sourceId, mangaId, chapterId);
        Json::Value d = Json::Value::object();
        d["key"] = ckey;
        d["source_id"] = sourceId;
        d["source_name"] = sourceName.empty() ? sourceId : sourceName;
        d["manga_id"] = mangaId;
        d["manga_title"] = mangaTitle.empty() ? mangaId : mangaTitle;
        d["chapter_id"] = chapterId;
        if (hasChapterNum) d["chapter_num"] = chapterNum;
        d["chapter_title"] = chapterTitle;
        d["storage_path"] = storagePath;
        d["is_folder"] = isFolder;
        d["downloaded_at"] = util::nowSeconds();
        root_["downloads"][ckey] = std::move(d);

        // Also mark downloaded in chapters_cache if cached
        std::string mkey = makeMangaKey(sourceId, mangaId);
        Json::Value& cached = root_["chapters_cache"][mkey];
        if (cached.isArray()) {
            for (auto& c : cached.asArrayMut()) {
                if (c["id"].str() == chapterId) c["downloaded"] = true;
            }
        }
        saveLocked();
    }

    Json::Value getDownloadRecord(const std::string& sourceId,
                                  const std::string& mangaId,
                                  const std::string& chapterId) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::string ckey = makeChapterKey(sourceId, mangaId, chapterId);
        return root_["downloads"][ckey];
    }

    void removeDownloadRecord(const std::string& sourceId,
                              const std::string& mangaId,
                              const std::string& chapterId) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string ckey = makeChapterKey(sourceId, mangaId, chapterId);
        const Json::Value& d = root_["downloads"][ckey];
        if (d.isObject()) {
            std::string sp = d["storage_path"].str();
            if (!sp.empty()) {
                std::error_code ec;
                fs::remove_all(sp, ec);
            }
        }
        root_["downloads"].erase(ckey);

        std::string mkey = makeMangaKey(sourceId, mangaId);
        Json::Value& cached = root_["chapters_cache"][mkey];
        if (cached.isArray()) {
            for (auto& c : cached.asArrayMut()) {
                if (c["id"].str() == chapterId) c["downloaded"] = false;
            }
        }
        saveLocked();
    }

    Json::Value listLibrary() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Json::Value> items;
        for (const auto& kv : root_["mangas"].asObject()) {
            if (kv.second["in_library"].boolean(false)) {
                Json::Value copy = kv.second;
                // Count downloaded chapters for this manga
                int dlCount = 0;
                for (const auto& dkv : root_["downloads"].asObject()) {
                    if (dkv.second["source_id"].str() == copy["source_id"].str() &&
                        dkv.second["manga_id"].str() == copy["manga_id"].str()) {
                        ++dlCount;
                    }
                }
                copy["downloaded_count"] = dlCount;
                items.push_back(std::move(copy));
            }
        }
        std::sort(items.begin(), items.end(), [](const Json::Value& a, const Json::Value& b) {
            int64_t ta = a["last_read_at"].integer(0);
            int64_t tb = b["last_read_at"].integer(0);
            if (ta != tb) return ta > tb;
            return util::naturalLess(a["title"].str(), b["title"].str());
        });

        Json::Value arr = Json::Value::array();
        for (auto& it : items) arr.push(std::move(it));
        return arr;
    }

    Json::Value listRecent() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Json::Value> items;
        for (const auto& kv : root_["mangas"].asObject()) {
            if (kv.second["last_read_at"].integer(0) > 0 &&
                !kv.second["last_chapter_id"].str().empty()) {
                items.push_back(kv.second);
            }
        }
        std::sort(items.begin(), items.end(), [](const Json::Value& a, const Json::Value& b) {
            return a["last_read_at"].integer(0) > b["last_read_at"].integer(0);
        });

        Json::Value arr = Json::Value::array();
        for (size_t i = 0; i < std::min<size_t>(items.size(), 50); ++i) {
            arr.push(std::move(items[i]));
        }
        return arr;
    }

    Json::Value getContinueReading() const {
        Json::Value recent = listRecent();
        if (!recent.empty()) {
            return recent[0];
        }
        // Fallback on very first launch: suggest first library item that has chapters cached
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& kv : root_["mangas"].asObject()) {
            if (!kv.second["in_library"].boolean(false)) continue;
            const Json::Value& chs = root_["chapters_cache"][kv.first];
            if (chs.isArray() && !chs.empty()) {
                // Pick first unread in ascending order, or first chapter
                std::vector<Json::Value> sorted = chs.asArray();
                std::sort(sorted.begin(), sorted.end(), [](const Json::Value& a, const Json::Value& b) {
                    return a["chapter_num"].num(0) < b["chapter_num"].num(0);
                });
                const Json::Value* chosen = &sorted[0];
                for (const auto& c : sorted) {
                    if (!c["read"].boolean(false)) {
                        chosen = &c;
                        break;
                    }
                }
                Json::Value hero = kv.second;
                hero["last_chapter_id"] = (*chosen)["id"].str();
                if ((*chosen).has("chapter_num")) hero["last_chapter_num"] = (*chosen)["chapter_num"].num();
                hero["last_chapter_title"] = (*chosen)["title"].str();
                hero["last_page_index"] = 0;
                hero["last_page_count"] = 0;
                return hero;
            }
        }
        return Json::Value();
    }

    Json::Value listDownloads() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Json::Value> items;
        for (const auto& kv : root_["downloads"].asObject()) {
            items.push_back(kv.second);
        }
        std::sort(items.begin(), items.end(), [](const Json::Value& a, const Json::Value& b) {
            int64_t ta = a["downloaded_at"].integer(0);
            int64_t tb = b["downloaded_at"].integer(0);
            if (ta != tb) return ta > tb;
            if (a["manga_title"].str() != b["manga_title"].str()) {
                return util::naturalLess(a["manga_title"].str(), b["manga_title"].str());
            }
            return a["chapter_num"].num(0) < b["chapter_num"].num(0);
        });
        Json::Value arr = Json::Value::array();
        for (auto& it : items) arr.push(std::move(it));
        return arr;
    }

    std::vector<Json::Value> listMangasMissingCovers() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Json::Value> out;
        for (const auto& kv : root_["mangas"].asObject()) {
            if (!kv.second["in_library"].boolean(false)) continue;
            std::string cp = kv.second["cover_path"].str();
            if (cp.empty()) {
                out.push_back(kv.second);
            }
        }
        return out;
    }
};

}  // namespace store
