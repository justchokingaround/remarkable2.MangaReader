#pragma once

#include "json.hpp"
#include "util.hpp"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace mangadex {

namespace fs = std::filesystem;

inline std::string pickLocalized(const Json::Value& dict, const Json::Value& altTitles = Json::Value()) {
    if (dict.isString()) return dict.str();
    if (dict.isObject()) {
        if (dict.has("en") && !dict["en"].str().empty()) return dict["en"].str();
        // Check altTitles for English title if primary is romanized Japanese
        if (altTitles.isArray()) {
            for (const auto& alt : altTitles.asArray()) {
                if (alt.isObject() && alt.has("en") && !alt["en"].str().empty()) {
                    return alt["en"].str();
                }
            }
        }
        if (dict.has("ja-ro") && !dict["ja-ro"].str().empty()) return dict["ja-ro"].str();
        if (dict.has("ko-ro") && !dict["ko-ro"].str().empty()) return dict["ko-ro"].str();
        for (const auto& kv : dict.asObject()) {
            if (!kv.second.str().empty()) return kv.second.str();
        }
    }
    return "";
}

class Client {
    std::string apiBase_ = "https://api.mangadex.org";
    mutable std::mutex langMu_;
    std::vector<std::string> languages_ = {"en"};

    std::string langParams(const std::string& key) const {
        std::lock_guard<std::mutex> lock(langMu_);
        std::string out;
        for (const auto& l : languages_) out += "&" + key + "[]=" + util::urlEncode(l);
        return out;
    }

public:
    // Language codes from settings.json "languages" (e.g. ["en","ru"]). "multi" is ignored.
    void setLanguages(const std::vector<std::string>& langs) {
        std::vector<std::string> clean;
        for (const auto& l : langs) {
            if (l.empty() || l == "multi") continue;
            if (std::find(clean.begin(), clean.end(), l) == clean.end()) clean.push_back(l);
        }
        if (clean.empty()) clean.push_back("en");
        std::lock_guard<std::mutex> lock(langMu_);
        languages_ = std::move(clean);
    }

    size_t languageCount() const {
        std::lock_guard<std::mutex> lock(langMu_);
        return languages_.size();
    }

    Client() {
        if (const char* envBase = std::getenv("RM2MANGA_MANGADEX_API")) {
            apiBase_ = envBase;
        }
    }

    static Json::Value parseMangaObject(const Json::Value& dataObj) {
        Json::Value out = Json::Value::object();
        std::string id = dataObj["id"].str();
        const Json::Value& attrs = dataObj["attributes"];

        out["id"] = id;
        Json::Value src = Json::Value::object();
        src["id"] = "mangadex";
        src["name"] = "MangaDex";
        src["version"] = 5;
        out["source"] = src;
        out["source_id"] = "mangadex";
        out["source_name"] = "MangaDex";

        std::string title = pickLocalized(attrs["title"], attrs["altTitles"]);
        out["title"] = title.empty() ? id : title;
        out["description"] = pickLocalized(attrs["description"]);
        out["status"] = attrs["status"].str();
        out["year"] = attrs["year"].integer(0);

        std::string author;
        std::string coverFile;
        if (dataObj["relationships"].isArray()) {
            for (const auto& rel : dataObj["relationships"].asArray()) {
                std::string rtype = rel["type"].str();
                if (rtype == "cover_art" && coverFile.empty()) {
                    coverFile = rel["attributes"]["fileName"].str();
                } else if ((rtype == "author" || rtype == "artist") && author.empty()) {
                    author = rel["attributes"]["name"].str();
                }
            }
        }
        out["author"] = author;
        if (!coverFile.empty() && !id.empty()) {
            out["cover_url"] = "https://uploads.mangadex.org/covers/" + id + "/" + coverFile + ".256.jpg";
        } else {
            out["cover_url"] = "";
        }
        return out;
    }

    Json::Value queryMangaList(const std::string& query, const std::string& mode, std::string* errOut = nullptr) {
        std::string url = apiBase_ + "/manga?limit=24"
                          "&includes[]=cover_art&includes[]=author"
                          "&contentRating[]=safe&contentRating[]=suggestive&contentRating[]=erotica"
                          "&hasAvailableChapters=true" +
                          langParams("availableTranslatedLanguage");
        if (mode == "latest") {
            url += "&order[latestUploadedChapter]=desc";
        } else if (mode == "popular" || query.empty()) {
            url += "&order[followedCount]=desc";
        } else {
            url += "&title=" + util::urlEncode(query) + "&order[relevance]=desc";
        }

        std::string body;
        if (!util::httpsGetString(url, body, 25)) {
            if (errOut) *errOut = "Failed to reach MangaDex API (check Wi-Fi)";
            return Json::Value::array();
        }
        bool ok = false;
        Json::Value root = Json::parse(body, &ok);
        if (!ok || !root["data"].isArray()) {
            if (errOut) *errOut = "Unexpected response from MangaDex";
            return Json::Value::array();
        }

        Json::Value list = Json::Value::array();
        for (const auto& item : root["data"].asArray()) {
            list.push(parseMangaObject(item));
        }
        return list;
    }

    Json::Value getMangaDetails(const std::string& mangaId, std::string* errOut = nullptr) {
        std::string url = apiBase_ + "/manga/" + util::urlEncode(mangaId) +
                          "?includes[]=cover_art&includes[]=author";
        std::string body;
        if (!util::httpsGetString(url, body, 20)) {
            if (errOut) *errOut = "Failed to fetch manga details from MangaDex";
            return Json::Value();
        }
        bool ok = false;
        Json::Value root = Json::parse(body, &ok);
        if (!ok || !root["data"].isObject()) {
            if (errOut) *errOut = "Invalid MangaDex manga metadata";
            return Json::Value();
        }
        return parseMangaObject(root["data"]);
    }

    // MangaDex stores tracker ids in attributes.links ("al" = AniList). 0 if unknown/offline.
    int64_t getAniListId(const std::string& mangaId) {
        std::string body;
        if (!util::httpsGetString(apiBase_ + "/manga/" + util::urlEncode(mangaId), body, 20)) return 0;
        bool ok = false;
        Json::Value root = Json::parse(body, &ok);
        if (!ok) return 0;
        return root["data"]["attributes"]["links"]["al"].integer(0);
    }

    Json::Value getChapters(const std::string& mangaId, std::string* errOut = nullptr) {
        Json::Value chapters = Json::Value::array();
        const bool multiLang = languageCount() > 1;
        int offset = 0;
        const int limit = 500;

        for (int page = 0; page < 6; ++page) {  // up to 3,000 chapters
            std::string url = apiBase_ + "/manga/" + util::urlEncode(mangaId) +
                              "/feed?limit=" + std::to_string(limit) +
                              langParams("translatedLanguage") +
                              "&offset=" + std::to_string(offset) +
                              "&order[chapter]=asc&order[volume]=asc"
                              "&includes[]=scanlation_group"
                              "&contentRating[]=safe&contentRating[]=suggestive&contentRating[]=erotica";
            std::string body;
            if (!util::httpsGetString(url, body, 30)) {
                if (page == 0 && errOut) *errOut = "Failed to fetch chapter feed from MangaDex";
                break;
            }
            bool ok = false;
            Json::Value root = Json::parse(body, &ok);
            if (!ok || !root["data"].isArray()) {
                if (page == 0 && errOut) *errOut = "Invalid chapter feed JSON";
                break;
            }

            const auto& arr = root["data"].asArray();
            for (const auto& ch : arr) {
                const Json::Value& attrs = ch["attributes"];
                // Skip external-only chapters (e.g., MangaPlus links with 0 hosted pages)
                if (!attrs["externalUrl"].isNull() && !attrs["externalUrl"].str().empty() &&
                    attrs["pages"].integer(0) <= 0) {
                    continue;
                }

                Json::Value item = Json::Value::object();
                item["id"] = ch["id"].str();
                item["source_id"] = "mangadex";
                item["manga_id"] = mangaId;

                std::string chStr = attrs["chapter"].str();
                if (!chStr.empty()) {
                    item["chapter_num"] = std::strtod(chStr.c_str(), nullptr);
                }
                std::string volStr = attrs["volume"].str();
                if (!volStr.empty()) {
                    item["volume_num"] = std::strtod(volStr.c_str(), nullptr);
                }
                item["title"] = attrs["title"].str();

                std::string groupName;
                if (ch["relationships"].isArray()) {
                    for (const auto& rel : ch["relationships"].asArray()) {
                        if (rel["type"].str() == "scanlation_group") {
                            groupName = rel["attributes"]["name"].str();
                            if (!groupName.empty()) break;
                        }
                    }
                }
                std::string lang = attrs["translatedLanguage"].str();
                item["language"] = lang;
                if (multiLang && !lang.empty()) {
                    std::string up;
                    for (char c : lang) up += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    groupName = "[" + up + "]" + (groupName.empty() ? "" : " " + groupName);
                }
                item["scanlator"] = groupName;
                item["read"] = false;
                item["downloaded"] = false;
                chapters.push(std::move(item));
            }

            int total = static_cast<int>(root["total"].integer(0));
            offset += static_cast<int>(arr.size());
            if (arr.empty() || offset >= total) break;
        }
        return chapters;
    }

    bool downloadChapterPages(const std::string& chapterId,
                              const fs::path& destDir,
                              const std::atomic<bool>* cancelFlag,
                              const std::function<void(const std::string&, int)>& onProgress,
                              std::string* errOut = nullptr) {
        std::string url = apiBase_ + "/at-home/server/" + util::urlEncode(chapterId);
        std::string body;
        if (!util::httpsGetString(url, body, 25)) {
            if (errOut) *errOut = "Failed to contact MangaDex At-Home server";
            return false;
        }
        bool ok = false;
        Json::Value root = Json::parse(body, &ok);
        if (!ok || root["result"].str() != "ok") {
            if (errOut) *errOut = "MangaDex At-Home returned an error";
            return false;
        }

        std::string baseUrl = root["baseUrl"].str();
        const Json::Value& chap = root["chapter"];
        std::string hash = chap["hash"].str();
        const auto& dataArr = chap["data"].asArray();
        const auto& saverArr = chap["dataSaver"].asArray();

        if (baseUrl.empty() || hash.empty() || dataArr.empty()) {
            if (errOut) *errOut = "No readable pages found for this chapter";
            return false;
        }

        std::error_code ec;
        fs::path tmpDir = destDir;
        tmpDir += ".downloading";
        fs::remove_all(tmpDir, ec);
        fs::create_directories(tmpDir, ec);

        size_t total = dataArr.size();
        for (size_t i = 0; i < total; ++i) {
            if (cancelFlag && cancelFlag->load()) {
                fs::remove_all(tmpDir, ec);
                if (errOut) *errOut = "Download cancelled";
                return false;
            }

            int pct = static_cast<int>(((i + 1) * 100) / total);
            if (onProgress) {
                onProgress("Downloading page " + std::to_string(i + 1) + " of " + std::to_string(total) + "...", pct);
            }

            std::string fileName = dataArr[i].str();
            std::string ext = ".jpg";
            auto dot = fileName.find_last_of('.');
            if (dot != std::string::npos) {
                ext = util::toLower(fileName.substr(dot));
            }

            char idxBuf[32];
            std::snprintf(idxBuf, sizeof(idxBuf), "%04zu%s", i + 1, ext.c_str());
            fs::path pagePath = tmpDir / idxBuf;

            std::string pageUrl = baseUrl + "/data/" + hash + "/" + fileName;
            bool pageOk = util::httpsDownloadFile(pageUrl, pagePath, 40);

            if (!pageOk && i < saverArr.size()) {
                std::string saverName = saverArr[i].str();
                std::string saverExt = ".jpg";
                auto sdot = saverName.find_last_of('.');
                if (sdot != std::string::npos) saverExt = util::toLower(saverName.substr(sdot));
                std::snprintf(idxBuf, sizeof(idxBuf), "%04zu%s", i + 1, saverExt.c_str());
                pagePath = tmpDir / idxBuf;
                std::string saverUrl = baseUrl + "/data-saver/" + hash + "/" + saverName;
                pageOk = util::httpsDownloadFile(saverUrl, pagePath, 40);
            }

            if (!pageOk) {
                fs::remove_all(tmpDir, ec);
                if (errOut) *errOut = "Failed downloading page " + std::to_string(i + 1);
                return false;
            }
        }

        fs::remove_all(destDir, ec);
        fs::create_directories(destDir.parent_path(), ec);
        fs::rename(tmpDir, destDir, ec);
        if (ec) {
            if (errOut) *errOut = "Failed finalizing chapter folder";
            return false;
        }
        return true;
    }

    // Lookup and cache 256px cover art for Aidoku mangas that don't expose cover_url
    bool fetchCoverByTitle(const std::string& title, const fs::path& destCoverPath) {
        if (title.empty()) return false;
        std::string url = apiBase_ + "/manga?limit=1&includes[]=cover_art"
                          "&contentRating[]=safe&contentRating[]=suggestive&contentRating[]=erotica"
                          "&order[relevance]=desc&title=" + util::urlEncode(title);
        std::string body;
        if (!util::httpsGetString(url, body, 15)) return false;

        bool ok = false;
        Json::Value root = Json::parse(body, &ok);
        if (!ok || !root["data"].isArray() || root["data"].empty()) return false;

        Json::Value parsed = parseMangaObject(root["data"][0]);
        std::string coverUrl = parsed["cover_url"].str();
        if (coverUrl.empty()) return false;

        return util::httpsDownloadFile(coverUrl, destCoverPath, 20);
    }
};

}  // namespace mangadex
