#pragma once
#include <cmath>

namespace pianoforge {
namespace dsp {

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class BiquadFilter {
public:
    enum Type {
        Lowpass,
        Highpass,
        Bandpass,
        LowShelf,
        HighShelf,
        Peaking
    };

    void reset() noexcept {
        z1 = 0.0f;
        z2 = 0.0f;
    }

    void setup(Type type, float frequencyHz, float sampleRate, float q = 0.7071f, float gainDb = 0.0f) noexcept {
        if (sampleRate <= 0.0f) return;
        
        // Clamp frequency to Nyquist safe range
        float f0 = std::fmax(10.0f, std::fmin(frequencyHz, sampleRate * 0.48f));
        float w0 = 2.0f * static_cast<float>(M_PI) * f0 / sampleRate;
        float cosW = std::cos(w0);
        float sinW = std::sin(w0);
        float alpha = sinW / (2.0f * std::fmax(0.01f, q));
        float A = std::pow(10.0f, gainDb / 40.0f);

        float a0 = 1.0f;

        switch (type) {
            case Lowpass:
                b0 = (1.0f - cosW) * 0.5f;
                b1 = 1.0f - cosW;
                b2 = (1.0f - cosW) * 0.5f;
                a0 = 1.0f + alpha;
                a1 = -2.0f * cosW;
                a2 = 1.0f - alpha;
                break;

            case Highpass:
                b0 = (1.0f + cosW) * 0.5f;
                b1 = -(1.0f + cosW);
                b2 = (1.0f + cosW) * 0.5f;
                a0 = 1.0f + alpha;
                a1 = -2.0f * cosW;
                a2 = 1.0f - alpha;
                break;

            case Bandpass:
                b0 = alpha;
                b1 = 0.0f;
                b2 = -alpha;
                a0 = 1.0f + alpha;
                a1 = -2.0f * cosW;
                a2 = 1.0f - alpha;
                break;

            case LowShelf: {
                float sqrtA = std::sqrt(A);
                b0 = A * ((A + 1.0f) - (A - 1.0f) * cosW + 2.0f * sqrtA * alpha);
                b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW);
                b2 = A * ((A + 1.0f) - (A - 1.0f) * cosW - 2.0f * sqrtA * alpha);
                a0 = (A + 1.0f) + (A - 1.0f) * cosW + 2.0f * sqrtA * alpha;
                a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW);
                a2 = (A + 1.0f) + (A - 1.0f) * cosW - 2.0f * sqrtA * alpha;
                break;
            }

            case HighShelf: {
                float sqrtA = std::sqrt(A);
                b0 = A * ((A + 1.0f) + (A - 1.0f) * cosW + 2.0f * sqrtA * alpha);
                b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosW);
                b2 = A * ((A + 1.0f) + (A - 1.0f) * cosW - 2.0f * sqrtA * alpha);
                a0 = (A + 1.0f) - (A - 1.0f) * cosW + 2.0f * sqrtA * alpha;
                a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosW);
                a2 = (A + 1.0f) - (A - 1.0f) * cosW - 2.0f * sqrtA * alpha;
                break;
            }

            case Peaking:
                b0 = 1.0f + alpha * A;
                b1 = -2.0f * cosW;
                b2 = 1.0f - alpha * A;
                a0 = 1.0f + alpha / A;
                a1 = -2.0f * cosW;
                a2 = 1.0f - alpha / A;
                break;
        }

        float invA0 = 1.0f / a0;
        b0 *= invA0;
        b1 *= invA0;
        b2 *= invA0;
        a1 *= invA0;
        a2 *= invA0;
    }

    inline float process(float in) noexcept {
        // Transposed Direct Form II
        float out = b0 * in + z1;
        z1 = b1 * in - a1 * out + z2;
        z2 = b2 * in - a2 * out;

        // Denormal protection
        if (std::fabs(z1) < 1e-18f) z1 = 0.0f;
        if (std::fabs(z2) < 1e-18f) z2 = 0.0f;

        return out;
    }

private:
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
};

} // namespace dsp
} // namespace pianoforge
