#pragma once

// AniList sync engine on top of anilist::Store. See the design notes in anilist.hpp.

#include "anilist.hpp"

#include <atomic>
#include <functional>

namespace anilist {

struct Hooks {
    // Highest chapter number (floored) the user finished locally for a source manga, 0 if none.
    std::function<int(const std::string& sid, const std::string& mid)> maxReadChapter;
    // Mark every cached chapter with number <= progress as read. Returns true if anything changed.
    std::function<bool(const std::string& sid, const std::string& mid, int progress)> markReadUpTo;
    // Whether the chapter list of a manga is cached locally (nothing to mark otherwise)
    std::function<bool(const std::string& sid, const std::string& mid)> hasCachedChapters;
    // Source mangas worth auto-linking: [{source_id, source_name, manga_id, title, description}]
    std::function<std::vector<Json::Value>()> autoLinkCandidates;
    // AniList id that MangaDex itself stores for a manga (0 if unknown)
    std::function<int64_t(const std::string& mid)> mangadexAniListId;
    // Source manga metadata {title, source_name, description}
    std::function<Json::Value(const std::string& sid, const std::string& mid)> mangaInfo;
    std::function<bool(const std::string& url, const fs::path& dest)> downloadFile;
};

struct SyncOutcome {
    bool attempted = false;
    bool ok = false;
    bool offline = false;
    bool authError = false;
    bool localChanged = false;  // local read marks changed -> library/detail views need a refresh
    int pushed = 0;
    int newConflicts = 0;
    std::string error;
};

inline std::string mangaKey(const std::string& sid, const std::string& mid) { return sid + "::" + mid; }

class Tracker {
    Store store_;
    Hooks hooks_;
    fs::path coversDir_;
    std::atomic<bool> syncing_{false};
    std::atomic<int> online_{-1};  // -1 unknown, 0 offline, 1 online
    std::mutex syncMu_;            // one sync at a time
    std::mutex sessionMu_;
    std::set<std::string> autoLinkTried_;  // per app session

    static const char* listQuery() {
        return "query($u:Int){MediaListCollection(userId:$u,type:MANGA){lists{entries{"
               "id status progress repeat updatedAt media{"
               "id chapters status format startDate{year} title{romaji english native userPreferred} "
               "synonyms coverImage{medium}}}}}}";
    }

    static Json::Value entryFromRemote(const Json::Value& e) {
        Json::Value o = compactMedia(e["media"]);
        o["entry_id"] = e["id"].integer(0);
        o["status"] = e["status"].str();
        o["progress"] = e["progress"].integer(0);
        o["repeat"] = e["repeat"].integer(0);
        o["updated_at"] = e["updatedAt"].integer(0);
        return o;
    }

    static std::string deriveStatus(const std::string& remoteStatus, int64_t progress, int64_t total) {
        if (total > 0 && progress >= total) return "COMPLETED";
        if (remoteStatus.empty() || remoteStatus == "PLANNING" || remoteStatus == "PAUSED" ||
            remoteStatus == "DROPPED") {
            return "CURRENT";
        }
        return remoteStatus;
    }

    static int64_t totalChapters(const Json::Value& root, const std::string& mid) {
        int64_t t = root["entries"][mid]["chapters"].integer(0);
        if (t > 0) return t;
        for (const auto& kv : root["links"].asObject()) {
            if (std::to_string(kv.second["media_id"].integer(0)) == mid) {
                t = kv.second["chapters"].integer(0);
                if (t > 0) return t;
            }
        }
        return 0;
    }

    // Must hold store lock (called from within mutate()).
    static void queueLocked(Json::Value& root, int64_t mediaId, int64_t progress, bool explicitEdit,
                            const std::string& status, const std::string& title) {
        std::string mid = std::to_string(mediaId);
        const Json::Value& croot = root;
        const Json::Value& e = croot["entries"][mid];
        Json::Value& pending = root["pending"];
        bool had = pending.has(mid);
        int64_t total = totalChapters(root, mid);
        if (!explicitEdit) {
            if (total > 0) progress = std::min(progress, total);
            int64_t effective = had ? pending[mid]["progress"].integer(0) : e["progress"].integer(0);
            if (progress <= effective) return;
        }
        Json::Value& p = pending[mid];
        if (!had) {
            p = Json::Value::object();
            p["media_id"] = mediaId;
            p["base_present"] = e.isObject();
            p["base_progress"] = e["progress"].integer(0);
            p["base_status"] = e["status"].str();
            p["base_updated_at"] = e["updated_at"].integer(0);
            p["explicit"] = false;
            p["status"] = "";
        }
        p["progress"] = progress;
        if (explicitEdit) {
            p["explicit"] = true;
            p["status"] = status;
        }
        if (!title.empty()) p["title"] = title;
        else if (!p.has("title")) p["title"] = e["title"].str();
        p["queued_at"] = util::nowSeconds();
        p["rev"] = p["rev"].integer(0) + 1;
        if (root["conflicts"].has(mid)) {
            root["conflicts"][mid]["local_progress"] = progress;
            if (explicitEdit) root["conflicts"][mid]["local_status"] = status;
        }
    }

    Json::Value linkInfoLocked(const Json::Value& root, const std::string& mkey) const {
        Json::Value out = Json::Value::object();
        out["connected"] = !root["token"].str().empty();
        const Json::Value& link = root["links"][mkey];
        if (!link.isObject()) {
            out["linked"] = false;
            out["unsure"] = root["unsure"].has(mkey);
            return out;
        }
        std::string mid = std::to_string(link["media_id"].integer(0));
        const Json::Value& e = root["entries"][mid];
        const Json::Value& p = root["pending"][mid];
        out["linked"] = true;
        out["media_id"] = link["media_id"].integer(0);
        out["title"] = e.isObject() ? e["title"].str() : link["title"].str();
        int64_t remote = e["progress"].integer(0);
        std::string status = e["status"].str();
        if (p.isObject()) {
            if (p["explicit"].boolean(false) && !p["status"].str().empty()) status = p["status"].str();
            else if (status.empty() || status == "PLANNING") status = "CURRENT";
        }
        out["remote_progress"] = remote;
        out["progress"] = p.isObject() ? p["progress"].integer(0) : remote;
        out["status"] = status;
        out["status_label"] = statusLabel(status);
        out["on_list"] = e.isObject();
        out["chapters"] = totalChapters(root, mid);
        out["pending"] = p.isObject();
        out["conflict"] = root["conflicts"].has(mid);
        out["auto"] = link["auto"].boolean(false);
        return out;
    }

    bool applyRemoteToLocal(const std::string& mkey, Json::Value& link, int64_t remoteProgress) {
        if (remoteProgress <= link["applied_progress"].integer(0)) return false;
        // Only remember it as applied once there are chapters to mark (otherwise retried later)
        if (hooks_.hasCachedChapters && !hooks_.hasCachedChapters(link["source_id"].str(), link["manga_id"].str())) {
            return false;
        }
        bool changed = false;
        if (hooks_.markReadUpTo) {
            changed = hooks_.markReadUpTo(link["source_id"].str(), link["manga_id"].str(),
                                          static_cast<int>(remoteProgress));
        }
        link["applied_progress"] = remoteProgress;
        (void)mkey;
        return changed;
    }

public:
    Tracker(const fs::path& dataDir, Hooks hooks)
        : store_(dataDir / "anilist.json"), hooks_(std::move(hooks)), coversDir_(dataDir / "covers") {
        // Power-user fallback: drop a token into data/anilist_token.txt via SSH
        fs::path tokFile = dataDir / "anilist_token.txt";
        std::string tok;
        if (util::readFileToString(tokFile, tok)) {
            tok = util::trim(tok);
            if (!tok.empty()) {
                store_.mutate([&](Json::Value& r) { r["token"] = tok; r["needs_reauth"] = false; });
            }
            std::error_code ec;
            fs::remove(tokFile, ec);
        }
    }

    bool connected() const { return store_.connected(); }
    std::string clientId() const { return store_.clientId(); }
    bool syncing() const { return syncing_.load(); }
    void markSyncing() { syncing_ = true; }  // so the "Syncing..." UI state can be pushed first

    void setClientId(const std::string& id) {
        store_.mutate([&](Json::Value& r) { r["client_id"] = id; });
    }

    // Validates and stores a token. Returns "" on success or an error message.
    std::string connect(const std::string& rawToken, std::string* userNameOut = nullptr) {
        std::string token = util::trim(rawToken);
        // People sometimes paste the whole redirect URL / fragment
        size_t at = token.find("access_token=");
        if (at != std::string::npos) {
            token = token.substr(at + 13);
            size_t amp = token.find('&');
            if (amp != std::string::npos) token.resize(amp);
        }
        if (token.size() < 20) return "That does not look like an AniList token";
        GqlResult r = graphql(token, "query{Viewer{id name avatar{medium}}}", Json::Value::object());
        if (r.netError) return "Could not reach AniList from the reMarkable (is Wi-Fi on?)";
        if (!r.ok() || r.data["Viewer"]["id"].integer(0) == 0) {
            return r.error.empty() ? "AniList rejected the token" : r.error;
        }
        std::string name = r.data["Viewer"]["name"].str();
        int64_t uid = r.data["Viewer"]["id"].integer(0);
        store_.mutate([&](Json::Value& root) {
            if (root["user"]["id"].integer(0) != uid) {
                // Different account: cached list/queue belong to the old one
                root["entries"] = Json::Value::object();
                root["pending"] = Json::Value::object();
                root["conflicts"] = Json::Value::object();
                for (auto& kv : root["links"].asObjectMut()) kv.second["applied_progress"] = 0;
            }
            root["token"] = token;
            root["needs_reauth"] = false;
            root["user"]["id"] = uid;
            root["user"]["name"] = name;
            root["last_error"] = "";
        });
        online_ = 1;
        if (userNameOut) *userNameOut = name;
        return "";
    }

    void logout() {
        store_.mutate([&](Json::Value& root) {
            root["token"] = "";
            root["user"] = Json::Value::object();
            root["entries"] = Json::Value::object();
            root["pending"] = Json::Value::object();
            root["conflicts"] = Json::Value::object();
            root["unsure"] = Json::Value::object();
            root["needs_reauth"] = false;
            root["last_sync_at"] = 0;
            for (auto& kv : root["links"].asObjectMut()) kv.second["applied_progress"] = 0;
        });
        std::lock_guard<std::mutex> lk(sessionMu_);
        autoLinkTried_.clear();
    }

    // Source ids the "find on sources" search should use (empty = all installed)
    std::vector<std::string> searchSources() const {
        return store_.with([](const Json::Value& r) {
            std::vector<std::string> out;
            for (const auto& s : r["search_sources"].asArray()) {
                if (!s.str().empty()) out.push_back(s.str());
            }
            return out;
        });
    }

    void setSearchSources(const std::vector<std::string>& sources) {
        store_.mutate([&](Json::Value& r) {
            Json::Value arr = Json::Value::array();
            for (const auto& s : sources) arr.push(s);
            r["search_sources"] = arr;
        });
    }

    bool hasPending() const {
        return store_.with([](const Json::Value& r) { return !r["pending"].empty(); });
    }

    Json::Value summary() const {
        return store_.with([this](const Json::Value& r) {
            Json::Value s = Json::Value::object();
            s["connected"] = !r["token"].str().empty();
            s["user_name"] = r["user"]["name"].str();
            s["needs_reauth"] = r["needs_reauth"].boolean(false);
            s["syncing"] = syncing_.load();
            s["online"] = online_.load();
            s["last_sync_at"] = r["last_sync_at"].integer(0);
            s["last_error"] = r["last_error"].str();
            int blocked = 0;
            for (const auto& kv : r["pending"].asObject()) if (!r["conflicts"].has(kv.first)) ++blocked;
            s["pending_count"] = blocked;
            s["conflict_count"] = static_cast<int64_t>(r["conflicts"].size());
            s["client_id_set"] = !(r["client_id"].str().empty() && std::string(kDefaultClientId).empty());
            return s;
        });
    }

    Json::Value detailInfo(const std::string& sid, const std::string& mid) const {
        std::string mkey = mangaKey(sid, mid);
        return store_.with([&](const Json::Value& r) { return linkInfoLocked(r, mkey); });
    }

    // Full payload for the AniList tab
    Json::Value listState() const {
        Json::Value out = summary();
        store_.with([&](const Json::Value& r) {
            std::map<std::string, Json::Value> linkByMedia;
            for (const auto& kv : r["links"].asObject()) {
                std::string mid = std::to_string(kv.second["media_id"].integer(0));
                if (!linkByMedia.count(mid)) linkByMedia[mid] = kv.second;
            }
            Json::Value arr = Json::Value::array();
            std::set<std::string> seen;
            auto emit = [&](const std::string& mid, const Json::Value& e, const Json::Value& p) {
                Json::Value it = Json::Value::object();
                it["media_id"] = static_cast<int64_t>(std::atoll(mid.c_str()));
                it["title"] = e["title"].str(p["title"].str());
                it["title_english"] = e["title_english"].str();
                it["title_romaji"] = e["title_romaji"].str();
                std::string status = e["status"].str();
                if (p.isObject()) {
                    if (p["explicit"].boolean(false) && !p["status"].str().empty()) status = p["status"].str();
                    else if (status.empty() || status == "PLANNING" || status == "PAUSED" || status == "DROPPED") status = "CURRENT";
                }
                it["status"] = status;
                it["status_label"] = statusLabel(status);
                it["remote_progress"] = e["progress"].integer(0);
                it["progress"] = p.isObject() ? p["progress"].integer(0) : e["progress"].integer(0);
                it["chapters"] = totalChapters(r, mid);
                it["format"] = e["format"].str();
                it["cover_path"] = e["cover_path"].str();
                it["updated_at"] = std::max(e["updated_at"].integer(0), p["queued_at"].integer(0));
                it["pending"] = p.isObject();
                it["conflict"] = r["conflicts"].has(mid);
                auto lk = linkByMedia.find(mid);
                if (lk != linkByMedia.end()) {
                    Json::Value l = Json::Value::object();
                    l["source_id"] = lk->second["source_id"].str();
                    l["source_name"] = lk->second["source_name"].str();
                    l["manga_id"] = lk->second["manga_id"].str();
                    l["title"] = lk->second["source_title"].str();
                    it["link"] = l;
                }
                arr.push(std::move(it));
            };
            for (const auto& kv : r["entries"].asObject()) {
                seen.insert(kv.first);
                emit(kv.first, kv.second, r["pending"][kv.first]);
            }
            // Queued adds for titles that are not on the list yet
            for (const auto& kv : r["pending"].asObject()) {
                if (seen.count(kv.first)) continue;
                Json::Value fake = Json::Value::object();
                for (const auto& lkv : r["links"].asObject()) {
                    if (std::to_string(lkv.second["media_id"].integer(0)) == kv.first) {
                        fake["title"] = lkv.second["title"].str();
                        fake["chapters"] = lkv.second["chapters"].integer(0);
                        break;
                    }
                }
                emit(kv.first, fake, kv.second);
            }
            out["entries"] = arr;
            Json::Value cs = Json::Value::array();
            for (const auto& kv : r["conflicts"].asObject()) cs.push(kv.second);
            out["conflicts"] = cs;
            return 0;
        });
        return out;
    }

    // ---------------------------------------------------------------- local events

    // A chapter was finished on the device. Returns true if a sync should be scheduled.
    bool onChapterRead(const std::string& sid, const std::string& mid, double chapterNum) {
        if (!connected()) return false;
        std::string mkey = mangaKey(sid, mid);
        bool linked = store_.with([&](const Json::Value& r) { return r["links"].has(mkey); });
        if (!linked) {
            std::lock_guard<std::mutex> lk(sessionMu_);
            autoLinkTried_.erase(mkey);  // give auto-linking another go on next sync
            return true;
        }
        if (chapterNum < 1.0) return false;
        int64_t n = static_cast<int64_t>(std::floor(chapterNum + 1e-6));
        return store_.mutate([&](Json::Value& r) {
            const Json::Value& cr = r;
            int64_t media = cr["links"][mkey]["media_id"].integer(0);
            std::string ms = std::to_string(media);
            int64_t rev = cr["pending"][ms]["rev"].integer(0);
            queueLocked(r, media, n, false, "", "");
            return cr["pending"][ms]["rev"].integer(0) != rev;
        });
    }

    // Chapter list (re)fetched: mirror AniList progress into the fresh read marks if needed.
    bool onChaptersCached(const std::string& sid, const std::string& mid) {
        std::string mkey = mangaKey(sid, mid);
        Json::Value link;
        int64_t prog = 0;
        store_.with([&](const Json::Value& r) {
            link = r["links"][mkey];
            if (link.isObject()) prog = r["entries"][std::to_string(link["media_id"].integer(0))]["progress"].integer(0);
            return 0;
        });
        if (!link.isObject()) return false;
        int64_t before = link["applied_progress"].integer(0);
        bool changed = applyRemoteToLocal(mkey, link, prog);
        if (link["applied_progress"].integer(0) != before) {
            store_.mutate([&](Json::Value& r) {
                if (r["links"].has(mkey)) r["links"][mkey]["applied_progress"] = link["applied_progress"].integer(0);
            });
        }
        return changed;
    }

    void onLibraryAdd(const std::string& sid, const std::string& mid) {
        std::lock_guard<std::mutex> lk(sessionMu_);
        autoLinkTried_.erase(mangaKey(sid, mid));
    }

    // User edit from the manga's AniList dialog (may lower progress / change status).
    void explicitUpdate(int64_t mediaId, int64_t progress, const std::string& status) {
        store_.mutate([&](Json::Value& r) {
            queueLocked(r, mediaId, std::max<int64_t>(0, progress), true, status, "");
        });
    }

    // choice: "remote" keeps AniList's values, "local" pushes the device's
    void resolveConflict(int64_t mediaId, const std::string& choice) {
        std::string mid = std::to_string(mediaId);
        std::vector<std::pair<std::string, Json::Value>> linksToApply;
        store_.mutate([&](Json::Value& r) {
            const Json::Value& cr = r;
            const Json::Value& e = cr["entries"][mid];
            if (choice == "local" && r["pending"].has(mid)) {
                Json::Value& p = r["pending"][mid];
                int64_t total = totalChapters(r, mid);
                std::string rs = e["status"].str();
                std::string st = p["explicit"].boolean(false) && !p["status"].str().empty()
                                     ? p["status"].str()
                                     : ((rs == "CURRENT" || rs == "REPEATING") ? rs : deriveStatus("", p["progress"].integer(0), total));
                p["explicit"] = true;
                p["status"] = st;
                p.erase("needs_confirm");
                p["base_present"] = e.isObject();
                p["base_progress"] = e["progress"].integer(0);
                p["base_status"] = e["status"].str();
                p["base_updated_at"] = e["updated_at"].integer(0);
                p["rev"] = p["rev"].integer(0) + 1;
            } else {
                r["pending"].erase(mid);
                for (auto& kv : r["links"].asObjectMut()) {
                    if (std::to_string(kv.second["media_id"].integer(0)) == mid) {
                        kv.second["applied_progress"] = 0;  // re-mirror AniList progress
                        linksToApply.emplace_back(kv.first, kv.second);
                    }
                }
            }
            r["conflicts"].erase(mid);
        });
        for (auto& [mkey, link] : linksToApply) {
            int64_t prog = store_.with([&](const Json::Value& r) { return r["entries"][mid]["progress"].integer(0); });
            Json::Value l = link;
            applyRemoteToLocal(mkey, l, prog);
            store_.mutate([&](Json::Value& r) {
                if (r["links"].has(mkey)) r["links"][mkey]["applied_progress"] = l["applied_progress"].integer(0);
            });
        }
    }

    // ---------------------------------------------------------------- linking

    // Links a source manga to an AniList media (compact media JSON, see compactMedia()).
    // Reconciles both ways: remote progress -> local read marks, local progress -> queue.
    // Returns true if local read marks changed.
    bool link(const std::string& sid, const std::string& mid, const Json::Value& media, bool autoLinked) {
        int64_t mediaId = media["media_id"].integer(0);
        if (mediaId <= 0) return false;
        std::string mkey = mangaKey(sid, mid);
        Json::Value info = hooks_.mangaInfo ? hooks_.mangaInfo(sid, mid) : Json::Value::object();
        int localMax = hooks_.maxReadChapter ? hooks_.maxReadChapter(sid, mid) : 0;
        std::string smid = std::to_string(mediaId);

        Json::Value linkCopy;
        int64_t remoteProgress = 0;
        store_.mutate([&](Json::Value& r) {
            Json::Value l = Json::Value::object();
            l["media_id"] = mediaId;
            l["title"] = media["title"].str();
            l["chapters"] = media["chapters"].integer(0);
            l["cover_url"] = media["cover_url"].str();
            l["source_id"] = sid;
            l["manga_id"] = mid;
            l["source_name"] = info["source_name"].str(sid);
            l["source_title"] = info["title"].str(mid);
            l["auto"] = autoLinked;
            l["linked_at"] = util::nowSeconds();
            l["applied_progress"] = 0;
            r["links"][mkey] = l;
            r["unsure"].erase(mkey);
            // The search result may know the list entry even if our cached list is stale
            const Json::Value& le = media["list_entry"];
            if (le.isObject() && !r["entries"].has(smid)) {
                Json::Value e = media;
                e.erase("list_entry");
                e["entry_id"] = le["id"].integer(0);
                e["status"] = le["status"].str();
                e["progress"] = le["progress"].integer(0);
                e["repeat"] = le["repeat"].integer(0);
                e["updated_at"] = le["updatedAt"].integer(0);
                r["entries"][smid] = e;
            }
            const Json::Value& cr = r;
            remoteProgress = cr["entries"][smid]["progress"].integer(0);
            if (localMax > remoteProgress) {
                queueLocked(r, mediaId, localMax, false, "", media["title"].str());
                // Device read further than an entry the user parked on AniList: ask, don't revive it
                std::string rs = cr["entries"][smid]["status"].str();
                if ((rs == "DROPPED" || rs == "PAUSED" || rs == "COMPLETED") && r["pending"].has(smid)) {
                    r["pending"][smid]["needs_confirm"] = true;
                }
            }
            linkCopy = r["links"][mkey];
        });
        bool changed = applyRemoteToLocal(mkey, linkCopy, remoteProgress);
        store_.mutate([&](Json::Value& r) {
            if (r["links"].has(mkey)) r["links"][mkey]["applied_progress"] = linkCopy["applied_progress"].integer(0);
        });
        return changed;
    }

    void unlink(const std::string& sid, const std::string& mid) {
        std::string mkey = mangaKey(sid, mid);
        store_.mutate([&](Json::Value& r) {
            r["links"].erase(mkey);
            r["unsure"][mkey] = Json::Value::object();
            r["unsure"][mkey]["at"] = util::nowSeconds();
            r["unsure"][mkey]["manual"] = true;
        });
    }

    std::string linkedSourceFor(int64_t mediaId, std::string* midOut) const {
        return store_.with([&](const Json::Value& r) {
            for (const auto& kv : r["links"].asObject()) {
                if (kv.second["media_id"].integer(0) == mediaId) {
                    if (midOut) *midOut = kv.second["manga_id"].str();
                    return kv.second["source_id"].str();
                }
            }
            return std::string();
        });
    }

    Json::Value entryFor(int64_t mediaId) const {
        return store_.with([&](const Json::Value& r) { return r["entries"][std::to_string(mediaId)]; });
    }

    // AniList search. Each result: compact media + list_entry (if on the user's list).
    Json::Value search(const std::string& query, std::string* err) {
        Json::Value out = Json::Value::array();
        if (util::trim(query).empty()) return out;
        Json::Value vars = Json::Value::object();
        vars["q"] = query;
        std::string q = std::string("query($q:String){Page(perPage:12){media(search:$q,type:MANGA,sort:SEARCH_MATCH){") +
                        kMediaFields + " mediaListEntry{id status progress repeat updatedAt}}}}";
        GqlResult r = graphql(store_.token(), q, vars);
        if (r.netError) online_ = 0;
        if (!r.ok()) {
            if (err) *err = r.netError ? "Offline: AniList search needs Wi-Fi" : r.error;
            return out;
        }
        online_ = 1;
        for (const auto& m : r.data["Page"]["media"].asArray()) {
            Json::Value c = compactMedia(m);
            if (m["mediaListEntry"].isObject()) c["list_entry"] = m["mediaListEntry"];
            out.push(std::move(c));
        }
        return out;
    }

    // Search using every title we know for a source manga (deduplicated, best first).
    Json::Value searchForManga(const std::string& sid, const std::string& mid, std::string* err) {
        Json::Value info = hooks_.mangaInfo ? hooks_.mangaInfo(sid, mid) : Json::Value::object();
        auto titles = localTitleCandidates(info["title"].str(mid), mid, info["description"].str());
        std::vector<std::string> queries;
        for (const auto& t : titles) if (isMostlyLatin(t)) queries.push_back(t);
        for (const auto& t : titles) if (!isMostlyLatin(t)) queries.push_back(t);
        if (queries.size() > 3) queries.resize(3);
        Json::Value out = Json::Value::array();
        std::set<int64_t> seen;
        std::vector<Json::Value> exact, rest;
        for (const auto& q : queries) {
            std::string e;
            Json::Value res = search(q, &e);
            if (!e.empty() && err && err->empty()) *err = e;
            if (online_.load() == 0) break;
            for (const auto& m : res.asArray()) {
                int64_t id = m["media_id"].integer(0);
                if (seen.count(id)) continue;
                seen.insert(id);
                (titlesMatch(m, titles) ? exact : rest).push_back(m);
            }
            if (exact.size() == 1) break;
        }
        for (auto& m : exact) {
            m["exact"] = true;
            m["exact_primary"] = primaryTitleMatch(m, titles);
            out.push(m);
        }
        for (auto& m : rest) out.push(m);
        if (!out.empty() && err) err->clear();
        return out;
    }

    // ---------------------------------------------------------------- sync

    SyncOutcome sync() {
        SyncOutcome out;
        std::unique_lock<std::mutex> slk(syncMu_, std::try_to_lock);
        if (!slk.owns_lock()) return out;
        syncing_ = true;
        struct Reset { std::atomic<bool>& f; ~Reset() { f = false; } } reset{syncing_};
        std::string token = store_.token();
        if (token.empty()) return out;
        out.attempted = true;

        int64_t uid = store_.with([](const Json::Value& r) { return r["user"]["id"].integer(0); });
        if (uid == 0) {
            GqlResult v = graphql(token, "query{Viewer{id name}}", Json::Value::object());
            if (!fail(v, out)) return out;
            uid = v.data["Viewer"]["id"].integer(0);
            store_.mutate([&](Json::Value& r) {
                r["user"]["id"] = uid;
                r["user"]["name"] = v.data["Viewer"]["name"].str();
            });
        }

        // 1. Fetch the whole list (one request) = fresh remote state
        Json::Value vars = Json::Value::object();
        vars["u"] = uid;
        GqlResult lr = graphql(token, listQuery(), vars, 40);
        if (!fail(lr, out)) return out;
        Json::Value remote = Json::Value::object();
        for (const auto& l : lr.data["MediaListCollection"]["lists"].asArray()) {
            for (const auto& e : l["entries"].asArray()) {
                Json::Value c = entryFromRemote(e);
                std::string mid = std::to_string(c["media_id"].integer(0));
                if (mid != "0" && !remote.has(mid)) remote[mid] = std::move(c);
            }
        }
        store_.mutate([&](Json::Value& r) {
            for (auto& kv : remote.asObjectMut()) {
                std::string cp = r["entries"][kv.first]["cover_path"].str();
                if (!cp.empty()) kv.second["cover_path"] = cp;
            }
            r["entries"] = remote;
            r["list_fetched_at"] = util::nowSeconds();
        });

        // 2. Auto-link source mangas the user reads, then reconcile queued changes
        out.localChanged |= autoLink(token);
        if (!reconcile(token, out)) return out;

        // 3. Mirror (higher) AniList progress into local read marks
        std::vector<std::pair<std::string, Json::Value>> links;
        store_.with([&](const Json::Value& r) {
            for (const auto& kv : r["links"].asObject()) links.emplace_back(kv.first, kv.second);
            return 0;
        });
        for (auto& [mkey, l] : links) {
            int64_t prog = store_.with([&](const Json::Value& r) {
                return r["entries"][std::to_string(l["media_id"].integer(0))]["progress"].integer(0);
            });
            int64_t before = l["applied_progress"].integer(0);
            out.localChanged |= applyRemoteToLocal(mkey, l, prog);
            if (l["applied_progress"].integer(0) != before) {
                store_.mutate([&](Json::Value& r) {
                    if (r["links"].has(mkey)) r["links"][mkey]["applied_progress"] = l["applied_progress"].integer(0);
                });
            }
        }

        // 4. Cover thumbnails for the AniList tab (small JPGs, a few per sync)
        fetchCovers(24);

        store_.mutate([&](Json::Value& r) {
            r["last_sync_at"] = util::nowSeconds();
            r["last_error"] = "";
            r["needs_reauth"] = false;
        });
        out.ok = true;
        return out;
    }

private:
    // Returns false (and fills `out`) if the request failed.
    bool fail(const GqlResult& r, SyncOutcome& out) {
        if (r.ok()) { online_ = 1; return true; }
        if (r.netError) {
            online_ = 0;
            out.offline = true;
            out.error = "Offline";
            return false;
        }
        online_ = 1;
        out.error = r.error;
        if (r.httpStatus == 401) {
            out.authError = true;
            store_.mutate([&](Json::Value& root) {
                root["needs_reauth"] = true;
                root["last_error"] = "AniList login expired, please reconnect";
            });
        } else {
            store_.mutate([&](Json::Value& root) { root["last_error"] = r.error; });
        }
        return false;
    }

    bool reconcile(const std::string& token, SyncOutcome& out) {
        Json::Value pending = store_.with([](const Json::Value& r) { return r["pending"]; });
        for (const auto& kv : pending.asObject()) {
            const std::string& mid = kv.first;
            const Json::Value& p = kv.second;
            Json::Value r, linkTotal;
            int64_t total = 0;
            store_.with([&](const Json::Value& root) {
                r = root["entries"][mid];
                total = totalChapters(root, mid);
                return 0;
            });
            bool present = r.isObject();
            bool basePresent = p["base_present"].boolean(false);
            bool remoteChanged = present ? (!basePresent || r["updated_at"].integer(0) != p["base_updated_at"].integer(0))
                                         : basePresent;
            int64_t want = p["progress"].integer(0);
            int64_t rp = r["progress"].integer(0);
            std::string rs = r["status"].str();
            bool isExplicit = p["explicit"].boolean(false);
            std::string wantStatus = p["status"].str();

            enum { PUSH, DROP, CONFLICT } action;
            std::string reason;
            if (isExplicit) {
                if (present && rp == want && (wantStatus.empty() || rs == wantStatus)) action = DROP;
                else if (!remoteChanged) action = PUSH;
                else { action = CONFLICT; reason = "explicit"; }
            } else if (present && rp >= want) {
                action = DROP;
            } else if (!remoteChanged) {
                action = PUSH;
            } else if (present && rp >= p["base_progress"].integer(0) && (rs == "CURRENT" || rs == "PLANNING")) {
                action = PUSH;
            } else {
                action = CONFLICT;
                reason = !present ? "removed" : (rp < p["base_progress"].integer(0) ? "went_back" : "status");
            }

            if (action == PUSH && !isExplicit && p["needs_confirm"].boolean(false)) {
                action = CONFLICT;
                reason = "status";
            }
            if (action == DROP) {
                dropPending(mid, p["rev"].integer(0));
                continue;
            }
            if (action == CONFLICT) {
                bool isNew = store_.with([&](const Json::Value& root) { return !root["conflicts"].has(mid); });
                store_.mutate([&](Json::Value& root) {
                    Json::Value c = Json::Value::object();
                    c["media_id"] = static_cast<int64_t>(std::atoll(mid.c_str()));
                    c["title"] = r["title"].str(p["title"].str());
                    c["reason"] = reason;
                    c["local_progress"] = want;
                    c["local_status"] = isExplicit ? wantStatus : deriveStatus(rs, want, total);
                    c["local_status_label"] = statusLabel(c["local_status"].str());
                    c["remote_present"] = present;
                    c["remote_progress"] = rp;
                    c["remote_status"] = rs;
                    c["remote_status_label"] = statusLabel(rs);
                    c["chapters"] = total;
                    root["conflicts"][mid] = c;
                });
                if (isNew) ++out.newConflicts;
                continue;
            }

            // PUSH
            std::string status = isExplicit && !wantStatus.empty() ? wantStatus : deriveStatus(rs, want, total);
            Json::Value vars = Json::Value::object();
            vars["m"] = static_cast<int64_t>(std::atoll(mid.c_str()));
            vars["p"] = want;
            vars["s"] = status;
            GqlResult mr = graphql(token,
                                   "mutation($m:Int,$p:Int,$s:MediaListStatus){SaveMediaListEntry(mediaId:$m,progress:$p,status:$s){"
                                   "id status progress repeat updatedAt}}",
                                   vars);
            if (mr.netError || mr.httpStatus == 401 || mr.httpStatus == 429 || mr.httpStatus >= 500) {
                fail(mr, out);
                if (mr.httpStatus == 429 || mr.httpStatus >= 500) out.error = mr.error;
                return false;
            }
            if (!mr.ok()) {
                // Permanent error for this entry (e.g. media removed): let the user decide
                store_.mutate([&](Json::Value& root) {
                    Json::Value c = Json::Value::object();
                    c["media_id"] = static_cast<int64_t>(std::atoll(mid.c_str()));
                    c["title"] = r["title"].str(p["title"].str());
                    c["reason"] = "error";
                    c["message"] = mr.error;
                    c["local_progress"] = want;
                    c["local_status"] = status;
                    c["local_status_label"] = statusLabel(status);
                    c["remote_present"] = present;
                    c["remote_progress"] = rp;
                    c["remote_status"] = rs;
                    c["remote_status_label"] = statusLabel(rs);
                    c["chapters"] = total;
                    root["conflicts"][mid] = c;
                });
                ++out.newConflicts;
                continue;
            }
            const Json::Value& saved = mr.data["SaveMediaListEntry"];
            store_.mutate([&](Json::Value& root) {
                Json::Value& e = root["entries"][mid];
                if (!e.isObject()) {
                    e = Json::Value::object();
                    e["media_id"] = static_cast<int64_t>(std::atoll(mid.c_str()));
                    for (const auto& lkv : root["links"].asObject()) {
                        if (std::to_string(lkv.second["media_id"].integer(0)) == mid) {
                            e["title"] = lkv.second["title"].str();
                            e["chapters"] = lkv.second["chapters"].integer(0);
                            e["cover_url"] = lkv.second["cover_url"].str();
                            break;
                        }
                    }
                    if (e["title"].str().empty()) e["title"] = p["title"].str();
                }
                e["entry_id"] = saved["id"].integer(0);
                e["status"] = saved["status"].str();
                e["progress"] = saved["progress"].integer(0);
                e["repeat"] = saved["repeat"].integer(0);
                e["updated_at"] = saved["updatedAt"].integer(0);
                root["conflicts"].erase(mid);
                const Json::Value& croot = root;
                if (croot["pending"][mid]["rev"].integer(0) == p["rev"].integer(0)) root["pending"].erase(mid);
                // Our own push must not be mirrored back as "remote progress"
                for (auto& lkv : root["links"].asObjectMut()) {
                    if (std::to_string(lkv.second["media_id"].integer(0)) == mid) {
                        lkv.second["applied_progress"] = std::max(lkv.second["applied_progress"].integer(0),
                                                                  saved["progress"].integer(0));
                    }
                }
            });
            ++out.pushed;
        }
        return true;
    }

    void dropPending(const std::string& mid, int64_t rev) {
        store_.mutate([&](Json::Value& root) {
            const Json::Value& cr = root;
            if (cr["pending"][mid]["rev"].integer(0) == rev) root["pending"].erase(mid);
            root["conflicts"].erase(mid);
        });
    }

    bool autoLink(const std::string& token) {
        if (!hooks_.autoLinkCandidates) return false;
        bool changed = false;
        int netBudget = 4;  // AniList allows ~30-90 req/min; keep background linking gentle
        for (const auto& c : hooks_.autoLinkCandidates()) {
            std::string sid = c["source_id"].str();
            std::string mid = c["manga_id"].str();
            std::string mkey = mangaKey(sid, mid);
            std::string title = c["title"].str(mid);
            bool skip = store_.with([&](const Json::Value& r) {
                if (r["links"].has(mkey)) return true;
                const Json::Value& u = r["unsure"][mkey];
                return u.isObject() && (u["manual"].boolean(false) || u["title"].str() == title);
            });
            if (skip) continue;
            {
                std::lock_guard<std::mutex> lk(sessionMu_);
                if (autoLinkTried_.count(mkey)) continue;
            }

            auto titles = localTitleCandidates(title, mid, c["description"].str());
            Json::Value chosen;

            // a) exactly one title match on the user's own list (no network needed)
            std::vector<Json::Value> hits;
            store_.with([&](const Json::Value& r) {
                for (const auto& kv : r["entries"].asObject()) {
                    if (kv.second["format"].str() == "NOVEL") continue;
                    if (titlesMatch(kv.second, titles)) hits.push_back(kv.second);
                }
                return 0;
            });
            if (hits.size() == 1) chosen = hits[0];
            if (hits.size() > 1) {
                std::vector<Json::Value> primary;
                for (const auto& h : hits) if (primaryTitleMatch(h, titles)) primary.push_back(h);
                if (primary.size() == 1) chosen = primary[0];
            }

            if (!chosen.isObject()) {
                if (netBudget <= 0) continue;  // try again on a later sync
                {
                    std::lock_guard<std::mutex> lk(sessionMu_);
                    autoLinkTried_.insert(mkey);
                }
                // b) MangaDex knows the AniList id
                if (sid == "mangadex" && hooks_.mangadexAniListId) {
                    --netBudget;
                    int64_t alId = hooks_.mangadexAniListId(mid);
                    if (alId > 0) {
                        Json::Value e = entryFor(alId);
                        if (e.isObject()) {
                            chosen = e;
                        } else {
                            --netBudget;
                            Json::Value vars = Json::Value::object();
                            vars["id"] = alId;
                            GqlResult r = graphql(token, std::string("query($id:Int){Media(id:$id,type:MANGA){") +
                                                             kMediaFields + " mediaListEntry{id status progress repeat updatedAt}}}",
                                                  vars);
                            if (r.ok() && r.data["Media"].isObject()) {
                                chosen = compactMedia(r.data["Media"]);
                                if (r.data["Media"]["mediaListEntry"].isObject()) {
                                    chosen["list_entry"] = r.data["Media"]["mediaListEntry"];
                                }
                            }
                        }
                    }
                }
                // c) exactly one exact title match in AniList search
                if (!chosen.isObject()) {
                    --netBudget;
                    std::string err;
                    Json::Value res = searchForManga(sid, mid, &err);
                    if (online_.load() == 0) {
                        std::lock_guard<std::mutex> lk(sessionMu_);
                        autoLinkTried_.erase(mkey);
                        break;
                    }
                    if (!err.empty() && res.empty()) continue;  // transient error: retry next session
                    std::vector<Json::Value> exact, primary, onList;
                    for (const auto& m : res.asArray()) {
                        if (m["format"].str() == "NOVEL" || !m["exact"].boolean(false)) continue;
                        exact.push_back(m);
                        if (m["exact_primary"].boolean(false)) primary.push_back(m);
                        if (m["list_entry"].isObject()) onList.push_back(m);
                    }
                    if (exact.size() == 1) chosen = exact[0];
                    else if (onList.size() == 1) chosen = onList[0];
                    else if (primary.size() == 1) chosen = primary[0];
                }
            }

            if (chosen.isObject()) {
                changed |= link(sid, mid, chosen, true);
            } else {
                // Not sure: the manga page offers "Link AniList" instead of us guessing
                store_.mutate([&](Json::Value& r) {
                    r["unsure"][mkey] = Json::Value::object();
                    r["unsure"][mkey]["title"] = title;
                    r["unsure"][mkey]["at"] = util::nowSeconds();
                });
            }
        }
        return changed;
    }

    void fetchCovers(int maxCount) {
        if (!hooks_.downloadFile) return;
        std::vector<std::pair<std::string, std::string>> todo;
        store_.with([&](const Json::Value& r) {
            for (const auto& kv : r["entries"].asObject()) {
                std::string cp = kv.second["cover_path"].str();
                if (!cp.empty()) {
                    std::error_code ec;
                    if (cp.rfind("file://", 0) == 0 && fs::exists(cp.substr(7), ec)) continue;
                }
                std::string url = kv.second["cover_url"].str();
                if (!url.empty()) todo.emplace_back(kv.first, url);
                if (static_cast<int>(todo.size()) >= maxCount) break;
            }
            return 0;
        });
        for (const auto& [mid, url] : todo) {
            fs::path dest = coversDir_ / ("al_" + mid + ".jpg");
            std::error_code ec;
            bool ok = (fs::exists(dest, ec) && fs::file_size(dest, ec) > 128) || hooks_.downloadFile(url, dest);
            if (!ok) continue;
            std::string uri = "file://" + fs::absolute(dest, ec).string();
            store_.mutate([&](Json::Value& r) {
                if (r["entries"].has(mid)) r["entries"][mid]["cover_path"] = uri;
            });
        }
    }
};

}  // namespace anilist
