#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(x, u) ::operator new(x)
#define STBTT_free(x, u) ::operator delete(x)
#include "stb_truetype.h"

#include "Font.hpp"

#include <cstdio>

namespace pforge {

Font::Font() : info_(new stbtt_fontinfo()) {}

Font::~Font() { delete info_; }

bool Font::load(const std::string& ttfPath, float pixelSize) {
    std::FILE* f = std::fopen(ttfPath.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    blob_.resize(size_t(sz));
    const size_t got = std::fread(blob_.data(), 1, size_t(sz), f);
    std::fclose(f);
    if (got != size_t(sz)) return false;

    const int offset = stbtt_GetFontOffsetForIndex(blob_.data(), 0);
    if (offset < 0 || !stbtt_InitFont(info_, blob_.data(), offset)) return false;

    scale_ = stbtt_ScaleForMappingEmToPixels(info_, pixelSize);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(info_, &asc, &desc, &gap);
    ascent_ = int(asc * scale_ + 0.5f);
    descent_ = int(-desc * scale_ + 0.5f);
    height_ = ascent_ + descent_ + int(gap * scale_ + 0.5f);
    cache_.clear();
    ok_ = true;
    return true;
}

bool Font::share(const Font& src, float pixelSize) {
    if (!src.ok_ || src.blob_.empty()) return false;
    blob_ = src.blob_;
    const int offset = stbtt_GetFontOffsetForIndex(blob_.data(), 0);
    if (offset < 0 || !stbtt_InitFont(info_, blob_.data(), offset)) return false;
    scale_ = stbtt_ScaleForMappingEmToPixels(info_, pixelSize);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(info_, &asc, &desc, &gap);
    ascent_ = int(asc * scale_ + 0.5f);
    descent_ = int(-desc * scale_ + 0.5f);
    height_ = ascent_ + descent_ + int(gap * scale_ + 0.5f);
    cache_.clear();
    ok_ = true;
    return true;
}

std::vector<uint32_t> Font::decodeUtf8(const std::string& text) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) { cp = c; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; extra = 1; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; extra = 2; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
        else { cp = '?'; }
        if (extra) {
            if (i + size_t(extra) >= text.size()) { out.push_back('?'); break; }
            for (int k = 1; k <= extra; ++k) {
                const unsigned char cc = static_cast<unsigned char>(text[i + k]);
                cp = (cp << 6) | (cc & 0x3F);
            }
        }
        i += size_t(extra) + 1;
        out.push_back(cp);
    }
    return out;
}

const Font::Glyph* Font::glyph(uint32_t cp) const {
    auto it = cache_.find(cp);
    if (it != cache_.end()) return &it->second;

    Glyph g;
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(info_, int(cp), &adv, &lsb);
    g.advance = int(adv * scale_ + 0.5f);

    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char* bmp = stbtt_GetCodepointBitmap(info_, scale_, scale_, int(cp),
                                                  &w, &h, &xoff, &yoff);
    if (bmp) {
        g.w = w; g.h = h; g.xoff = xoff; g.yoff = yoff;
        g.mask.assign(bmp, bmp + size_t(w) * h);
        stbtt_FreeBitmap(bmp, nullptr);
    }
    return &cache_.emplace(cp, std::move(g)).first->second;
}

int Font::width(const std::string& text) const {
    return widthSpaced(text, 0.0f);
}

int Font::widthSpaced(const std::string& text, float spacing) const {
    if (!ok_) return 0;
    const std::vector<uint32_t> cps = decodeUtf8(text);
    // Mesma acumulacao de drawSpaced(), senao o centro calculado aqui nao e o
    // centro em que o texto sai desenhado.
    float w = 0.0f;
    for (size_t i = 0; i < cps.size(); ++i) {
        w += float(glyph(cps[i])->advance) + spacing;
        if (i + 1 < cps.size()) w += float(stbtt_GetCodepointKernAdvance(
            info_, int(cps[i]), int(cps[i + 1]))) * scale_;
    }
    return int(w + 0.5f);
}

void Font::drawSpaced(Surface& s, int x, int baselineY, const std::string& text,
                      float spacing, uint32_t color) const {
    if (!ok_) return;
    const std::vector<uint32_t> cps = decodeUtf8(text);
    float pen = float(x);
    for (size_t i = 0; i < cps.size(); ++i) {
        const Glyph& g = *glyph(cps[i]);
        const int gx = int(pen + 0.5f) + g.xoff;
        const int gy = baselineY + g.yoff;
        for (int j = 0; j < g.h; ++j) {
            for (int k = 0; k < g.w; ++k) {
                const uint8_t v = g.mask[size_t(j) * g.w + k];
                if (v) s.blendPix(gx + k, gy + j, color, v / 255.0f);
            }
        }
        // O avanco do glifo e inteiro (texto nitido), mas o letter-spacing pedido
        // e o kerning da fonte nao: truncar 1.2 px por letra a 1 px somava 2 px de
        // erro em "RESONANCIA" e o espacamento ficava irregular entre pares.
        pen += float(g.advance) + spacing;
        if (i + 1 < cps.size()) {
            pen += float(stbtt_GetCodepointKernAdvance(info_, int(cps[i]),
                                                       int(cps[i + 1]))) * scale_;
        }
    }
}

void Font::draw(Surface& s, int x, int baselineY, const std::string& text, uint32_t color) const {
    drawSpaced(s, x, baselineY, text, 0.0f, color);
}

void Font::drawCentered(Surface& s, int cx, int baselineY, const std::string& text,
                        uint32_t color) const {
    draw(s, cx - width(text) / 2, baselineY, text, color);
}

void Font::drawRight(Surface& s, int right, int baselineY, const std::string& text,
                     uint32_t color) const {
    draw(s, right - width(text), baselineY, text, color);
}

} // namespace pforge
