#pragma once

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace kindle {

// NXP i.MX7Dual EPDC Structures for Kindle Oasis 2/3 (Zelda Platform)
struct mxcfb_rect {
    uint32_t top;
    uint32_t left;
    uint32_t width;
    uint32_t height;
};

struct mxcfb_alt_buffer_data {
    uint32_t phys_addr;
    uint32_t width;
    uint32_t height;
    struct mxcfb_rect alt_update_region;
};

struct mxcfb_update_data_zelda {
    struct mxcfb_rect update_region;
    uint32_t waveform_mode;
    uint32_t update_mode;
    uint32_t update_marker;
    int temp;
    unsigned flags;
    int dither_mode;
    int quant_bit;
    struct mxcfb_alt_buffer_data alt_buffer_data;
    uint32_t hist_bw_waveform_mode;
    uint32_t hist_gray_waveform_mode;
    uint32_t ts_pxp;
    uint32_t ts_epdc;
};

// IOCTL and Waveform Constants
static constexpr unsigned IOCTL_MXCFB_SEND_UPDATE_ZELDA = 1079526958;

enum WaveformMode : uint32_t {
    WAVEFORM_DU = 1,          // 1-bit direct update (fastest, high contrast)
    WAVEFORM_GC16 = 2,        // 16-level grayscale (photos, covers)
    WAVEFORM_GL16 = 3,        // Grayscale clear
    WAVEFORM_GLR16 = 4,       // Regal anti-ghosting waveform (manga page turns)
    WAVEFORM_REAGLD = 5,      // Regal with direct black update
    WAVEFORM_A2 = 6,          // 2-bit fast typing / touch feedback
    WAVEFORM_AUTO = 257       // Automatic selection
};

enum UpdateMode : uint32_t {
    UPDATE_PARTIAL = 0,
    UPDATE_FULL = 1
};

class DisplayZelda {
public:
    static constexpr int WIDTH = 1264;
    static constexpr int HEIGHT = 1680;
    static constexpr int STRIDE = 1280; // Hardware line pitch on Kindle Oasis 2/3
    static constexpr size_t FB_SIZE = STRIDE * HEIGHT; // 1280 * 1680 = 2,150,400 bytes

private:
    int fb_fd_ = -1;
    uint8_t* fb_mem_ = nullptr;
    uint32_t update_marker_ = 0;

public:
    DisplayZelda() = default;
    ~DisplayZelda() { close(); }

    bool init(const char* fb_device = "/dev/fb0") {
        fb_fd_ = open(fb_device, O_RDWR);
        if (fb_fd_ < 0) {
            perror("DisplayZelda: Failed to open framebuffer");
            return false;
        }

        fb_mem_ = (uint8_t*)mmap(nullptr, FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd_, 0);
        if (fb_mem_ == MAP_FAILED) {
            perror("DisplayZelda: Failed to mmap framebuffer");
            ::close(fb_fd_);
            fb_fd_ = -1;
            fb_mem_ = nullptr;
            return false;
        }

        printf("DisplayZelda: Initialized Kindle Oasis display (%dx%d Y8)\n", WIDTH, HEIGHT);
        return true;
    }

    void close() {
        if (fb_mem_ && fb_mem_ != MAP_FAILED) {
            munmap(fb_mem_, FB_SIZE);
            fb_mem_ = nullptr;
        }
        if (fb_fd_ >= 0) {
            ::close(fb_fd_);
            fb_fd_ = -1;
        }
    }

    uint8_t* getBuffer() { return fb_mem_; }
    int width() const { return WIDTH; }
    int height() const { return HEIGHT; }

    // Clear buffer to white (0xFF) or black (0x00)
    void clear(uint8_t color = 0xFF) {
        if (fb_mem_) {
            memset(fb_mem_, color, FB_SIZE);
        }
    }

    // Send hardware update to E-Ink EPDC controller
    bool update(int x, int y, int w, int h, WaveformMode mode = WAVEFORM_GLR16, UpdateMode upMode = UPDATE_PARTIAL) {
        if (fb_fd_ < 0) return false;

        // Clip to screen boundaries
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > WIDTH) w = WIDTH - x;
        if (y + h > HEIGHT) h = HEIGHT - y;
        if (w <= 0 || h <= 0) return true;

        struct mxcfb_update_data_zelda upd;
        memset(&upd, 0, sizeof(upd));
        upd.update_region.left = x;
        upd.update_region.top = y;
        upd.update_region.width = w;
        upd.update_region.height = h;
        upd.waveform_mode = static_cast<uint32_t>(mode);
        upd.update_mode = static_cast<uint32_t>(upMode);
        upd.update_marker = ++update_marker_;
        upd.temp = 4096; // TEMP_USE_ZELDA_AUTO

        // Configure Regal / Hist settings
        if (mode == WAVEFORM_GLR16) {
            upd.hist_bw_waveform_mode = WAVEFORM_GLR16;
            upd.hist_gray_waveform_mode = WAVEFORM_GLR16;
        } else {
            upd.hist_bw_waveform_mode = WAVEFORM_DU;
            upd.hist_gray_waveform_mode = WAVEFORM_GC16;
        }

        int ret = ioctl(fb_fd_, IOCTL_MXCFB_SEND_UPDATE_ZELDA, &upd);
        return ret == 0;
    }

    // Full screen refresh (e.g. when clearing ghosting)
    bool refreshFull(WaveformMode mode = WAVEFORM_GC16) {
        return update(0, 0, WIDTH, HEIGHT, mode, UPDATE_FULL);
    }

    // Fast, non-flashing full view refresh (GLR16 regal partial update, ~120ms, zero black flash)
    bool refreshView(WaveformMode mode = WAVEFORM_GLR16) {
        return update(0, 0, WIDTH, HEIGHT, mode, UPDATE_PARTIAL);
    }

    // Direct fast update (DU 1-bit, ~30-40ms, zero flash)
    bool refreshDirect(int x = 0, int y = 0, int w = WIDTH, int h = HEIGHT) {
        return update(x, y, w, h, WAVEFORM_DU, UPDATE_PARTIAL);
    }

    // Fast UI touch refresh (A2 mode)
    bool refreshUI(int x, int y, int w, int h) {
        return update(x, y, w, h, WAVEFORM_A2, UPDATE_PARTIAL);
    }

    // Reading page turn refresh (Regal GLR16 anti-ghosting)
    bool refreshPage() {
        return update(0, 0, WIDTH, HEIGHT, WAVEFORM_GLR16, UPDATE_PARTIAL);
    }
};

} // namespace kindle
