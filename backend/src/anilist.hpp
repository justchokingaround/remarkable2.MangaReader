#pragma once

// AniList tracker: GraphQL client + offline-first progress sync.
//
// Design (everything works offline, network is only used opportunistically):
//  - data/anilist.json holds the token, the last fetched copy of the user's manga list
//    ("entries", which doubles as the 3-way-merge *base*), manga<->AniList links, the queue
//    of not-yet-pushed changes ("pending") and unresolved conflicts.
//  - Reading a chapter only touches local state and queues "progress = N" for the linked
//    AniList entry, remembering what the entry looked like at that moment (base_*).
//  - A sync fetches the whole list once, then for each pending change:
//      * entry unchanged on AniList since base          -> push ours
//      * AniList already at/above our progress           -> nothing to do (drop ours)
//      * both moved forward, AniList still Reading/Plan  -> push ours (it is further)
//      * anything else (progress went back, entry removed, status changed to Dropped,
//        Paused, Completed, Rereading, or an explicit edit raced a remote one)
//                                                        -> conflict, the user decides
//  - Progress never goes down automatically; remote progress is mirrored into local read
//    marks (only ever marking more chapters read).

#include "json.hpp"
#include "util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <type_traits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace anilist {

namespace fs = std::filesystem;

inline const char* kApiUrl = "https://graphql.anilist.co";
inline const char* kPinRedirect = "https://anilist.co/api/v2/oauth/pin";

// Set this to your own AniList API client id to skip the "create a client" step of the
// phone setup page (https://anilist.co/settings/developer, redirect URL = kPinRedirect).
inline const char* kDefaultClientId = "";

inline std::string authorizeUrl(const std::string& clientId) {
    return "https://anilist.co/api/v2/oauth/authorize?client_id=" + util::urlEncode(clientId) +
           "&response_type=token";
}

// ------------------------------------------------------------------ HTTP / GraphQL

inline std::string apiUrl() {
    const char* e = std::getenv("RM2MANGA_ANILIST_URL");  // tests point this at a mock server
    return (e && *e) ? e : kApiUrl;
}

struct GqlResult {
    bool netError = false;     // could not reach AniList at all (offline, DNS, TLS, timeout)
    int httpStatus = 0;
    Json::Value data;          // response "data" member
    std::string error;         // human readable error (GraphQL or HTTP)
    bool ok() const { return !netError && httpStatus == 200 && error.empty() && data.isObject(); }
    bool authError() const { return httpStatus == 401; }
};

inline GqlResult graphql(const std::string& token, const std::string& query, const Json::Value& vars,
                         int timeoutSec = 25) {
    GqlResult res;
    const std::string& curl = util::curlBinary();
    if (curl.empty()) {
        res.netError = true;
        res.error = "curl not available";
        return res;
    }

    Json::Value body = Json::Value::object();
    body["query"] = query;
    body["variables"] = vars.isObject() ? vars : Json::Value::object();

    // Body + headers go through 0600 temp files: keeps the token out of `ps` output.
    char bodyPath[] = "/tmp/rm2manga-al-XXXXXX";
    char hdrPath[] = "/tmp/rm2manga-alh-XXXXXX";
    int bfd = ::mkstemp(bodyPath);
    int hfd = ::mkstemp(hdrPath);
    if (bfd < 0 || hfd < 0) {
        if (bfd >= 0) { ::close(bfd); ::unlink(bodyPath); }
        if (hfd >= 0) { ::close(hfd); ::unlink(hdrPath); }
        res.netError = true;
        res.error = "temp file failed";
        return res;
    }
    std::string b = Json::stringify(body);
    std::string h = "Content-Type: application/json\nAccept: application/json\n";
    if (!token.empty()) h += "Authorization: Bearer " + token + "\n";
    bool wrote = ::write(bfd, b.data(), b.size()) == static_cast<ssize_t>(b.size()) &&
                 ::write(hfd, h.data(), h.size()) == static_cast<ssize_t>(h.size());
    ::close(bfd);
    ::close(hfd);

    util::CmdResult r;
    if (wrote) {
        r = util::runProcess({curl, "-sS", "--compressed", "--max-time", std::to_string(timeoutSec),
                              "-X", "POST", "-H", std::string("@") + hdrPath,
                              "--data-binary", std::string("@") + bodyPath,
                              "-w", "\n__HTTP__%{http_code}", apiUrl()},
                             timeoutSec + 5);
    }
    ::unlink(bodyPath);
    ::unlink(hdrPath);

    if (!wrote || r.exitCode != 0) {
        res.netError = true;
        res.error = "AniList unreachable (offline?)";
        return res;
    }
    std::string out = r.stdoutData;
    size_t mark = out.rfind("\n__HTTP__");
    if (mark != std::string::npos) {
        res.httpStatus = std::atoi(out.c_str() + mark + 9);
        out.resize(mark);
    }
    if (res.httpStatus == 0) {
        res.netError = true;
        res.error = "AniList unreachable";
        return res;
    }
    bool ok = false;
    Json::Value root = Json::parse(out, &ok);
    if (ok && root["errors"].isArray() && !root["errors"].empty()) {
        res.error = root["errors"][size_t(0)]["message"].str("AniList error");
        int st = static_cast<int>(root["errors"][size_t(0)]["status"].integer(0));
        if (st == 401 || res.error.find("Invalid token") != std::string::npos) res.httpStatus = 401;
    } else if (res.httpStatus == 429) {
        res.error = "AniList rate limit, retrying later";
    } else if (res.httpStatus != 200) {
        res.error = "AniList HTTP " + std::to_string(res.httpStatus);
    } else if (!ok) {
        res.error = "Invalid AniList response";
    }
    if (ok) res.data = root["data"];
    return res;
}

inline const char* kMediaFields =
    "id chapters status format startDate { year } "
    "title { romaji english native userPreferred } synonyms coverImage { medium }";

inline Json::Value compactMedia(const Json::Value& m) {
    Json::Value o = Json::Value::object();
    o["media_id"] = m["id"].integer(0);
    o["chapters"] = m["chapters"].integer(0);
    o["media_status"] = m["status"].str();
    o["format"] = m["format"].str();
    o["year"] = m["startDate"]["year"].integer(0);
    const Json::Value& t = m["title"];
    o["title"] = t["userPreferred"].str(t["romaji"].str(t["english"].str(t["native"].str())));
    o["title_romaji"] = t["romaji"].str();
    o["title_english"] = t["english"].str();
    o["title_native"] = t["native"].str();
    Json::Value syn = Json::Value::array();
    for (const auto& s : m["synonyms"].asArray()) {
        if (!s.str().empty()) syn.push(s.str());
    }
    o["synonyms"] = syn;
    o["cover_url"] = m["coverImage"]["medium"].str();
    return o;
}

// ------------------------------------------------------------------ title matching

inline std::vector<uint32_t> decodeUtf8(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c;
        int extra = 0;
        if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        }
        out.push_back(cp);
    }
    return out;
}

inline void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Lowercase (ASCII + Cyrillic), drop whitespace and punctuation: "Attack on Titan!" -> "attackontitan"
inline std::string normalizeTitle(const std::string& s) {
    std::string out;
    for (uint32_t cp : decodeUtf8(s)) {
        if (cp < 0x80) {
            if (std::isalnum(static_cast<int>(cp))) out.push_back(static_cast<char>(std::tolower(static_cast<int>(cp))));
            continue;
        }
        if (cp >= 0x0410 && cp <= 0x042F) cp += 0x20;          // А-Я -> а-я
        else if (cp >= 0x0400 && cp <= 0x040F) cp += 0x50;     // Ѐ-Џ -> ѐ-џ
        if (cp == 0x0451) cp = 0x0435;                         // ё -> е
        if ((cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x3000 && cp <= 0x303F) ||
            (cp >= 0xFF00 && cp <= 0xFF0F) || cp == 0x00A0 || cp == 0x00B7 ||
            (cp >= 0x00A1 && cp <= 0x00BF) || cp == 0x30FB || cp == 0xFF1A || cp == 0xFF01 || cp == 0xFF1F) {
            continue;
        }
        appendUtf8(out, cp);
    }
    return out;
}

inline bool isMostlyLatin(const std::string& s) {
    int latin = 0, other = 0;
    for (uint32_t cp : decodeUtf8(s)) {
        if (cp < 0x80) { if (std::isalpha(static_cast<int>(cp))) ++latin; }
        else ++other;
    }
    return latin > 0 && latin >= other;
}

// Titles we can use to recognise a source manga on AniList: its title, alternative names some
// sources put in the description (MangaLib: "Альтернативные названия:<br/>..."), and slug-style
// ids ("247--shingeki-no-kyojin" -> "shingeki no kyojin").
inline std::vector<std::string> localTitleCandidates(const std::string& title, const std::string& mangaId,
                                                     const std::string& description) {
    std::vector<std::string> out;
    auto add = [&](std::string t) {
        t = util::trim(t);
        if (t.size() < 2 || t.size() > 200) return;
        for (const auto& e : out) if (normalizeTitle(e) == normalizeTitle(t)) return;
        if (normalizeTitle(t).empty()) return;
        out.push_back(t);
    };
    add(title);

    for (const char* marker : {"Альтернативные названия:", "Alternative titles:", "Alternative Titles:",
                               "Alt titles:", "Also known as:"}) {
        size_t p = description.find(marker);
        if (p == std::string::npos) continue;
        std::string rest = description.substr(p + std::strlen(marker));
        size_t endp = rest.find("\n\n");
        if (endp != std::string::npos) rest = rest.substr(0, endp);
        std::string cur;
        auto flush = [&]() { add(cur); cur.clear(); };
        for (size_t i = 0; i < rest.size();) {
            if (rest.compare(i, 5, "<br/>") == 0) { flush(); i += 5; continue; }
            if (rest.compare(i, 4, "<br>") == 0) { flush(); i += 4; continue; }
            if (rest[i] == '\n' || rest[i] == ';' || rest[i] == '/') { flush(); ++i; continue; }
            cur.push_back(rest[i]);
            ++i;
        }
        flush();
        break;
    }

    // "<digits>--slug" ids (MangaLib/ReManga style)
    size_t dd = mangaId.find("--");
    if (dd != std::string::npos && dd > 0 &&
        std::all_of(mangaId.begin(), mangaId.begin() + static_cast<long>(dd), ::isdigit)) {
        std::string slug = mangaId.substr(dd + 2);
        std::string t;
        for (char c : slug) t.push_back((c == '-' || c == '_') ? ' ' : c);
        add(t);
    }
    return out;
}

inline std::set<std::string> mediaTitleKeys(const Json::Value& media) {
    std::set<std::string> keys;
    for (const char* k : {"title_romaji", "title_english", "title_native", "title"}) {
        std::string n = normalizeTitle(media[k].str());
        if (!n.empty()) keys.insert(n);
    }
    for (const auto& s : media["synonyms"].asArray()) {
        std::string n = normalizeTitle(s.str());
        if (!n.empty()) keys.insert(n);
    }
    return keys;
}

// Match on the main titles only (not synonyms): disambiguates "One Piece" vs a parody that
// lists "One Piece" as a synonym.
inline bool primaryTitleMatch(const Json::Value& media, const std::vector<std::string>& local) {
    std::set<std::string> keys;
    for (const char* k : {"title_romaji", "title_english", "title_native"}) {
        std::string n = normalizeTitle(media[k].str());
        if (!n.empty()) keys.insert(n);
    }
    for (const auto& t : local) {
        if (keys.count(normalizeTitle(t))) return true;
    }
    return false;
}

inline bool titlesMatch(const Json::Value& media, const std::vector<std::string>& local) {
    auto keys = mediaTitleKeys(media);
    for (const auto& t : local) {
        if (keys.count(normalizeTitle(t))) return true;
    }
    return false;
}

inline const char* statusLabel(const std::string& s) {
    if (s == "CURRENT") return "Reading";
    if (s == "PLANNING") return "Plan to read";
    if (s == "COMPLETED") return "Completed";
    if (s == "DROPPED") return "Dropped";
    if (s == "PAUSED") return "Paused";
    if (s == "REPEATING") return "Rereading";
    return "Not on list";
}

// ------------------------------------------------------------------ persistent tracker state

class Store {
    fs::path file_;
    mutable std::mutex mu_;
    Json::Value root_;

    void ensureLocked() {
        if (!root_.isObject()) root_ = Json::Value::object();
        for (const char* k : {"links", "entries", "pending", "conflicts", "unsure", "user"}) {
            if (!root_[k].isObject()) root_[k] = Json::Value::object();
        }
    }
    void saveLocked() { util::atomicWriteFile(file_, Json::stringify(root_)); }

public:
    explicit Store(fs::path file) : file_(std::move(file)) {
        std::string raw;
        if (util::readFileToString(file_, raw) && !raw.empty()) {
            bool ok = false;
            Json::Value v = Json::parse(raw, &ok);
            if (ok && v.isObject()) root_ = std::move(v);
        }
        ensureLocked();
    }

    // Generic locked access for the sync engine
    template <typename F>
    auto with(F&& fn) const {
        std::lock_guard<std::mutex> lk(mu_);
        return fn(static_cast<const Json::Value&>(root_));
    }
    template <typename F>
    auto mutate(F&& fn) {
        std::lock_guard<std::mutex> lk(mu_);
        if constexpr (std::is_void_v<decltype(fn(root_))>) {
            fn(root_);
            ensureLocked();
            saveLocked();
        } else {
            auto r = fn(root_);
            ensureLocked();
            saveLocked();
            return r;
        }
    }

    std::string token() const { std::lock_guard<std::mutex> lk(mu_); return root_["token"].str(); }
    bool connected() const { return !token().empty(); }
    std::string clientId() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::string c = root_["client_id"].str();
        return c.empty() ? std::string(kDefaultClientId) : c;
    }
};

}  // namespace anilist
