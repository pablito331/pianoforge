// Implementacao do shell VST3. As decisoes de desenho estao em PianoForgeVst3.hpp.

#include "PianoForgeVst3.hpp"

#include "ForgeUi.hpp"
#include "Parameters.hpp"
#include "SoundbankManager.hpp"
#include "StatusPorts.hpp"
#include "UiFactory.hpp"

#include "base/source/fstring.h"
#include "pluginterfaces/base/fstrdefs.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/commonstringconvert.h"
#include "public.sdk/source/common/pluginview.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#if SMTG_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace pianoforge {
namespace vst3 {

namespace {

void logLine(const std::string& msg) {
    std::fprintf(stderr, "[PianoForge VST3] %s\n", msg.c_str());
}

// Pasta do binario do plugin. Serve a uma coisa so: achar o Contents/Resources do
// bundle, onde vao as fontes do painel. Caminho relativo ao processo (o
// "assets/ui" que a UI usa como reserva) nao serve dentro de um DAW, que roda com
// o cwd dele.
std::string moduleDir() {
#if SMTG_OS_WINDOWS
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleDir), &mod) ||
        !mod)
        return {};
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(mod, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
    if (!n) return {};
    std::wstring w(buf, n);
    const size_t cut = w.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return {};
    w.resize(cut);
    const int len =
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &out[0], len, nullptr, nullptr);
    return out;
#else
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&moduleDir), &info) || !info.dli_fname) return {};
    std::string p(info.dli_fname);
    const size_t cut = p.find_last_of('/');
    return cut == std::string::npos ? std::string() : p.substr(0, cut);
#endif
}

// O bundle VST3 e <X>.vst3/Contents/<arch>-<so>/<binario> nos tres sistemas (e
// Contents/MacOS no macOS), entao ../Resources vale em todos.
std::string resourcesDir() {
    const std::string dir = moduleDir();
    return dir.empty() ? std::string() : dir + "/../Resources";
}

bool readExact(Steinberg::IBStream* s, void* dst, Steinberg::int32 n) {
    Steinberg::int32 got = 0;
    return s->read(dst, n, &got) == Steinberg::kResultOk && got == n;
}

bool writeExact(Steinberg::IBStream* s, const void* src, Steinberg::int32 n) {
    Steinberg::int32 put = 0;
    return s->write(const_cast<void*>(src), n, &put) == Steinberg::kResultOk && put == n;
}

// Estado: identificador + versao antes de qualquer campo. Um host que abra com a
// nossa versao futura recusa, em vez de ler 17 floats de outro layout como som.
constexpr Steinberg::int32 kStateMagic = 0x50465633;   // 'PFV3'
constexpr Steinberg::int32 kStateVersion = 1;

// write_() do painel chega aqui: a numeracao de portas do proprio painel.
void uiWriteThunk(void* controller, uint32_t port, uint32_t size, uint32_t format,
                  const void* buffer) {
    static_cast<PianoForgeProcessor*>(controller)->uiWrite(port, size, format, buffer);
}

// Titulo de parametro: o VST3 e UTF-16 e kParameterDefs e UTF-8 com acento
// ("Curva de Dinamica" sai "Dinamica" com til). UString so tem fromAscii, que
// transformaria cada acento em dois caracteres errados -- o conversor do proprio
// SDK e a traducao certa, e o buffer de destino e o String128 do ParameterInfo.
void copyTo128(const char* utf8, Steinberg::Vst::String128 dst) {
    const std::u16string src = Steinberg::StringConvert::convert(std::string(utf8 ? utf8 : ""));
    const size_t n = std::min<size_t>(src.size(), 127);
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    dst[n] = 0;
}

} // namespace

// ===========================================================================
// ciclo de vida
// ===========================================================================

PianoForgeProcessor::PianoForgeProcessor() {
    // uiSent_ e NaN, nao 0: 0 e um valor de parametro legitimo (Graves em 0.0) e
    // trataria o primeiro push como "ja mandei".
    for (int i = 0; i < Param_Count; ++i)
        uiSent_[i] = std::numeric_limits<float>::quiet_NaN();
}

PianoForgeProcessor::~PianoForgeProcessor() = default;

Steinberg::tresult PLUGIN_API PianoForgeProcessor::initialize(Steinberg::FUnknown* context) {
    const Steinberg::tresult r = SingleComponentEffect::initialize(context);
    if (r != Steinberg::kResultOk) return r;

    // Instrumento: sem entrada de audio, uma saida estereo, uma entrada de eventos.
    addAudioOutput(STR16("Saida"), Steinberg::Vst::SpeakerArr::kStereo);
    addEventInput(STR16("MIDI"), 16);

    // Os 17 do motor, com os nomes em portugues que o painel mostra. O titulo do
    // VST3 e wide (TChar), e copyTo128 traduz o UTF-8 de kParameterDefs.
    for (int i = 0; i < Param_Count; ++i) {
        const auto& def = kParameterDefs[i];
        Steinberg::Vst::String128 title, shortTitle;
        copyTo128(def.name, title);
        copyTo128(def.shortName, shortTitle);
        const Steinberg::int32 flags =
            Steinberg::Vst::ParameterInfo::kCanAutomate |
            (def.isVisible ? 0 : Steinberg::Vst::ParameterInfo::kIsHidden);
        parameters.addParameter(new Steinberg::Vst::RangeParameter(
            title, Steinberg::Vst::ParamID(i), nullptr, def.minValue, def.maxValue,
            def.defaultValue, Steinberg::Vst::kStepCountContinuous, flags,
            Steinberg::Vst::kRootUnitId, shortTitle));
    }

    // Banco e preset: no painel sao evento e o painel e o unico controle. No VST3 o
    // host precisa deles no estado e na automacao, e por isso viram parametro --
    // escondidos do painel generico, porque quem escolhe piano e o painel.
    parameters.addParameter(new Steinberg::Vst::RangeParameter(
        STR16("Banco"), Steinberg::Vst::ParamID(kBankParamId), nullptr, -1.0, kMaxBanks - 1,
        -1.0, kMaxBanks, Steinberg::Vst::ParameterInfo::kCanAutomate |
                            Steinberg::Vst::ParameterInfo::kIsHidden));
    parameters.addParameter(new Steinberg::Vst::RangeParameter(
        STR16("Preset"), Steinberg::Vst::ParamID(kPresetParamId), nullptr, 0.0, kMaxPresets,
        0.0, kMaxPresets, Steinberg::Vst::ParameterInfo::kCanAutomate |
                             Steinberg::Vst::ParameterInfo::kIsHidden));

    bundlePath_ = resourcesDir();

    // A thread de carga nasce com o plugin, nao com a ativacao: o painel pede
    // piano com o transporte parado (e o caso normal de escolher um banco).
    loaderRun_.store(true, std::memory_order_release);
    loader_ = std::thread([this] { loaderLoop(); });
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API PianoForgeProcessor::terminate() {
    loaderRun_.store(false, std::memory_order_release);
    if (loader_.joinable()) loader_.join();
    engine_.drainRetireNow();
    return SingleComponentEffect::terminate();
}

Steinberg::tresult PLUGIN_API PianoForgeProcessor::setupProcessing(
    Steinberg::Vst::ProcessSetup& setup) {
    const Steinberg::tresult r = SingleComponentEffect::setupProcessing(setup);
    if (r != Steinberg::kResultOk) return r;
    sampleRate_ = setup.sampleRate;
    blockSize_ = size_t(std::max<Steinberg::int32>(setup.maxSamplesPerBlock, 64));
    engine_.prepare(float(sampleRate_), blockSize_);
    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API PianoForgeProcessor::setActive(Steinberg::TBool state) {
    if (state) {
        engine_.allNotesOff();
    } else {
        engine_.allNotesOff();
        // O host garante que nao ha process() concorrente neste instante: e a hora
        // certa de devolver os bancos aposentados (munmap de centenas de MB nunca
        // pertence a thread de audio).
        engine_.drainRetireNow();
    }
    return Steinberg::kResultOk;
}

Steinberg::uint32 PLUGIN_API PianoForgeProcessor::getTailSamples() {
    // Nao e kInfiniteTail: um DAW em freeze/render esperaria para sempre. Seis
    // segundos cobrem a cauda do reverb mais longo do painel.
    return Steinberg::uint32(std::max(1.0, sampleRate_ * 6.0));
}

// ===========================================================================
// audio
// ===========================================================================

Steinberg::tresult PLUGIN_API PianoForgeProcessor::process(Steinberg::Vst::ProcessData& data) {
    if (data.symbolicSampleSize != Steinberg::Vst::kSample32) return Steinberg::kResultFalse;
    if (data.numSamples <= 0 || data.numOutputs < 1) return Steinberg::kResultOk;
    auto& out = data.outputs[0];
    if (out.numChannels < 2 || !out.channelBuffers32) return Steinberg::kResultOk;
    float* const outL = out.channelBuffers32[0];
    float* const outR = out.channelBuffers32[1];
    if (!outL || !outR) return Steinberg::kResultOk;
    out.silenceFlags = 0;
    const size_t frames = size_t(data.numSamples);

    // beginBlock PRIMEIRO, antes de qualquer evento: e ele que zera a fila de
    // eventos pendentes (Engine.hpp), e enfileirar o MIDI antes dele jogava
    // fora toda nota -- o sintoma era um piano mudo com o banco carregado.
    engine_.beginBlock();      // snapshot do banco + arma a aposentadoria

    // 1. Automacao: aplica o ultimo ponto do bloco. O motor suaviza 20 ms, entao
    // fatiar o render a cada ponto de automacao nao mudaria o som de um piano --
    // e a diferenca de um bloco fica abaixo do que o ouvido localiza.
    if (data.inputParameterChanges) {
        const Steinberg::int32 count = data.inputParameterChanges->getParameterCount();
        for (Steinberg::int32 i = 0; i < count; ++i) {
            auto* queue = data.inputParameterChanges->getParameterData(i);
            if (!queue) continue;
            const Steinberg::int32 points = queue->getPointCount();
            if (points <= 0) continue;
            Steinberg::int32 offset = 0;
            Steinberg::Vst::ParamValue value = 0.0;
            if (queue->getPoint(points - 1, offset, value) != Steinberg::kResultOk) continue;
            const auto tag = Steinberg::Vst::ParamID(queue->getParameterId());
            // getParameter e leitura do container, montado no initialize: seguro
            // depois disso. setNormalized NAO e chamado daqui (a thread da UI le
            // esses objetos); o valor autoritativo do som e o do motor.
            const auto* p = parameters.getParameter(tag);
            applyPlain(tag, p ? float(p->toPlain(value)) : float(value));
        }
    }

    // 2. MIDI. Evento fora do bloco e clampado, e nao descartado: um host que
    // mande sampleOffset == numSamples estaria dizendo "no fim do bloco", e
    // perder a nota e pior que toca-la no ultimo frame.
    if (data.inputEvents) {
        const Steinberg::int32 count = data.inputEvents->getEventCount();
        for (Steinberg::int32 i = 0; i < count; ++i) {
            Steinberg::Vst::Event e{};
            if (data.inputEvents->getEvent(i, e) != Steinberg::kResultOk) continue;
            const int delay = int(std::clamp<Steinberg::int32>(e.sampleOffset, 0,
                                                               data.numSamples - 1));
            switch (e.type) {
                case Steinberg::Vst::Event::kNoteOnEvent: {
                    const int vel = int(std::lround(e.noteOn.velocity * 127.0f));
                    if (vel > 0) engine_.handleNoteOn(delay, int(e.noteOn.pitch), vel);
                    else engine_.handleNoteOff(delay, int(e.noteOn.pitch), 0);
                    break;
                }
                case Steinberg::Vst::Event::kNoteOffEvent:
                    engine_.handleNoteOff(delay, int(e.noteOff.pitch),
                                          int(std::lround(e.noteOff.velocity * 127.0f)));
                    break;
                case Steinberg::Vst::Event::kLegacyMIDICCOutEvent: {
                    // No VST3 o caminho "correto" de um CC e o host mapea-lo em
                    // parametro (IMidiMapping), o que exige expor pedal e mod como
                    // parametro escondido e depender do host fazer o mapeamento.
                    // Varios hosts mandam o CC cru por aqui; recusar seria perder o
                    // sustain em metade deles, e aceitar nao custa nada a quem usa
                    // o caminho mapeado.
                    const auto& cc = e.midiCCOut;
                    if (cc.controlNumber == Steinberg::Vst::kPitchBend) {
                        // value = MSB, value2 = LSB: 14 bits, centro em 8192.
                        const int bend = ((int(cc.value) << 7) | (int(cc.value2) & 0x7f)) - 8192;
                        engine_.handlePitchBend(delay, bend);
                    } else if (cc.controlNumber <= 127) {
                        engine_.handleCC(delay, int(cc.controlNumber), int(cc.value));
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    // 3. Audio. O render espera o buffer do bloco zerado; o motor mistura nele.
    std::memset(outL, 0, frames * sizeof(float));
    std::memset(outR, 0, frames * sizeof(float));
    engine_.render(outL, outR, frames);
    return Steinberg::kResultOk;
}

// ===========================================================================
// parametros
// ===========================================================================

Steinberg::tresult PLUGIN_API PianoForgeProcessor::setParamNormalized(
    Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value) {
    auto* p = parameters.getParameter(tag);
    if (!p) return Steinberg::kInvalidArgument;
    p->setNormalized(value);
    applyPlain(tag, float(p->toPlain(value)));
    return Steinberg::kResultOk;
}

void PianoForgeProcessor::applyPlain(Steinberg::Vst::ParamID tag, float plain) {
    if (tag >= 0 && tag < Param_Count) {
        engine_.getParameters().setValue(int(tag), plain);
        return;
    }
    if (tag == kBankParamId) {
        loadFailed_.store(false, std::memory_order_relaxed);
        loadInFlight_.store(true, std::memory_order_relaxed);
        requestBank(int(std::lround(plain)));
        return;
    }
    if (tag == kPresetParamId) {
        const int idx = std::max(0, int(std::lround(plain)));
        if (engine_.setPresetIndex(idx)) currentPreset_ = idx;
    }
}

float PianoForgeProcessor::plainOf(Steinberg::Vst::ParamID tag) const {
    if (tag >= 0 && tag < Param_Count) return engine_.getParameters().getValue(int(tag));
    if (tag == kBankParamId) return float(published_.load(std::memory_order_relaxed));
    if (tag == kPresetParamId) return float(currentPreset_);
    return 0.0f;
}

// ===========================================================================
// carga de banco (a thread, no lugar do worker que o host nao nos da)
// ===========================================================================

void PianoForgeProcessor::requestBank(int index) noexcept {
    jobIndex_.store(index, std::memory_order_release);
}

void PianoForgeProcessor::requestPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(jobMx_);
    jobPath_ = path;
}

int PianoForgeProcessor::indexOfPath(const std::string& path) {
    return SoundbankManager::getInstance().findByPath(path);
}

void PianoForgeProcessor::loaderLoop() {
    while (loaderRun_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Liberar o banco aposentado e trabalho de quem nao e a thread de audio.
        if (engine_.retirePending()) {
            engine_.freeRetiredBanks();
            engine_.clearRetirePending();
        }
        std::string path;
        {
            std::lock_guard<std::mutex> lk(jobMx_);
            path.swap(jobPath_);
        }
        const int index = jobIndex_.exchange(-2, std::memory_order_acq_rel);
        if (!path.empty() || index != -2) loadNow(index, path);
    }
}

void PianoForgeProcessor::loadNow(int index, const std::string& path) {
    // Dono unico do arquivo: um pedido novo enquanto o anterior roi o disco e
    // simplesmente descartado (o proximo laco pega o mais recente).
    bool livre = false;
    if (!loadingNow_.compare_exchange_strong(livre, true)) return;
    struct Soltar {
        std::atomic<bool>& flag;
        ~Soltar() { flag.store(false, std::memory_order_release); }
    } soltar{loadingNow_};

    // A trilha de quem escolheu o piano: uma linha por pedido aceito, nao por
    // bloco. E o que responde "por que nao soou" num DAW sem debugger.
    logLine("pedido de carga: indice " + std::to_string(index) + ", caminho \"" + path + "\"");

    auto refuse = [&](const std::string& what) {
        loadInFlight_.store(false, std::memory_order_relaxed);
        loadFailed_.store(true, std::memory_order_relaxed);
        logLine(what + " (indice " + std::to_string(index) + ")");
    };

    // -1 = nenhum piano (o painel manda isso ao remover o banco atual).
    if (path.empty() && index < 0) {
        engine_.allNotesOff();
        engine_.publishBank(nullptr);
        published_.store(-1, std::memory_order_relaxed);
        publishedPresets_.store(0, std::memory_order_relaxed);
        currentPreset_ = 0;
        loadInFlight_.store(false, std::memory_order_relaxed);
        loadFailed_.store(false, std::memory_order_relaxed);
        logLine("nenhum piano selecionado (silencio)");
        return;
    }

    SoundbankManager& mgr = SoundbankManager::getInstance();
    mgr.scan();

    std::string bankPath = path;
    int resolvedIndex = index;
    if (!bankPath.empty()) {
        // Caminho salvo no estado: vale mesmo que o registro tenha mudado (e o
        // caso de sessao aberta noutra maquina, com o mesmo arquivo noutro lugar).
        // Sem registro, toca e o painel mostra -1, que e a verdade.
        resolvedIndex = indexOfPath(bankPath);
    } else {
        SoundbankInfo info;
        if (!mgr.getBankAt(index, info)) {
            refuse("indice de banco inexistente, " + std::to_string(mgr.getBankCount()) +
                   " registrados");
            return;
        }
        if (!info.exists) {
            refuse("arquivo nao encontrado: " + info.path);
            return;
        }
        bankPath = info.path;
    }

    // RAM antes de I/O: um SF2 de 612 MB pede 1,2 GB (o tsf converte int16 em
    // float), e carregar o novo com o velho publicado poria 1,7 GB no ar. O preco
    // e um bloco de silencio numa troca que ja ia trocar de piano.
    if (published_.load(std::memory_order_relaxed) >= 0 || engine_.holdingRetired()) {
        engine_.allNotesOff();
        engine_.publishBank(nullptr);
        published_.store(-1, std::memory_order_relaxed);
        publishedPresets_.store(0, std::memory_order_relaxed);
        currentPreset_ = 0;
        if (engine_.retirePending()) {
            engine_.freeRetiredBanks();
            engine_.clearRetirePending();
        } else {
            // A thread de audio arma retirePending em beginBlock(). Com o
            // transporte parado nao ha bloco nenhum: 300 ms e o teto que assume o
            // pico de RAM em vez de ficar "carregando" para sempre.
            for (int i = 0; i < 3 && !engine_.retirePending(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            engine_.freeRetiredBanks();
            engine_.clearRetirePending();
        }
    }

    Bank* next = engine_.buildBank(bankPath);
    if (!next) {
        refuse("falha ao carregar: " + bankPath);
        return;
    }

    engine_.allNotesOff();
    engine_.publishBank(next);
    currentPreset_ = 0;
    published_.store(resolvedIndex, std::memory_order_relaxed);
    publishedPresets_.store(next->presetCount, std::memory_order_relaxed);
    loadInFlight_.store(false, std::memory_order_relaxed);
    loadFailed_.store(false, std::memory_order_relaxed);
    logLine("piano carregado: " + bankPath + " (" + std::to_string(next->presetCount) +
            " presets)");
}

// ===========================================================================
// estado
// ===========================================================================

Steinberg::tresult PLUGIN_API PianoForgeProcessor::getState(Steinberg::IBStream* state) {
    if (!state) return Steinberg::kInvalidArgument;
    if (!writeExact(state, &kStateMagic, sizeof(kStateMagic))) return Steinberg::kResultFalse;
    if (!writeExact(state, &kStateVersion, sizeof(kStateVersion))) return Steinberg::kResultFalse;

    for (int i = 0; i < Param_Count; ++i) {
        const float v = plainOf(Steinberg::Vst::ParamID(i));
        if (!writeExact(state, &v, sizeof(v))) return Steinberg::kResultFalse;
    }

    const Steinberg::int32 bank = published_.load(std::memory_order_relaxed);
    const Steinberg::int32 preset = currentPreset_;
    if (!writeExact(state, &bank, sizeof(bank))) return Steinberg::kResultFalse;
    if (!writeExact(state, &preset, sizeof(preset))) return Steinberg::kResultFalse;

    // O caminho, e nao so o indice: o registro e global e muda entre maquinas. O
    // indice fica como reserva para o caso do arquivo ter sido movido.
    std::string path;
    {
        SoundbankInfo info;
        if (bank >= 0 && SoundbankManager::getInstance().getBankAt(bank, info)) path = info.path;
    }
    const Steinberg::int32 pathLen = Steinberg::int32(path.size());
    if (!writeExact(state, &pathLen, sizeof(pathLen))) return Steinberg::kResultFalse;
    if (pathLen > 0 && !writeExact(state, path.data(), pathLen)) return Steinberg::kResultFalse;

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API PianoForgeProcessor::setState(Steinberg::IBStream* state) {
    if (!state) return Steinberg::kInvalidArgument;
    Steinberg::int32 magic = 0, version = 0;
    if (!readExact(state, &magic, sizeof(magic)) || magic != kStateMagic)
        return Steinberg::kResultFalse;
    if (!readExact(state, &version, sizeof(version)) || version != kStateVersion)
        return Steinberg::kResultFalse;

    for (int i = 0; i < Param_Count; ++i) {
        float v = 0.0f;
        if (!readExact(state, &v, sizeof(v))) return Steinberg::kResultFalse;
        if (auto* p = parameters.getParameter(Steinberg::Vst::ParamID(i)))
            p->setNormalized(p->toNormalized(v));
        engine_.getParameters().setValue(i, v);
    }

    Steinberg::int32 bank = -1, preset = 0;
    if (!readExact(state, &bank, sizeof(bank))) return Steinberg::kResultFalse;
    if (!readExact(state, &preset, sizeof(preset))) return Steinberg::kResultFalse;
    Steinberg::int32 pathLen = 0;
    if (!readExact(state, &pathLen, sizeof(pathLen))) return Steinberg::kResultFalse;
    std::string path;
    if (pathLen > 0 && pathLen < 32768) {
        path.resize(size_t(pathLen));
        if (!readExact(state, &path[0], pathLen)) return Steinberg::kResultFalse;
    }

    // Restaurar o estado e um pedido de som: quem abre a sessao espera o piano
    // de volta. O estado foi salvo pela nossa getState, e o plugin continua
    // abrindo mudo quando nada foi restaurado.
    loadFailed_.store(false, std::memory_order_relaxed);
    loadInFlight_.store(true, std::memory_order_relaxed);
    if (!path.empty()) requestPath(path);
    else requestBank(bank);

    const int idx = std::max(0, int(preset));
    if (engine_.setPresetIndex(idx)) currentPreset_ = idx;
    return Steinberg::kResultOk;
}

// ===========================================================================
// ponte com o painel
// ===========================================================================

void PianoForgeProcessor::uiWrite(uint32_t port, uint32_t size, uint32_t format,
                                  const void* buffer) {
    if (format != 0 || !buffer || size != sizeof(float)) return;
    const float v = *static_cast<const float*>(buffer);
    if (!std::isfinite(v)) return;

    if (port == kUiPortBank) {
        loadFailed_.store(false, std::memory_order_relaxed);
        loadInFlight_.store(true, std::memory_order_relaxed);
        requestBank(int(std::lround(v)));
        return;
    }
    if (port == kUiPortPreset) {
        applyPlain(kPresetParamId, v);
        return;
    }

    const int pid = int(port) - int(kUiPortParamBase);
    if (pid < 0 || pid >= Param_Count) return;
    auto* p = parameters.getParameter(Steinberg::Vst::ParamID(pid));
    if (!p) return;
    const Steinberg::Vst::ParamValue norm = p->toNormalized(v);
    p->setNormalized(norm);
    engine_.getParameters().setValue(pid, v);

    // Gesto de um write so. O painel manda um valor por passo do arrasto, e o
    // beginEdit/endEdit em volta de cada um e o que faz o host gravar automacao
    // sem inventar "o usuario soltou" no meio de um gesto que continua. Um gancho
    // de press/release no ForgeUi seria mais fino; este e o que nao perde dado.
    if (componentHandler) {
        componentHandler->beginEdit(Steinberg::Vst::ParamID(pid));
        componentHandler->performEdit(Steinberg::Vst::ParamID(pid), norm);
        componentHandler->endEdit(Steinberg::Vst::ParamID(pid));
    }
}

void PianoForgeProcessor::attachUi(pfui::ForgeUi* ui) {
    ui_ = ui;
    if (!ui_) return;
    for (int i = 0; i < Param_Count; ++i)
        uiSent_[i] = std::numeric_limits<float>::quiet_NaN();
    uiSentState_ = INT_MIN;
    uiSentPresets_ = -1;
    uiSentBank_ = -2;
    uiSentPreset_ = -1;
    // O painel nao tem idle() no VST3: este gancho (chamado de tick(), na thread
    // da UI) e o que faz automacao e estado do banco chegarem na tela.
    ui_->onHostPoll = [this] { pushToUi(); };
}

void PianoForgeProcessor::detachUi() {
    if (ui_) ui_->onHostPoll = nullptr;
    ui_ = nullptr;
}

void PianoForgeProcessor::pushToUi() {
    if (!ui_) return;

    const int stateCode = pianoforge::bankStateCode(loadInFlight_.load(std::memory_order_relaxed),
                                                    loadFailed_.load(std::memory_order_relaxed),
                                                    published_.load(std::memory_order_relaxed));
    if (stateCode != uiSentState_) {
        uiSentState_ = stateCode;
        float v = float(stateCode);
        ui_->portEvent(pianoforge::kPortBankState, sizeof(v), 0, &v);
    }

    const int presets = publishedPresets_.load(std::memory_order_relaxed);
    if (presets != uiSentPresets_) {
        uiSentPresets_ = presets;
        float v = float(presets);
        ui_->portEvent(pianoforge::kPortPresetCount, sizeof(v), 0, &v);
    }

    const int bank = published_.load(std::memory_order_relaxed);
    if (bank != uiSentBank_) {
        uiSentBank_ = bank;
        float v = float(bank);
        ui_->portEvent(kUiPortBank, sizeof(v), 0, &v);
    }

    if (currentPreset_ != uiSentPreset_) {
        uiSentPreset_ = currentPreset_;
        float v = float(currentPreset_);
        ui_->portEvent(kUiPortPreset, sizeof(v), 0, &v);
    }

    for (int i = 0; i < Param_Count; ++i) {
        const float v = plainOf(Steinberg::Vst::ParamID(i));
        // Comparacao sem tolerancia: so reenvia o que mudou de verdade. Sem isto o
        // painel redesenharia a 6 Hz para sempre.
        if (v == uiSent_[i]) continue;
        uiSent_[i] = v;
        ui_->portEvent(kUiPortParamBase + uint32_t(i), sizeof(v), 0, &v);
    }
}

// ===========================================================================
// a view (IPlugView) e o painel
// ===========================================================================

class PianoForgeView final : public Steinberg::CPluginView {
public:
    explicit PianoForgeView(PianoForgeProcessor* proc) : proc_(proc) {
        rect.left = 0;
        rect.top = 0;
        rect.right = pfui::ForgeUi::preferredWidth();
        rect.bottom = pfui::ForgeUi::preferredHeight();
    }
    ~PianoForgeView() override { detach(); }

    // getPlatformType() nao existe mais no IPlugView (saiu no 3.6.0): quem diz o
    // tipo do handle e o `type` que chega em attached(). A funcao aqui e so para
    // nao repetir o #if em isPlatformTypeSupported.
    static Steinberg::FIDString platformType() {
#if SMTG_OS_WINDOWS
        return Steinberg::kPlatformTypeHWND;
#elif SMTG_OS_MACOS
        return Steinberg::kPlatformTypeNSView;
#else
        return Steinberg::kPlatformTypeX11EmbedWindowID;
#endif
    }

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override {
        return (type && std::strcmp(type, platformType()) == 0) ? Steinberg::kResultTrue
                                                               : Steinberg::kResultFalse;
    }

    // O host chamou attached() com o container dele: aqui o painel nasce. No
    // Windows isto roda na thread que chamou o host e que e dona do HWND --
    // criar o filho de outra thread trava o processo (ver o topo de
    // backend_win32.cpp).
    void attachedToParent() override {
        if (ui_) return;
        void* native = nullptr;
        ui_ = pfui::createUi(systemWindow, proc_->bundlePath().c_str(), &uiWriteThunk, proc_,
                             &native);
        if (!ui_) {
            logLine("o painel nao pode ser criado; a view abre vazia");
            return;
        }
        proc_->attachUi(ui_);
        ui_->show();      // o host ja nos deu o container: nao ha "janela propria"
    }

    void removedFromParent() override { detach(); }

    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) override {
        if (!size) return Steinberg::kInvalidArgument;
        *size = rect;
        return Steinberg::kResultOk;
    }

    // Tamanho fixo: o painel e um desenho de 1000x520 e o layout nao reflui. Um
    // host que abra em container menor recebe o painel reduzido pelo
    // ForgeUi::adoptSize (WM_SIZE/ConfigureNotify), nunca esticado.
    Steinberg::tresult PLUGIN_API canResize() override { return Steinberg::kResultFalse; }

    OBJ_METHODS(PianoForgeView, CPluginView)
    DEFINE_INTERFACES
        DEF_INTERFACE(Steinberg::IPlugView)
    END_DEFINE_INTERFACES(CPluginView)
    REFCOUNT_METHODS(CPluginView)

private:
    void detach() {
        if (!ui_) return;
        proc_->detachUi();
        pfui::destroyUi(ui_);
        ui_ = nullptr;
    }

    PianoForgeProcessor* proc_ = nullptr;
    pfui::ForgeUi* ui_ = nullptr;
};

Steinberg::IPlugView* PLUGIN_API PianoForgeProcessor::createView(Steinberg::FIDString name) {
    if (name && std::strcmp(name, Steinberg::Vst::ViewType::kEditor) != 0) return nullptr;
    return new PianoForgeView(this);   // refcount inicial 1: o host libera
}

} // namespace vst3
} // namespace pianoforge
