#pragma once
#include "BiquadFilter.hpp"
#include <cmath>

namespace pianoforge {
namespace dsp {

class LidFilter {
public:
    void prepare(float sampleRate) {
        sr = sampleRate > 1000.0f ? sampleRate : 44100.0f;
        reset();
        updateFilter();
    }

    void reset() noexcept {
        filterL.reset();
        filterR.reset();
    }

    void setLid(float lidPos) noexcept {
        if (std::fabs(lidPos - lid) > 0.02f) {
            lid = lidPos;
            updateFilter();
        }
    }

    void process(float* left, float* right, size_t numFrames) noexcept {
        // If lid is nearly 100% open, bypass filter for maximum clarity
        if (lid >= 0.98f) return;

        for (size_t i = 0; i < numFrames; ++i) {
            left[i] = filterL.process(left[i]);
            right[i] = filterR.process(right[i]);
        }
    }

private:
    void updateFilter() noexcept {
        // Cutoff moves exponentially from ~2500 Hz (closed) to 16000 Hz (open)
        float cutoff = 2500.0f * std::pow(16000.0f / 2500.0f, lid);
        float q = 0.5f + 0.2f * lid; // gently damp resonance when closed
        filterL.setup(BiquadFilter::Lowpass, cutoff, sr, q);
        filterR.setup(BiquadFilter::Lowpass, cutoff, sr, q);
    }

    float sr = 44100.0f;
    float lid = 0.7f;
    BiquadFilter filterL;
    BiquadFilter filterR;
};

} // namespace dsp
} // namespace pianoforge
