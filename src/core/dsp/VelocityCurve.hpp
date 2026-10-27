#pragma once
#include <cmath>
#include <algorithm>

namespace pianoforge {
namespace dsp {

class VelocityCurve {
public:
    static int process(int rawVelocity, float curve) noexcept {
        if (rawVelocity <= 0) return 0;
        if (rawVelocity >= 127) return 127;

        float norm = static_cast<float>(rawVelocity) / 127.0f;
        // curve: 0.0 (very soft/concave) to 0.5 (linear) to 1.0 (very hard/convex)
        float gamma = std::exp((0.5f - curve) * 3.0f);
        float curved = std::pow(norm, gamma);
        
        int result = static_cast<int>(std::round(curved * 127.0f));
        return std::clamp(result, 1, 127);
    }
};

} // namespace dsp
} // namespace pianoforge
