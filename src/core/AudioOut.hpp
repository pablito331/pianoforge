#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pforge {

// Saida de audio em tempo real por ALSA. A maquina nao tem <alsa/*.h>, entao o
// libasound.so.2 e aberto com dlopen e so as seis funcoes usadas sao declaradas
// aqui. "default" passa pelo plugin alsa-pipewire quando ele existe, o que mantem
// o mixer do desktop funcionando; plughw:0,0 vai direto na placa.
class AudioOut {
public:
    AudioOut() = default;
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    bool open(const std::string& device, uint32_t rate, uint32_t latencyUs,
              std::string& error);
    void close();

    bool isOpen() const { return handle_ != nullptr; }

    // Entregue quadros de stereo entrelacado. Bloqueia quando o buffer do driver
    // esta cheio: e isso que cadencia o render em tempo real.
    bool write(const int16_t* interleaved, size_t frames, std::string& error);

    // Recupera de underrun (-EPIPE). Sem isso, um bloco perdido cala a saida para
    // sempre, que e o pior tipo de silencio.
    bool recover(std::string& error);

    uint64_t underruns() const { return underruns_; }
    const std::string& device() const { return device_; }

private:
    void* lib_ = nullptr;
    void* handle_ = nullptr;
    std::string device_;
    uint64_t underruns_ = 0;

    int (*open_)(void**, const char*, int, unsigned int) = nullptr;
    int (*setParams_)(void*, int, int, unsigned, unsigned, int, unsigned) = nullptr;
    long (*writei_)(void*, const void*, unsigned long) = nullptr;
    int (*prepare_)(void*) = nullptr;
    int (*drain_)(void*) = nullptr;
    int (*close_)(void*) = nullptr;
    const char* (*strError_)(int) = nullptr;
};

} // namespace pforge
