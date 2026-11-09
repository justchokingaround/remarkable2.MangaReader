#pragma once

#include "../third_party/stb_truetype.h"
#include "canvas.hpp"
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include <algorithm>

namespace kindle {

class FontManager {
private:
    std::vector<uint8_t> regular_data_;
    std::vector<uint8_t> bold_data_;
    stbtt_fontinfo regular_font_{};
    stbtt_fontinfo bold_font_{};
    bool regular_loaded_ = false;
    bool bold_loaded_ = false;

    bool loadFile(const std::string& path, std::vector<uint8_t>& out) {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs) return false;
        std::streamsize sz = ifs.tellg();
        if (sz <= 0) return false;
        ifs.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(sz));
        if (ifs.read(reinterpret_cast<char*>(out.data()), sz)) {
            return true;
        }
        return false;
    }

public:
    FontManager() = default;

    bool init(const std::string& regular_path = "/usr/java/lib/fonts/Amazon-Ember-Regular.ttf",
              const std::string& bold_path = "/usr/java/lib/fonts/Amazon-Ember-Bold.ttf") {
        if (loadFile(regular_path, regular_data_)) {
            if (stbtt_InitFont(&regular_font_, regular_data_.data(), 0)) {
                regular_loaded_ = true;
            }
        }
        if (loadFile(bold_path, bold_data_)) {
            if (stbtt_InitFont(&bold_font_, bold_data_.data(), 0)) {
                bold_loaded_ = true;
            }
        }
        return regular_loaded_;
    }

    bool isLoaded() const { return regular_loaded_; }

    int measureText(const std::string& text, int pixelHeight, bool bold = false) const {
        if (!regular_loaded_) {
            int scale = std::max(1, pixelHeight / 16);
            return static_cast<int>(text.length()) * 8 * scale;
        }

        const stbtt_fontinfo* font = (bold && bold_loaded_) ? &bold_font_ : &regular_font_;
        float scale = stbtt_ScaleForPixelHeight(font, static_cast<float>(pixelHeight));
        int totalWidth = 0;

        for (size_t i = 0; i < text.length(); ++i) {
            char c = text[i];
            int advance = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(font, static_cast<int>(c), &advance, &lsb);
            totalWidth += static_cast<int>(std::round(advance * scale));

            if (i + 1 < text.length()) {
                int kern = stbtt_GetCodepointKernAdvance(font, static_cast<int>(c), static_cast<int>(text[i + 1]));
                totalWidth += static_cast<int>(std::round(kern * scale));
            }
        }
        return totalWidth;
    }

    int drawText(Canvas& canvas, int x, int y, const std::string& text, int pixelHeight, uint8_t color = 0x00, bool bold = false) {
        if (!regular_loaded_) {
            int scale = std::max(1, pixelHeight / 16);
            canvas.drawText(x, y, text, scale, color);
            return measureText(text, pixelHeight, bold);
        }

        const stbtt_fontinfo* font = (bold && bold_loaded_) ? &bold_font_ : &regular_font_;
        float scale = stbtt_ScaleForPixelHeight(font, static_cast<float>(pixelHeight));

        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(font, &ascent, &descent, &lineGap);
        int baseline = static_cast<int>(std::round(ascent * scale));

        int curX = x;
        for (size_t i = 0; i < text.length(); ++i) {
            char c = text[i];
            if (c == '\n') {
                curX = x;
                y += pixelHeight + 6;
                continue;
            }

            int advance = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(font, static_cast<int>(c), &advance, &lsb);

            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetCodepointBitmapBox(font, static_cast<int>(c), scale, scale, &x0, &y0, &x1, &y1);

            int glyphW = x1 - x0;
            int glyphH = y1 - y0;

            if (glyphW > 0 && glyphH > 0) {
                std::vector<uint8_t> bmp(static_cast<size_t>(glyphW * glyphH), 0);
                stbtt_MakeCodepointBitmap(font, bmp.data(), glyphW, glyphH, glyphW, scale, scale, static_cast<int>(c));

                int outY = y + baseline + y0;
                int outX = curX + x0;

                for (int r = 0; r < glyphH; ++r) {
                    int targetY = outY + r;
                    if (targetY < 0 || targetY >= canvas.height()) continue;

                    for (int col = 0; col < glyphW; ++col) {
                        int targetX = outX + col;
                        if (targetX < 0 || targetX >= canvas.width()) continue;

                        uint8_t alpha = bmp[static_cast<size_t>(r * glyphW + col)];
                        if (alpha == 0) continue;

                        uint8_t bg = canvas.getPixel(targetX, targetY);
                        // Alpha blending
                        uint8_t blended = static_cast<uint8_t>((bg * (255 - alpha) + color * alpha) / 255);
                        canvas.setPixel(targetX, targetY, blended);
                    }
                }
            }

            curX += static_cast<int>(std::round(advance * scale));

            if (i + 1 < text.length()) {
                int kern = stbtt_GetCodepointKernAdvance(font, static_cast<int>(c), static_cast<int>(text[i + 1]));
                curX += static_cast<int>(std::round(kern * scale));
            }
        }
        return curX - x;
    }
};

} // namespace kindle
