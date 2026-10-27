#pragma once
#include "BiquadFilter.hpp"
#include <cmath>

namespace pianoforge {
namespace dsp {

class SoundboardEQ {
public:
    void prepare(float sampleRate) {
        sr = sampleRate > 1000.0f ? sampleRate : 44100.0f;
        reset();
        updateFilters();
    }

    void reset() noexcept {
        lowL.reset(); lowR.reset();
        midL.reset(); midR.reset();
        highL.reset(); highR.reset();
    }

    void setGains(float bass, float mid, float high) noexcept {
        float newBassDb = (bass - 0.5f) * 16.0f; // -8dB to +8dB
        float newMidDb = (mid - 0.5f) * 16.0f;
        float newHighDb = (high - 0.5f) * 16.0f;

        if (std::fabs(newBassDb - bassDb) > 0.05f ||
            std::fabs(newMidDb - midDb) > 0.05f ||
            std::fabs(newHighDb - highDb) > 0.05f) {
            bassDb = newBassDb;
            midDb = newMidDb;
            highDb = newHighDb;
            updateFilters();
        }
    }

    void process(float* left, float* right, size_t numFrames) noexcept {
        for (size_t i = 0; i < numFrames; ++i) {
            float l = left[i];
            float r = right[i];

            l = lowL.process(l);
            l = midL.process(l);
            l = highL.process(l);

            r = lowR.process(r);
            r = midR.process(r);
            r = highR.process(r);

            left[i] = l;
            right[i] = r;
        }
    }

private:
    void updateFilters() noexcept {
        lowL.setup(BiquadFilter::LowShelf, 120.0f, sr, 0.7071f, bassDb);
        lowR.setup(BiquadFilter::LowShelf, 120.0f, sr, 0.7071f, bassDb);

        midL.setup(BiquadFilter::Peaking, 1000.0f, sr, 0.8f, midDb);
        midR.setup(BiquadFilter::Peaking, 1000.0f, sr, 0.8f, midDb);

        highL.setup(BiquadFilter::HighShelf, 5000.0f, sr, 0.7071f, highDb);
        highR.setup(BiquadFilter::HighShelf, 5000.0f, sr, 0.7071f, highDb);
    }

    float sr = 44100.0f;
    float bassDb = 0.0f;
    float midDb = 0.0f;
    float highDb = 0.0f;

    BiquadFilter lowL, lowR;
    BiquadFilter midL, midR;
    BiquadFilter highL, highR;
};

} // namespace dsp
} // namespace pianoforge
