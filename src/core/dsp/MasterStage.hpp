#pragma once
#include <cmath>
#include <algorithm>

namespace pianoforge {
namespace dsp {

class MasterStage {
public:
    // O clipper cubico antigo terminava em exactly 1,0 no joelho e `return 1.0f`
    // acima dele: todo amostra mais quente que 1,5 viria 1,0 puro -- topo chato,
    // que e clipping digital com outro nome, e a forma de onda quadrada que da
    // intermodulacao num acorde. Medido com o volume do painel em 1,0, a saida do
    // plugin alcancava exactly 0 dBFS (78 amostras em FS na gravacao do usuario).
    //
    // Agora: o joelho desce para 1,494 para que a cubica pare em 0,996 com
    // inclinacao ZERO, e a cauda racional sobe de 0,996 assintoticamente a 1 sem
    // nunca encostar. |saida| < 1 por construcao; nenhum host vai mais ver FS.
    // Abaixo do joelho a curva e a mesma a 0,1% -- em volume de todo dia nada muda.
    static constexpr float kKnee = 1.494f;
    static constexpr float kCubic = 1.0f / (3.0f * kKnee * kKnee);
    static constexpr float kRoof = 2.0f * kKnee / 3.0f;   // = 0,996, o topo no joelho
    static constexpr float kTailDelta = 1.0f - kRoof;     // = 0,004
    static constexpr float kTailQ = 4.0f;

    static inline float softClip(float x) noexcept {
        const float sign = x < 0.0f ? -1.0f : 1.0f;
        const float a = sign * x;                 // |x| sem chamar fabs
        if (a <= kKnee) return sign * (a - kCubic * a * a * a);
        const float u = a - kKnee;
        return sign * (1.0f - kTailDelta / (1.0f + kTailQ * u * u));
    }

    void process(float* left, float* right, size_t numFrames,
                 float volume, float trimDb, float stereoWidth) noexcept {
        float trimGain = std::pow(10.0f, trimDb / 20.0f);
        float totalGain = volume * trimGain;
        float width = std::clamp(stereoWidth, 0.0f, 2.0f);

        for (size_t i = 0; i < numFrames; ++i) {
            float l = left[i];
            float r = right[i];

            // Mid/Side stereo processing
            float mid = 0.5f * (l + r);
            float side = 0.5f * (l - r) * width;

            l = (mid + side) * totalGain;
            r = (mid - side) * totalGain;

            // Soft-clipping protection
            left[i] = softClip(l);
            right[i] = softClip(r);
        }
    }
};

} // namespace dsp
} // namespace pianoforge
