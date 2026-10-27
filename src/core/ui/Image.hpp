#pragma once
// Imagem decodificada no formato da Surface (0xAARRGGBB). A arte de um preset
// vem do blob gravado dentro do .pforge, nunca de um caminho solto no disco.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pforge {

struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> px;      // 0xAARRGGBB, linha a linha
    bool empty() const { return px.empty(); }
    void clear() { w = h = 0; px.clear(); }
};

// PNG/JPEG cru -> Image. Falso se o blob nao e uma imagem reconhecida: arte
// errada tem de deixar o painel igual, nao derrubar a UI.
bool decodeImage(const uint8_t* data, size_t n, Image& out);
inline bool decodeImage(const std::vector<uint8_t>& v, Image& out) {
    return decodeImage(v.data(), v.size(), out);
}

} // namespace pforge
