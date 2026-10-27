#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace pforge {

// Cabecalho WAV PCM 16 bits estéreo, empacado sem padding (20 + 8 + dataSize).
#pragma pack(push, 1)
struct WavHeader {
    char riffTag[4] = {'R', 'I', 'F', 'F'};
    uint32_t riffSize = 0;
    char waveTag[4] = {'W', 'A', 'V', 'E'};
    char fmtTag[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1;       // PCM
    uint16_t numChannels = 2;
    uint32_t sampleRate = 44100;
    uint32_t byteRate = 44100 * 4;
    uint16_t blockAlign = 4;
    uint16_t bitsPerSample = 16;
    char dataTag[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize = 0;
};
#pragma pack(pop)

static_assert(sizeof(WavHeader) == 44, "WAV canonical: 12 + 20 + 8");

// Flutuante -> int16 entrelacado, com clamp. O mesmo codigo serve o arquivo e o
// pipe da placa de audio: se divergirem, o que se ouve nao e o que se renderiza.
inline std::vector<int16_t> toInterleavedS16(const std::vector<float>& left,
                                             const std::vector<float>& right) {
    const size_t n = std::min(left.size(), right.size());
    std::vector<int16_t> pcm(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const float l = std::fmax(-1.0f, std::fmin(1.0f, left[i]));
        const float r = std::fmax(-1.0f, std::fmin(1.0f, right[i]));
        pcm[i * 2 + 0] = static_cast<int16_t>(std::lround(l * 32767.0f));
        pcm[i * 2 + 1] = static_cast<int16_t>(std::lround(r * 32767.0f));
    }
    return pcm;
}

inline bool writeWavFile(const std::string& filename,
                         const std::vector<float>& left,
                         const std::vector<float>& right,
                         uint32_t sampleRate) {
    std::ofstream out(filename, std::ios::binary);
    if (!out) return false;

    const std::vector<int16_t> pcm = toInterleavedS16(left, right);
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));

    WavHeader header;
    header.sampleRate = sampleRate;
    header.byteRate = sampleRate * 4;
    header.dataSize = dataBytes;
    header.riffSize = 36 + dataBytes;

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);
    return static_cast<bool>(out);
}

} // namespace pforge
