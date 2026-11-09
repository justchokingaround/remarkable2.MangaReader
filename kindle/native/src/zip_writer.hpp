#pragma once
// Minimal store-only (no compression) ZIP writer for producing .cbz archives.
// Manga pages are already-compressed JPEG/PNG, so STORE costs nothing in size.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace kindle {

class ZipStoreWriter {
    struct Entry {
        std::string name;
        uint32_t crc;
        uint32_t size;
        uint32_t offset;
    };
    FILE* f_ = nullptr;
    uint32_t pos_ = 0;
    std::vector<Entry> entries_;

    static uint32_t crc32(const uint8_t* data, size_t len) {
        static uint32_t table[256];
        static bool init = false;
        if (!init) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            init = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
    void w16(uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; fwrite(b, 1, 2, f_); pos_ += 2; }
    void w32(uint32_t v) {
        uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        fwrite(b, 1, 4, f_); pos_ += 4;
    }
    void wbytes(const void* d, size_t n) { fwrite(d, 1, n, f_); pos_ += static_cast<uint32_t>(n); }

public:
    ~ZipStoreWriter() { if (f_) fclose(f_); }

    bool open(const std::string& path) {
        f_ = fopen(path.c_str(), "wb");
        pos_ = 0;
        entries_.clear();
        return f_ != nullptr;
    }

    bool add(const std::string& name, const std::vector<uint8_t>& data) {
        if (!f_) return false;
        Entry e{name, crc32(data.data(), data.size()), static_cast<uint32_t>(data.size()), pos_};
        w32(0x04034b50); w16(20); w16(0); w16(0); w16(0); w16(0x21);
        w32(e.crc); w32(e.size); w32(e.size);
        w16(static_cast<uint16_t>(name.size())); w16(0);
        wbytes(name.data(), name.size());
        wbytes(data.data(), data.size());
        entries_.push_back(e);
        return !ferror(f_);
    }

    bool close() {
        if (!f_) return false;
        uint32_t cdStart = pos_;
        for (const auto& e : entries_) {
            w32(0x02014b50); w16(20); w16(20); w16(0); w16(0); w16(0); w16(0x21);
            w32(e.crc); w32(e.size); w32(e.size);
            w16(static_cast<uint16_t>(e.name.size())); w16(0); w16(0); w16(0); w16(0); w32(0);
            w32(e.offset);
            wbytes(e.name.data(), e.name.size());
        }
        uint32_t cdSize = pos_ - cdStart;
        w32(0x06054b50); w16(0); w16(0);
        w16(static_cast<uint16_t>(entries_.size())); w16(static_cast<uint16_t>(entries_.size()));
        w32(cdSize); w32(cdStart); w16(0);
        bool ok = !ferror(f_);
        fclose(f_);
        f_ = nullptr;
        return ok;
    }
};

} // namespace kindle
