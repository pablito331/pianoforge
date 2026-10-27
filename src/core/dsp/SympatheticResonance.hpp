#pragma once
#include "BiquadFilter.hpp"
#include <array>
#include <cmath>
#include <vector>

namespace pianoforge {
namespace dsp {

struct Resonator {
    int note = -1;
    BiquadFilter filterL;
    BiquadFilter filterR;
    float gain = 0.0f;
    float env = 0.0f;
    float decay = 0.999f;
    bool active = false;
};

class SympatheticResonance {
public:
    void prepare(float sampleRate) {
        sr = sampleRate > 1000.0f ? sampleRate : 44100.0f;
        for (auto& r : resonators) {
            r.active = false;
            r.env = 0.0f;
            r.filterL.reset();
            r.filterR.reset();
        }
        std::fill(activeKeys.begin(), activeKeys.end(), false);
        sustainPedal = false;
    }

    void noteOn(int note, int /*velocity*/) noexcept {
        if (note >= 0 && note < 128) {
            activeKeys[note] = true;
            updateResonators();
        }
    }

    void noteOff(int note) noexcept {
        if (note >= 0 && note < 128) {
            activeKeys[note] = false;
            if (!sustainPedal) {
                updateResonators();
            }
        }
    }

    void setSustainPedal(bool down) noexcept {
        if (sustainPedal != down) {
            sustainPedal = down;
            updateResonators();
        }
    }

    void setParameters(float resonanceAmount, float resonanceTune) noexcept {
        amount = resonanceAmount;
        tuneOffset = (resonanceTune - 0.5f) * 0.1f; // fine tune variation
    }

    void process(float* left, float* right, size_t numFrames) noexcept {
        if (amount < 0.001f) return;

        for (size_t i = 0; i < numFrames; ++i) {
            float inL = left[i];
            float inR = right[i];
            float monoIn = (inL + inR) * 0.5f;

            float resL = 0.0f;
            float resR = 0.0f;

            for (auto& r : resonators) {
                if (!r.active && r.env < 0.0001f) continue;

                float outL = r.filterL.process(monoIn);
                float outR = r.filterR.process(monoIn);

                resL += outL * r.env * r.gain;
                resR += outR * r.env * r.gain;

                if (!r.active) {
                    r.env *= r.decay;
                }
            }

            left[i] += resL * amount * 0.4f;
            right[i] += resR * amount * 0.4f;
        }
    }

private:
    float noteToFreq(int note) const noexcept {
        return 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f);
    }

    void updateResonators() noexcept {
        // Find which notes should resonate
        // When sustain is down, open up a chordal spread of harmonics
        int activeCount = 0;
        for (int n = 0; n < 128; ++n) {
            if (activeKeys[n] || sustainPedal) {
                if (activeCount < static_cast<int>(resonators.size())) {
                    auto& r = resonators[activeCount];
                    r.note = n;
                    r.active = true;
                    r.env = 1.0f;
                    r.gain = 1.0f / (1.0f + 0.1f * activeCount);
                    
                    float freq = noteToFreq(n) * (1.0f + tuneOffset);
                    if (freq < sr * 0.45f) {
                        r.filterL.setup(BiquadFilter::Bandpass, freq, sr, 8.0f);
                        r.filterR.setup(BiquadFilter::Bandpass, freq * 1.002f, sr, 8.0f); // slight stereo spread
                    }
                    activeCount++;
                }
            }
        }

        // Deactivate remaining
        for (size_t i = activeCount; i < resonators.size(); ++i) {
            resonators[i].active = false;
        }
    }

    float sr = 44100.0f;
    float amount = 0.5f;
    float tuneOffset = 0.0f;
    bool sustainPedal = false;
    std::array<bool, 128> activeKeys{};
    std::array<Resonator, 16> resonators{};
};

} // namespace dsp
} // namespace pianoforge
