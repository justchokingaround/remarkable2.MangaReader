#pragma once

#include "util.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace extractor {

namespace fs = std::filesystem;

class WebpConverter {
    std::mutex mu_;
    bool initialized_ = false;
    bool qtHasWebp_ = false;
    void* libHandle_ = nullptr;

    using DecodeYuvFn = uint8_t* (*)(const uint8_t* data, size_t data_size,
                                     int* width, int* height,
                                     uint8_t** u, uint8_t** v,
                                     int* stride, int* uv_stride);
    using FreeFn = void (*)(void* ptr);

    DecodeYuvFn decodeYuv_ = nullptr;
    FreeFn webpFree_ = nullptr;

    void initLocked() {
        if (initialized_) return;
        initialized_ = true;

        std::error_code ec;
        if (fs::exists("/usr/lib/plugins/imageformats/libqwebp.so", ec) ||
            fs::exists("/usr/lib/qt6/plugins/imageformats/libqwebp.so", ec) ||
            fs::exists("/usr/lib/qml/imageformats/libqwebp.so", ec)) {
            qtHasWebp_ = true;
            return;
        }

        const char* candidates[] = {
            "/home/root/xovi/exthome/appload/koreader/libs/libwebp.so.7",
            "/home/root/xovi/exthome/appload/koreader/libs/libwebp.so",
            "libwebp.so.7",
            "libwebp.so"
        };
        for (const char* path : candidates) {
            libHandle_ = ::dlopen(path, RTLD_LAZY);
            if (libHandle_) {
                decodeYuv_ = reinterpret_cast<DecodeYuvFn>(::dlsym(libHandle_, "WebPDecodeYUV"));
                webpFree_ = reinterpret_cast<FreeFn>(::dlsym(libHandle_, "WebPFree"));
                if (decodeYuv_) break;
                ::dlclose(libHandle_);
                libHandle_ = nullptr;
            }
        }
    }

public:
    static bool isWebpMagic(const fs::path& file) {
        std::ifstream ifs(file, std::ios::binary);
        if (!ifs) return false;
        char hdr[12] = {};
        ifs.read(hdr, 12);
        if (ifs.gcount() < 12) return false;
        return std::memcmp(hdr, "RIFF", 4) == 0 && std::memcmp(hdr + 8, "WEBP", 4) == 0;
    }

    // If file is WebP and Qt lacks libqwebp, convert luma channel directly to 8-bit PGM (P5).
    fs::path ensureQtReadable(const fs::path& imgPath) {
        if (!isWebpMagic(imgPath)) return imgPath;

        std::lock_guard<std::mutex> lock(mu_);
        initLocked();
        if (qtHasWebp_ || !decodeYuv_) {
            return imgPath;
        }

        fs::path pgmPath = imgPath;
        pgmPath.replace_extension(".pgm");
        std::error_code ec;
        if (fs::exists(pgmPath, ec) && fs::file_size(pgmPath, ec) > 32) {
            return pgmPath;
        }

        std::string raw;
        if (!util::readFileToString(imgPath, raw) || raw.size() < 16) {
            return imgPath;
        }

        int width = 0, height = 0, stride = 0, uvStride = 0;
        uint8_t* u = nullptr;
        uint8_t* v = nullptr;
        uint8_t* y = decodeYuv_(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(),
                                &width, &height, &u, &v, &stride, &uvStride);
        if (!y || width <= 0 || height <= 0) {
            return imgPath;
        }

        std::string pgmHeader = "P5\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
        std::string out;
        out.reserve(pgmHeader.size() + static_cast<size_t>(width) * static_cast<size_t>(height));
        out += pgmHeader;
        for (int row = 0; row < height; ++row) {
            out.append(reinterpret_cast<const char*>(y + row * stride), static_cast<size_t>(width));
        }

        if (webpFree_) {
            webpFree_(y);
        } else {
            std::free(y);
        }

        if (util::atomicWriteFile(pgmPath, out)) {
            fs::remove(imgPath, ec);
            return pgmPath;
        }
        return imgPath;
    }
};

class PageCache {
    fs::path cacheRoot_;
    std::mutex mu_;
    std::vector<std::string> lruKeys_;
    WebpConverter webp_;

    static bool isImageExt(const std::string& extLower) {
        return extLower == ".jpg" || extLower == ".jpeg" || extLower == ".png" ||
               extLower == ".webp" || extLower == ".gif" || extLower == ".bmp" ||
               extLower == ".pgm";
    }

    void touchKeyLocked(const std::string& key, const std::set<std::string>& pinnedKeys) {
        lruKeys_.erase(std::remove(lruKeys_.begin(), lruKeys_.end(), key), lruKeys_.end());
        lruKeys_.push_back(key);

        // Keep at most 6 extracted chapters in /tmp RAM disk
        const size_t maxCachedChapters = 6;
        size_t idx = 0;
        while (lruKeys_.size() > maxCachedChapters && idx < lruKeys_.size()) {
            const std::string& candidate = lruKeys_[idx];
            if (pinnedKeys.count(candidate) == 0 && candidate != key) {
                std::error_code ec;
                fs::remove_all(cacheRoot_ / candidate, ec);
                lruKeys_.erase(lruKeys_.begin() + static_cast<ptrdiff_t>(idx));
            } else {
                ++idx;
            }
        }
    }

public:
    explicit PageCache(fs::path root = "/tmp/rm2manga_pages") : cacheRoot_(std::move(root)) {
        std::error_code ec;
        fs::create_directories(cacheRoot_, ec);
    }

    fs::path root() const { return cacheRoot_; }

    void clearAll() {
        std::lock_guard<std::mutex> lock(mu_);
        std::error_code ec;
        fs::remove_all(cacheRoot_, ec);
        fs::create_directories(cacheRoot_, ec);
        lruKeys_.clear();
    }

    std::vector<std::string> collectImagesFromDir(const fs::path& dir) {
        std::error_code ec;
        if (!fs::exists(dir, ec)) return {};

        std::vector<fs::path> found;
        for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::string fname = entry.path().filename().string();
            if (fname.empty() || fname[0] == '.') continue;
            if (entry.path().string().find("__MACOSX") != std::string::npos) continue;

            std::string ext = util::toLower(entry.path().extension().string());
            if (isImageExt(ext)) {
                found.push_back(entry.path());
            }
        }

        // Convert any WebP files if Qt lacks WebP plugin
        for (auto& p : found) {
            p = webp_.ensureQtReadable(p);
        }

        // Deduplicate in case both .webp and .pgm existed
        std::sort(found.begin(), found.end(), [](const fs::path& a, const fs::path& b) {
            return util::naturalLess(a.generic_string(), b.generic_string());
        });
        found.erase(std::unique(found.begin(), found.end()), found.end());

        std::vector<std::string> urls;
        urls.reserve(found.size());
        for (const auto& p : found) {
            urls.push_back("file://" + fs::absolute(p, ec).string());
        }
        return urls;
    }

    std::vector<std::string> extractCbz(const std::string& chapterKey,
                                        const fs::path& cbzPath,
                                        const std::set<std::string>& pinnedKeys,
                                        std::string* errOut = nullptr) {
        std::error_code ec;
        if (!fs::exists(cbzPath, ec)) {
            if (errOut) *errOut = "Archive file missing on disk: " + cbzPath.string();
            return {};
        }

        std::string slug = util::hashSlug(chapterKey);
        fs::path outDir = cacheRoot_ / slug;

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (fs::exists(outDir / ".ready", ec)) {
                auto cached = collectImagesFromDir(outDir);
                if (!cached.empty()) {
                    touchKeyLocked(slug, pinnedKeys);
                    return cached;
                }
            }
            fs::remove_all(outDir, ec);
            fs::create_directories(outDir, ec);
        }

        const char* unzipBin = util::hasBinary("/usr/bin/unzip") ? "/usr/bin/unzip" : "unzip";
        auto res = util::runProcess({unzipBin, "-oq", cbzPath.string(), "-d", outDir.string()}, 60);
        if (res.exitCode != 0) {
            // BusyBox unzip sometimes returns 1 on minor warnings; check if images extracted anyway
            auto fallbackCheck = collectImagesFromDir(outDir);
            if (fallbackCheck.empty()) {
                if (errOut) *errOut = "Failed to extract CBZ archive (exit " + std::to_string(res.exitCode) + ")";
                return {};
            }
        }

        auto pages = collectImagesFromDir(outDir);
        if (pages.empty()) {
            if (errOut) *errOut = "CBZ archive contained no readable image pages";
            return {};
        }

        util::atomicWriteFile(outDir / ".ready", "1");
        {
            std::lock_guard<std::mutex> lock(mu_);
            touchKeyLocked(slug, pinnedKeys);
        }
        return pages;
    }

    // Prepares a directory of loose image pages
    std::vector<std::string> prepareFolder(const fs::path& folderPath, std::string* errOut = nullptr) {
        auto pages = collectImagesFromDir(folderPath);
        if (pages.empty() && errOut) {
            *errOut = "Chapter folder has no readable pages";
        }
        return pages;
    }
};

}  // namespace extractor
