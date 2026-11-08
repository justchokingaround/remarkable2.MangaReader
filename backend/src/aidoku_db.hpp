#pragma once

// Read-only access to the Aidoku runtime's SQLite database (<home>/database.db).
// The runtime stores each installed source's own cover URL in `manga_informations.cover_url`
// but does not expose it over its HTTP API, so we read it directly.
// libsqlite3 is loaded at runtime via dlopen so no build-time dependency is needed.

#include <dlfcn.h>
#include <filesystem>
#include <mutex>
#include <string>

namespace aidoku {

class CoverDb {
    using sqlite3 = void;
    using sqlite3_stmt = void;
    using OpenV2Fn = int (*)(const char*, sqlite3**, int, const char*);
    using CloseFn = int (*)(sqlite3*);
    using PrepareFn = int (*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
    using BindTextFn = int (*)(sqlite3_stmt*, int, const char*, int, void (*)(void*));
    using StepFn = int (*)(sqlite3_stmt*);
    using ColumnTextFn = const unsigned char* (*)(sqlite3_stmt*, int);
    using FinalizeFn = int (*)(sqlite3_stmt*);
    using BusyTimeoutFn = int (*)(sqlite3*, int);

    static constexpr int SQLITE_OK = 0;
    static constexpr int SQLITE_ROW = 100;
    static constexpr int SQLITE_OPEN_READONLY = 0x00000001;

    std::mutex mu_;
    std::filesystem::path dbPath_;
    bool initTried_ = false;
    void* lib_ = nullptr;
    OpenV2Fn open_ = nullptr;
    CloseFn close_ = nullptr;
    PrepareFn prepare_ = nullptr;
    BindTextFn bindText_ = nullptr;
    StepFn step_ = nullptr;
    ColumnTextFn columnText_ = nullptr;
    FinalizeFn finalize_ = nullptr;
    BusyTimeoutFn busyTimeout_ = nullptr;

    bool initLocked() {
        if (initTried_) return lib_ != nullptr;
        initTried_ = true;
        const char* candidates[] = {"libsqlite3.so.0", "/usr/lib/libsqlite3.so.0", "/lib/libsqlite3.so.0",
                                    "libsqlite3.so", "libsqlite3.dylib", "/usr/lib/libsqlite3.dylib"};
        for (const char* c : candidates) {
            lib_ = ::dlopen(c, RTLD_LAZY);
            if (lib_) break;
        }
        if (!lib_) return false;
        open_ = reinterpret_cast<OpenV2Fn>(::dlsym(lib_, "sqlite3_open_v2"));
        close_ = reinterpret_cast<CloseFn>(::dlsym(lib_, "sqlite3_close"));
        prepare_ = reinterpret_cast<PrepareFn>(::dlsym(lib_, "sqlite3_prepare_v2"));
        bindText_ = reinterpret_cast<BindTextFn>(::dlsym(lib_, "sqlite3_bind_text"));
        step_ = reinterpret_cast<StepFn>(::dlsym(lib_, "sqlite3_step"));
        columnText_ = reinterpret_cast<ColumnTextFn>(::dlsym(lib_, "sqlite3_column_text"));
        finalize_ = reinterpret_cast<FinalizeFn>(::dlsym(lib_, "sqlite3_finalize"));
        busyTimeout_ = reinterpret_cast<BusyTimeoutFn>(::dlsym(lib_, "sqlite3_busy_timeout"));
        if (!open_ || !close_ || !prepare_ || !bindText_ || !step_ || !columnText_ || !finalize_) {
            ::dlclose(lib_);
            lib_ = nullptr;
            return false;
        }
        return true;
    }

public:
    explicit CoverDb(std::filesystem::path dbPath) : dbPath_(std::move(dbPath)) {}

    // Returns the source-provided cover URL for a manga, or "" if unknown.
    std::string lookupCoverUrl(const std::string& sourceId, const std::string& mangaId) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!initLocked()) return "";
        std::error_code ec;
        if (!std::filesystem::exists(dbPath_, ec)) return "";

        sqlite3* db = nullptr;
        if (open_(dbPath_.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK || !db) {
            if (db) close_(db);
            return "";
        }
        if (busyTimeout_) busyTimeout_(db, 1500);

        std::string result;
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT cover_url FROM manga_informations WHERE source_id = ?1 AND manga_id = ?2";
        if (prepare_(db, sql, -1, &stmt, nullptr) == SQLITE_OK && stmt) {
            bindText_(stmt, 1, sourceId.c_str(), -1, reinterpret_cast<void (*)(void*)>(-1));  // SQLITE_TRANSIENT
            bindText_(stmt, 2, mangaId.c_str(), -1, reinterpret_cast<void (*)(void*)>(-1));
            if (step_(stmt) == SQLITE_ROW) {
                const unsigned char* txt = columnText_(stmt, 0);
                if (txt) result = reinterpret_cast<const char*>(txt);
            }
            finalize_(stmt);
        }
        close_(db);
        return result;
    }
};

}  // namespace aidoku
