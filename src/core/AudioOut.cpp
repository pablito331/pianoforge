#include "AudioOut.hpp"

#include <cstring>

#if defined(_WIN32)
// MinGW-w64 nao tem <dlfcn.h> nem libasound, entao o --live do standalone nao tem o
// que abrir. Um stub que responde "nao da" deixa o pforge_standalone compilar e
// linkar no Windows (e o CI produz o mesmo binario nos tres sistemas) em vez de
// sumir com o target inteiro por causa de uma bandeira de -live.
namespace pforge {

AudioOut::~AudioOut() = default;

bool AudioOut::open(const std::string&, uint32_t, uint32_t, std::string& error) {
    error = "saida de audio em tempo real exige ALSA (Linux) ou CoreAudio: "
            "use -o arquivo.wav neste sistema";
    return false;
}

void AudioOut::close() {}

bool AudioOut::write(const int16_t*, size_t, std::string& error) {
    error = "saida de audio fechada";
    return false;
}

bool AudioOut::recover(std::string& error) {
    error = "saida de audio fechada";
    return false;
}

} // namespace pforge
#else
#include <dlfcn.h>

namespace pforge {
namespace {

constexpr int kStreamPlayback = 0;
// Valores do <sound/asound.h> (kernel), que e o que o libasound ve por baixo:
// S16_LE e 2 (a enum tem buracos pras variantes de endian) e RW_INTERLEAVED e 3.
// Com access 0 (MMAP_INTERLEAVED) o snd_pcm_writei devolve -EINVAL em todo bloco.
constexpr int kFormatS16LE = 2;
constexpr int kAccessRWInterleaved = 3;
constexpr int kEpipe = -32;             // underrun: -EPIPE
constexpr int kEagain = -11;
constexpr int kEintr = -4;

void* loadSym(void* lib, const char* name) {
    return dlsym(lib, name);
}

} // namespace

AudioOut::~AudioOut() {
    close();
}

bool AudioOut::open(const std::string& device, uint32_t rate, uint32_t latencyUs,
                    std::string& error) {
    lib_ = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib_) lib_ = dlopen("libasound.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
        error = "libasound nao encontrada: " + std::string(dlerror() ? dlerror() : "");
        return false;
    }

    open_ = reinterpret_cast<decltype(open_)>(loadSym(lib_, "snd_pcm_open"));
    setParams_ = reinterpret_cast<decltype(setParams_)>(loadSym(lib_, "snd_pcm_set_params"));
    writei_ = reinterpret_cast<decltype(writei_)>(loadSym(lib_, "snd_pcm_writei"));
    prepare_ = reinterpret_cast<decltype(prepare_)>(loadSym(lib_, "snd_pcm_prepare"));
    drain_ = reinterpret_cast<decltype(drain_)>(loadSym(lib_, "snd_pcm_drain"));
    close_ = reinterpret_cast<decltype(close_)>(loadSym(lib_, "snd_pcm_close"));
    strError_ = reinterpret_cast<decltype(strError_)>(loadSym(lib_, "snd_strerror"));
    if (!open_ || !setParams_ || !writei_ || !prepare_ || !close_) {
        error = "libasound sem as funcoes snd_pcm_* necessarias";
        return false;
    }

    int rc = open_(&handle_, device.c_str(), kStreamPlayback, 0);
    if (rc < 0) {
        error = "snd_pcm_open(" + device + "): " + (strError_ ? strError_(rc) : "?");
        handle_ = nullptr;
        return false;
    }

    rc = setParams_(handle_, kFormatS16LE, kAccessRWInterleaved, 2, rate,
                    1 /*resample se precisar*/, latencyUs);
    if (rc < 0) {
        error = "snd_pcm_set_params(" + device + ", " + std::to_string(rate) + " Hz): " +
                (strError_ ? strError_(rc) : "?");
        close_(handle_);
        handle_ = nullptr;
        return false;
    }

    device_ = device;
    return true;
}

void AudioOut::close() {
    if (handle_ && drain_) drain_(handle_);
    if (handle_ && close_) close_(handle_);
    handle_ = nullptr;
    if (lib_) dlclose(lib_);
    lib_ = nullptr;
}

bool AudioOut::write(const int16_t* interleaved, size_t frames, std::string& error) {
    if (!handle_) {
        error = "saida de audio fechada";
        return false;
    }
    size_t done = 0;
    while (done < frames) {
        long rc = writei_(handle_, interleaved + done * 2,
                          static_cast<unsigned long>(frames - done));
        if (rc == kEintr) continue;
        if (rc == kEagain) continue;
        if (rc < 0) {
            if (rc == kEpipe) {          // underrun: re-arma e escreve o resto
                ++underruns_;
                if (!recover(error)) return false;
                continue;
            }
            error = "snd_pcm_writei: " + std::string(strError_ ? strError_(static_cast<int>(rc)) : "?");
            return false;
        }
        done += static_cast<size_t>(rc);
    }
    return true;
}

bool AudioOut::recover(std::string& error) {
    const int rc = prepare_(handle_);
    if (rc < 0) {
        error = "snd_pcm_prepare: " + std::string(strError_ ? strError_(rc) : "?");
        return false;
    }
    return true;
}

} // namespace pforge
#endif  // _WIN32
