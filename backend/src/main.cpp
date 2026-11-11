#include "aidoku_client.hpp"
#include "aidoku_db.hpp"
#include "aidokurun_client.hpp"
#include "anilist_setup.hpp"
#include "anilist_tracker.hpp"
#include "appload_ipc.hpp"
#include "cbz_extractor.hpp"
#include "json.hpp"
#include "mangadex_client.hpp"
#include "state_store.hpp"
#include "util.hpp"
#include "third_party/qrcodegen.cpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <future>
#include <filesystem>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Frontend -> Backend request message types
constexpr int32_t REQ_BOOTSTRAP = 1;
constexpr int32_t REQ_REFRESH_LIBRARY = 2;
constexpr int32_t REQ_SEARCH = 3;
constexpr int32_t REQ_MANGA_DETAILS = 4;
constexpr int32_t REQ_TOGGLE_LIBRARY = 5;
constexpr int32_t REQ_OPEN_CHAPTER = 6;
constexpr int32_t REQ_SAVE_PROGRESS = 7;
constexpr int32_t REQ_MARK_CHAPTER_READ = 8;
constexpr int32_t REQ_DOWNLOAD_CHAPTERS = 9;
constexpr int32_t REQ_CANCEL_DOWNLOAD = 10;
constexpr int32_t REQ_DELETE_DOWNLOAD = 11;
constexpr int32_t REQ_LIST_SOURCES = 12;
constexpr int32_t REQ_INSTALL_SOURCE = 13;
constexpr int32_t REQ_UPDATE_SETTINGS = 14;
constexpr int32_t REQ_SAVE_SOURCE_CONFIG = 15;
constexpr int32_t REQ_ANILIST_STATE = 20;
constexpr int32_t REQ_ANILIST_SYNC = 21;
constexpr int32_t REQ_ANILIST_CONNECT = 22;
constexpr int32_t REQ_ANILIST_LOGOUT = 23;
constexpr int32_t REQ_ANILIST_SEARCH = 24;
constexpr int32_t REQ_ANILIST_LINK = 25;
constexpr int32_t REQ_ANILIST_UPDATE = 26;
constexpr int32_t REQ_ANILIST_RESOLVE = 27;
constexpr int32_t REQ_ANILIST_FIND_SOURCES = 28;
constexpr int32_t REQ_ANILIST_MANGA_INFO = 29;
constexpr int32_t REQ_OPEN_LINK = 30;

// Backend -> Frontend event message types
constexpr int32_t EVT_BOOTSTRAP = 100;
constexpr int32_t EVT_LIBRARY_UPDATED = 101;
constexpr int32_t EVT_SEARCH_RESULTS = 102;
constexpr int32_t EVT_MANGA_DETAILS = 103;
constexpr int32_t EVT_READER_LOADING = 104;
constexpr int32_t EVT_CHAPTER_PAGES = 105;
constexpr int32_t EVT_DOWNLOADS_UPDATED = 106;
constexpr int32_t EVT_SOURCES_LIST = 107;
constexpr int32_t EVT_COVER_UPDATED = 108;
constexpr int32_t EVT_TOAST = 109;
constexpr int32_t EVT_ANILIST_STATE = 120;
constexpr int32_t EVT_ANILIST_CONNECT = 121;
constexpr int32_t EVT_ANILIST_SEARCH = 122;
constexpr int32_t EVT_MANGA_ANILIST = 123;
constexpr int32_t EVT_ANILIST_SOURCES = 124;
constexpr int32_t EVT_OPEN_LINK = 125;

struct DownloadTask {
    std::string key;
    std::string sourceId;
    std::string sourceName;
    std::string mangaId;
    std::string mangaTitle;
    std::string chapterId;
    double chapterNum = 0.0;
    bool hasChapterNum = false;
    std::string chapterTitle;
    std::string status = "Queued";
    int progressPct = 0;
};

struct CoverTask {
    std::string sourceId;
    std::string mangaId;
    std::string title;
    std::string coverUrl;
};

class Rm2MangaApp {
    appload::IpcConnection ipc_;
    store::StateStore state_;
    aidoku::Client aidoku_;
    aidoku::CoverDb coverDb_;
    mangadex::Client mangadex_;
    aidokurun::Client arun_;
    extractor::PageCache pageCache_;
    std::unique_ptr<anilist::Tracker> anilist_;
    std::unique_ptr<anilist::SetupServer> alSetup_;

    // AniList background sync worker
    std::mutex alMu_;
    std::condition_variable alCv_;
    std::chrono::steady_clock::time_point alDueAt_ = std::chrono::steady_clock::time_point::max();
    bool alManual_ = false;
    std::thread alWorker_;

    std::atomic<bool> shuttingDown_{false};
    std::atomic<bool> shutdownJoined_{false};
    std::atomic<uint64_t> searchSeq_{0};
    std::atomic<uint64_t> readerSeq_{0};

    // Short-lived discover cache so switching installed source pills is instant
    std::mutex discoverMu_;
    Json::Value discoverCache_ = Json::Value::array();
    int64_t discoverCacheTs_ = 0;

    // Pinned chapter slugs in /tmp/rm2manga_pages (current + preloaded next)
    std::mutex pinMu_;
    std::string activeChapterSlug_;
    std::string preloadedChapterSlug_;
    std::set<std::string> preloadingKeys_;

    // Download queue
    std::mutex dlMu_;
    std::condition_variable dlCv_;
    std::deque<DownloadTask> dlQueue_;
    std::unique_ptr<DownloadTask> activeDl_;
    std::atomic<bool> cancelActiveDl_{false};
    std::thread dlWorker_;

    // Cover art queue
    std::mutex coverMu_;
    std::condition_variable coverCv_;
    std::deque<CoverTask> coverQueue_;
    std::set<std::string> attemptedCovers_;
    std::thread coverWorker_;

    // Track request worker threads for deterministic join on teardown
    struct WorkerSlot {
        std::thread th;
        std::atomic<bool> done{false};
    };
    std::mutex asyncMu_;
    std::list<WorkerSlot> workers_;
    bool disableCovers_ = (std::getenv("RM2MANGA_DISABLE_COVERS") != nullptr);

public:
    explicit Rm2MangaApp(const fs::path& workDir)
        : state_(workDir / "data"),
          aidoku_(workDir / "data" / "aidoku"),
          coverDb_(fs::path(aidoku_.homeDir()) / "database.db"),
          arun_(workDir / "data" / "aidoku-run", workDir / "backend" / "aidoku-run"),
          pageCache_(std::getenv("RM2MANGA_PAGE_CACHE_DIR")
                         ? std::getenv("RM2MANGA_PAGE_CACHE_DIR")
                         : "/tmp/rm2manga_pages") {
        applyLanguages();
        initAniList(workDir / "data");
    }

    ~Rm2MangaApp() {
        shutdown();
    }

    bool start(const std::string& socketPath) {
        if (!ipc_.connectTo(socketPath)) {
            return false;
        }
        dlWorker_ = std::thread([this]() { downloadLoop(); });
        coverWorker_ = std::thread([this]() { coverLoop(); });
        alWorker_ = std::thread([this]() { anilistLoop(); });
        return true;
    }

    void run() {
        ipc_.runLoop([this](int32_t msgType, const std::string& contents) {
            handleIncoming(msgType, contents);
        });
        shutdown();
    }

    void shutdown() {
        shuttingDown_ = true;
        bool expected = false;
        if (!shutdownJoined_.compare_exchange_strong(expected, true)) return;

        cancelActiveDl_ = true;
        dlCv_.notify_all();
        coverCv_.notify_all();
        alCv_.notify_all();
        if (alSetup_) alSetup_->stop();
        ipc_.closeSocket();

        if (dlWorker_.joinable()) dlWorker_.join();
        if (coverWorker_.joinable()) coverWorker_.join();
        if (alWorker_.joinable()) alWorker_.join();

        while (true) {
            std::list<WorkerSlot> batch;
            {
                std::lock_guard<std::mutex> lock(asyncMu_);
                if (workers_.empty()) break;
                batch.splice(batch.end(), workers_);
            }
            for (auto& w : batch) {
                if (w.th.joinable()) {
                    w.th.join();
                }
            }
        }
    }

private:
    void spawnAsync(std::function<void()> fn) {
        if (shuttingDown_.load()) return;

        std::list<WorkerSlot> finished;
        {
            std::lock_guard<std::mutex> lock(asyncMu_);
            if (shuttingDown_.load()) return;

            for (auto it = workers_.begin(); it != workers_.end();) {
                if (it->done.load() && it->th.get_id() != std::this_thread::get_id()) {
                    auto cur = it++;
                    finished.splice(finished.end(), workers_, cur);
                } else {
                    ++it;
                }
            }

            workers_.emplace_back();
            WorkerSlot& slot = workers_.back();
            slot.th = std::thread([this, &slot, fn = std::move(fn)]() {
                if (!shuttingDown_.load()) {
                    fn();
                }
                slot.done.store(true);
            });
        }

        for (auto& w : finished) {
            if (w.th.joinable()) {
                w.th.join();
            }
        }
    }

    // ------------------------------------------------------------------ AniList

    void initAniList(const fs::path& dataDir) {
        anilist::Hooks h;
        h.maxReadChapter = [this](const std::string& sid, const std::string& mid) {
            return state_.maxReadChapter(sid, mid);
        };
        h.markReadUpTo = [this](const std::string& sid, const std::string& mid, int progress) {
            return state_.markReadUpTo(sid, mid, progress);
        };
        h.hasCachedChapters = [this](const std::string& sid, const std::string& mid) {
            return !state_.getCachedChapters(sid, mid).empty();
        };
        h.autoLinkCandidates = [this]() { return state_.listTrackCandidates(); };
        h.mangadexAniListId = [this](const std::string& mid) { return mangadex_.getAniListId(mid); };
        h.mangaInfo = [this](const std::string& sid, const std::string& mid) {
            Json::Value m = state_.getManga(sid, mid);
            Json::Value o = Json::Value::object();
            o["title"] = m["title"].str(mid);
            o["source_name"] = m["source_name"].str(sid);
            o["description"] = m["description"].str();
            return o;
        };
        h.downloadFile = [](const std::string& url, const fs::path& dest) {
            return util::httpsDownloadFile(url, dest, 20);
        };
        anilist_ = std::make_unique<anilist::Tracker>(dataDir, std::move(h));

        anilist::SetupServer::Callbacks cb;
        cb.clientId = [this]() { return anilist_->clientId(); };
        cb.setClientId = [this](const std::string& id) {
            anilist_->setClientId(id);
            pushAnilistConnect();
        };
        cb.submitToken = [this](const std::string& tok, std::string* user) {
            return anilist_->connect(tok, user);
        };
        cb.onConnected = [this](const std::string& user) {
            sendToast("AniList connected as " + user);
            Json::Value ev = Json::Value::object();
            ev["running"] = false;
            ev["connected"] = true;
            ipc_.sendJson(EVT_ANILIST_CONNECT, ev);
            pushAnilistState();
            requestAnilistSync(0, true);
        };
        alSetup_ = std::make_unique<anilist::SetupServer>(std::move(cb));
    }

    void requestAnilistSync(int delaySec, bool manual = false) {
        if (!anilist_ || !anilist_->connected()) return;
        std::lock_guard<std::mutex> lk(alMu_);
        auto due = std::chrono::steady_clock::now() + std::chrono::seconds(delaySec);
        if (due < alDueAt_) alDueAt_ = due;
        if (manual) alManual_ = true;
        alCv_.notify_all();
    }

    void anilistLoop() {
        int backoff = 60;
        while (!shuttingDown_.load()) {
            bool manual = false;
            {
                std::unique_lock<std::mutex> lk(alMu_);
                alCv_.wait_until(lk, alDueAt_ == std::chrono::steady_clock::time_point::max()
                                         ? std::chrono::steady_clock::now() + std::chrono::hours(1)
                                         : alDueAt_,
                                 [this]() {
                                     return shuttingDown_.load() || std::chrono::steady_clock::now() >= alDueAt_;
                                 });
                if (shuttingDown_.load()) break;
                if (std::chrono::steady_clock::now() < alDueAt_) continue;
                alDueAt_ = std::chrono::steady_clock::time_point::max();
                manual = alManual_;
                alManual_ = false;
            }
            if (!anilist_->connected()) continue;

            anilist_->markSyncing();
            pushAnilistState();  // shows "Syncing..."
            anilist::SyncOutcome r = anilist_->sync();
            if (shuttingDown_.load()) break;

            int nextIn;
            if (r.ok) {
                backoff = 60;
                nextIn = 30 * 60;  // refresh the list periodically while the app is open
                if (manual && r.newConflicts == 0) {
                    sendToast(r.pushed > 0 ? ("AniList synced (" + std::to_string(r.pushed) + " updated)")
                                           : "AniList is up to date");
                }
            } else if (r.authError) {
                nextIn = -1;  // wait for reconnect
                sendToast("AniList login expired: reconnect in the AniList tab", true);
            } else {
                // Offline / rate limited / server error: retry with backoff (cheap when Wi-Fi is off)
                nextIn = backoff;
                backoff = std::min(backoff * 2, 15 * 60);
                if (manual) sendToast(r.offline ? "AniList: offline, changes are saved and will sync later"
                                                : ("AniList: " + r.error), true);
            }
            if (r.newConflicts > 0) {
                sendToast("AniList: " + std::to_string(r.newConflicts) +
                          (r.newConflicts == 1 ? " entry needs" : " entries need") + " your decision");
            }
            if (r.localChanged) pushLibraryEvent();
            Json::Value st = anilist_->listState();
            st["local_changed"] = r.localChanged;
            ipc_.sendJson(EVT_ANILIST_STATE, st);
            if (nextIn > 0) requestAnilistSync(nextIn);
        }
    }

    void pushAnilistState() {
        if (!anilist_) return;
        ipc_.sendJson(EVT_ANILIST_STATE, anilist_->listState());
    }

    void pushAnilistConnect(const std::string& error = "") {
        Json::Value ev = Json::Value::object();
        bool running = alSetup_ && alSetup_->running();
        ev["running"] = running;
        ev["connected"] = anilist_->connected();
        ev["client_id_set"] = !anilist_->clientId().empty();
        ev["error"] = error;
        Json::Value urls = Json::Value::array();
        if (running) {
            for (const auto& u : alSetup_->urls()) urls.push(u);
            if (!urls.empty()) ev["qr"] = anilist::SetupServer::qrRows(urls[size_t(0)].str());
        }
        ev["urls"] = urls;
        ipc_.sendJson(EVT_ANILIST_CONNECT, ev);
    }

    void pushMangaAnilist(const std::string& sid, const std::string& mid) {
        Json::Value ev = Json::Value::object();
        ev["source_id"] = sid;
        ev["manga_id"] = mid;
        ev["anilist"] = anilist_->detailInfo(sid, mid);
        ipc_.sendJson(EVT_MANGA_ANILIST, ev);
    }

    void onLocalChapterRead(const std::string& sid, const std::string& mid, double chapterNum) {
        if (!anilist_ || !anilist_->connected()) return;
        if (anilist_->onChapterRead(sid, mid, chapterNum)) {
            requestAnilistSync(8);  // short delay batches consecutive chapters
            pushAnilistState();
        }
    }

    // Searches the selected installed sources for an AniList entry's titles (AniList tab).
    // `probe` also asks each hit how many chapters it has, so entries whose chapters are
    // hidden (MangaLib licensed titles, region blocks, ...) are obvious before opening one.
    Json::Value searchSourcesForTitles(const std::vector<std::string>& titles,
                                       const std::vector<std::string>& onlySources,
                                       bool probe, std::string* errOut) {
        Json::Value out = Json::Value::array();
        std::set<std::string> seen;
        std::set<std::string> wanted(onlySources.begin(), onlySources.end());
        auto allowed = [&](const std::string& sid) {
            return wanted.empty() || wanted.count(sid) > 0;
        };
        auto addAll = [&](const Json::Value& items) {
            for (const auto& m : items.asArray()) {
                std::string sid = m["source"]["id"].str(m["source_id"].str());
                if (sid == "multi.mangadex" || !allowed(sid)) continue;
                std::string mid = m["id"].str(m["manga_id"].str());
                if (sid.empty() || mid.empty() || !seen.insert(sid + "::" + mid).second) continue;
                Json::Value it = Json::Value::object();
                it["source_id"] = sid;
                it["source_name"] = m["source"]["name"].str(m["source_name"].str(sid));
                it["id"] = mid;
                it["title"] = m["title"].str(mid);
                it["cover_url"] = m["cover_url"].str();
                it["author"] = m["author"].str();
                std::string d = m["description"].str();
                if (d.size() > 240) {
                    size_t cut = 240;
                    while (cut > 0 && (static_cast<unsigned char>(d[cut]) & 0xC0) == 0x80) --cut;
                    d = d.substr(0, cut) + "...";
                }
                it["description"] = d;
                Json::Value stored = state_.getManga(sid, mid);
                it["in_library"] = stored["in_library"].boolean(false);
                if (!stored["cover_path"].str().empty()) it["cover_path"] = stored["cover_path"].str();
                it["chapter_count"] = -1;  // unknown
                out.push(std::move(it));
            }
        };

        bool legacyOnline = aidoku_.hasLegacySources() && aidoku_.ensureRunning();
        bool wantMangadex = allowed("mangadex");
        for (size_t i = 0; i < titles.size() && i < 2; ++i) {
            const std::string& q = titles[i];
            if (util::trim(q).empty()) continue;
            std::string v2Err, mdErr, aiErr;
            std::future<Json::Value> v2;
            if (arun_.available() && (wanted.empty() || std::any_of(wanted.begin(), wanted.end(), aidokurun::isV2Source))) {
                std::vector<std::string> only;
                for (const auto& w : wanted) if (aidokurun::isV2Source(w)) only.push_back(w);
                v2 = std::async(std::launch::async, [this, q, only, &v2Err]() {
                    return arun_.search(q, only.size() == 1 ? only[0] : "", &v2Err);
                });
            }
            if (wantMangadex) addAll(mangadex_.queryMangaList(q, "search", &mdErr));
            if (legacyOnline && (wanted.empty() || std::any_of(wanted.begin(), wanted.end(),
                                                               [](const std::string& w) { return !aidokurun::isV2Source(w); }))) {
                addAll(aidoku_.searchMangas(q, &aiErr));
            }
            if (v2.valid()) addAll(v2.get());
            if (errOut && errOut->empty() && out.empty()) {
                *errOut = !mdErr.empty() ? mdErr : (!v2Err.empty() ? v2Err : aiErr);
            }
        }

        if (probe && !out.empty()) {
            // Cap the probing: each runs the source's wasm in its own process (~30MB).
            size_t cap = std::min<size_t>(out.size(), 6);
            std::vector<std::thread> ths;
            for (size_t i = 0; i < cap; ++i) {
                ths.emplace_back([&, i]() {
                    std::string sid = out[i]["source_id"].str();
                    std::string mid = out[i]["id"].str();
                    int n = -1;
                    if (aidokurun::isV2Source(sid)) {
                        n = arun_.chapterCount(sid, mid, nullptr);
                    } else if (sid == "mangadex") {
                        n = static_cast<int>(mangadex_.getChapters(mid, nullptr).size());
                    }
                    out.asArrayMut()[i]["chapter_count"] = n;
                });
            }
            for (auto& t : ths) t.join();
            // Usable entries first, then the rest (keep discovery order inside each group)
            std::stable_sort(out.asArrayMut().begin(), out.asArrayMut().end(),
                             [](const Json::Value& a, const Json::Value& b) {
                                 int ca = static_cast<int>(a["chapter_count"].integer(-1));
                                 int cb = static_cast<int>(b["chapter_count"].integer(-1));
                                 bool ua = ca != 0, ub = cb != 0;
                                 return ua && !ub;
                             });
        }
        if (!out.empty() && errOut) errOut->clear();
        return out;
    }

    void handleAniList(int32_t msgType, const Json::Value& req) {
        switch (msgType) {
            case REQ_ANILIST_STATE:
                pushAnilistState();
                break;
            case REQ_ANILIST_SYNC:
                if (!anilist_->connected()) break;
                requestAnilistSync(0, true);
                break;
            case REQ_ANILIST_CONNECT: {
                if (req["start"].boolean(true)) {
                    std::string err;
                    if (!alSetup_->start()) err = "Could not open a local port for the setup page";
                    else if (alSetup_->urls().empty()) err = "Turn on Wi-Fi so your phone can reach the reMarkable";
                    pushAnilistConnect(err);
                } else {
                    alSetup_->stop();
                    pushAnilistConnect();
                }
                break;
            }
            case REQ_ANILIST_LOGOUT:
                anilist_->logout();
                pushAnilistState();
                sendToast("Disconnected from AniList");
                break;
            case REQ_ANILIST_SEARCH: {
                std::string q = util::trim(req["query"].str());
                std::string sid = req["source_id"].str();
                std::string mid = req["manga_id"].str();
                spawnAsync([this, q, sid, mid]() {
                    std::string err;
                    Json::Value res = q.empty() ? anilist_->searchForManga(sid, mid, &err)
                                                : anilist_->search(q, &err);
                    for (auto& m : res.asArrayMut()) {
                        if (m["list_entry"].isObject()) {
                            m["list_status_label"] = anilist::statusLabel(m["list_entry"]["status"].str());
                            m["list_progress"] = m["list_entry"]["progress"].integer(0);
                        }
                    }
                    Json::Value ev = Json::Value::object();
                    ev["source_id"] = sid;
                    ev["manga_id"] = mid;
                    ev["query"] = q;
                    ev["results"] = res;
                    ev["error"] = (res.empty() && err.empty()) ? "No matches on AniList" : err;
                    ipc_.sendJson(EVT_ANILIST_SEARCH, ev);
                });
                break;
            }
            case REQ_ANILIST_LINK: {
                std::string sid = req["source_id"].str();
                std::string mid = req["manga_id"].str();
                Json::Value media = req["media"];
                if (sid.empty() || mid.empty()) break;
                // Make sure the manga exists in the local store (linking from the AniList tab)
                if (!req["manga_title"].str().empty()) {
                    state_.upsertMangaMeta(sid, req["source_name"].str(sid), mid, req["manga_title"].str(),
                                           req["cover_url"].str(), req["author"].str(), req["description"].str());
                }
                bool changed = false;
                if (media.isObject() && media["media_id"].integer(0) > 0) {
                    changed = anilist_->link(sid, mid, media, false);
                    sendToast("Linked to AniList: " + media["title"].str());
                } else {
                    anilist_->unlink(sid, mid);
                    sendToast("AniList link removed");
                }
                if (changed) pushLibraryEvent();
                pushMangaAnilist(sid, mid);
                Json::Value st = anilist_->listState();
                st["local_changed"] = changed;
                ipc_.sendJson(EVT_ANILIST_STATE, st);
                requestAnilistSync(2);
                break;
            }
            case REQ_ANILIST_UPDATE: {
                int64_t media = req["media_id"].integer(0);
                if (media <= 0) break;
                anilist_->explicitUpdate(media, req["progress"].integer(0), req["status"].str());
                if (!req["source_id"].str().empty()) pushMangaAnilist(req["source_id"].str(), req["manga_id"].str());
                pushAnilistState();
                requestAnilistSync(1);
                break;
            }
            case REQ_ANILIST_RESOLVE: {
                int64_t media = req["media_id"].integer(0);
                if (media <= 0) break;
                std::string choice = req["choice"].str("remote");
                anilist_->resolveConflict(media, choice);
                pushLibraryEvent();
                Json::Value st = anilist_->listState();
                st["local_changed"] = true;
                ipc_.sendJson(EVT_ANILIST_STATE, st);
                if (choice == "local") requestAnilistSync(1);
                break;
            }
            case REQ_ANILIST_FIND_SOURCES: {
                int64_t media = req["media_id"].integer(0);
                std::vector<std::string> titles;
                for (const auto& t : req["titles"].asArray()) {
                    std::string s = util::trim(t.str());
                    if (s.empty()) continue;
                    bool dup = false;
                    for (const auto& e : titles) dup |= anilist::normalizeTitle(e) == anilist::normalizeTitle(s);
                    if (!dup) titles.push_back(s);
                }
                // The chosen source filter is persisted so it is remembered next time
                std::vector<std::string> only;
                if (req.has("sources")) {
                    for (const auto& s : req["sources"].asArray()) {
                        if (!s.str().empty()) only.push_back(s.str());
                    }
                    anilist_->setSearchSources(only);
                } else {
                    only = anilist_->searchSources();
                }
                bool probe = req.has("probe") ? req["probe"].boolean(true) : true;
                spawnAsync([this, media, titles, only, probe]() {
                    std::string err;
                    Json::Value res = searchSourcesForTitles(titles, only, probe, &err);
                    Json::Value ev = Json::Value::object();
                    ev["media_id"] = media;
                    ev["results"] = res;
                    Json::Value sel = Json::Value::array();
                    for (const auto& sid : only) sel.push(sid);
                    ev["sources"] = sel;
                    ev["probed"] = probe;
                    ev["error"] = res.empty() ? (err.empty() ? "Not found on your selected sources" : err) : "";
                    ipc_.sendJson(EVT_ANILIST_SOURCES, ev);
                });
                break;
            }
            case REQ_ANILIST_MANGA_INFO:
                pushMangaAnilist(req["source_id"].str(), req["manga_id"].str());
                break;
            default:
                break;
        }
    }

    // "Open a link": many sources hide entries from search (MangaLib licensed/removed titles,
    // age restricted ones, ...) while the entry itself may still be reachable. Accepts a full
    // URL, a "<id>--<slug>" key, a bare slug, or a MangaDex UUID.
    Json::Value resolveLink(const std::string& rawText, std::string* errOut) {
        Json::Value out = Json::Value::object();
        std::string text = util::trim(rawText);
        if (text.empty()) {
            if (errOut) *errOut = "Nothing to open";
            return out;
        }

        // MangaDex UUID (bare or in a mangadex.org URL)
        std::string uuid;
        for (size_t i = 0; i + 36 <= text.size(); ++i) {
            bool okUuid = true;
            for (size_t k = 0; k < 36 && okUuid; ++k) {
                char c = text[i + k];
                if (k == 8 || k == 13 || k == 18 || k == 23) okUuid = (c == '-');
                else okUuid = std::isxdigit(static_cast<unsigned char>(c)) != 0;
            }
            if (okUuid) { uuid = text.substr(i, 36); break; }
        }

        // Host + path segments
        std::string host, path;
        size_t scheme = text.find("://");
        std::string rest = scheme == std::string::npos ? text : text.substr(scheme + 3);
        size_t slash = rest.find('/');
        if (slash == std::string::npos) { host = rest; }
        else { host = rest.substr(0, slash); path = rest.substr(slash); }
        if (host.empty()) path = text;
        size_t q = path.find_first_of("?#");
        if (q != std::string::npos) path.resize(q);
        std::vector<std::string> segs;
        {
            std::string cur;
            for (char c : path) {
                if (c == '/') { if (!cur.empty()) { segs.push_back(cur); cur.clear(); } }
                else cur.push_back(c);
            }
            if (!cur.empty()) segs.push_back(cur);
        }
        // Skip locales like /ru/ and /en/
        static const std::set<std::string> kLocales = {"ru", "en", "ja", "uk", "multi"};
        std::vector<std::string> keys;
        auto pushKey = [&](const std::string& k) {
            if (k.empty() || kLocales.count(k)) return;
            for (const auto& e : keys) if (e == k) return;
            keys.push_back(k);
        };
        for (size_t i = 0; i < segs.size(); ++i) {
            if ((segs[i] == "read" || segs[i] == "chapter" || segs[i] == "chapters") && i > 0) pushKey(segs[i - 1]);
            if (segs[i] == "manga" || segs[i] == "series" || segs[i] == "comic") {
                if (i + 1 < segs.size()) pushKey(segs[i + 1]);
            }
        }
        for (auto it = segs.rbegin(); it != segs.rend(); ++it) pushKey(*it);
        if (keys.empty() && !text.empty() && text.find('/') == std::string::npos) pushKey(text);

        // Likely source for this host
        static const std::vector<std::pair<std::string, std::string>> kHosts = {
            {"mangalib", "v2.ru.mangalib"}, {"cdnlibs", "v2.ru.mangalib"}, {"imglib", "v2.ru.mangalib"},
            {"hentaicdn", "v2.ru.mangalib"}, {"remanga", "v2.ru.remanga"}, {"mangadex", "mangadex"},
            {"weebcentral", "v2.en.weebcentral"}, {"mangafire", "v2.multi.mangafire"},
        };
        std::vector<std::string> trySources;
        for (const auto& [needle, sid] : kHosts) {
            if (host.find(needle) != std::string::npos) { trySources.push_back(sid); break; }
        }
        auto installed = buildInstalledSourcesList(nullptr);
        for (const auto& s : installed.asArray()) {
            std::string sid = s["id"].str();
            if (sid == "mangadex" || !aidokurun::isV2Source(sid)) continue;
            if (std::find(trySources.begin(), trySources.end(), sid) == trySources.end()) trySources.push_back(sid);
        }
        if (uuid.empty() && std::find(trySources.begin(), trySources.end(), std::string("mangadex")) == trySources.end()) {
            trySources.push_back("mangadex");
        }

        auto sourceName = [&](const std::string& sid) {
            for (const auto& s : installed.asArray()) if (s["id"].str() == sid) return s["name"].str(sid);
            return sid;
        };

        // MangaDex first when we have a UUID
        if (!uuid.empty()) {
            std::string e, chErr;
            Json::Value details = mangadex_.getMangaDetails(uuid, &e);
            int n = static_cast<int>(mangadex_.getChapters(uuid, &chErr).size());
            if (details.isObject() && (n > 0 || chErr.empty())) {
                out["source_id"] = "mangadex";
                out["source_name"] = "MangaDex";
                out["manga_id"] = uuid;
                out["title"] = details["title"].str(uuid);
                out["cover_url"] = details["cover_url"].str();
                out["author"] = details["author"].str();
                out["description"] = details["description"].str();
                out["chapter_count"] = n > 0 ? n : -1;  // unknown beats a wrong "0 chapters"
                return out;
            }
        }

        int attempts = 0;
        std::string firstErr;
        for (const auto& sid : trySources) {
            if (!aidokurun::isV2Source(sid) || !arun_.available()) continue;
            for (const auto& key : keys) {
                if (++attempts > 6) break;  // keep it snappy: each attempt loads the source's wasm
                std::string err;
                Json::Value res = arun_.getManga(sid, key, aidoku_.languages(), &err);
                const Json::Value& chs = res["chapters"];
                if (!chs.isArray()) { if (firstErr.empty()) firstErr = err; continue; }
                std::string title = res["manga"]["title"].str();
                if (title.empty() || title == key) {
                    title.clear();
                    std::string pretty = key;
                    size_t dd = pretty.find("--");
                    if (dd != std::string::npos) pretty = pretty.substr(dd + 2);
                    for (char& c : pretty) if (c == '-' || c == '_') c = ' ';
                    if (!pretty.empty()) pretty[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(pretty[0])));
                    title = util::trim(pretty);
                }
                out["source_id"] = sid;
                out["source_name"] = sourceName(sid);
                out["manga_id"] = key;
                out["title"] = title;
                out["cover_url"] = res["manga"]["cover_url"].str();
                out["author"] = res["manga"]["author"].str();
                out["description"] = res["manga"]["description"].str();
                out["chapter_count"] = static_cast<int>(chs.size());
                return out;
            }
        }
        if (errOut) {
            std::string msg = firstErr.empty() ? std::string() : firstErr;
            // Source errors are often raw upstream junk (HTML blocks, JSON parse noise)
            if (msg.empty() || msg.find('<') != std::string::npos ||
                msg.find("JSON error") != std::string::npos || msg.find("HTTP") != std::string::npos) {
                msg = "Could not open that link - the manga may be removed or the source is rate limited";
            }
            if (msg.size() > 200) msg = msg.substr(0, 200);
            *errOut = msg;
        }
        return out;
    }

    void sendToast(const std::string& message, bool isError = false) {
        Json::Value ev = Json::Value::object();
        ev["message"] = message;
        ev["is_error"] = isError;
        ipc_.sendJson(EVT_TOAST, ev);
    }

    void invalidateDiscoverCache() {
        std::lock_guard<std::mutex> lock(discoverMu_);
        discoverCache_ = Json::Value::array();
        discoverCacheTs_ = 0;
        arun_.clearDiscoverCache();
    }

    // Sources whose library/read state lives in the rakuyomi server
    static bool isRakuyomiSource(const std::string& sid) {
        return sid != "mangadex" && !aidokurun::isV2Source(sid);
    }

    void applyLanguages() {
        mangadex_.setLanguages(aidoku_.languages());
    }

    bool hasAnySourceLists() {
        return !aidoku_.legacySourceLists().empty() || !aidoku_.v2SourceLists().empty();
    }

    // Available sources from both engines (rakuyomi legacy lists + aidoku-run v2 lists)
    Json::Value listAllAvailableSources(bool rakuyomiOnline, std::string* errOut) {
        Json::Value out = Json::Value::array();
        std::string err1, err2;
        auto v2Lists = aidoku_.v2SourceLists();
        std::future<Json::Value> v2Fut;
        if (!v2Lists.empty() && arun_.available()) {
            auto langs = aidoku_.languages();
            v2Fut = std::async(std::launch::async, [this, v2Lists, langs, &err2]() {
                return arun_.listAvailable(v2Lists, langs, &err2);
            });
        }
        if (rakuyomiOnline && !aidoku_.legacySourceLists().empty()) {
            Json::Value a = aidoku_.listAvailableSources(&err1);
            for (const auto& s : a.asArray()) out.push(s);
        }
        if (v2Fut.valid()) {
            Json::Value b = v2Fut.get();
            for (const auto& s : b.asArray()) out.push(s);
        }
        if (errOut) *errOut = !err1.empty() ? err1 : err2;
        return out;
    }

    Json::Value fetchV2Chapters(const std::string& sid, const std::string& mid,
                                const std::string& fallbackTitle, std::string* err) {
        Json::Value res = arun_.getManga(sid, mid, aidoku_.languages(), err);
        if (res["manga"].isObject()) {
            const Json::Value& m = res["manga"];
            state_.upsertMangaMeta(sid, m["source_name"].str(sid), mid,
                                   m["title"].str(fallbackTitle), m["cover_url"].str(),
                                   m["author"].str(), m["description"].str());
            if (!m["cover_url"].str().empty()) {
                enqueueCoverFetch(sid, mid, m["title"].str(fallbackTitle), m["cover_url"].str());
            }
        }
        return res["chapters"].isArray() ? res["chapters"] : Json::Value::array();
    }

    void enqueueCoverFetch(const std::string& sourceId,
                           const std::string& mangaId,
                           const std::string& title,
                           const std::string& coverUrl = "") {
        if (disableCovers_) return;
        std::string mkey = store::makeMangaKey(sourceId, mangaId);
        Json::Value existing = state_.getManga(sourceId, mangaId);
        if (existing.isObject() && !existing["cover_path"].str().empty()) {
            std::string existingPath = existing["cover_path"].str();
            if (existingPath.rfind("file://", 0) == 0) {
                std::error_code ec;
                if (fs::exists(existingPath.substr(7), ec)) return;
            }
        }

        // The Aidoku runtime keeps each source's own cover URL in its SQLite DB
        std::string url = coverUrl;
        if (url.empty() && existing.isObject()) url = existing["cover_url"].str();
        if (url.empty()) url = coverDb_.lookupCoverUrl(sourceId, mangaId);
        if (url.empty() && title.empty()) return;

        std::lock_guard<std::mutex> lock(coverMu_);
        if (attemptedCovers_.count(mkey) > 0) return;
        attemptedCovers_.insert(mkey);
        coverQueue_.push_back(CoverTask{sourceId, mangaId, title, url});
        coverCv_.notify_one();
    }

    Json::Value buildInstalledSourcesList(bool* aidokuOnlineOut = nullptr) {
        Json::Value list = Json::Value::array();

        // Built-in MangaDex source is always available
        Json::Value md = Json::Value::object();
        md["id"] = "mangadex";
        md["name"] = "MangaDex";
        md["version"] = 5;
        md["builtin"] = true;
        list.push(md);

        bool online = aidoku_.hasLegacySources() && aidoku_.ensureRunning();
        if (aidokuOnlineOut) *aidokuOnlineOut = online;
        Json::Value v2 = arun_.listInstalled();
        if (online) {
            std::string err;
            Json::Value installed = aidoku_.listInstalledSources(&err);
            if (installed.isArray()) {
                for (const auto& s : installed.asArray()) {
                    std::string sid = s["id"].str();
                    if (sid == "multi.mangadex") continue;
                    Json::Value copy = s;
                    copy["builtin"] = false;
                    list.push(std::move(copy));
                }
            }
        }
        for (const auto& s : v2.asArray()) list.push(s);
        return list;
    }

    void syncAidokuIntoStore(bool refreshRemoteChapters = false) {
        if (!aidoku_.hasLegacySources() || !aidoku_.ensureRunning()) return;

        std::string err;
        Json::Value lib = aidoku_.getLibrary(&err);
        if (err.empty() && lib.isArray()) {
            state_.syncAidokuLibraryList(lib);

            for (const auto& item : lib.asArray()) {
                std::string sid = item["source"]["id"].str();
                std::string sname = item["source"]["name"].str(sid);
                std::string mid = item["id"].str();
                std::string mtitle = item["title"].str(mid);
                if (sid.empty() || mid.empty()) continue;

                if (refreshRemoteChapters) {
                    aidoku_.refreshChapters(sid, mid, nullptr);
                }

                std::string chErr;
                Json::Value chs = aidoku_.listCachedChapters(sid, mid, &chErr);
                if (chErr.empty() && chs.isArray()) {
                    state_.mergeAndCacheChapters(sid, sname, mid, mtitle, chs);
                }
                enqueueCoverFetch(sid, mid, mtitle, "");
            }
        }

        if (refreshRemoteChapters) {
            Json::Value lib = state_.listLibrary();
            for (const auto& m : lib.asArray()) {
                std::string sid = m["source_id"].str();
                if (!aidokurun::isV2Source(sid)) continue;
                std::string mid = m["manga_id"].str();
                std::string mtitle = m["title"].str(mid);
                std::string chErr;
                Json::Value chs = fetchV2Chapters(sid, mid, mtitle, &chErr);
                if (chs.isArray() && !chs.empty()) {
                    state_.mergeAndCacheChapters(sid, m["source_name"].str(sid), mid, mtitle, chs);
                }
            }
        }
    }

    Json::Value serializeDownloadQueue() {
        std::lock_guard<std::mutex> lock(dlMu_);
        Json::Value arr = Json::Value::array();
        if (activeDl_) {
            Json::Value item = Json::Value::object();
            item["key"] = activeDl_->key;
            item["source_id"] = activeDl_->sourceId;
            item["manga_id"] = activeDl_->mangaId;
            item["manga_title"] = activeDl_->mangaTitle;
            item["chapter_id"] = activeDl_->chapterId;
            if (activeDl_->hasChapterNum) item["chapter_num"] = activeDl_->chapterNum;
            item["chapter_title"] = activeDl_->chapterTitle;
            item["status"] = activeDl_->status;
            item["progress_pct"] = activeDl_->progressPct;
            item["active"] = true;
            arr.push(std::move(item));
        }
        for (const auto& q : dlQueue_) {
            Json::Value item = Json::Value::object();
            item["key"] = q.key;
            item["source_id"] = q.sourceId;
            item["manga_id"] = q.mangaId;
            item["manga_title"] = q.mangaTitle;
            item["chapter_id"] = q.chapterId;
            if (q.hasChapterNum) item["chapter_num"] = q.chapterNum;
            item["chapter_title"] = q.chapterTitle;
            item["status"] = "Queued";
            item["progress_pct"] = 0;
            item["active"] = false;
            arr.push(std::move(item));
        }
        return arr;
    }

    void pushDownloadsEvent() {
        Json::Value ev = Json::Value::object();
        ev["queue"] = serializeDownloadQueue();
        ev["downloads"] = state_.listDownloads();
        uint64_t bytes = util::directorySizeBytes(pageCache_.root()) +
                         util::directorySizeBytes(state_.localChaptersDir()) +
                         util::directorySizeBytes(fs::path(aidoku_.homeDir()) / "downloads") +
                         util::directorySizeBytes(arun_.downloadsDir());
        ev["cache_bytes"] = static_cast<int64_t>(bytes);
        ipc_.sendJson(EVT_DOWNLOADS_UPDATED, ev);
    }

    void pushLibraryEvent() {
        Json::Value ev = Json::Value::object();
        ev["library"] = state_.listLibrary();
        ev["continue_reading"] = state_.getContinueReading();
        ev["recent"] = state_.listRecent();
        ipc_.sendJson(EVT_LIBRARY_UPDATED, ev);
    }

    void pushBootstrap() {
        // Send immediate local snapshot first so UI paints in <30ms
        {
            Json::Value fast = Json::Value::object();
            fast["library"] = state_.listLibrary();
            fast["continue_reading"] = state_.getContinueReading();
            fast["recent"] = state_.listRecent();
            fast["downloads"] = state_.listDownloads();
            fast["queue"] = serializeDownloadQueue();
            fast["settings"] = state_.getSettings();
            Json::Value fastMd = Json::Value::array();
            Json::Value md = Json::Value::object();
            md["id"] = "mangadex";
            md["name"] = "MangaDex";
            md["version"] = 5;
            md["builtin"] = true;
            fastMd.push(md);
            fast["installed_sources"] = std::move(fastMd);
            fast["source_config"] = aidoku_.getSourceConfig();
            fast["aidoku_online"] = false;
            ipc_.sendJson(EVT_BOOTSTRAP, fast);
        }

        // Now sync Aidoku library & installed sources and push enriched state
        bool aidokuOnline = false;
        Json::Value sources = buildInstalledSourcesList(&aidokuOnline);
        if (aidokuOnline) {
            syncAidokuIntoStore(false);
        }

        for (const auto& m : state_.listMangasMissingCovers()) {
            enqueueCoverFetch(m["source_id"].str(), m["manga_id"].str(),
                              m["title"].str(), m["cover_url"].str());
        }

        Json::Value full = Json::Value::object();
        full["library"] = state_.listLibrary();
        full["continue_reading"] = state_.getContinueReading();
        full["recent"] = state_.listRecent();
        full["downloads"] = state_.listDownloads();
        full["queue"] = serializeDownloadQueue();
        full["settings"] = state_.getSettings();
        full["installed_sources"] = std::move(sources);
        full["source_config"] = aidoku_.getSourceConfig();
        full["aidoku_online"] = aidokuOnline;
        ipc_.sendJson(EVT_BOOTSTRAP, full);
        pushAnilistState();
        requestAnilistSync(2);
    }

    // Chronological ascending sort + deduplicated neighbor selection
    static std::vector<Json::Value> sortChaptersChronological(const Json::Value& chaptersArr) {
        std::vector<Json::Value> vec = chaptersArr.asArray();
        bool anyNum = false;
        for (const auto& c : vec) {
            if (c.has("chapter_num")) {
                anyNum = true;
                break;
            }
        }
        if (!anyNum) {
            return vec;
        }
        std::stable_sort(vec.begin(), vec.end(), [](const Json::Value& a, const Json::Value& b) {
            double na = a.has("chapter_num") ? a["chapter_num"].num(0) : -1.0;
            double nb = b.has("chapter_num") ? b["chapter_num"].num(0) : -1.0;
            if (std::fabs(na - nb) > 1e-4) return na < nb;
            double va = a.has("volume_num") ? a["volume_num"].num(0) : 0.0;
            double vb = b.has("volume_num") ? b["volume_num"].num(0) : 0.0;
            return va < vb;
        });
        return vec;
    }

    static void findPrevNextChapter(const Json::Value& chaptersArr,
                                    const std::string& currentChapterId,
                                    Json::Value& prevOut,
                                    Json::Value& nextOut) {
        prevOut = Json::Value();
        nextOut = Json::Value();
        if (!chaptersArr.isArray() || chaptersArr.empty()) return;

        std::vector<Json::Value> sorted = sortChaptersChronological(chaptersArr);
        int curIdx = -1;
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (sorted[i]["id"].str() == currentChapterId) {
                curIdx = static_cast<int>(i);
                break;
            }
        }
        if (curIdx < 0) return;

        const Json::Value& cur = sorted[static_cast<size_t>(curIdx)];
        bool hasNum = cur.has("chapter_num");
        double curNum = cur["chapter_num"].num(0.0);
        std::string curScan = cur["scanlator"].str();

        if (!hasNum) {
            if (curIdx > 0) prevOut = sorted[static_cast<size_t>(curIdx - 1)];
            if (curIdx + 1 < static_cast<int>(sorted.size())) nextOut = sorted[static_cast<size_t>(curIdx + 1)];
            return;
        }

        // Next chapter: find smallest chapter_num strictly greater than curNum
        for (size_t i = static_cast<size_t>(curIdx + 1); i < sorted.size(); ++i) {
            if (!sorted[i].has("chapter_num")) {
                nextOut = sorted[i];
                break;
            }
            double n = sorted[i]["chapter_num"].num(0.0);
            if (n > curNum + 0.0005) {
                // Collect candidates with this same target chapter_num and prefer downloaded or matching scanlator
                double targetNum = n;
                const Json::Value* best = &sorted[i];
                for (size_t j = i; j < sorted.size(); ++j) {
                    double nj = sorted[j]["chapter_num"].num(0.0);
                    if (std::fabs(nj - targetNum) > 0.0005) break;
                    if (sorted[j]["downloaded"].boolean(false)) {
                        best = &sorted[j];
                        break;
                    }
                    if (!curScan.empty() && sorted[j]["scanlator"].str() == curScan) {
                        best = &sorted[j];
                    }
                }
                nextOut = *best;
                break;
            }
        }

        // Previous chapter: find largest chapter_num strictly smaller than curNum
        for (int i = curIdx - 1; i >= 0; --i) {
            const auto& cand = sorted[static_cast<size_t>(i)];
            if (!cand.has("chapter_num")) {
                prevOut = cand;
                break;
            }
            double n = cand["chapter_num"].num(0.0);
            if (n < curNum - 0.0005) {
                double targetNum = n;
                const Json::Value* best = &cand;
                for (int j = i; j >= 0; --j) {
                    double nj = sorted[static_cast<size_t>(j)]["chapter_num"].num(0.0);
                    if (std::fabs(nj - targetNum) > 0.0005) break;
                    if (sorted[static_cast<size_t>(j)]["downloaded"].boolean(false)) {
                        best = &sorted[static_cast<size_t>(j)];
                        break;
                    }
                    if (!curScan.empty() && sorted[static_cast<size_t>(j)]["scanlator"].str() == curScan) {
                        best = &sorted[static_cast<size_t>(j)];
                    }
                }
                prevOut = *best;
                break;
            }
        }
    }

    std::set<std::string> currentPinnedSlugs() {
        std::lock_guard<std::mutex> lock(pinMu_);
        std::set<std::string> s;
        if (!activeChapterSlug_.empty()) s.insert(activeChapterSlug_);
        if (!preloadedChapterSlug_.empty()) s.insert(preloadedChapterSlug_);
        return s;
    }

    // Downloads (if needed) and prepares page file:// URLs for any chapter
    std::vector<std::string> resolveChapterPages(
        const std::string& sourceId,
        const std::string& sourceName,
        const std::string& mangaId,
        const std::string& mangaTitle,
        const std::string& chapterId,
        double chapterNum,
        bool hasChapterNum,
        const std::string& chapterTitle,
        const std::atomic<bool>* cancelFlag,
        const std::function<void(const std::string&, int)>& onProgress,
        std::string* errOut) {
        std::string ckey = store::makeChapterKey(sourceId, mangaId, chapterId);
        auto pinned = currentPinnedSlugs();

        // 1. Check if we already have a verified local path recorded
        Json::Value dlRec = state_.getDownloadRecord(sourceId, mangaId, chapterId);
        if (dlRec.isObject()) {
            std::string sp = dlRec["storage_path"].str();
            bool isFolder = dlRec["is_folder"].boolean(false);
            std::error_code ec;
            if (!sp.empty() && fs::exists(sp, ec)) {
                if (isFolder) {
                    return pageCache_.prepareFolder(sp, errOut);
                }
                return pageCache_.extractCbz(ckey, sp, pinned, errOut);
            }
        }

        // 2. MangaDex chapter via MangaDex client
        if (sourceId == "mangadex") {
            fs::path chFolder = state_.localChaptersDir() / "mangadex" / mangaId / chapterId;
            std::error_code ec;
            if (fs::exists(chFolder, ec) && !fs::is_empty(chFolder, ec)) {
                return pageCache_.prepareFolder(chFolder.string(), errOut);
            }
            if (onProgress) onProgress("Downloading chapter from MangaDex...", 20);
            std::string mdErr;
            bool ok = mangadex_.downloadChapterPages(
                chapterId, chFolder, cancelFlag, onProgress, &mdErr);
            if (!ok) {
                if (errOut) *errOut = mdErr.empty() ? "Failed downloading from MangaDex" : mdErr;
                return {};
            }
            state_.recordDownload(sourceId, "MangaDex", mangaId, mangaTitle,
                                  chapterId, chapterNum, hasChapterNum, chapterTitle,
                                  chFolder.string(), true);
            if (onProgress) onProgress("Preparing pages...", 95);
            return pageCache_.prepareFolder(chFolder.string(), errOut);
        }

        // 3. New-format Aidoku source via aidoku-run
        if (aidokurun::isV2Source(sourceId)) {
            if (onProgress) onProgress("Downloading from " + (sourceName.empty() ? sourceId : sourceName) + "...", 15);
            std::string dlErr;
            std::string cbzPath = arun_.downloadChapter(sourceId, mangaId, chapterId, cancelFlag, onProgress, &dlErr);
            if (cbzPath.empty()) {
                if (errOut) *errOut = dlErr.empty() ? "Failed to download chapter" : dlErr;
                return {};
            }
            state_.recordDownload(sourceId, sourceName, mangaId, mangaTitle,
                                  chapterId, chapterNum, hasChapterNum, chapterTitle,
                                  cbzPath, false);
            if (onProgress) onProgress("Extracting pages...", 90);
            return pageCache_.extractCbz(ckey, cbzPath, pinned, errOut);
        }

        // 4. Aidoku chapter via local Aidoku server
        if (onProgress) onProgress("Preparing chapter from " + (sourceName.empty() ? sourceId : sourceName) + "...", 15);
        std::string dlErr;
        std::string cbzPath = aidoku_.downloadChapter(
            sourceId, mangaId, chapterId, chapterNum, hasChapterNum, cancelFlag,
            [&](const std::string& msg) {
                if (onProgress) onProgress(msg, 45);
            },
            &dlErr);
        if (cbzPath.empty()) {
            if (errOut) *errOut = dlErr.empty() ? "Failed to download chapter" : dlErr;
            return {};
        }

        state_.recordDownload(sourceId, sourceName, mangaId, mangaTitle,
                              chapterId, chapterNum, hasChapterNum, chapterTitle,
                              cbzPath, false);
        if (onProgress) onProgress("Extracting pages...", 85);
        return pageCache_.extractCbz(ckey, cbzPath, pinned, errOut);
    }

    void triggerBackgroundPreload(const std::string& sourceId,
                                  const std::string& sourceName,
                                  const std::string& mangaId,
                                  const std::string& mangaTitle,
                                  const Json::Value& nextChap) {
        if (!nextChap.isObject() || nextChap["id"].str().empty()) return;
        std::string nextCid = nextChap["id"].str();
        std::string ckey = store::makeChapterKey(sourceId, mangaId, nextCid);

        {
            std::lock_guard<std::mutex> lock(pinMu_);
            if (preloadingKeys_.count(ckey) > 0) return;
            preloadingKeys_.insert(ckey);
            preloadedChapterSlug_ = util::hashSlug(ckey);
        }

        double chNum = nextChap["chapter_num"].num(0.0);
        bool hasNum = nextChap.has("chapter_num");
        std::string chTitle = nextChap["title"].str();

        spawnAsync([this, sourceId, sourceName, mangaId, mangaTitle, nextCid, chNum, hasNum, chTitle, ckey]() {
            if (shuttingDown_.load()) return;
            std::string err;
            resolveChapterPages(sourceId, sourceName, mangaId, mangaTitle,
                                nextCid, chNum, hasNum, chTitle,
                                &shuttingDown_, nullptr, &err);
            std::lock_guard<std::mutex> lock(pinMu_);
            preloadingKeys_.erase(ckey);
        });
    }

    void downloadLoop() {
        while (!shuttingDown_.load()) {
            std::unique_ptr<DownloadTask> task;
            {
                std::unique_lock<std::mutex> lock(dlMu_);
                dlCv_.wait(lock, [this]() {
                    return shuttingDown_.load() || !dlQueue_.empty();
                });
                if (shuttingDown_.load()) break;
                task = std::make_unique<DownloadTask>(std::move(dlQueue_.front()));
                dlQueue_.pop_front();
                activeDl_ = std::move(task);
                cancelActiveDl_ = false;
                activeDl_->status = "Starting...";
                activeDl_->progressPct = 5;
            }
            pushDownloadsEvent();

            DownloadTask snapshot;
            {
                std::lock_guard<std::mutex> lock(dlMu_);
                snapshot = *activeDl_;
            }

            std::string err;
            resolveChapterPages(
                snapshot.sourceId, snapshot.sourceName,
                snapshot.mangaId, snapshot.mangaTitle,
                snapshot.chapterId, snapshot.chapterNum, snapshot.hasChapterNum,
                snapshot.chapterTitle,
                &cancelActiveDl_,
                [this](const std::string& st, int pct) {
                    {
                        std::lock_guard<std::mutex> lock(dlMu_);
                        if (activeDl_) {
                            activeDl_->status = st;
                            activeDl_->progressPct = pct;
                        }
                    }
                    pushDownloadsEvent();
                },
                &err);

            {
                std::lock_guard<std::mutex> lock(dlMu_);
                activeDl_.reset();
            }
            if (!err.empty() && err != "Download cancelled") {
                sendToast(snapshot.mangaTitle + ": " + err, true);
            }
            pushDownloadsEvent();
            pushLibraryEvent();
        }
    }

    void coverLoop() {
        while (!shuttingDown_.load()) {
            CoverTask task;
            {
                std::unique_lock<std::mutex> lock(coverMu_);
                coverCv_.wait(lock, [this]() {
                    return shuttingDown_.load() || !coverQueue_.empty();
                });
                if (shuttingDown_.load()) break;
                task = std::move(coverQueue_.front());
                coverQueue_.pop_front();
            }

            std::string mkey = store::makeMangaKey(task.sourceId, task.mangaId);
            fs::path dest = state_.coversDir() / (util::hashSlug(mkey) + ".jpg");
            std::error_code ec;

            fs::path pgm = dest;
            pgm.replace_extension(".pgm");

            bool ok = false;
            if (fs::exists(dest, ec) && fs::file_size(dest, ec) > 128) {
                ok = true;
            } else if (fs::exists(pgm, ec) && fs::file_size(pgm, ec) > 128) {
                dest = pgm;
                ok = true;
            } else if (!task.coverUrl.empty()) {
                ok = util::httpsDownloadFile(task.coverUrl, dest, 20);
            }

            if (!ok && !task.title.empty()) {
                ok = mangadex_.fetchCoverByTitle(task.title, dest);
            }

            // Qt on the rM2 has no WebP plugin; convert WebP covers to grayscale PGM
            if (ok) dest = pageCache_.webp().ensureQtReadable(dest);

            if (ok && !shuttingDown_.load()) {
                std::string uri = "file://" + fs::absolute(dest, ec).string();
                state_.setCoverPath(task.sourceId, task.mangaId, uri);

                Json::Value ev = Json::Value::object();
                ev["source_id"] = task.sourceId;
                ev["manga_id"] = task.mangaId;
                ev["cover_path"] = uri;
                ipc_.sendJson(EVT_COVER_UPDATED, ev);
            }
        }
    }

    void handleSearch(const Json::Value& req) {
        uint64_t mySeq = ++searchSeq_;
        std::string query = util::trim(req["query"].str());
        std::string filter = req["source_filter"].str("all");
        std::string mode = req["mode"].str(query.empty() ? "discover" : "search");
        if (mode == "popular" || mode == "latest") mode = "discover";
        bool forceRefresh = req["force_refresh"].boolean(false);

        spawnAsync([this, mySeq, query, filter, mode, forceRefresh]() {
            if (shuttingDown_.load() || searchSeq_.load() != mySeq) return;

            auto enrichItem = [this](Json::Value item) -> Json::Value {
                std::string sid = item["source"]["id"].str(item["source_id"].str());
                std::string sname = item["source"]["name"].str(item["source_name"].str(sid));
                std::string mid = item["id"].str();
                std::string title = item["title"].str(mid);
                item["source_id"] = sid;
                item["source_name"] = sname;

                Json::Value stored = state_.getManga(sid, mid);
                if (stored.isObject()) {
                    item["in_library"] = stored["in_library"].boolean(false);
                    if (!stored["cover_path"].str().empty()) {
                        item["cover_path"] = stored["cover_path"].str();
                    }
                } else {
                    item["in_library"] = false;
                }

                if (item["cover_url"].str().empty()) {
                    std::string dbUrl = coverDb_.lookupCoverUrl(sid, mid);
                    if (!dbUrl.empty()) item["cover_url"] = dbUrl;
                }
                if (item["cover_path"].str().empty()) {
                    enqueueCoverFetch(sid, mid, title, item["cover_url"].str());
                }
                return item;
            };

            Json::Value combined = Json::Value::array();
            std::string errors;

            // 0. aidoku-run (new-format) sources run in parallel with everything else
            bool wantV2 = (filter == "all" || aidokurun::isV2Source(filter)) && arun_.available();
            std::string v2Err;
            std::future<Json::Value> v2Fut;
            if (wantV2) {
                std::string only = (filter == "all") ? "" : filter;
                v2Fut = std::async(std::launch::async, [this, query, only, &v2Err]() {
                    return arun_.search(query, only, &v2Err);
                });
            }
            auto collectV2 = [&]() {
                if (!v2Fut.valid()) return;
                Json::Value items = v2Fut.get();
                for (const auto& m : items.asArray()) combined.push(enrichItem(m));
                if (!v2Err.empty() && filter != "all" && items.empty()) errors = v2Err;
            };

            // 1. MangaDex results (if filter is "all" or "mangadex")
            if (filter == "all" || filter == "mangadex") {
                std::string mdErr;
                Json::Value mdItems = mangadex_.queryMangaList(query, mode, &mdErr);
                if (searchSeq_.load() != mySeq) { collectV2(); return; }
                if (mdItems.isArray()) {
                    for (const auto& m : mdItems.asArray()) {
                        combined.push(enrichItem(m));
                    }
                }
                if (!mdErr.empty() && filter == "mangadex") {
                    errors = mdErr;
                }
            }

            // 2. Aidoku results (if filter is "all" or specific Aidoku source)
            bool hasLegacyInstalled = false;
            if (filter == "all" || isRakuyomiSource(filter)) {
                Json::Value installedNow = buildInstalledSourcesList(nullptr);
                for (const auto& s : installedNow.asArray()) {
                    if (isRakuyomiSource(s["id"].str())) { hasLegacyInstalled = true; break; }
                }
            }
            if (filter != "mangadex" && !aidokurun::isV2Source(filter) && hasLegacyInstalled) {
                bool online = false;
                Json::Value installed = buildInstalledSourcesList(&online);
                if (online && !installed.empty()) {
                    Json::Value rawItems = Json::Value::array();
                    std::string aiErr;

                    if (query.empty()) {
                        bool hitCache = false;
                        if (!forceRefresh) {
                            std::lock_guard<std::mutex> lock(discoverMu_);
                            if (discoverCache_.isArray() && !discoverCache_.empty() &&
                                (util::nowSeconds() - discoverCacheTs_) < 60) {
                                rawItems = discoverCache_;
                                hitCache = true;
                            }
                        }
                        if (!hitCache) {
                            rawItems = aidoku_.searchMangas("", &aiErr);
                            if (searchSeq_.load() != mySeq) { collectV2(); return; }
                            if (aiErr.empty() && rawItems.isArray() && !rawItems.empty()) {
                                std::lock_guard<std::mutex> lock(discoverMu_);
                                discoverCache_ = rawItems;
                                discoverCacheTs_ = util::nowSeconds();
                            }
                        }
                    } else {
                        rawItems = aidoku_.searchMangas(query, &aiErr);
                    }

                    if (searchSeq_.load() != mySeq) { collectV2(); return; }

                    for (const auto& m : rawItems.asArray()) {
                        std::string sid = m["source"]["id"].str(m["source_id"].str());
                        if (sid == "multi.mangadex") continue;
                        if (filter != "all" && sid != filter) continue;
                        combined.push(enrichItem(m));
                    }

                    // Fallback for search-only .aix plugins that return 0 items on empty query q=""
                    if (query.empty() && combined.empty() && aiErr.empty()) {
                        std::string fbErr;
                        Json::Value fbItems = aidoku_.searchMangas("a", &fbErr);
                        if (searchSeq_.load() != mySeq) { collectV2(); return; }
                        for (const auto& m : fbItems.asArray()) {
                            std::string sid = m["source"]["id"].str(m["source_id"].str());
                            if (sid == "multi.mangadex") continue;
                            if (filter != "all" && sid != filter) continue;
                            combined.push(enrichItem(m));
                        }
                    }

                    if (!aiErr.empty() && combined.empty() && filter != "all") {
                        errors = aiErr;
                    }
                }
            }

            collectV2();
            if (searchSeq_.load() != mySeq) return;

            // Keep search payloads reasonably small for the QML side (and well below the IPC
            // socket buffer, see appload_ipc.hpp): trim descriptions, drop trailing results.
            for (auto& it : combined.asArrayMut()) {
                std::string d = it["description"].str();
                if (d.size() > 240) {
                    size_t cut = 240;
                    while (cut > 0 && (static_cast<unsigned char>(d[cut]) & 0xC0) == 0x80) --cut;  // UTF-8 safe
                    it["description"] = d.substr(0, cut) + "...";
                }
            }
            while (combined.size() > 20 && Json::stringify(combined).size() > 900000) {
                auto& arr = combined.asArrayMut();
                arr.resize(arr.size() * 9 / 10);
            }
            Json::Value done = Json::Value::object();
            done["query"] = query;
            done["source_filter"] = filter;
            done["mode"] = mode;
            done["results"] = std::move(combined);
            done["loading"] = false;
            done["error"] = errors;
            ipc_.sendJson(EVT_SEARCH_RESULTS, done);
        });
    }

    void handleMangaDetails(const Json::Value& req) {
        std::string sourceId = req["source_id"].str();
        std::string sourceName = req["source_name"].str(sourceId);
        std::string mangaId = req["manga_id"].str();
        std::string mangaTitle = req["manga_title"].str();
        std::string coverUrl = req["cover_url"].str();
        bool forceRefresh = req["force_refresh"].boolean(false);

        if (sourceId.empty() || mangaId.empty()) return;

        state_.upsertMangaMeta(sourceId, sourceName, mangaId, mangaTitle, coverUrl,
                               req["author"].str(), req["description"].str());
        enqueueCoverFetch(sourceId, mangaId, mangaTitle, coverUrl);

        spawnAsync([this, sourceId, sourceName, mangaId, mangaTitle, forceRefresh]() {
            if (shuttingDown_.load()) return;

            auto sendDetailsPayload = [this](const std::string& sid,
                                             const std::string& mid,
                                             const Json::Value& chapters,
                                             bool loading,
                                             const std::string& err) {
                Json::Value m = state_.getManga(sid, mid);
                Json::Value ev = Json::Value::object();
                ev["source_id"] = sid;
                ev["source_name"] = m["source_name"].str(sid);
                ev["manga_id"] = mid;
                ev["manga_title"] = m["title"].str(mid);
                ev["author"] = m["author"].str();
                ev["description"] = m["description"].str();
                ev["status"] = m["status"].str();
                ev["cover_path"] = m["cover_path"].str();
                ev["cover_url"] = m["cover_url"].str();
                ev["in_library"] = m["in_library"].boolean(false);
                ev["chapters"] = chapters;
                ev["resume_chapter_id"] = m["last_chapter_id"].str();
                ev["resume_page"] = m["last_page_index"].integer(0);
                ev["loading"] = loading;
                ev["error"] = err;
                ev["anilist"] = anilist_->detailInfo(sid, mid);
                ipc_.sendJson(EVT_MANGA_DETAILS, ev);
            };

            Json::Value cached = state_.getCachedChapters(sourceId, mangaId);
            if (!cached.empty() && anilist_->onChaptersCached(sourceId, mangaId)) {
                cached = state_.getCachedChapters(sourceId, mangaId);
                pushLibraryEvent();
            }
            if (!cached.empty()) {
                sendDetailsPayload(sourceId, mangaId, cached, forceRefresh, "");
                if (!forceRefresh) {
                    // Still refresh quietly if online? Only if forceRefresh or cache empty to keep opens instant!
                    return;
                }
            } else {
                sendDetailsPayload(sourceId, mangaId, Json::Value::array(), true, "");
            }

            std::string err;
            Json::Value freshChapters = Json::Value::array();

            if (sourceId == "mangadex") {
                if (forceRefresh || cached.empty()) {
                    Json::Value details = mangadex_.getMangaDetails(mangaId, &err);
                    if (details.isObject()) {
                        state_.upsertMangaMeta(
                            sourceId, "MangaDex", mangaId,
                            details["title"].str(mangaTitle),
                            details["cover_url"].str(),
                            details["author"].str(),
                            details["description"].str());
                        if (!details["cover_url"].str().empty()) {
                            enqueueCoverFetch(sourceId, mangaId, details["title"].str(), details["cover_url"].str());
                        }
                    }
                    freshChapters = mangadex_.getChapters(mangaId, &err);
                }
            } else if (aidokurun::isV2Source(sourceId)) {
                freshChapters = fetchV2Chapters(sourceId, mangaId, mangaTitle, &err);
            } else {
                if (forceRefresh || cached.empty()) {
                    Json::Value preCached = aidoku_.listCachedChapters(sourceId, mangaId, nullptr);
                    if (forceRefresh || !preCached.isArray() || preCached.empty()) {
                        aidoku_.refreshChapters(sourceId, mangaId, &err);
                    }
                }
                std::string listErr;
                freshChapters = aidoku_.listCachedChapters(sourceId, mangaId, &listErr);
                if (!listErr.empty() && err.empty()) err = listErr;
            }

            if (freshChapters.isArray() && !freshChapters.empty()) {
                Json::Value merged = state_.mergeAndCacheChapters(sourceId, sourceName, mangaId, mangaTitle, freshChapters);
                if (anilist_->onChaptersCached(sourceId, mangaId)) {
                    merged = state_.getCachedChapters(sourceId, mangaId);
                }
                enqueueCoverFetch(sourceId, mangaId, mangaTitle, "");
                sendDetailsPayload(sourceId, mangaId, merged, false, "");
                pushLibraryEvent();
            } else {
                sendDetailsPayload(sourceId, mangaId, cached, false, err);
            }
        });
    }

    void handleOpenChapter(const Json::Value& req) {
        uint64_t mySeq = ++readerSeq_;
        std::string sourceId = req["source_id"].str();
        std::string sourceName = req["source_name"].str(sourceId);
        std::string mangaId = req["manga_id"].str();
        std::string mangaTitle = req["manga_title"].str();
        std::string chapterId = req["chapter_id"].str();
        double chapterNum = req["chapter_num"].num(0.0);
        bool hasChapterNum = req.has("chapter_num");
        std::string chapterTitle = req["chapter_title"].str();
        bool startAtEnd = req["start_at_end"].boolean(false);

        if (sourceId.empty() || mangaId.empty() || chapterId.empty()) return;

        std::string ckey = store::makeChapterKey(sourceId, mangaId, chapterId);
        {
            std::lock_guard<std::mutex> lock(pinMu_);
            activeChapterSlug_ = util::hashSlug(ckey);
        }

        spawnAsync([this, mySeq, sourceId, sourceName, mangaId, mangaTitle,
                     chapterId, chapterNum, hasChapterNum, chapterTitle, startAtEnd]() {
            if (shuttingDown_.load() || readerSeq_.load() != mySeq) return;

            auto sendLoading = [this, mySeq, &chapterId](const std::string& text, int pct) {
                if (readerSeq_.load() != mySeq) return;
                Json::Value ev = Json::Value::object();
                ev["chapter_id"] = chapterId;
                ev["status_text"] = text;
                ev["progress_pct"] = pct;
                ipc_.sendJson(EVT_READER_LOADING, ev);
            };

            sendLoading("Opening chapter...", 10);

            // Ensure chapters list is cached so prev/next chapter links work even on direct Continue Reading jump
            Json::Value chapters = state_.getCachedChapters(sourceId, mangaId);
            if (chapters.empty()) {
                if (sourceId == "mangadex") {
                    std::string chErr;
                    Json::Value fetched = mangadex_.getChapters(mangaId, &chErr);
                    if (fetched.isArray() && !fetched.empty()) {
                        chapters = state_.mergeAndCacheChapters(sourceId, "MangaDex", mangaId, mangaTitle, fetched);
                    }
                } else if (aidokurun::isV2Source(sourceId)) {
                    std::string chErr;
                    Json::Value fetched = fetchV2Chapters(sourceId, mangaId, mangaTitle, &chErr);
                    if (fetched.isArray() && !fetched.empty()) {
                        chapters = state_.mergeAndCacheChapters(sourceId, sourceName, mangaId, mangaTitle, fetched);
                    }
                } else if (aidoku_.hasLegacySources() && aidoku_.ensureRunning()) {
                    Json::Value fetched = aidoku_.listCachedChapters(sourceId, mangaId, nullptr);
                    if (!fetched.isArray() || fetched.empty()) {
                        aidoku_.refreshChapters(sourceId, mangaId, nullptr);
                        fetched = aidoku_.listCachedChapters(sourceId, mangaId, nullptr);
                    }
                    if (fetched.isArray() && !fetched.empty()) {
                        chapters = state_.mergeAndCacheChapters(sourceId, sourceName, mangaId, mangaTitle, fetched);
                    }
                }
            }

            std::string err;
            auto pages = resolveChapterPages(
                sourceId, sourceName, mangaId, mangaTitle,
                chapterId, chapterNum, hasChapterNum, chapterTitle,
                &shuttingDown_, sendLoading, &err);

            if (readerSeq_.load() != mySeq) return;

            Json::Value prevCh, nextCh;
            findPrevNextChapter(chapters, chapterId, prevCh, nextCh);

            int initialPage = 0;
            if (!pages.empty()) {
                if (startAtEnd) {
                    initialPage = static_cast<int>(pages.size()) - 1;
                } else {
                    Json::Value m = state_.getManga(sourceId, mangaId);
                    if (m["chapter_pages"][chapterId].isObject()) {
                        int saved = static_cast<int>(m["chapter_pages"][chapterId]["page"].integer(0));
                        if (saved > 0 && saved < static_cast<int>(pages.size())) {
                            initialPage = saved;
                        }
                    } else if (m["last_chapter_id"].str() == chapterId) {
                        int saved = static_cast<int>(m["last_page_index"].integer(0));
                        if (saved > 0 && saved < static_cast<int>(pages.size())) {
                            initialPage = saved;
                        }
                    }
                }

                state_.saveProgress(sourceId, sourceName, mangaId, mangaTitle,
                                    chapterId, chapterNum, hasChapterNum, chapterTitle,
                                    initialPage, static_cast<int>(pages.size()), false);
                pushLibraryEvent();
                pushDownloadsEvent();

                // If opening near the end of a short chapter, start preloading next chapter immediately
                if (initialPage >= static_cast<int>(pages.size()) - 3 &&
                    state_.getSettings()["autoPreloadNext"].boolean(true)) {
                    triggerBackgroundPreload(sourceId, sourceName, mangaId, mangaTitle, nextCh);
                }
            }

            Json::Value ev = Json::Value::object();
            ev["source_id"] = sourceId;
            ev["source_name"] = sourceName;
            ev["manga_id"] = mangaId;
            ev["manga_title"] = mangaTitle;
            ev["chapter_id"] = chapterId;
            if (hasChapterNum) ev["chapter_num"] = chapterNum;
            ev["chapter_title"] = chapterTitle;

            Json::Value pagesArr = Json::Value::array();
            for (const auto& u : pages) pagesArr.push(u);
            ev["pages"] = std::move(pagesArr);
            ev["initial_page"] = initialPage;
            ev["prev_chapter"] = prevCh;
            ev["next_chapter"] = nextCh;
            ev["error"] = err;
            ipc_.sendJson(EVT_CHAPTER_PAGES, ev);
        });
    }

    void handleIncoming(int32_t msgType, const std::string& contents) {
        if (msgType == appload::MSG_SYSTEM_TERMINATE) {
            shuttingDown_ = true;
            return;
        }
        if (msgType == appload::MSG_SYSTEM_NEW_COORDINATOR) {
            spawnAsync([this]() { pushBootstrap(); });
            return;
        }
        if (msgType == appload::MSG_SYSTEM_LOST_COORDINATOR) {
            return;
        }

        bool ok = false;
        Json::Value req = contents.empty() ? Json::Value::object() : Json::parse(contents, &ok);

        if (msgType >= REQ_ANILIST_STATE && msgType <= REQ_ANILIST_MANGA_INFO) {
            handleAniList(msgType, req);
            return;
        }

        switch (msgType) {
            case REQ_BOOTSTRAP: {
                spawnAsync([this]() { pushBootstrap(); });
                break;
            }
            case REQ_REFRESH_LIBRARY: {
                bool deep = req["deep"].boolean(false);
                spawnAsync([this, deep]() {
                    syncAidokuIntoStore(deep);
                    pushLibraryEvent();
                    sendToast(deep ? "Library & chapters refreshed" : "Library synced");
                });
                break;
            }
            case REQ_SEARCH: {
                handleSearch(req);
                break;
            }
            case REQ_MANGA_DETAILS: {
                handleMangaDetails(req);
                break;
            }
            case REQ_TOGGLE_LIBRARY: {
                std::string sid = req["source_id"].str();
                std::string sname = req["source_name"].str(sid);
                std::string mid = req["manga_id"].str();
                std::string title = req["manga_title"].str(mid);
                std::string coverUrl = req["cover_url"].str();
                bool add = req["add"].boolean(true);

                spawnAsync([this, sid, sname, mid, title, coverUrl, add, req]() {
                    std::string err;
                    if (isRakuyomiSource(sid)) {
                        if (add) aidoku_.addToLibrary(sid, mid, &err);
                        else aidoku_.removeFromLibrary(sid, mid, &err);
                    }
                    state_.upsertMangaMeta(sid, sname, mid, title, coverUrl,
                                           req["author"].str(), req["description"].str(), "",
                                           add ? 1 : 0);
                    if (add) enqueueCoverFetch(sid, mid, title, coverUrl);
                    if (add && anilist_->connected()) {
                        anilist_->onLibraryAdd(sid, mid);
                        requestAnilistSync(3);
                    }
                    pushLibraryEvent();
                    sendToast(add ? ("Added \"" + title + "\" to Library") : ("Removed from Library"));
                });
                break;
            }
            case REQ_OPEN_CHAPTER: {
                handleOpenChapter(req);
                break;
            }
            case REQ_SAVE_PROGRESS: {
                std::string sid = req["source_id"].str();
                std::string sname = req["source_name"].str(sid);
                std::string mid = req["manga_id"].str();
                std::string mtitle = req["manga_title"].str();
                std::string cid = req["chapter_id"].str();
                double chNum = req["chapter_num"].num(0.0);
                bool hasNum = req.has("chapter_num");
                std::string chTitle = req["chapter_title"].str();
                int pageIdx = static_cast<int>(req["page_index"].integer(0));
                int pageCount = static_cast<int>(req["page_count"].integer(0));
                bool completed = req["completed"].boolean(false) ||
                                 (pageCount > 0 && pageIdx >= pageCount - 1);

                state_.saveProgress(sid, sname, mid, mtitle, cid, chNum, hasNum, chTitle,
                                    pageIdx, pageCount, completed);

                if (completed && isRakuyomiSource(sid)) {
                    spawnAsync([this, sid, mid, cid]() {
                        aidoku_.markChapterRead(sid, mid, cid, nullptr);
                    });
                }
                if (completed) {
                    onLocalChapterRead(sid, mid, hasNum ? chNum : state_.chapterNumFor(sid, mid, cid));
                }

                // Pre-download & pre-extract next chapter when within 3 pages of end
                if (pageCount > 0 && pageIdx >= pageCount - 3 &&
                    state_.getSettings()["autoPreloadNext"].boolean(true)) {
                    Json::Value chs = state_.getCachedChapters(sid, mid);
                    Json::Value prevCh, nextCh;
                    findPrevNextChapter(chs, cid, prevCh, nextCh);
                    triggerBackgroundPreload(sid, sname, mid, mtitle, nextCh);
                }

                pushLibraryEvent();
                break;
            }
            case REQ_MARK_CHAPTER_READ: {
                std::string sid = req["source_id"].str();
                std::string mid = req["manga_id"].str();
                std::string cid = req["chapter_id"].str();
                bool read = req["read"].boolean(true);
                state_.markChapterRead(sid, mid, cid, read);
                if (read && isRakuyomiSource(sid)) {
                    spawnAsync([this, sid, mid, cid]() {
                        aidoku_.markChapterRead(sid, mid, cid, nullptr);
                    });
                }
                if (read) onLocalChapterRead(sid, mid, state_.chapterNumFor(sid, mid, cid));
                pushLibraryEvent();
                break;
            }
            case REQ_DOWNLOAD_CHAPTERS: {
                const Json::Value& items = req["items"];
                if (!items.isArray()) break;
                int added = 0;
                {
                    std::lock_guard<std::mutex> lock(dlMu_);
                    for (const auto& it : items.asArray()) {
                        std::string sid = it["source_id"].str();
                        std::string mid = it["manga_id"].str();
                        std::string cid = it["chapter_id"].str();
                        if (sid.empty() || mid.empty() || cid.empty()) continue;

                        std::string ckey = store::makeChapterKey(sid, mid, cid);
                        if (activeDl_ && activeDl_->key == ckey) continue;
                        bool dup = false;
                        for (const auto& q : dlQueue_) {
                            if (q.key == ckey) {
                                dup = true;
                                break;
                            }
                        }
                        if (dup) continue;

                        DownloadTask t;
                        t.key = ckey;
                        t.sourceId = sid;
                        t.sourceName = it["source_name"].str(sid);
                        t.mangaId = mid;
                        t.mangaTitle = it["manga_title"].str(mid);
                        t.chapterId = cid;
                        t.chapterNum = it["chapter_num"].num(0.0);
                        t.hasChapterNum = it.has("chapter_num");
                        t.chapterTitle = it["chapter_title"].str();
                        dlQueue_.push_back(std::move(t));
                        ++added;
                    }
                }
                if (added > 0) {
                    dlCv_.notify_one();
                    pushDownloadsEvent();
                    sendToast("Queued " + std::to_string(added) + (added == 1 ? " chapter" : " chapters"));
                }
                break;
            }
            case REQ_CANCEL_DOWNLOAD: {
                bool cancelAll = req["all"].boolean(false);
                std::string key = req["key"].str();
                {
                    std::lock_guard<std::mutex> lock(dlMu_);
                    if (cancelAll) {
                        dlQueue_.clear();
                        if (activeDl_) cancelActiveDl_ = true;
                    } else if (!key.empty()) {
                        if (activeDl_ && activeDl_->key == key) {
                            cancelActiveDl_ = true;
                        } else {
                            dlQueue_.erase(
                                std::remove_if(dlQueue_.begin(), dlQueue_.end(),
                                               [&](const DownloadTask& t) { return t.key == key; }),
                                dlQueue_.end());
                        }
                    }
                }
                pushDownloadsEvent();
                break;
            }
            case REQ_DELETE_DOWNLOAD: {
                if (req["clear_cache"].boolean(false)) {
                    pageCache_.clearAll();
                    pushDownloadsEvent();
                    sendToast("Cleared temporary page cache");
                } else {
                    state_.removeDownloadRecord(req["source_id"].str(),
                                                req["manga_id"].str(),
                                                req["chapter_id"].str());
                    pushDownloadsEvent();
                    pushLibraryEvent();
                }
                break;
            }
            case REQ_LIST_SOURCES: {
                bool includeAvail = req["include_available"].boolean(true);
                spawnAsync([this, includeAvail]() {
                    Json::Value cfg = aidoku_.getSourceConfig();
                    bool hasLists = hasAnySourceLists();
                    Json::Value ev = Json::Value::object();
                    bool online = false;
                    ev["installed"] = buildInstalledSourcesList(&online);
                    ev["available"] = Json::Value::array();
                    ev["source_config"] = cfg;
                    ev["loading"] = includeAvail && hasLists;
                    ev["error"] = "";
                    ipc_.sendJson(EVT_SOURCES_LIST, ev);

                    if (includeAvail && hasLists) {
                        std::string availErr;
                        Json::Value avail = listAllAvailableSources(online, &availErr);
                        Json::Value ev2 = Json::Value::object();
                        ev2["installed"] = buildInstalledSourcesList(nullptr);
                        ev2["available"] = avail.isArray() ? avail : Json::Value::array();
                        ev2["source_config"] = aidoku_.getSourceConfig();
                        ev2["loading"] = false;
                        ev2["error"] = availErr;
                        ipc_.sendJson(EVT_SOURCES_LIST, ev2);
                    }
                });
                break;
            }
            case REQ_INSTALL_SOURCE: {
                std::string sid = req["source_id"].str();
                bool install = req["install"].boolean(true);
                if (sid.empty()) break;

                spawnAsync([this, sid, install]() {
                    std::string err;
                    bool ok;
                    if (aidokurun::isV2Source(sid)) {
                        ok = install ? arun_.install(sid, aidoku_.v2SourceLists(), aidoku_.languages(), &err)
                                     : arun_.uninstall(sid, &err);
                    } else {
                        ok = install ? aidoku_.installSource(sid, &err) : aidoku_.uninstallSource(sid, &err);
                    }
                    invalidateDiscoverCache();
                    if (!ok) {
                        sendToast("Source error: " + err, true);
                    } else {
                        sendToast(install ? ("Installed " + sid) : ("Removed " + sid));
                    }
                    Json::Value cfg = aidoku_.getSourceConfig();
                    bool hasLists = hasAnySourceLists();
                    Json::Value ev = Json::Value::object();
                    bool online = false;
                    ev["installed"] = buildInstalledSourcesList(&online);
                    Json::Value avail = hasLists ? listAllAvailableSources(online, nullptr) : Json::Value::array();
                    ev["available"] = avail.isArray() ? avail : Json::Value::array();
                    ev["source_config"] = cfg;
                    ev["loading"] = false;
                    ev["error"] = ok ? "" : err;
                    ipc_.sendJson(EVT_SOURCES_LIST, ev);
                });
                break;
            }
            case REQ_UPDATE_SETTINGS: {
                Json::Value updated = state_.updateSettings(req);
                Json::Value ev = Json::Value::object();
                ev["library"] = state_.listLibrary();
                ev["continue_reading"] = state_.getContinueReading();
                ev["recent"] = state_.listRecent();
                ev["downloads"] = state_.listDownloads();
                ev["queue"] = serializeDownloadQueue();
                ev["settings"] = updated;
                bool online = false;
                ev["installed_sources"] = buildInstalledSourcesList(&online);
                ev["source_config"] = aidoku_.getSourceConfig();
                ev["aidoku_online"] = online;
                ipc_.sendJson(EVT_BOOTSTRAP, ev);
                break;
            }
            case REQ_OPEN_LINK: {
                std::string text = req["text"].str();
                spawnAsync([this, text]() {
                    std::string err;
                    Json::Value res = resolveLink(text, &err);
                    Json::Value ev = Json::Value::object();
                    ev["text"] = text;
                    ev["ok"] = res.isObject() && !res["source_id"].str().empty();
                    for (const auto& kv : res.asObject()) ev[kv.first] = kv.second;
                    ev["error"] = ev["ok"].boolean(false) ? std::string() : (err.empty() ? "Not found" : err);
                    ipc_.sendJson(EVT_OPEN_LINK, ev);
                });
                break;
            }
            case REQ_SAVE_SOURCE_CONFIG: {
                spawnAsync([this, req]() {
                    bool reloadDisk = req["reload_disk"].boolean(false);
                    std::string err;
                    if (reloadDisk) {
                        aidoku_.ensureSettingsInitialized();
                        aidoku_.restartServer();
                    } else {
                        if (!aidoku_.saveSourceConfig(req, &err)) {
                            sendToast(err, true);
                            Json::Value errEv = Json::Value::object();
                            errEv["installed"] = buildInstalledSourcesList(nullptr);
                            errEv["available"] = Json::Value::array();
                            errEv["source_config"] = aidoku_.getSourceConfig();
                            errEv["loading"] = false;
                            errEv["error"] = err;
                            ipc_.sendJson(EVT_SOURCES_LIST, errEv);
                            return;
                        }
                    }

                    invalidateDiscoverCache();
                    applyLanguages();
                    arun_.applyLanguageSettingAll(aidoku_.languages());
                    Json::Value cfg = aidoku_.getSourceConfig();
                    bool hasLists = hasAnySourceLists();
                    bool online = false;
                    Json::Value installed = buildInstalledSourcesList(&online);

                    if (hasLists) {
                        Json::Value loadingEv = Json::Value::object();
                        loadingEv["installed"] = installed;
                        loadingEv["available"] = Json::Value::array();
                        loadingEv["source_config"] = cfg;
                        loadingEv["loading"] = true;
                        loadingEv["error"] = "";
                        ipc_.sendJson(EVT_SOURCES_LIST, loadingEv);
                    }

                    std::string availErr;
                    Json::Value avail = hasLists ? listAllAvailableSources(online, &availErr)
                                                 : Json::Value::array();
                    Json::Value doneEv = Json::Value::object();
                    doneEv["installed"] = buildInstalledSourcesList(&online);
                    doneEv["available"] = avail.isArray() ? avail : Json::Value::array();
                    doneEv["source_config"] = aidoku_.getSourceConfig();
                    doneEv["loading"] = false;
                    doneEv["error"] = availErr;
                    ipc_.sendJson(EVT_SOURCES_LIST, doneEv);

                    if (!availErr.empty()) {
                        sendToast("Saved settings.json, fetch error: " + availErr, true);
                    } else if (reloadDisk) {
                        sendToast("Reloaded settings.json (" + std::to_string(doneEv["available"].size()) + " available)");
                    } else if (hasLists) {
                        sendToast("Saved source config (" + std::to_string(doneEv["available"].size()) + " sources available)");
                    } else {
                        sendToast("Cleared source lists");
                    }
                });
                break;
            }
            default:
                break;
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <appload_socket_path>\n";
        return 1;
    }

    fs::path workDir = fs::current_path();
    if (const char* envDir = std::getenv("RM2MANGA_DATA_ROOT")) {
        workDir = envDir;
    }

    Rm2MangaApp app(workDir);
    if (!app.start(argv[1])) {
        return 1;
    }
    app.run();
    return 0;
}
