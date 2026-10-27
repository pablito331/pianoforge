// A unica instancia do decodificador de imagem do painel. Nada aqui roda na
// thread de audio: a arte e lida na thread da UI quando o banco muda.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO          // o blob vem de dentro do bundle, nunca de caminho
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#include "Image.hpp"

namespace pforge {

bool decodeImage(const uint8_t* data, size_t n, Image& out) {
    out.clear();
    if (!data || n == 0) return false;
    int w = 0, h = 0, ch = 0;
    stbi_uc* rgba = stbi_load_from_memory(data, int(n), &w, &h, &ch, 4);
    if (!rgba) return false;
    if (w <= 0 || h <= 0 || size_t(w) * size_t(h) > 8u * 1024u * 1024u) {
        stbi_image_free(rgba);
        return false;               // arte de painel nao e um poster de 100 MP
    }
    out.w = w;
    out.h = h;
    out.px.resize(size_t(w) * size_t(h));
    for (size_t i = 0; i < out.px.size(); ++i) {
        out.px[i] = (uint32_t(rgba[i * 4 + 3]) << 24) | (uint32_t(rgba[i * 4]) << 16) |
                    (uint32_t(rgba[i * 4 + 1]) << 8) | uint32_t(rgba[i * 4 + 2]);
    }
    stbi_image_free(rgba);
    return true;
}

} // namespace pforge
