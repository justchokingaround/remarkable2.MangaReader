#pragma once

#include "../../../backend/src/json.hpp"
#include "../../../backend/src/util.hpp"
#include <string>
#include <map>
#include <vector>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

namespace kindle {

struct MangaProgress {
    std::string manga_title;
    std::string chapter_title;
    int page_index = 0;
    int total_pages = 0;
    time_t last_read = 0;
};

struct AppSettings {
    int frontlight = 12; // 0 to 24
    std::string reading_direction = "RTL"; // "RTL" or "LTR"
    bool trim_margins = true;
    bool button_inverted = false;
    int refresh_interval = 8;
};

class StateManager {
private:
    std::string state_file_ = "/mnt/us/manga/state.json";
    AppSettings settings_;
    std::string active_manga_;
    std::map<std::string, MangaProgress> progress_map_;

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
        }

        if (root.has("history") && root["history"].isObject()) {
            for (const auto& kv : root["history"].asObject()) {
                MangaProgress p;
                p.manga_title = kv.first;
                p.chapter_title = kv.second["chapter_title"].str();
                p.page_index = static_cast<int>(kv.second["page_index"].integer(0));
                p.total_pages = static_cast<int>(kv.second["total_pages"].integer(0));
                p.last_read = static_cast<time_t>(kv.second["last_read"].integer(0));
                progress_map_[kv.first] = p;
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
        root["settings"] = s;

        Json::Value h = Json::Value::object();
        for (const auto& kv : progress_map_) {
            Json::Value p = Json::Value::object();
            p["chapter_title"] = kv.second.chapter_title;
            p["page_index"] = static_cast<double>(kv.second.page_index);
            p["total_pages"] = static_cast<double>(kv.second.total_pages);
            p["last_read"] = static_cast<double>(kv.second.last_read);
            h[kv.first] = p;
        }
        root["history"] = h;

        std::string jsonStr = Json::stringify(root);
        util::atomicWriteFile(state_file_, jsonStr);
    }

    void updateProgress(const std::string& manga, const std::string& chapter, int page, int total) {
        if (manga.empty()) return;
        active_manga_ = manga;

        MangaProgress p;
        p.manga_title = manga;
        p.chapter_title = chapter;
        p.page_index = page;
        p.total_pages = total;
        p.last_read = std::time(nullptr);
        progress_map_[manga] = p;

        save();
    }

    bool getProgress(const std::string& manga, MangaProgress& out) const {
        auto it = progress_map_.find(manga);
        if (it != progress_map_.end()) {
            out = it->second;
            return true;
        }
        return false;
    }

    bool getHeroManga(MangaProgress& out) const {
        if (!active_manga_.empty()) {
            auto it = progress_map_.find(active_manga_);
            if (it != progress_map_.end()) {
                out = it->second;
                return true;
            }
        }
        // Fallback to highest timestamp
        time_t bestTime = 0;
        bool found = false;
        for (const auto& kv : progress_map_) {
            if (kv.second.last_read > bestTime) {
                bestTime = kv.second.last_read;
                out = kv.second;
                found = true;
            }
        }
        return found;
    }
};

} // namespace kindle
