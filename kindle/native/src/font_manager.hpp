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

    bool init(const std::string& custom_reg = "", const std::string& custom_bold = "") {
        const std::vector<std::string> reg_candidates = {
            custom_reg,
            "/usr/java/lib/fonts/Amazon-Ember-Regular.ttf",
            "/usr/java/lib/fonts/AmazonEmber-Regular.ttf",
            "/usr/java/lib/fonts/Amazon-Ember_Rg.ttf",
            "/usr/java/lib/fonts/AmazonEmber_Rg.ttf",
            "/usr/share/fonts/AmazonEmber-Regular.ttf",
            "/usr/share/fonts/Amazon-Ember-Regular.ttf",
            "/usr/share/fonts/truetype/AmazonEmber-Regular.ttf",
            "/usr/share/fonts/opentype/AmazonEmber-Regular.ttf",
            "/usr/share/fonts/default/AmazonEmber-Regular.ttf",
            "/usr/share/fonts/default/truetype/AmazonEmber-Regular.ttf",
            "/opt/amazon/ebook/fonts/AmazonEmber-Regular.ttf",
            "/opt/amazon/ebook/fonts/Amazon-Ember-Regular.ttf",
            "/var/local/java/lib/fonts/AmazonEmber-Regular.ttf",
            "/mnt/us/fonts/AmazonEmber-Regular.ttf",
            "/mnt/us/system/fonts/AmazonEmber-Regular.ttf",
            "/mnt/us/extensions/rm2manga/fonts/regular.ttf",
            "/usr/java/lib/fonts/Bookerly-Regular.ttf",
            "/usr/share/fonts/truetype/Bookerly-Regular.ttf",
            "/usr/java/lib/fonts/Caecilia_LT_65_Medium.ttf",
            "/usr/java/lib/fonts/Caecilia_LT_55_Roman.ttf",
            "/usr/java/lib/fonts/Helvetica-Roman.ttf",
            "/mnt/us/koreader/fonts/freefont/FreeSans.ttf",
            "/opt/koreader/fonts/freefont/FreeSans.ttf",
            "/mnt/us/koreader/fonts/noto/NotoSans-Regular.ttf"
        };

        for (const auto& p : reg_candidates) {
            if (p.empty()) continue;
            if (loadFile(p, regular_data_)) {
                if (stbtt_InitFont(&regular_font_, regular_data_.data(), 0)) {
                    regular_loaded_ = true;
                    break;
                }
            }
        }

        const std::vector<std::string> bold_candidates = {
            custom_bold,
            "/usr/java/lib/fonts/Amazon-Ember-Bold.ttf",
            "/usr/java/lib/fonts/AmazonEmber-Bold.ttf",
            "/usr/java/lib/fonts/Amazon-Ember_Bd.ttf",
            "/usr/java/lib/fonts/AmazonEmber_Bd.ttf",
            "/usr/share/fonts/AmazonEmber-Bold.ttf",
            "/usr/share/fonts/Amazon-Ember-Bold.ttf",
            "/usr/share/fonts/truetype/AmazonEmber-Bold.ttf",
            "/usr/share/fonts/opentype/AmazonEmber-Bold.ttf",
            "/usr/share/fonts/default/AmazonEmber-Bold.ttf",
            "/usr/share/fonts/default/truetype/AmazonEmber-Bold.ttf",
            "/opt/amazon/ebook/fonts/AmazonEmber-Bold.ttf",
            "/opt/amazon/ebook/fonts/Amazon-Ember-Bold.ttf",
            "/var/local/java/lib/fonts/AmazonEmber-Bold.ttf",
            "/mnt/us/fonts/AmazonEmber-Bold.ttf",
            "/mnt/us/system/fonts/AmazonEmber-Bold.ttf",
            "/mnt/us/extensions/rm2manga/fonts/bold.ttf",
            "/usr/java/lib/fonts/Bookerly-Bold.ttf",
            "/usr/share/fonts/truetype/Bookerly-Bold.ttf",
            "/usr/java/lib/fonts/Caecilia_LT_75_Bold.ttf",
            "/usr/java/lib/fonts/Helvetica-Bold.ttf",
            "/mnt/us/koreader/fonts/freefont/FreeSansBold.ttf",
            "/opt/koreader/fonts/freefont/FreeSansBold.ttf"
        };

        for (const auto& p : bold_candidates) {
            if (p.empty()) continue;
            if (loadFile(p, bold_data_)) {
                if (stbtt_InitFont(&bold_font_, bold_data_.data(), 0)) {
                    bold_loaded_ = true;
                    break;
                }
            }
        }

        return regular_loaded_;
    }

    bool isLoaded() const { return regular_loaded_; }

    static inline uint32_t nextCodepoint(const std::string& str, size_t& i) {
        uint8_t c = static_cast<uint8_t>(str[i]);
        if (c < 0x80) {
            i += 1;
            return c;
        }
        if ((c & 0xE0) == 0xC0 && i + 1 < str.size()) {
            uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(str[i + 1]) & 0x3F);
            i += 2;
            return cp;
        }
        if ((c & 0xF0) == 0xE0 && i + 2 < str.size()) {
            uint32_t cp = ((c & 0x0F) << 12) | ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 6) | (static_cast<uint8_t>(str[i + 2]) & 0x3F);
            i += 3;
            return cp;
        }
        if ((c & 0xF8) == 0xF0 && i + 3 < str.size()) {
            uint32_t cp = ((c & 0x07) << 18) | ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 12) | ((static_cast<uint8_t>(str[i + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(str[i + 3]) & 0x3F);
            i += 4;
            return cp;
        }
        i += 1;
        return c;
    }

    int measureText(const std::string& text, int pixelHeight, bool bold = false) const {
        if (!regular_loaded_) {
            int scale = std::max(1, pixelHeight / 16);
            return static_cast<int>(text.length()) * 8 * scale;
        }

        const stbtt_fontinfo* font = (bold && bold_loaded_) ? &bold_font_ : &regular_font_;
        float scale = stbtt_ScaleForPixelHeight(font, static_cast<float>(pixelHeight));
        int totalWidth = 0;

        size_t i = 0;
        while (i < text.length()) {
            uint32_t cp = nextCodepoint(text, i);
            int advance = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(font, static_cast<int>(cp), &advance, &lsb);
            totalWidth += static_cast<int>(std::round(advance * scale));
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
        size_t i = 0;
        while (i < text.length()) {
            if (text[i] == '\n') {
                curX = x;
                y += pixelHeight + 6;
                i++;
                continue;
            }

            uint32_t cp = nextCodepoint(text, i);
            int advance = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(font, static_cast<int>(cp), &advance, &lsb);

            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetCodepointBitmapBox(font, static_cast<int>(cp), scale, scale, &x0, &y0, &x1, &y1);

            int glyphW = x1 - x0;
            int glyphH = y1 - y0;

            if (glyphW > 0 && glyphH > 0) {
                std::vector<uint8_t> bmp(static_cast<size_t>(glyphW * glyphH), 0);
                stbtt_MakeCodepointBitmap(font, bmp.data(), glyphW, glyphH, glyphW, scale, scale, static_cast<int>(cp));

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
                        uint8_t blended = static_cast<uint8_t>((bg * (255 - alpha) + color * alpha) / 255);
                        canvas.setPixel(targetX, targetY, blended);
                    }
                }
            }

            curX += static_cast<int>(std::round(advance * scale));
        }
        return curX - x;
    }

    void drawButton(Canvas& canvas, int x, int y, int w, int h, const std::string& label, bool selected = false, int fontSize = 24) {
        uint8_t bg = selected ? 0x00 : 0xFF;
        uint8_t fg = selected ? 0xFF : 0x00;
        canvas.fillRect(x, y, w, h, bg);
        canvas.drawRect(x, y, w, h, 0x00, 2);

        int text_w = measureText(label, fontSize, true);
        int text_x = x + (w - text_w) / 2;
        int text_y = y + (h - fontSize) / 2;
        drawText(canvas, text_x, text_y, label, fontSize, fg, true);
    }
};

} // namespace kindle
