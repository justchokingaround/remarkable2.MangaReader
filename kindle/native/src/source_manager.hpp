#pragma once

#include "types.hpp"
#include "../../../backend/src/mangadex_client.hpp"
#include "../../../backend/src/json.hpp"
#include "../../../backend/src/util.hpp"
#include <string>
#include <vector>
#include <regex>
#include <set>

namespace kindle {

class SourceManager {
private:
    mangadex::Client mangadex_client_;
    std::string active_source_id_ = "mangadex";
    std::vector<SourceInfo> available_sources_;

public:
    SourceManager() {
        available_sources_.push_back({"mangadex", "MangaDex", "Official API (Multi-language)"});
        available_sources_.push_back({"mangafox", "MangaFox", "FanFox & MangaHere (English)"});
        available_sources_.push_back({"mangakatana", "MangaKatana", "Fast Scans (English)"});
    }

    const std::vector<SourceInfo>& getSources() const { return available_sources_; }
    const std::string& getActiveSourceId() const { return active_source_id_; }
    void setActiveSourceId(const std::string& id) { active_source_id_ = id; }

    std::vector<UnifiedManga> getPopular(std::string* errOut = nullptr) {
        if (active_source_id_ == "mangafox") {
            return fetchMangaFoxPopular(errOut);
        }
        if (active_source_id_ == "mangakatana") {
            return fetchMangaKatanaPopular(errOut);
        }
        // Default to MangaDex
        return fetchMangaDexPopular(errOut);
    }

    std::vector<UnifiedManga> search(const std::string& query, std::string* errOut = nullptr) {
        if (active_source_id_ == "mangafox") {
            return searchMangaFox(query, errOut);
        }
        if (active_source_id_ == "mangakatana") {
            return searchMangaKatana(query, errOut);
        }
        return searchMangaDex(query, errOut);
    }

    std::vector<UnifiedChapter> getChapters(const std::string& mangaId, std::string* errOut = nullptr) {
        if (active_source_id_ == "mangafox") {
            return fetchMangaFoxChapters(mangaId, errOut);
        }
        if (active_source_id_ == "mangakatana") {
            return fetchMangaKatanaChapters(mangaId, errOut);
        }
        return fetchMangaDexChapters(mangaId, errOut);
    }

    mangadex::Client& mangadexClient() { return mangadex_client_; }

    std::vector<std::string> getChapterPageUrls(const UnifiedChapter& ch, std::string* errOut = nullptr) {
        if (ch.source_id == "mangafox") {
            return fetchMangaFoxPageUrls(ch.url.empty() ? ch.id : ch.url, errOut);
        }
        if (ch.source_id == "mangakatana") {
            return fetchMangaKatanaPageUrls(ch.url.empty() ? ch.id : ch.url, errOut);
        }
        return {};
    }

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

    std::vector<UnifiedManga> fetchMangaFoxPopular(std::string* errOut) {
        std::string html;
        if (!util::httpsGetString("https://fanfox.net/manga/", html, 25) &&
            !util::httpsGetString("https://www.mangahere.cc/directory/", html, 25)) {
            if (errOut) *errOut = "Failed connecting to MangaFox";
            return {};
        }
        return parseFoxMangaList(html);
    }

    std::vector<UnifiedManga> searchMangaFox(const std::string& query, std::string* errOut) {
        std::string html;
        std::string url1 = "https://fanfox.net/search?title=" + util::urlEncode(query);
        std::string url2 = "https://www.mangahere.cc/search?title=" + util::urlEncode(query);
        if (!util::httpsGetString(url1, html, 25) &&
            !util::httpsGetString(url2, html, 25)) {
            if (errOut) *errOut = "Search failed on MangaFox";
            return {};
        }
        return parseFoxMangaList(html);
    }

    std::vector<UnifiedManga> parseFoxMangaList(const std::string& html) {
        std::vector<UnifiedManga> list;
        std::regex itemRegex(R"raw(<a[^>]*href="(/manga/[^"]+/)"[^>]*title="([^"]+)">[\s\S]*?<img[^>]*src="([^"]+)")raw");
        auto words_begin = std::sregex_iterator(html.begin(), html.end(), itemRegex);
        auto words_end = std::sregex_iterator();

        for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
            std::smatch match = *i;
            UnifiedManga m;
            m.id = match[1].str();
            m.title = match[2].str();
            std::string img = match[3].str();
            if (img.rfind("//", 0) == 0) img = "https:" + img;
            m.cover_url = img;
            m.source_id = "mangafox";
            m.status = "MangaFox";
            list.push_back(m);
        }
        return list;
    }

    std::vector<UnifiedChapter> fetchMangaFoxChapters(const std::string& mangaId, std::string* errOut) {
        std::vector<UnifiedChapter> list;
        std::string slug = mangaId;
        if (slug.rfind("/manga/", 0) == 0) {
            slug = slug.substr(7);
        }
        while (!slug.empty() && slug.back() == '/') slug.pop_back();

        std::string html;
        std::string url1 = "https://fanfox.net/manga/" + slug + "/";
        std::string url2 = "https://www.mangahere.cc/manga/" + slug + "/";
        if (!util::httpsGetString(url1, html, 25) && !util::httpsGetString(url2, html, 25)) {
            if (errOut) *errOut = "Failed loading chapters from MangaFox";
            return {};
        }

        std::regex chRegex(R"raw(<a[^>]*href="(/manga/[^/]+/(?:v[0-9]+/)?c([0-9.]+)/1\.html)"[^>]*title="([^"]+)")raw");
        auto words_begin = std::sregex_iterator(html.begin(), html.end(), chRegex);
        auto words_end = std::sregex_iterator();

        std::set<std::string> seenUrls;
        for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
            std::smatch match = *i;
            std::string link = match[1].str();
            if (!slug.empty() && link.find(slug) == std::string::npos) continue;
            if (seenUrls.find(link) != seenUrls.end()) continue;
            seenUrls.insert(link);

            UnifiedChapter ch;
            ch.url = link;
            ch.id = link;
            ch.chapterNum = match[2].str();
            ch.title = match[3].str();
            ch.source_id = "mangafox";
            list.push_back(ch);
        }
        return list;
    }

    static std::string unpackDeanEdwards(const std::string& script) {
        size_t start = script.find("}('");
        if (start == std::string::npos) return "";
        start += 3;

        std::string tail = script.substr(start);
        std::regex paramRegex(R"raw(,(\d+),(\d+),'([^']+)'\.split\('\|'\))raw");
        std::smatch match;
        if (!std::regex_search(tail, match, paramRegex)) {
            return "";
        }

        std::string templateStr = tail.substr(0, match.position(0));
        std::string wordsStr = match[3].str();

        std::vector<std::string> dict;
        std::stringstream ss(wordsStr);
        std::string item;
        while (std::getline(ss, item, '|')) {
            dict.push_back(item);
        }

        auto decodeBase62 = [](const std::string& token) -> int {
            int val = 0;
            for (char c : token) {
                val *= 62;
                if (c >= '0' && c <= '9') val += (c - '0');
                else if (c >= 'a' && c <= 'z') val += (c - 'a' + 10);
                else if (c >= 'A' && c <= 'Z') val += (c - 'A' + 36);
                else return -1;
            }
            return val;
        };

        std::string result;
        result.reserve(templateStr.size() * 3);
        size_t i = 0;
        while (i < templateStr.size()) {
            if (std::isalnum(static_cast<unsigned char>(templateStr[i]))) {
                size_t tokenStart = i;
                while (i < templateStr.size() && std::isalnum(static_cast<unsigned char>(templateStr[i]))) {
                    i++;
                }
                std::string token = templateStr.substr(tokenStart, i - tokenStart);
                int idx = decodeBase62(token);
                if (idx >= 0 && idx < static_cast<int>(dict.size()) && !dict[idx].empty()) {
                    result += dict[idx];
                } else {
                    result += token;
                }
            } else {
                result += templateStr[i++];
            }
        }
        return result;
    }

    std::vector<std::string> fetchMangaFoxPageUrls(const std::string& chapterUrl, std::string* errOut) {
        std::string html;
        std::string fullUrl = chapterUrl;
        if (fullUrl.rfind("http", 0) != 0) {
            fullUrl = "https://fanfox.net" + chapterUrl;
        }

        std::string primaryUrl = fullUrl;
        std::string primaryError;
        if (!util::httpsGetString(primaryUrl, html, 25, &primaryError)) {
            std::string fallbackUrl = primaryUrl;
            if (primaryUrl.find("fanfox.net") != std::string::npos) {
                size_t pos = fallbackUrl.find("fanfox.net");
                fallbackUrl.replace(pos, 10, "www.mangahere.cc");
            } else if (primaryUrl.find("mangahere.cc") != std::string::npos) {
                size_t pos = fallbackUrl.find("www.mangahere.cc");
                if (pos != std::string::npos) {
                    fallbackUrl.replace(pos, 16, "fanfox.net");
                } else {
                    pos = fallbackUrl.find("mangahere.cc");
                    if (pos != std::string::npos) fallbackUrl.replace(pos, 12, "fanfox.net");
                }
            }
            std::string fallbackError;
            if (util::httpsGetString(fallbackUrl, html, 25, &fallbackError)) {
                fullUrl = fallbackUrl;
            } else if (errOut) {
                *errOut = "Could not load MangaFox chapter HTML (" +
                          (primaryError.empty() ? "request failed" : primaryError) +
                          "; fallback: " +
                          (fallbackError.empty() ? "request failed" : fallbackError) + ")";
            }
        }

        if (html.empty()) {
            if (errOut && errOut->empty()) {
                *errOut = "Could not load MangaFox chapter HTML (" + primaryUrl + ")";
            }
            return {};
        }

        size_t evalPos = html.find("eval(function(p,a,c,k,e,d)");
        if (evalPos == std::string::npos) {
            evalPos = html.find("eval(function(p,a,c,k,e,r)");
        }
        if (evalPos == std::string::npos) {
            if (errOut) *errOut = "MangaFox chapter script not found";
            return {};
        }

        size_t evalEnd = html.find("</script>", evalPos);
        std::string script = html.substr(evalPos, (evalEnd != std::string::npos) ? (evalEnd - evalPos) : 2000);
        std::string unpacked = unpackDeanEdwards(script);

        std::vector<std::string> pages;

        // 1. Try extracting direct image URLs (matching images with optional tokens/queries)
        std::regex urlRegex(R"raw((?:https?:)?//[^\s'",;]+\.(?:jpg|jpeg|png|webp)(?:\?[^\s'",;]*)?)raw");
        auto begin = std::sregex_iterator(unpacked.begin(), unpacked.end(), urlRegex);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string u = it->str();
            if (u.rfind("//", 0) == 0) u = "https:" + u;
            if (pages.empty() || pages.back() != u) {
                pages.push_back(u);
            }
        }

        // 2. If no direct page URLs in the script, check for DM5 chapterfun.ashx mechanism
        if (pages.empty()) {
            std::smatch cidMatch, countMatch, guidMatch;
            std::regex cidRegex(R"raw(var\s+chapterid\s*=\s*(\d+);)raw");
            std::regex countRegex(R"raw(var\s+imagecount\s*=\s*(\d+);)raw");
            std::regex guidRegex(R"raw(guidkey\s*=\s*([^;]+);)raw");

            std::string cidStr, guidStr;
            int totalPages = 0;

            if (std::regex_search(html, cidMatch, cidRegex)) {
                cidStr = cidMatch[1].str();
            }
            if (std::regex_search(html, countMatch, countRegex)) {
                totalPages = std::atoi(countMatch[1].str().c_str());
            }
            if (std::regex_search(unpacked, guidMatch, guidRegex)) {
                std::string rawGuid = guidMatch[1].str();
                for (char c : rawGuid) {
                    if (std::isalnum(static_cast<unsigned char>(c))) {
                        guidStr += c;
                    }
                }
            }

            if (!cidStr.empty() && totalPages > 0) {
                std::string baseDir = fullUrl;
                size_t lastSlash = baseDir.rfind('/');
                if (lastSlash != std::string::npos) {
                    baseDir = baseDir.substr(0, lastSlash + 1);
                } else {
                    baseDir += "/";
                }

                // Batch fetch chapterfun.ashx (each call typically yields 2 pages)
                for (int p = 1; p <= totalPages; p += 2) {
                    std::string funUrl = baseDir + "chapterfun.ashx?cid=" + cidStr +
                                         "&page=" + std::to_string(p) +
                                         "&key=" + guidStr;
                    std::string funResp;
                    if (!util::httpsGetString(funUrl, funResp, 15)) {
                        break;
                    }

                    std::string funUnpacked = unpackDeanEdwards(funResp);
                    if (funUnpacked.empty()) funUnpacked = funResp;

                    std::smatch pixMatch, pvalMatch;
                    std::regex pixRegex(R"raw(var\s+pix\s*=\s*["']([^"']+)["'])raw");
                    std::regex pvalRegex(R"raw(var\s+pvalue\s*=\s*\[([^\]]+)\])raw");

                    std::string pix = "";
                    if (std::regex_search(funUnpacked, pixMatch, pixRegex)) {
                        pix = pixMatch[1].str();
                        if (pix.rfind("//", 0) == 0) pix = "https:" + pix;
                    }

                    if (std::regex_search(funUnpacked, pvalMatch, pvalRegex)) {
                        std::string rawList = pvalMatch[1].str();
                        std::stringstream ss(rawList);
                        std::string item;
                        while (std::getline(ss, item, ',')) {
                            std::string cleaned;
                            for (char c : item) {
                                if (c != '"' && c != '\'' && c != ' ' && c != '\r' && c != '\n') {
                                    cleaned += c;
                                }
                            }
                            if (!cleaned.empty()) {
                                std::string fullImg = cleaned;
                                if (fullImg.rfind("http", 0) != 0 && fullImg.rfind("//", 0) != 0) {
                                    fullImg = pix + cleaned;
                                } else if (fullImg.rfind("//", 0) == 0) {
                                    fullImg = "https:" + fullImg;
                                }
                                if (pages.empty() || pages.back() != fullImg) {
                                    pages.push_back(fullImg);
                                }
                            }
                        }
                    } else {
                        auto bit = std::sregex_iterator(funUnpacked.begin(), funUnpacked.end(), urlRegex);
                        for (; bit != end; ++bit) {
                            std::string u = bit->str();
                            if (u.rfind("//", 0) == 0) u = "https:" + u;
                            if (pages.empty() || pages.back() != u) {
                                pages.push_back(u);
                            }
                        }
                    }

                    if (static_cast<int>(pages.size()) >= totalPages) {
                        break;
                    }
                }
            }
        }

        if (pages.empty() && errOut) {
            *errOut = "Failed to parse MangaFox page URLs";
        }
        return pages;
    }

    std::vector<std::string> fetchMangaKatanaPageUrls(const std::string& chapterUrl, std::string* errOut) {
        std::string html;
        std::string fullUrl = chapterUrl;
        if (fullUrl.rfind("http", 0) != 0) {
            fullUrl = "https://mangakatana.com" + chapterUrl;
        }

        if (!util::httpsGetString(fullUrl, html, 25)) {
            if (errOut) *errOut = "Could not load MangaKatana chapter HTML";
            return {};
        }

        std::vector<std::string> pages;
        size_t thzPos = html.find("var thzq=[");
        if (thzPos != std::string::npos) {
            size_t thzEnd = html.find("];", thzPos);
            std::string slice = html.substr(thzPos, (thzEnd != std::string::npos) ? (thzEnd - thzPos + 2) : 2000);
            std::regex urlRegex(R"raw(https?://[^\s'",;]+\.(?:jpg|jpeg|png|webp))raw");
            auto begin = std::sregex_iterator(slice.begin(), slice.end(), urlRegex);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it) {
                pages.push_back(it->str());
            }
        }

        if (pages.empty() && errOut) {
            *errOut = "Failed to parse MangaKatana page URLs";
        }
        return pages;
    }
};

} // namespace kindle
