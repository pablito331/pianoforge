#pragma once
// Surface: framebuffer ARGB32 (0xAARRGGBB) desenhado em software, sem Cairo,
// sem FreeType, sem GL. E a unica forma de a UI do plugin ser identica dentro
// do X11UI e dentro do pforge_ui_preview (que renderiza offscreen pra eu
// conferir o visual sem host LV2 instalado).
//
// Antialiasing por distancia assinada: cada primitiva circular/arredondada sabe
// a quantos pixels do seu contorno o centro do pixel esta, e a cobertura e
// 0.5 - sd. Amostrar uma grade NxN (como este arquivo fazia) quantiza a borda em
// degraus de 1/N: num disco de 55 px de raio o degrau de 1/16 na circunferencia
// virava exatamente o serrilhado que se ve na tela. Custo menor, tambem: o
// retangulo arredondado so faz aritmetica de raio nas 4 faixas r x r dos cantos e
// preenche o resto por linha.

#include "Image.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// O MSVC so expoe M_PI no <cmath> com _USE_MATH_DEFINES definido antes de tudo, e
// definir isso em um translation unit nao alcance os outros. O arco do knob e o
// unico lugar da UI que precisa de pi, e ele mora aqui.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace pforge {

inline uint32_t rgb(uint32_t hex) { return 0xFF000000u | (hex & 0x00FFFFFFu); }

inline uint32_t withAlpha(uint32_t argb, float a) {
    const uint32_t al = static_cast<uint32_t>(std::max(0.0f, std::min(1.0f, a)) * 255.0f + 0.5f);
    return (argb & 0x00FFFFFFu) | (al << 24);
}

inline uint32_t mix(uint32_t a, uint32_t b, float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    const auto ch = [&](int shift) {
        const int va = (a >> shift) & 0xFF, vb = (b >> shift) & 0xFF;
        return static_cast<uint32_t>(va + (vb - va) * t + 0.5f) << shift;
    };
    return ch(24) | ch(16) | ch(8) | ch(0);
}

// Cobertura a partir da distancia assinada em pixels (negativo = dentro). A
// transicao ocupa exatamente 1 px, continua, e nao depende de N nem do tamanho
// da forma.
inline float aaMask(float sd) { return std::max(0.0f, std::min(1.0f, 0.5f - sd)); }

struct Surface {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> px;

    void resize(int nw, int nh) {
        if (nw == w && nh == h && static_cast<int>(px.size()) == nw * nh) return;
        w = std::max(1, nw);
        h = std::max(1, nh);
        px.assign(size_t(w) * h, 0xFF000000u);
    }

    void clear(uint32_t argb) { std::fill(px.begin(), px.end(), argb); }

    void set(int x, int y, uint32_t argb) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        px[size_t(y) * w + x] = argb;
    }

    void blendPix(int x, int y, uint32_t argb, float coverage) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        const float sa = ((argb >> 24) & 0xFF) / 255.0f * coverage;
        if (sa <= 0.0f) return;
        uint32_t& d = px[size_t(y) * w + x];
        if (sa >= 1.0f) { d = argb; return; }
        const auto ch = [&](int shift) -> uint32_t {
            const float s = float((argb >> shift) & 0xFFu);
            const float o = float((d >> shift) & 0xFFu);
            return uint32_t(s * sa + o * (1.0f - sa) + 0.5f) << shift;
        };
        const uint32_t oa = uint32_t(sa * 255.0f + float((d >> 24) & 0xFFu) * (1.0f - sa) + 0.5f);
        d = (std::min(oa, 255u) << 24) | ch(16) | ch(8) | ch(0);
    }

    // Linha horizontal de cobertura constante. Existe porque o resto do arquivo
    // precisa de area: um painel de 1000x520 sao 520 000 pixels, e cair em
    // blendPix por pixel custa o redraw inteiro em aritmetica de ponto flutuante
    // que a saida opaca nao precisa. Clip aqui, para que as primitivas arredonde-
    // das possam preencher por linha sem conferir borda.
    void fillRow(int x, int y, int n, uint32_t argb, float coverage) {
        if (y < 0 || y >= h || n <= 0 || coverage <= 0.0f) return;
        const float sa = ((argb >> 24) & 0xFF) / 255.0f * coverage;
        if (sa <= 0.0f) return;
        if (x < 0) { n += x; x = 0; }
        if (x + n > w) n = w - x;
        if (n <= 0) return;
        uint32_t* d = px.data() + size_t(y) * w + x;
        if (sa >= 1.0f) { std::fill(d, d + n, argb); return; }
        // alpha em 8.8 fixo: al + ia = 256, entao (s*al + o*ia) >> 8 nunca passa
        // de 255 e nao precisa de clamp. A versao anterior somava `s * 256` dentro
        // do campo e mascarava -- o termo da fonte e multiplo de 256, entao o
        // resultado era so o destino escurecido e a COR DA FONTE SE PERDIA: o veu
        // #0c0f13 saa (10,26,14) em vez de (16,19,24), verde em vez de ferro.
        const uint32_t al = std::min(255u, uint32_t(sa * 256.0f + 0.5f));
        const uint32_t ia = 256u - al;
        const uint32_t s16 = (argb >> 16) & 0xFF, s8 = (argb >> 8) & 0xFF, s0 = argb & 0xFF;
        const uint32_t sa16 = s16 * al, sa8 = s8 * al, sa0 = s0 * al;
        const uint32_t sal = ((argb >> 24) & 0xFF) * al;
        for (int i = 0; i < n; ++i) {
            const uint32_t o = d[i];
            const uint32_t oa = (sal + (((o >> 24) & 0xFFu) * ia)) >> 8;
            const uint32_t r = (sa16 + ((((o >> 16) & 0xFFu)) * ia)) >> 8;
            const uint32_t g = (sa8 + ((((o >> 8) & 0xFFu)) * ia)) >> 8;
            const uint32_t b = (sa0 + ((o & 0xFFu) * ia)) >> 8;
            d[i] = (std::min(oa, 255u) << 24) | (r << 16) | (g << 8) | b;
        }
    }

    void fillRect(int x, int y, int rw, int rh, uint32_t argb) {
        for (int j = 0; j < rh; ++j) fillRow(x, y + j, rw, argb, 1.0f);
    }

    void vgrad(int x, int y, int rw, int rh, uint32_t top, uint32_t bottom) {
        if (rh <= 0) return;
        for (int j = 0; j < rh; ++j) {
            const float t = (rh == 1) ? 0.0f : float(j) / float(rh - 1);
            fillRow(x, y + j, rw, mix(top, bottom, t), 1.0f);
        }
    }

    void hgrad(int x, int y, int rw, int rh, uint32_t left, uint32_t right) {
        if (rw <= 0 || rh <= 0) return;
        for (int i = 0; i < rw; ++i) {
            const float t = (rw == 1) ? 0.0f : float(i) / float(rw - 1);
            const uint32_t c = mix(left, right, t);
            for (int j = 0; j < rh; ++j) blendPix(x + i, y + j, c, 1.0f);
        }
    }

    void hline(int x, int y, int rw, uint32_t argb) { fillRow(x, y, rw, argb, 1.0f); }
    void vline(int x, int y, int rh, uint32_t argb) { fillRect(x, y, 1, rh, argb); }

    // Retangulo arredondado. O nucleo e solido por linha; so as quatro faixas
    // r x r dos cantos fazem aritmetica de raio.
    void rrect(int x, int y, int rw, int rh, int r, uint32_t argb) {
        r = std::max(0, std::min({r, rw / 2, rh / 2}));
        if (r <= 0) { fillRect(x, y, rw, rh, argb); return; }
        for (int j = 0; j < rh; ++j) {
            if (j >= r && j < rh - r) { fillRow(x, y + j, rw, argb, 1.0f); continue; }
            fillRow(x + r, y + j, rw - 2 * r, argb, 1.0f);
            for (int i = 0; i < r; ++i) {
                const float cov = cornerCoverage(i, j, rw, rh, r);
                if (cov > 0.0f) blendPix(x + i, y + j, argb, cov);
                const float cov2 = cornerCoverage(rw - 1 - i, j, rw, rh, r);
                if (cov2 > 0.0f) blendPix(x + rw - 1 - i, y + j, argb, cov2);
            }
        }
    }

    void rrectStroke(int x, int y, int rw, int rh, int r, int thick, uint32_t argb) {
        r = std::max(0, std::min({r, rw / 2, rh / 2}));
        if (thick <= 0) return;
        const int ri = std::max(0, r - thick);
        for (int j = 0; j < rh; ++j) {
            for (int i = 0; i < rw; ++i) {
                const float out = roundedBoxSd(float(i) + 0.5f, float(j) + 0.5f, rw, rh, r);
                if (out > 0.5f) continue;
                const float in = roundedBoxSd(float(i) + 0.5f, float(j) + 0.5f, rw, rh, ri);
                // Anel = intersecao de "fora do buraco" com "dentro do contorno":
                // a distancia assinada de uma intersecao e o maximo das duas, o que
                // conserva o AA de 1 px nas duas bordas em vez de escurecer o meio
                // como fazia a subtracao de coberturas.
                const float cov = aaMask(std::max(out, -in));
                if (cov > 0.0f) blendPix(x + i, y + j, argb, cov);
            }
        }
    }

    // Halo radial (brasa embaixo do knob, luz da tampa etc).
    void glow(int cx, int cy, int radius, uint32_t argb, float strength = 1.0f) {
        const int x0 = cx - radius, y0 = cy - radius;
        for (int j = 0; j <= 2 * radius; ++j) {
            for (int i = 0; i <= 2 * radius; ++i) {
                const float dx = float(i - radius) / float(radius);
                const float dy = float(j - radius) / float(radius);
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d >= 1.0f) continue;
                const float f = (1.0f - d) * (1.0f - d);
                blendPix(x0 + i, y0 + j, withAlpha(argb, f * strength), 1.0f);
            }
        }
    }

    // Arco de valor: anel entre rIn e rOut, de ang0 a ang1 (radianos, 0 = 3h,
    // sentido horario porque Y cresce pra baixo).
    //
    // As duas bordas radiais usam a distancia assinada do anel; as bordas ANGULARES
    // usam rampa em arco-comprimento (dang * r), nunca em radiano -- em radiano o
    // rampa teria 1 rad de largura e o corte sairia um dente de serra de varios
    // pixels. O span e normalizado para (0, 2pi], senao um arco de 270 graus
    // (o trilho do knob) e lido como 90 e o valor aparece no lado errado.
    void arc(int cx, int cy, float rIn, float rOut, float ang0, float ang1, uint32_t argb,
             bool roundCaps = false) {
        const float mid = 0.5f * (rIn + rOut);
        const float half = std::max(0.0f, 0.5f * (rOut - rIn));
        const float twoPi = 2.0f * float(M_PI);
        float span = ang1 - ang0;
        while (span < 0.0f) span += twoPi;
        if (span > twoPi) span = twoPi;
        const float rMax = mid + half + 1.0f, rMin = std::max(0.0f, mid - half - 1.0f);
        const int rad = int(std::ceil(rMax));
        const float rMax2 = rMax * rMax, rMin2 = rMin * rMin;
        for (int j = -rad; j <= rad; ++j) {
            for (int i = -rad; i <= rad; ++i) {
                // O centro do pixel esta exatamente a (i, j) pixels do centro do
                // arco: cx e cy sao centros inteiros, nao cantos de celula.
                const float d2 = float(i) * float(i) + float(j) * float(j);
                if (d2 > rMax2 || d2 < rMin2) continue;
                const float d = std::sqrt(d2);
                float cov = aaMask(std::fabs(d - mid) - half);
                if (cov <= 0.0f) continue;
                if (span < twoPi - 1e-4f && d > 0.5f) {
                    float a = std::atan2(float(j), float(i));
                    if (a < 0.0f) a += twoPi;
                    float rel = a - ang0;
                    while (rel < 0.0f) rel += twoPi;
                    // Rampa pela distancia PERPENDICULAR ao raio de corte
                    // (d*sin(dang)), nao pelo arco em r=mid: com o arco medido no
                    // meio do anel, a borda externa do corte ficava ~2 px a frente
                    // da interna e o fim do arco saia com um gancho.
                    const float s0 = (rel > 1.5f) ? -1.0f : -d * std::sin(rel);
                    const float left = span - rel;
                    const float s1 = (left > 1.5f) ? -1.0f : -d * std::sin(left);
                    cov *= aaMask(s0) * aaMask(s1);
                    if (cov <= 0.0f) continue;
                }
                blendPix(cx + i, cy + j, argb, cov);
            }
        }
        // Pontas redondas (stroke-linecap:round do SVG do prototipo): dois discos
        // de raio half centrados no meio do anel, nos dois angulos extremos.
        if (roundCaps && half > 0.0f) {
            disc(cx + int(std::lround(mid * std::cos(ang0))),
                 cy + int(std::lround(mid * std::sin(ang0))), half, argb);
            if (span < twoPi - 1e-4f)
                disc(cx + int(std::lround(mid * std::cos(ang0 + span))),
                     cy + int(std::lround(mid * std::sin(ang0 + span))), half, argb);
        }
    }

    // Segmento de espessura constante com AA por distancia assinada (capsula).
    // O ponteiro do knob usava um passo de 1 px ao longo do raio: a linha e
    // continua, mas a ESPESSURA virava degrau de 1 px em todo angulo que nao e
    // 0/45/90, e a ponta ficava com um furo. Entre argbA (na origem) e argbB (no
    // fim) a cor interpola pela fracao ao longo do segmento.
    void segment(float x0, float y0, float x1, float y1, float width,
                 uint32_t argbA, uint32_t argbB = 0) {
        if (argbB == 0) argbB = argbA;
        const float vx = x1 - x0, vy = y1 - y0;
        const float len2 = vx * vx + vy * vy;
        const float hw = width * 0.5f;
        const int bx0 = int(std::floor(std::min(x0, x1) - hw - 1.0f));
        const int bx1 = int(std::ceil(std::max(x0, x1) + hw + 1.0f));
        const int by0 = int(std::floor(std::min(y0, y1) - hw - 1.0f));
        const int by1 = int(std::ceil(std::max(y0, y1) + hw + 1.0f));
        for (int j = by0; j <= by1; ++j) {
            for (int i = bx0; i <= bx1; ++i) {
                const float dx = float(i) - x0, dy = float(j) - y0;
                float t = len2 > 0.0f ? (dx * vx + dy * vy) / len2 : 0.0f;
                t = std::max(0.0f, std::min(1.0f, t));
                const float qx = dx - vx * t, qy = dy - vy * t;
                const float cov = aaMask(std::sqrt(qx * qx + qy * qy) - hw);
                if (cov <= 0.0f) continue;
                blendPix(i, j, t == 0.0f ? argbA : (t == 1.0f ? argbB : mix(argbA, argbB, t)), cov);
            }
        }
    }

    // Disco. Existe separado de rrect porque a convensao de (cx, cy) aqui e o
    // CENTRO DO PIXEL, igual a arc() e glow(): um circulo desenhado como rrect de
    // raio R tem seu centro na FRONTEIRA entre dois pixels, entao disco e arco
    // ficavam 0,5 px fora de concentricidade e a trilha saia com espessura
    // desigual -- serrilhado que nao vem da cobertura, vem da geometria.
    void disc(int cx, int cy, float r, uint32_t argb) {
        const int ri = int(std::ceil(r));
        for (int j = -ri; j <= ri; ++j) {
            for (int i = -ri; i <= ri; ++i) {
                const float cov = aaMask(std::sqrt(float(i) * float(i) + float(j) * float(j)) - r);
                if (cov > 0.0f) blendPix(cx + i, cy + j, argb, cov);
            }
        }
    }

    // Face do knob no SVG do prototipo: gradiente diagonal (x1,y1 -> x2,y2 =
    // 0,0 -> 1,1) com parada em 58%, recortada no circulo.
    void faceDisc(int cx, int cy, float r, uint32_t tl, uint32_t mid, uint32_t br) {
        const int ri = int(std::ceil(r));
        for (int j = -ri; j <= ri; ++j) {
            for (int i = -ri; i <= ri; ++i) {
                const float cov = aaMask(std::sqrt(float(i) * float(i) + float(j) * float(j)) - r);
                if (cov <= 0.0f) continue;
                const float t = std::max(0.0f, std::min(1.0f, 0.5f + float(i + j) / (4.0f * r)));
                const uint32_t c = t < 0.58f ? mix(tl, mid, t / 0.58f)
                                             : mix(mid, br, (t - 0.58f) / 0.42f);
                blendPix(cx + i, cy + j, c, cov);
            }
        }
    }

    // Gradiente vertical recortado no disco. E assim que a luz de cima do knob e
    // desenhada: sem o recorte o retangulo de highlight sai pelos lados do
    // circulo e o disco le como um quadrado com uma rodela por cima.
    // `top` e a primeira linha do gradiente, em offset relativo ao centro.
    void vgradDisc(int cx, int cy, int r, int top, int height,
                   uint32_t topColor, uint32_t bottomColor) {
        if (r <= 0 || height <= 0) return;
        const float rr = float(r);
        for (int j = -r; j <= r; ++j) {
            const float t = std::max(0.0f, std::min(1.0f, float(j - top) / float(height)));
            const uint32_t c = mix(topColor, bottomColor, t);
            if (((c >> 24) & 0xFF) == 0) continue;
            const float dy = float(j);
            for (int i = -r; i <= r; ++i) {
                const float cov = aaMask(std::sqrt(dy * dy + float(i) * float(i)) - rr);
                if (cov > 0.0f) blendPix(cx + i, cy + j, c, cov);
            }
        }
    }

    // Arte do preset: bilinear dentro do retangulo de destino, respeitando o
    // alpha da imagem e uma opacidade extra (o painel esvanece quando nao ha
    // piano escolhido). Borda a bordo, sem clip arredondado -- a placa e reta.
    void blendImage(const Image& img, int x, int y, int rw, int rh, float opacity = 1.0f) {
        if (img.empty() || rw <= 0 || rh <= 0 || opacity <= 0.0f) return;
        const auto clampIdx = [](int i, int n) { return i < 0 ? 0 : (i >= n ? n - 1 : i); };
        const auto sample = [&](float u, float v) {
            const float fx = u * float(img.w) - 0.5f;
            const float fy = v * float(img.h) - 0.5f;
            const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            const float tx = fx - float(x0), ty = fy - float(y0);
            const int x1 = clampIdx(x0 + 1, img.w), y1 = clampIdx(y0 + 1, img.h);
            const int cx0 = clampIdx(x0, img.w), cy0 = clampIdx(y0, img.h);
            const uint32_t a = img.px[size_t(cy0) * img.w + cx0];
            const uint32_t b = img.px[size_t(cy0) * img.w + x1];
            const uint32_t c = img.px[size_t(y1) * img.w + cx0];
            const uint32_t d = img.px[size_t(y1) * img.w + x1];
            return mix(mix(a, b, tx), mix(c, d, tx), ty);
        };
        for (int j = 0; j < rh; ++j) {
            const float v = (float(j) + 0.5f) / float(rh);
            for (int i = 0; i < rw; ++i)
                blendPix(x + i, y + j, sample((float(i) + 0.5f) / float(rw), v), opacity);
        }
    }

private:
    // Distancia assinada ao contorno de um retangulo arredondado de tamanho
    // (bw x bh) e raio r, no ponto (x, y) em coordenadas continuas da caixa.
    // Formula exata do "rounded box" SDF; o sqrt so acontece dentro do quadrante
    // da esquina, que e onde o AA de fato mora.
    static float roundedBoxSd(float x, float y, float bw, float bh, float r) {
        const float qx = std::fabs(x - 0.5f * bw) - (0.5f * bw - r);
        const float qy = std::fabs(y - 0.5f * bh) - (0.5f * bh - r);
        if (qx > 0.0f && qy > 0.0f) return std::sqrt(qx * qx + qy * qy) + std::min(qx, qy) - r;
        return std::max(qx, qy) - r;
    }

    static float cornerCoverage(int i, int j, int rw, int rh, int r) {
        return aaMask(roundedBoxSd(float(i) + 0.5f, float(j) + 0.5f, float(rw), float(rh), float(r)));
    }
};

} // namespace pforge
