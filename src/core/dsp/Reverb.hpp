#pragma once
#include <vector>
#include <array>
#include <cmath>

namespace pianoforge {
namespace dsp {

class CombFilter {
public:
    void setup(size_t delayLength, float feedbackGain, float dampingCoeff) {
        buffer.assign(delayLength, 0.0f);
        writeIdx = 0;
        feedback = feedbackGain;
        damp = dampingCoeff;
        filterState = 0.0f;
    }

    inline float process(float in) noexcept {
        float out = buffer[writeIdx];
        filterState = out * (1.0f - damp) + filterState * damp;
        buffer[writeIdx] = in + filterState * feedback;
        writeIdx = (writeIdx + 1) % buffer.size();
        return out;
    }

private:
    std::vector<float> buffer;
    size_t writeIdx = 0;
    float feedback = 0.8f;
    float damp = 0.2f;
    float filterState = 0.0f;
};

class AllpassFilter {
public:
    void setup(size_t delayLength, float feedbackGain = 0.5f) {
        buffer.assign(delayLength, 0.0f);
        writeIdx = 0;
        feedback = feedbackGain;
    }

    inline float process(float in) noexcept {
        float bufOut = buffer[writeIdx];
        float out = -in + bufOut;
        buffer[writeIdx] = in + bufOut * feedback;
        writeIdx = (writeIdx + 1) % buffer.size();
        return out;
    }

private:
    std::vector<float> buffer;
    size_t writeIdx = 0;
    float feedback = 0.5f;
};

class Reverb {
public:
    void prepare(float sampleRate) {
        sr = sampleRate > 1000.0f ? sampleRate : 44100.0f;
        updateType();
    }

    void setMix(float mixAmount) noexcept {
        mix = std::fmax(0.0f, std::fmin(1.0f, mixAmount));
    }

    void setType(int type) noexcept {
        int clamped = std::clamp(type, 0, 3);
        if (clamped != currentType) {
            currentType = clamped;
            updateType();
        }
    }

    void process(float* left, float* right, size_t numFrames) noexcept {
        if (mix < 0.001f) return;

        for (size_t i = 0; i < numFrames; ++i) {
            float inL = left[i];
            float inR = right[i];
            float monoIn = (inL + inR) * 0.5f;

            // Parallel comb filters
            float outCombL = combsL[0].process(monoIn) + combsL[1].process(monoIn) +
                             combsL[2].process(monoIn) + combsL[3].process(monoIn);

            float outCombR = combsR[0].process(monoIn) + combsR[1].process(monoIn) +
                             combsR[2].process(monoIn) + combsR[3].process(monoIn);

            // Series allpass diffusion
            float wetL = allpassesL[1].process(allpassesL[0].process(outCombL * 0.25f));
            float wetR = allpassesR[1].process(allpassesR[0].process(outCombR * 0.25f));

            // Equal-power crossfade
            left[i] = inL * (1.0f - mix * 0.4f) + wetL * (mix * 0.8f);
            right[i] = inR * (1.0f - mix * 0.4f) + wetR * (mix * 0.8f);
        }
    }

private:
    void updateType() noexcept {
        float scale = sr / 44100.0f;

        // Base delays (prime numbers to prevent resonance buildup)
        const size_t combLengthsL[4] = {
            static_cast<size_t>(1116 * scale),
            static_cast<size_t>(1188 * scale),
            static_cast<size_t>(1277 * scale),
            static_cast<size_t>(1356 * scale)
        };
        const size_t combLengthsR[4] = {
            static_cast<size_t>(1139 * scale),
            static_cast<size_t>(1211 * scale),
            static_cast<size_t>(1299 * scale),
            static_cast<size_t>(1379 * scale)
        };

        float feedback = 0.8f;
        float damp = 0.2f;

        switch (currentType) {
            case 0: // Small Room
                feedback = 0.72f;
                damp = 0.45f;
                break;
            case 1: // Studio Hall
                feedback = 0.84f;
                damp = 0.25f;
                break;
            case 2: // Plate
                feedback = 0.88f;
                damp = 0.15f;
                break;
            case 3: // Cathedral
                feedback = 0.93f;
                damp = 0.10f;
                break;
        }

        for (int k = 0; k < 4; ++k) {
            combsL[k].setup(combLengthsL[k], feedback, damp);
            combsR[k].setup(combLengthsR[k], feedback, damp);
        }

        allpassesL[0].setup(static_cast<size_t>(225 * scale), 0.5f);
        allpassesL[1].setup(static_cast<size_t>(556 * scale), 0.5f);

        allpassesR[0].setup(static_cast<size_t>(237 * scale), 0.5f);
        allpassesR[1].setup(static_cast<size_t>(568 * scale), 0.5f);
    }

    float sr = 44100.0f;
    float mix = 0.25f;
    int currentType = 1;

    std::array<CombFilter, 4> combsL;
    std::array<CombFilter, 4> combsR;
    std::array<AllpassFilter, 2> allpassesL;
    std::array<AllpassFilter, 2> allpassesR;
};

} // namespace dsp
} // namespace pianoforge
