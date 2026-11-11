#pragma once

// Second Aidoku engine: drives `aidoku-run` (https://github.com/ohaiibuzzle/aidoku-backend-0.7),
// a Go reimplementation of AidokuRunner that executes *new-format* Aidoku sources
// (Aidoku 0.7+, e.g. https://aidoku-community.github.io/sources/index.min.json).
// rakuyomi (aidoku_client.hpp) only understands the legacy source format, and the
// legacy list has lost most sources (no working Russian ones at all), so this engine
// unlocks MangaLib, ReManga, InkStory, MangaDex v14, Weeb Central v8, ...
//
// aidoku-run is a one-shot CLI (no daemon): every call loads the .aix fresh and prints
// JSON on stdout. Source IDs exposed to the rest of the app are prefixed with "v2." so
// they never collide with legacy rakuyomi sources of the same key (e.g. multi.mangadex).

#include "json.hpp"
#include "util.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace aidokurun {

namespace fs = std::filesystem;

inline const std::string kPrefix = "v2.";

inline bool isV2Source(const std::string& sid) {
    return sid.rfind(kPrefix, 0) == 0;
}
inline std::string toKey(const std::string& sid) {
    return isV2Source(sid) ? sid.substr(kPrefix.size()) : sid;
}
inline std::string toSid(const std::string& key) {
    return kPrefix + key;
}

struct RunResult {
    int exitCode = -1;
    bool cancelled = false;
    bool timedOut = false;
    std::string out;
    std::string errTail;  // last ~4KB of stderr
};

class Client {
    fs::path home_;
    std::string bin_;

    std::mutex manifestMu_;
    struct ManifestEntry {
        fs::file_time_type mtime;
        Json::Value manifest;
    };
    std::map<std::string, ManifestEntry> manifestCache_;  // path -> manifest

    std::mutex availMu_;
    Json::Value availCache_ = Json::Value::array();
    std::map<std::string, std::string> availListUrl_;  // key -> index url
    std::string availCacheSig_;
    int64_t availCacheTs_ = 0;

    // Limit concurrent aidoku-run processes (each ~30MB RSS, rM2 has 1GB and no swap)
    std::mutex semMu_;
    std::condition_variable semCv_;
    int running_ = 0;
    static constexpr int kMaxParallel = 2;  // each aidoku-run ~30MB RSS; rM2 has no swap

    std::mutex discoverMu_;
    std::map<std::string, std::pair<int64_t, Json::Value>> discoverCache_;  // sid -> (ts, items)

public:
    Client(const fs::path& home, const fs::path& defaultBin) {
        std::error_code ec;
        home_ = fs::absolute(home, ec);
        if (const char* e = std::getenv("RM2MANGA_AIDOKU_RUN")) {
            bin_ = e;
        } else {
            bin_ = fs::absolute(defaultBin, ec).string();
        }
        fs::create_directories(sourcesDir(), ec);
        fs::create_directories(settingsDir(), ec);
        fs::create_directories(downloadsDir(), ec);
    }

    bool available() const { return ::access(bin_.c_str(), X_OK) == 0; }
    fs::path sourcesDir() const { return home_ / "sources"; }
    fs::path settingsDir() const { return home_ / "settings"; }
    fs::path downloadsDir() const { return home_ / "downloads"; }

    // ---------------------------------------------------------------- process

    RunResult run(const std::vector<std::string>& args,
                  int timeoutSec,
                  const std::atomic<bool>* cancelFlag = nullptr,
                  const std::function<void(const std::string&)>& onStderrLine = nullptr) {
        RunResult res;
        if (!available()) {
            res.errTail = "aidoku-run binary missing at " + bin_;
            return res;
        }

        {
            std::unique_lock<std::mutex> lk(semMu_);
            semCv_.wait(lk, [this]() { return running_ < kMaxParallel; });
            ++running_;
        }
        struct SemGuard {
            Client* c;
            ~SemGuard() {
                std::lock_guard<std::mutex> lk(c->semMu_);
                --c->running_;
                c->semCv_.notify_one();
            }
        } guard{this};

        int outPipe[2], errPipe[2];
        if (::pipe(outPipe) != 0) return res;
        if (::pipe(errPipe) != 0) {
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            return res;
        }

        // Build argv/envp before fork (no allocation in the child)
        std::vector<std::string> argStore;
        argStore.push_back(bin_);
        for (const auto& a : args) argStore.push_back(a);
        std::vector<char*> argv;
        for (auto& s : argStore) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);

        std::vector<std::string> envStore;
        for (char** e = environ; e && *e; ++e) {
            if (std::strncmp(*e, "AIDOKU_SETTINGS_DIR=", 20) == 0) continue;
            envStore.emplace_back(*e);
        }
        envStore.push_back("AIDOKU_SETTINGS_DIR=" + settingsDir().string());
        std::vector<char*> envp;
        for (auto& s : envStore) envp.push_back(const_cast<char*>(s.c_str()));
        envp.push_back(nullptr);

        std::string cwd = home_.string();

        pid_t pid = ::fork();
        if (pid < 0) {
            ::close(outPipe[0]); ::close(outPipe[1]);
            ::close(errPipe[0]); ::close(errPipe[1]);
            return res;
        }
        if (pid == 0) {
            ::setpgid(0, 0);
            ::dup2(outPipe[1], STDOUT_FILENO);
            ::dup2(errPipe[1], STDERR_FILENO);
            ::close(outPipe[0]); ::close(outPipe[1]);
            ::close(errPipe[0]); ::close(errPipe[1]);
            if (::chdir(cwd.c_str()) != 0) { /* ignore */ }
            ::execve(argv[0], argv.data(), envp.data());
            _exit(127);
        }

        ::close(outPipe[1]);
        ::close(errPipe[1]);
        for (int fd : {outPipe[0], errPipe[0]}) {
            int fl = ::fcntl(fd, F_GETFL, 0);
            if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
        bool outOpen = true, errOpen = true;
        std::string errLineBuf, errAll;
        char buf[8192];

        while (outOpen || errOpen) {
            if (cancelFlag && cancelFlag->load()) {
                ::kill(-pid, SIGKILL);
                ::kill(pid, SIGKILL);
                res.cancelled = true;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                ::kill(-pid, SIGKILL);
                ::kill(pid, SIGKILL);
                res.timedOut = true;
                break;
            }
            struct pollfd pfds[2];
            int n = 0;
            int outIdx = -1, errIdx = -1;
            if (outOpen) { pfds[n] = {outPipe[0], POLLIN | POLLHUP, 0}; outIdx = n++; }
            if (errOpen) { pfds[n] = {errPipe[0], POLLIN | POLLHUP, 0}; errIdx = n++; }
            int pr = ::poll(pfds, n, 250);
            if (pr <= 0) continue;

            auto drain = [&](int fd, bool& open, bool isErr) {
                while (true) {
                    ssize_t r = ::read(fd, buf, sizeof(buf));
                    if (r > 0) {
                        if (!isErr) {
                            res.out.append(buf, static_cast<size_t>(r));
                        } else {
                            errAll.append(buf, static_cast<size_t>(r));
                            if (errAll.size() > 8192) errAll.erase(0, errAll.size() - 4096);
                            errLineBuf.append(buf, static_cast<size_t>(r));
                            size_t nl;
                            while ((nl = errLineBuf.find_first_of("\r\n")) != std::string::npos) {
                                std::string line = errLineBuf.substr(0, nl);
                                errLineBuf.erase(0, nl + 1);
                                if (!line.empty() && onStderrLine) onStderrLine(line);
                            }
                        }
                    } else if (r == 0) {
                        open = false;
                        break;
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) open = false;
                        break;
                    }
                }
            };
            if (outIdx >= 0 && pfds[outIdx].revents) drain(outPipe[0], outOpen, false);
            if (errIdx >= 0 && pfds[errIdx].revents) drain(errPipe[0], errOpen, true);
        }
        ::close(outPipe[0]);
        ::close(errPipe[0]);

        int status = 0;
        ::waitpid(pid, &status, 0);
        if (WIFEXITED(status)) res.exitCode = WEXITSTATUS(status);
        res.errTail = errAll.size() > 4096 ? errAll.substr(errAll.size() - 4096) : errAll;
        return res;
    }

    static std::string lastErrorLine(const RunResult& r) {
        if (r.cancelled) return "Cancelled";
        if (r.timedOut) return "Source timed out";
        std::string s = r.errTail;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        size_t nl = s.find_last_of('\n');
        std::string line = (nl == std::string::npos) ? s : s.substr(nl + 1);
        if (line.rfind("error: ", 0) == 0) line = line.substr(7);
        if (line.size() > 200) line = line.substr(0, 200) + "...";
        return line.empty() ? ("aidoku-run exited with " + std::to_string(r.exitCode)) : line;
    }

    Json::Value runJson(const std::vector<std::string>& args, int timeoutSec, std::string* errOut,
                        const std::atomic<bool>* cancelFlag = nullptr) {
        RunResult r = run(args, timeoutSec, cancelFlag);
        if (r.exitCode != 0 || r.cancelled || r.timedOut) {
            if (errOut) *errOut = lastErrorLine(r);
            return Json::Value();
        }
        // Strip any non-JSON preamble ("[print] ..." lines from sources)
        size_t start = r.out.find_first_of("{[");
        if (start == std::string::npos) {
            if (errOut) *errOut = "Empty response from source";
            return Json::Value();
        }
        bool ok = false;
        Json::Value v = Json::parse(r.out.substr(start), &ok);
        if (!ok) {
            if (errOut) *errOut = "Invalid JSON from aidoku-run";
            return Json::Value();
        }
        return v;
    }

    // ---------------------------------------------------------------- sources

    // Returns [{path, key, name, version, languages}]
    std::vector<Json::Value> installedManifests() {
        std::vector<Json::Value> out;
        std::error_code ec;
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(sourcesDir(), ec)) {
            if (e.is_regular_file(ec) && e.path().extension() == ".aix") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& p : files) {
            auto mt = fs::last_write_time(p, ec);
            {
                std::lock_guard<std::mutex> lk(manifestMu_);
                auto it = manifestCache_.find(p.string());
                if (it != manifestCache_.end() && it->second.mtime == mt) {
                    out.push_back(it->second.manifest);
                    continue;
                }
            }
            std::string err;
            Json::Value m = runJson({p.string(), "manifest"}, 20, &err);
            if (!m.isObject() || m["key"].str().empty()) continue;
            m["path"] = p.string();
            {
                std::lock_guard<std::mutex> lk(manifestMu_);
                manifestCache_[p.string()] = ManifestEntry{mt, m};
            }
            out.push_back(m);
        }
        return out;
    }

    std::string pathForSid(const std::string& sid) {
        std::string key = toKey(sid);
        for (const auto& m : installedManifests()) {
            if (m["key"].str() == key) return m["path"].str();
        }
        return "";
    }

    std::string nameForSid(const std::string& sid) {
        std::string key = toKey(sid);
        for (const auto& m : installedManifests()) {
            if (m["key"].str() == key) return m["name"].str(key);
        }
        return key;
    }

    Json::Value listInstalled() {
        Json::Value list = Json::Value::array();
        if (!available()) return list;
        for (const auto& m : installedManifests()) {
            Json::Value s = Json::Value::object();
            s["id"] = toSid(m["key"].str());
            s["name"] = m["name"].str(m["key"].str());
            s["version"] = m["version"].integer(0);
            s["engine"] = "aidoku-run";
            s["builtin"] = false;
            list.push(std::move(s));
        }
        return list;
    }

    static bool langMatch(const Json::Value& srcLangs, const std::set<std::string>& wanted) {
        if (wanted.empty()) return true;
        if (!srcLangs.isArray() || srcLangs.empty()) return true;
        for (const auto& l : srcLangs.asArray()) {
            std::string s = l.str();
            if (s == "multi" || wanted.count(s)) return true;
        }
        return false;
    }

    Json::Value listAvailable(const std::vector<std::string>& lists,
                              const std::vector<std::string>& langs,
                              std::string* errOut = nullptr,
                              bool force = false) {
        std::string sig;
        for (const auto& u : lists) sig += u + "|";
        sig += "#";
        for (const auto& l : langs) sig += l + ",";
        {
            std::lock_guard<std::mutex> lk(availMu_);
            if (!force && sig == availCacheSig_ && util::nowSeconds() - availCacheTs_ < 600 &&
                availCache_.isArray() && !availCache_.empty()) {
                return availCache_;
            }
        }

        std::set<std::string> wanted(langs.begin(), langs.end());
        Json::Value out = Json::Value::array();
        std::map<std::string, std::string> keyToList;
        std::set<std::string> seen;
        std::string firstErr;

        for (const auto& url : lists) {
            std::string err;
            Json::Value idx = runJson({"-", "repo", "list", url}, 60, &err);
            if (!idx.isObject() || !idx["sources"].isArray()) {
                if (firstErr.empty()) firstErr = err.empty() ? ("Failed to read " + url) : err;
                continue;
            }
            for (const auto& s : idx["sources"].asArray()) {
                std::string key = s["id"].str();
                if (key.empty() || seen.count(key)) continue;
                if (!langMatch(s["languages"], wanted)) continue;
                seen.insert(key);
                keyToList[key] = url;
                Json::Value item = Json::Value::object();
                item["id"] = toSid(key);
                item["name"] = s["name"].str(key);
                item["version"] = s["version"].integer(0);
                item["engine"] = "aidoku-run";
                Json::Value langsArr = s["languages"];
                item["languages"] = langsArr.isArray() ? langsArr : Json::Value::array();
                item["nsfw"] = s["contentRating"].integer(0) >= 2;
                out.push(std::move(item));
            }
        }

        if (errOut && out.empty()) *errOut = firstErr;
        std::lock_guard<std::mutex> lk(availMu_);
        if (!out.empty()) {
            availCache_ = out;
            availListUrl_ = keyToList;
            availCacheSig_ = sig;
            availCacheTs_ = util::nowSeconds();
        }
        return out;
    }

    bool install(const std::string& sid,
                 const std::vector<std::string>& lists,
                 const std::vector<std::string>& langs,
                 std::string* errOut) {
        std::string key = toKey(sid);
        std::string url;
        {
            std::lock_guard<std::mutex> lk(availMu_);
            auto it = availListUrl_.find(key);
            if (it != availListUrl_.end()) url = it->second;
        }
        if (url.empty()) {
            listAvailable(lists, langs, nullptr, true);
            std::lock_guard<std::mutex> lk(availMu_);
            auto it = availListUrl_.find(key);
            if (it != availListUrl_.end()) url = it->second;
        }
        if (url.empty()) {
            if (errOut) *errOut = "Source " + key + " not found in any source list";
            return false;
        }

        fs::path tmpDir = home_ / ("install-tmp-" + util::hashSlug(key));
        std::error_code ec;
        fs::remove_all(tmpDir, ec);
        fs::create_directories(tmpDir, ec);
        RunResult r = run({"-", "repo", "install", url, key, tmpDir.string()}, 120);
        if (r.exitCode != 0) {
            if (errOut) *errOut = lastErrorLine(r);
            fs::remove_all(tmpDir, ec);
            return false;
        }
        fs::path newFile;
        for (const auto& e : fs::directory_iterator(tmpDir, ec)) {
            if (e.path().extension() == ".aix") newFile = e.path();
        }
        if (newFile.empty()) {
            if (errOut) *errOut = "Install produced no .aix file";
            fs::remove_all(tmpDir, ec);
            return false;
        }
        removeInstalledKey(key);
        fs::rename(newFile, sourcesDir() / (key + ".aix"), ec);
        if (ec) {
            fs::copy_file(newFile, sourcesDir() / (key + ".aix"), fs::copy_options::overwrite_existing, ec);
        }
        fs::remove_all(tmpDir, ec);
        bool ok = !ec;
        if (ok) applyLanguageSetting((sourcesDir() / (key + ".aix")).string(), langs);
        clearDiscoverCache();
        return ok;
    }

    // Many multi-language sources (e.g. MangaDex) expose a "languages" multi-select setting that
    // defaults to English only. Mirror the app's configured languages into it.
    void applyLanguageSetting(const std::string& aixPath, const std::vector<std::string>& langs) {
        std::string err;
        Json::Value defs = runJson({aixPath, "settings"}, 30, &err);
        if (!defs.isArray()) return;
        std::function<void(const Json::Value&)> walk = [&](const Json::Value& items) {
            for (const auto& it : items.asArray()) {
                if (it["Type"].str() == "group" && it["Group"]["Items"].isArray()) {
                    walk(it["Group"]["Items"]);
                    continue;
                }
                if (it["Key"].str() != "languages" || it["Type"].str() != "multi-select") continue;
                std::vector<std::string> pick;
                for (const auto& want : langs) {
                    for (const auto& v : it["Multiselect"]["Values"].asArray()) {
                        if (util::toLower(v.str()) == util::toLower(want)) pick.push_back(v.str());
                    }
                }
                if (pick.empty()) continue;
                std::vector<std::string> args = {aixPath, "settings", "set", "languages", "multiselect"};
                for (const auto& p : pick) args.push_back(p);
                run(args, 30);
            }
        };
        walk(defs);
    }

    void applyLanguageSettingAll(const std::vector<std::string>& langs) {
        for (const auto& m : installedManifests()) {
            if (m["languages"].isArray()) {
                bool multi = m["languages"].size() > 1;
                for (const auto& l : m["languages"].asArray()) if (l.str() == "multi") multi = true;
                if (!multi) continue;
            }
            applyLanguageSetting(m["path"].str(), langs);
        }
        clearDiscoverCache();
    }

    bool uninstall(const std::string& sid, std::string* errOut) {
        bool removed = removeInstalledKey(toKey(sid));
        clearDiscoverCache();
        if (!removed && errOut) *errOut = "Source not installed";
        return removed;
    }

    // ---------------------------------------------------------------- content

    static Json::Value mapManga(const Json::Value& e, const std::string& sid, const std::string& sname) {
        Json::Value m = Json::Value::object();
        m["id"] = e["Key"].str();
        m["title"] = e["Title"].str(e["Key"].str());
        Json::Value src = Json::Value::object();
        src["id"] = sid;
        src["name"] = sname;
        src["version"] = 0;
        m["source"] = src;
        m["source_id"] = sid;
        m["source_name"] = sname;
        m["cover_url"] = e["Cover"].str();
        std::string author;
        if (e["Authors"].isArray() && !e["Authors"].empty()) author = e["Authors"][size_t(0)].str();
        else if (e["Artists"].isArray() && !e["Artists"].empty()) author = e["Artists"][size_t(0)].str();
        m["author"] = author;
        m["description"] = e["Description"].str();
        int st = static_cast<int>(e["Status"].integer(0));
        static const char* kStatus[] = {"unknown", "ongoing", "completed", "cancelled", "hiatus"};
        m["status"] = (st >= 0 && st <= 4) ? kStatus[st] : "unknown";
        return m;
    }

    void clearDiscoverCache() {
        std::lock_guard<std::mutex> lk(discoverMu_);
        discoverCache_.clear();
    }

    // Searches one installed source. Empty query = source's default listing (discover).
    Json::Value searchSource(const std::string& sid, const std::string& query, std::string* errOut) {
        if (query.empty()) {
            std::lock_guard<std::mutex> lk(discoverMu_);
            auto it = discoverCache_.find(sid);
            if (it != discoverCache_.end() && util::nowSeconds() - it->second.first < 300) {
                return it->second.second;
            }
        }
        std::string path = pathForSid(sid);
        if (path.empty()) {
            if (errOut) *errOut = "Source not installed";
            return Json::Value::array();
        }
        std::string sname = nameForSid(sid);
        Json::Value res = runJson({path, "search", query}, 45, errOut);
        Json::Value out = Json::Value::array();
        if (res.isObject() && res["Entries"].isArray()) {
            for (const auto& e : res["Entries"].asArray()) {
                if (e["Key"].str().empty()) continue;
                out.push(mapManga(e, sid, sname));
            }
        }
        if (query.empty() && !out.empty()) {
            std::lock_guard<std::mutex> lk(discoverMu_);
            discoverCache_[sid] = {util::nowSeconds(), out};
        }
        return out;
    }

    // Searches all installed sources (or only `onlySid`) in parallel.
    Json::Value search(const std::string& query, const std::string& onlySid, std::string* errOut) {
        std::vector<std::string> sids;
        if (!onlySid.empty()) {
            sids.push_back(onlySid);
        } else {
            for (const auto& m : installedManifests()) sids.push_back(toSid(m["key"].str()));
        }
        std::vector<Json::Value> results(sids.size());
        std::vector<std::string> errs(sids.size());
        std::vector<std::thread> ths;
        for (size_t i = 0; i < sids.size(); ++i) {
            ths.emplace_back([&, i]() { results[i] = searchSource(sids[i], query, &errs[i]); });
        }
        for (auto& t : ths) t.join();

        Json::Value out = Json::Value::array();
        for (size_t i = 0; i < sids.size(); ++i) {
            for (const auto& m : results[i].asArray()) out.push(m);
            if (errOut && errOut->empty() && !errs[i].empty()) {
                *errOut = nameForSid(sids[i]) + ": " + errs[i];
            }
        }
        return out;
    }

    // Returns {"manga": {...}, "chapters": [...]} ; chapters filtered to `langs` when possible
    Json::Value getManga(const std::string& sid, const std::string& mangaId,
                         const std::vector<std::string>& langs, std::string* errOut) {
        Json::Value out = Json::Value::object();
        std::string path = pathForSid(sid);
        if (path.empty()) {
            if (errOut) *errOut = "Source " + toKey(sid) + " is not installed";
            return out;
        }
        Json::Value res = runJson({path, "manga", mangaId}, 90, errOut);
        if (!res.isObject()) return out;

        out["manga"] = mapManga(res, sid, nameForSid(sid));

        std::set<std::string> wanted(langs.begin(), langs.end());
        wanted.erase("multi");
        Json::Value all = Json::Value::array();
        Json::Value filtered = Json::Value::array();
        std::set<std::string> chapterLangs;
        size_t lockedCount = 0;
        if (res["Chapters"].isArray()) {
            for (const auto& c : res["Chapters"].asArray()) {
                if (!c["Locked"].isNull() && c["Locked"].boolean(false)) {
                    ++lockedCount;
                    continue;
                }
                Json::Value ch = Json::Value::object();
                ch["id"] = c["Key"].str();
                if (ch["id"].str().empty()) continue;
                if (c["ChapterNumber"].isNumber()) ch["chapter_num"] = c["ChapterNumber"].num(0.0);
                if (c["VolumeNumber"].isNumber() && c["VolumeNumber"].num(0.0) > 0) {
                    ch["volume_num"] = c["VolumeNumber"].num(0.0);
                }
                ch["title"] = c["Title"].str();
                std::string scan;
                if (c["Scanlators"].isArray()) {
                    for (const auto& s : c["Scanlators"].asArray()) {
                        if (s.str().empty()) continue;
                        if (!scan.empty()) scan += ", ";
                        scan += s.str();
                    }
                }
                std::string lang = c["Language"].str();
                ch["language"] = lang;
                ch["scanlator"] = scan;
                ch["read"] = false;
                ch["downloaded"] = false;
                if (!lang.empty()) chapterLangs.insert(lang);
                all.push(ch);
                if (lang.empty() || wanted.empty() || wanted.count(lang)) filtered.push(ch);
            }
        }
        // An empty list with no error otherwise just shows "No Chapters" in the UI, which looks
        // like the app is broken. Explain it instead (e.g. MangaLib hides licensed / restricted
        // titles from logged-out users, and aidoku-run has no web login support).
        if (all.empty() && errOut && errOut->empty()) {
            *errOut = lockedCount > 0
                ? "All " + std::to_string(lockedCount) + " chapters are locked (paid or login required)"
                : "Source returned no chapters. The title may be licensed, removed, or need a site login (not supported)";
        }
        Json::Value& chosen = filtered.empty() ? all : filtered;
        // Tag scanlator with language when several languages are shown (e.g. MangaDex en+ru)
        std::set<std::string> shownLangs;
        for (const auto& c : chosen.asArray()) {
            if (!c["language"].str().empty()) shownLangs.insert(c["language"].str());
        }
        if (shownLangs.size() > 1) {
            for (auto& c : chosen.asArrayMut()) {
                std::string l = util::toLower(c["language"].str());
                std::string up;
                for (char ch : l) up += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                std::string scan = c["scanlator"].str();
                c["scanlator"] = "[" + up + "]" + (scan.empty() ? "" : " " + scan);
            }
        }
        out["chapters"] = chosen;
        return out;
    }

    // Number of readable (unlocked) chapters, or -1 when the source could not be asked.
    // Used to warn about entries whose chapters are hidden (licensed / login required).
    int chapterCount(const std::string& sid, const std::string& mangaId, std::string* errOut = nullptr) {
        std::string path = pathForSid(sid);
        if (path.empty()) {
            if (errOut) *errOut = "Source not installed";
            return -1;
        }
        Json::Value res = runJson({path, "manga", mangaId}, 60, errOut);
        if (!res.isObject() || !res["Chapters"].isArray()) return -1;
        int n = 0;
        for (const auto& c : res["Chapters"].asArray()) {
            if (!c["Locked"].isNull() && c["Locked"].boolean(false)) continue;
            ++n;
        }
        return n;
    }

    // Downloads a chapter to a CBZ under downloads/. Returns the CBZ path or "".
    std::string downloadChapter(const std::string& sid,
                                const std::string& mangaId,
                                const std::string& chapterId,
                                const std::atomic<bool>* cancelFlag,
                                const std::function<void(const std::string&, int)>& onProgress,
                                std::string* errOut) {
        std::string path = pathForSid(sid);
        if (path.empty()) {
            if (errOut) *errOut = "Source " + toKey(sid) + " is not installed";
            return "";
        }
        fs::path dir = downloadsDir() / toKey(sid) / util::hashSlug(mangaId);
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path cbz = dir / (util::hashSlug(chapterId) + ".cbz");
        if (fs::exists(cbz, ec) && fs::file_size(cbz, ec) > 0) return cbz.string();

        fs::path part = cbz;
        part += ".part.cbz";
        fs::remove(part, ec);

        RunResult r = run({path, "download", mangaId, chapterId, part.string()}, 900, cancelFlag,
                          [&](const std::string& line) {
                              // "[download] page 12/51"
                              size_t p = line.find("page ");
                              size_t slash = line.find('/', p == std::string::npos ? 0 : p);
                              if (p == std::string::npos || slash == std::string::npos) return;
                              int cur = std::atoi(line.c_str() + p + 5);
                              int tot = std::atoi(line.c_str() + slash + 1);
                              if (tot <= 0 || !onProgress) return;
                              int pct = 15 + (cur * 70) / tot;
                              onProgress("Downloading page " + std::to_string(cur) + " of " +
                                             std::to_string(tot) + "...",
                                         pct);
                          });
        if (r.exitCode != 0 || r.cancelled || r.timedOut) {
            fs::remove(part, ec);
            if (errOut) *errOut = r.cancelled ? "Download cancelled" : lastErrorLine(r);
            return "";
        }
        if (!fs::exists(part, ec) || fs::file_size(part, ec) == 0) {
            if (errOut) *errOut = "Source returned no pages";
            return "";
        }
        fs::rename(part, cbz, ec);
        if (ec) {
            if (errOut) *errOut = "Failed to finalize CBZ";
            return "";
        }
        return cbz.string();
    }

private:
    bool removeInstalledKey(const std::string& key) {
        bool removed = false;
        for (const auto& m : installedManifests()) {
            if (m["key"].str() == key) {
                std::error_code ec;
                fs::remove(m["path"].str(), ec);
                removed = removed || !ec;
                std::lock_guard<std::mutex> lk(manifestMu_);
                manifestCache_.erase(m["path"].str());
            }
        }
        return removed;
    }
};

}  // namespace aidokurun
