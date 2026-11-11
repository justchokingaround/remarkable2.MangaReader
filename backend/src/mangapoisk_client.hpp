#pragma once

// Native MangaPoisk engine (mangapoisk.me) - a Russian catalogue.
//
// Added because the Aidoku Russian sources lose a lot of titles: MangaLib removes
// licensed works from its catalogue (Attack on Titan, Tomodachi Game, ...), ReManga does
// not carry them, and MangaDex has no Russian scanlation for many of them.
//
// The site is server rendered HTML plus two JSON/HTML endpoints, so this client scrapes:
//   /search?q=<q>                            -> result cards (slug, title, cover)
//   /manga/<slug>                            -> ld+json (title/description) + poster + status
//   /manga/<slug>/chapterSelector/<anyId>    -> JSON list of all chapters (id, volume, number)
//   /manga/<slug>/chapter/<chapterId>        -> reader page with <img> page URLs

#include "json.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace mangapoisk {

namespace fs = std::filesystem;

class Client {
    std::string base_ = "https://mangapoisk.me";

    // ------------------------------------------------------------------ tiny HTML helpers

    static std::string between(const std::string& hay, const std::string& startMarker,
                               const std::string& endMarker) {
        size_t p = hay.find(startMarker);
        if (p == std::string::npos) return "";
        p += startMarker.size();
        size_t e = hay.find(endMarker, p);
        if (e == std::string::npos) return "";
        return hay.substr(p, e - p);
    }

    static std::string attr(const std::string& tag, const std::string& name) {
        std::string needle = name + "=\"";
        size_t p = tag.find(needle);
        if (p == std::string::npos) return "";
        p += needle.size();
        size_t e = tag.find('"', p);
        if (e == std::string::npos) return "";
        return tag.substr(p, e - p);
    }

    static std::string decodeEntities(const std::string& s) {
        struct Ent { const char* code; const char* ch; };
        static const Ent kEnts[] = {
            {"&quot;", "\""}, {"&#039;", "'"}, {"&apos;", "'"}, {"&lt;", "<"},
            {"&gt;", ">"}, {"&amp;", "&"}, {"&nbsp;", " "}, {"&mdash;", "-"},
            {"&ndash;", "-"}, {"&laquo;", "\""}, {"&raquo;", "\""},
        };
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            bool matched = false;
            if (s[i] == '&') {
                for (const auto& e : kEnts) {
                    size_t n = std::strlen(e.code);
                    if (s.compare(i, n, e.code) == 0) {
                        out += e.ch;
                        i += n;
                        matched = true;
                        break;
                    }
                }
                if (!matched && s.compare(i, 2, "&#") == 0) {
                    size_t semi = s.find(';', i);
                    if (semi != std::string::npos && semi - i < 9) {
                        std::string num = s.substr(i + 2, semi - i - 2);
                        long cp = (num.size() > 1 && (num[0] == 'x' || num[0] == 'X'))
                                      ? std::strtol(num.c_str() + 1, nullptr, 16)
                                      : std::strtol(num.c_str(), nullptr, 10);
                        out += util::cpToUtf8(static_cast<uint32_t>(cp));
                        i = semi + 1;
                        matched = true;
                    }
                }
            }
            if (!matched) out.push_back(s[i++]);
        }
        return out;
    }

    static std::string stripTags(std::string s) {
        std::string out;
        bool inTag = false;
        for (char c : s) {
            if (c == '<') inTag = true;
            else if (c == '>') inTag = false;
            else if (!inTag) out.push_back(c);
        }
        return util::trim(decodeEntities(out));
    }

    static std::string prettyTitle(const std::string& slug) {
        std::string t;
        for (char c : slug) t.push_back((c == '-' || c == '_') ? ' ' : c);
        if (!t.empty()) t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
        return util::trim(t);
    }

    bool get(const std::string& path, std::string& body, int timeoutSec = 25) {
        return util::httpsGetString(base_ + path, body, timeoutSec);
    }

public:
    // ------------------------------------------------------------------ search / listing

    // Handles both card layouts the site uses: <article> cards on /search and
    // <div class="manga-card"> blocks on /top. Each card is an <a href="/manga/<slug>"
    // title="..."> plus a nearby <img src|data-src>.
    Json::Value search(const std::string& query, std::string* errOut = nullptr) {
        Json::Value out = Json::Value::array();
        std::string path = query.empty() ? "/top" : ("/search?q=" + util::urlEncode(query));
        std::string body;
        if (!get(path, body, 30)) {
            if (errOut) *errOut = "MangaPoisk is unreachable";
            return out;
        }
        std::set<std::string> seen;
        size_t pos = 0;
        while ((pos = body.find("href=\"/manga/", pos)) != std::string::npos) {
            size_t slugStart = pos + std::strlen("href=\"/manga/");
            size_t slugEnd = body.find('"', slugStart);
            if (slugEnd == std::string::npos) break;
            std::string slug = body.substr(slugStart, slugEnd - slugStart);
            // The element that carries this link (the anchor whose attribute we found)
            size_t tagStart = body.rfind('<', pos);
            size_t tagEnd = body.find('>', slugEnd);
            pos = slugEnd;
            if (slug.empty() || slug.find('/') != std::string::npos) continue;
            if (!seen.insert(slug).second) continue;

            std::string title, cover;
            if (tagStart != std::string::npos && tagEnd != std::string::npos && tagEnd > tagStart) {
                std::string tag = body.substr(tagStart, tagEnd - tagStart);
                title = attr(tag, "title");
            }
            // Card body (title heading + poster) follows the anchor
            size_t blockEnd = body.find("</article>", pos);
            if (blockEnd == std::string::npos) blockEnd = pos + 2000;
            std::string block = body.substr(pos, blockEnd - pos);
            if (title.empty()) {
                size_t h2 = block.find("<h2");
                if (h2 != std::string::npos) {
                    size_t h2End = block.find("</h2>", h2);
                    if (h2End != std::string::npos) title = stripTags(block.substr(h2, h2End - h2));
                }
            }
            size_t img = block.find("<img");
            if (img != std::string::npos) {
                std::string tag = block.substr(img, block.find('>', img) - img);
                cover = attr(tag, "src");
                if (cover.empty() || cover.rfind("data:", 0) == 0) cover = attr(tag, "data-src");
                if (cover.rfind("data:", 0) == 0) cover.clear();
                if (cover.rfind("//", 0) == 0) cover = "https:" + cover;
            }
            if (title.empty()) title = prettyTitle(slug);

            Json::Value m = Json::Value::object();
            m["id"] = slug;
            m["title"] = title;
            m["cover_url"] = cover;
            m["source"] = "mangapoisk";
            m["status"] = "";
            m["description"] = "";
            m["author"] = "";
            out.push(std::move(m));
        }
        if (out.empty() && errOut && errOut->empty() && !query.empty()) {
            *errOut = "No results on MangaPoisk";
        }
        return out;
    }

    // ------------------------------------------------------------------ details

    struct Details {
        std::string title, titleAlt, description, coverUrl, author, status;
    };

    // The site is an Inertia app: <div id="app" data-page="{json}"> carries the manga data.
    static Json::Value pageData(const std::string& body) {
        size_t p = body.find("data-page=\"");
        if (p == std::string::npos) return Json::Value();
        p += std::strlen("data-page=\"");
        size_t e = body.find('"', p);
        if (e == std::string::npos) return Json::Value();
        std::string raw = decodeEntities(body.substr(p, e - p));
        bool ok = false;
        Json::Value v = Json::parse(raw, &ok);
        return ok ? v : Json::Value();
    }

    Details getDetails(const std::string& slug) {
        Details d;
        std::string body;
        if (!get("/manga/" + slug, body, 30)) return d;

        Json::Value page = pageData(body);
        Json::Value mg = page["props"]["manga"]["data"];
        if (mg.isObject()) {
            d.title = mg["name"].str();
            d.titleAlt = mg["name_en"].str(mg["name_alt"].str());
            d.description = mg["description"].str();
            d.coverUrl = mg["poster"]["link"].str();
        }

        // unpublished/unknown entries fall back to the ld+json block
        if (d.title.empty()) {
            size_t p = body.find("application/ld+json");
            if (p != std::string::npos) {
                std::string block = between(body.substr(p), ">", "</script>");
                bool ok = false;
                Json::Value ld = Json::parse(decodeEntities(block), &ok);
                if (ok && ld.isObject()) {
                    d.title = ld["headline"].str(ld["name"].str());
                    d.titleAlt = ld["alternativeHeadline"].str();
                    d.description = ld["description"].str(d.description);
                    if (d.coverUrl.empty()) d.coverUrl = ld["image"].str();
                }
            }
        }
        if (d.title.empty()) {
            std::string t = between(body, "<title>", "</title>");
            size_t cut = t.find(" читать");
            if (cut != std::string::npos) t.resize(cut);
            d.title = stripTags(t);
        }
        if (d.title.empty()) d.title = prettyTitle(slug);
        if (d.title.rfind("Манга ", 0) == 0) d.title = d.title.substr(6);
        if (d.coverUrl.rfind("//", 0) == 0) d.coverUrl = "https:" + d.coverUrl;

        // No status field in the JSON; the page renders a "Статус" label
        size_t st = body.find("Статус");
        if (st != std::string::npos) {
            std::string val = stripTags(body.substr(st, 600));
            size_t colon = val.find(':');
            if (colon != std::string::npos) {
                std::string v = util::trim(val.substr(colon + 1));
                size_t nl = v.find_first_of("\n\r");
                if (nl != std::string::npos) v.resize(nl);
                d.status = util::trim(v);
            }
        }
        std::string s = util::toLower(d.status);
        if (s.find("выпуск") != std::string::npos || s.find("продолж") != std::string::npos) d.status = "ongoing";
        else if (s.find("заверш") != std::string::npos) d.status = "completed";
        else if (s.find("заморож") != std::string::npos || s.find("приостан") != std::string::npos) d.status = "hiatus";
        else if (s.find("брош") != std::string::npos) d.status = "cancelled";
        else d.status.clear();
        return d;
    }

    Json::Value getChapters(const std::string& slug, std::string* errOut = nullptr) {
        Json::Value out = Json::Value::array();
        std::string body;
        // The endpoint accepts any chapter id; "0" returns the full list
        if (!get("/manga/" + slug + "/chapterSelector/0", body, 25)) {
            if (errOut) *errOut = "Failed to fetch chapter list from MangaPoisk";
            return out;
        }
        bool ok = false;
        Json::Value root = Json::parse(body, &ok);
        if (!ok || !root["chapters"].isArray()) {
            if (errOut) *errOut = "Unexpected chapter list from MangaPoisk";
            return out;
        }
        for (const auto& c : root["chapters"].asArray()) {
            std::string link = c["link"].str();
            std::string id = link.rfind("/chapter/", 0) == 0 || link.find("/chapter/") != std::string::npos
                                 ? link.substr(link.find("/chapter/") + 9)
                                 : std::to_string(c["id"].integer(0));
            if (id.empty()) continue;
            Json::Value ch = Json::Value::object();
            ch["id"] = id;
            std::string num = c["number"].str();
            if (!num.empty()) {
                try {
                    ch["chapter_num"] = std::stod(num);
                } catch (...) {
                }
            }
            std::string vol = c["volume"].str();
            if (!vol.empty()) {
                try {
                    double v = std::stod(vol);
                    if (v > 0) ch["volume_num"] = v;
                } catch (...) {
                }
            }
            ch["title"] = c["name"].str();
            ch["language"] = "ru";
            ch["scanlator"] = "MangaPoisk";
            ch["read"] = false;
            ch["downloaded"] = false;
            out.push(std::move(ch));
        }
        return out;
    }

    // ------------------------------------------------------------------ pages

    std::vector<std::string> getPageUrls(const std::string& slug, const std::string& chapterId) {
        std::vector<std::string> urls;
        std::string body;
        if (!get("/manga/" + slug + "/chapter/" + chapterId, body, 30)) return urls;

        size_t pos = 0;
        while ((pos = body.find("<img", pos)) != std::string::npos) {
            size_t end = body.find('>', pos);
            if (end == std::string::npos) break;
            std::string tag = body.substr(pos, end - pos);
            pos = end;
            if (tag.find("/pages/") == std::string::npos) continue;
            std::string src = attr(tag, "src");
            if (src.rfind("data:", 0) == 0) src = attr(tag, "data-src");
            if (src.empty() || src.rfind("data:", 0) == 0) continue;
            if (src.rfind("//", 0) == 0) src = "https:" + src;
            if (std::find(urls.begin(), urls.end(), src) == urls.end()) urls.push_back(src);
        }
        return urls;
    }

    bool downloadChapterPages(const std::string& slug,
                              const std::string& chapterId,
                              const fs::path& destDir,
                              const std::atomic<bool>* cancelFlag,
                              const std::function<void(const std::string&, int)>& onProgress,
                              std::string* errOut = nullptr) {
        std::vector<std::string> urls = getPageUrls(slug, chapterId);
        if (urls.empty()) {
            if (errOut) *errOut = "MangaPoisk returned no pages for this chapter";
            return false;
        }

        std::error_code ec;
        fs::path tmpDir = destDir;
        tmpDir += ".downloading";
        fs::remove_all(tmpDir, ec);
        fs::create_directories(tmpDir, ec);

        for (size_t i = 0; i < urls.size(); ++i) {
            if (cancelFlag && cancelFlag->load()) {
                fs::remove_all(tmpDir, ec);
                if (errOut) *errOut = "Download cancelled";
                return false;
            }
            int pct = static_cast<int>(((i + 1) * 100) / urls.size());
            if (onProgress) {
                onProgress("Downloading page " + std::to_string(i + 1) + " of " +
                               std::to_string(urls.size()) + "...",
                           pct);
            }
            std::string ext = ".jpg";
            auto dot = urls[i].find_last_of('.');
            if (dot != std::string::npos) {
                std::string e = util::toLower(urls[i].substr(dot));
                if (e.size() <= 5) ext = e;
            }
            char idxBuf[32];
            std::snprintf(idxBuf, sizeof(idxBuf), "%04zu%s", i + 1, ext.c_str());
            if (!util::httpsDownloadFile(urls[i], tmpDir / idxBuf, 40)) {
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
};

}  // namespace mangapoisk
