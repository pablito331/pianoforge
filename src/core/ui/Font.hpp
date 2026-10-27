#pragma once
// Fonte TTF rasterizada pela Surface software. FreeType/Cairo nao existem com
// header nesta maquina, e o visual precisa ser o mesmo no X11UI e no preview
// offscreen -- entao stb_truetype + cache de glifo resolve as duas coisas.

#include "Surface.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct stbtt_fontinfo;

namespace pforge {

class Font {
public:
    Font();
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    bool load(const std::string& ttfPath, float pixelSize);

    // Mesmo blob, outro tamanho. O painel do conceito v2 tem dez corpo de letra
    // diferentes (7, 8, 9, 10, 11, 14, 16, 17 px) e dez Font::load() releria o
    // mesmo TTF dez vezes; share() copia o blob e so refaz a escala e as metricas.
    bool share(const Font& src, float pixelSize);

    int ascent() const { return ascent_; }
    int descent() const { return descent_; }
    int height() const { return height_; }

    int width(const std::string& text) const;
    int widthSpaced(const std::string& text, float spacing) const;

    // baselineY e a linha base do texto (nao o topo).
    void draw(Surface& s, int x, int baselineY, const std::string& text, uint32_t color) const;
    void drawSpaced(Surface& s, int x, int baselineY, const std::string& text,
                    float spacing, uint32_t color) const;
    void drawCentered(Surface& s, int cx, int baselineY, const std::string& text,
                      uint32_t color) const;
    void drawRight(Surface& s, int right, int baselineY, const std::string& text,
                   uint32_t color) const;

    bool ok() const { return ok_; }

private:
    struct Glyph {
        std::vector<uint8_t> mask;
        int w = 0, h = 0, xoff = 0, yoff = 0, advance = 0;
    };

    const Glyph* glyph(uint32_t codepoint) const;
    static std::vector<uint32_t> decodeUtf8(const std::string& text);

    std::vector<unsigned char> blob_;
    stbtt_fontinfo* info_ = nullptr;
    mutable std::unordered_map<uint32_t, Glyph> cache_;
    float scale_ = 0.0f;
    int ascent_ = 0, descent_ = 0, height_ = 0;
    bool ok_ = false;
};

} // namespace pforge
