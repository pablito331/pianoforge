#pragma once
#include <vector>
#include <cmath>

namespace pianoforge {
namespace dsp {

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class Chorus {
public:
    void prepare(float sampleRate) {
        sr = sampleRate > 1000.0f ? sampleRate : 44100.0f;
        // Maximum delay ~ 50ms
        maxDelay = static_cast<size_t>(sr * 0.05f) + 16;
        bufferL.assign(maxDelay, 0.0f);
        bufferR.assign(maxDelay, 0.0f);
        writePos = 0;
        phase = 0.0f;
        lfoInc = 2.0f * static_cast<float>(M_PI) * 0.8f / sr; // 0.8 Hz LFO rate
    }

    void setMix(float mixAmount) noexcept {
        mix = std::fmax(0.0f, std::fmin(1.0f, mixAmount));
    }

    void process(float* left, float* right, size_t numFrames) noexcept {
        if (mix < 0.001f || bufferL.empty()) return;

        float baseDelay = sr * 0.015f; // 15 ms base
        float depth = sr * 0.004f;     // 4 ms depth modulation

        for (size_t i = 0; i < numFrames; ++i) {
            float inL = left[i];
            float inR = right[i];

            bufferL[writePos] = inL;
            bufferR[writePos] = inR;

            float lfoL = std::sin(phase);
            float lfoR = std::cos(phase); // 90 degree stereo offset

            float readPosL = static_cast<float>(writePos) - (baseDelay + depth * lfoL);
            float readPosR = static_cast<float>(writePos) - (baseDelay + depth * lfoR);

            float wetL = readInterpolated(bufferL, readPosL);
            float wetR = readInterpolated(bufferR, readPosR);

            left[i] = inL * (1.0f - mix * 0.5f) + wetL * (mix * 0.7f);
            right[i] = inR * (1.0f - mix * 0.5f) + wetR * (mix * 0.7f);

            writePos = (writePos + 1) % maxDelay;
            phase += lfoInc;
            if (phase >= 2.0f * static_cast<float>(M_PI)) {
                phase -= 2.0f * static_cast<float>(M_PI);
            }
        }
    }

private:
    float readInterpolated(const std::vector<float>& buf, float pos) const noexcept {
        while (pos < 0.0f) pos += static_cast<float>(buf.size());
        size_t idx0 = static_cast<size_t>(pos) % buf.size();
        size_t idx1 = (idx0 + 1) % buf.size();
        float frac = pos - std::floor(pos);
        return buf[idx0] * (1.0f - frac) + buf[idx1] * frac;
    }

    float sr = 44100.0f;
    float mix = 0.1f;
    size_t maxDelay = 2048;
    size_t writePos = 0;
    float phase = 0.0f;
    float lfoInc = 0.0f;

    std::vector<float> bufferL;
    std::vector<float> bufferR;
};

} // namespace dsp
} // namespace pianoforge
