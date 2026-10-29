// pforge_vst3_probe: host VST3 minimo, so para verificar o binario.
//
//   A  fabrica          -> GetPluginFactory, uma classe, kVstAudioEffectClass
//   B  instanciacao      -> IComponent + IAudioProcessor + IEditController no
//                          MESMO objeto (componente combinado, sem classId)
//   C  barramento/params -> uma saida estereo, uma entrada de eventos, 19 params
//                          com titulo (os 17 do motor + banco + preset)
//   D  estado            -> getState/setState ida e volta; estado com magic
//                          errado e recusado em vez de lido como som
//   E  audio             -> setState com um SF2 sintetico (tools/SynthBank.hpp)
//                          e o piano SOA; antes disso, silencio absoluto
//   F  view              -> createView, tipo de plataforma, attached num HWND de
//                          verdade (Windows) e pixels pintados
//
// Uso: pforge_vst3_probe <PianoForge.vst3 | pasta do bundle>
//        --work <dir>   onde gravar o SF2 sintetico (o sandbox do CI pode barrar
//                       o diretorio temporario do sistema, e um load que nao
//                       acontece daria "FALHOU: sem som" sem dizer por que)
//        --ui           abre o painel e confere os pixels: no Windows o pai e um
//                       HWND, no Linux um container X11 (o CI sobe um Xvfb). No
//                       macOS nao ha prova de UI em CI.
//
// Sai com 0 quando tudo passa e 1 quando algo falha: e o que o CI le.
#include "Parameters.hpp"

#include "pluginterfaces/base/fplatform.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"

#include "Dl.hpp"             // dlopen em tres SOs (o mingw nao tem <dlfcn.h>)
#include "SynthBank.hpp"      // banco sintetico: o CI nao tem (nem pode ter) soundfont

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
// NOMINMAX antes do <windows.h>: o MSVC define min/max como MACRO e este probe
// usa std::min/std::max (inclusive em initializer_list) no corpo dos testes.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace Steinberg;
namespace Vst = Steinberg::Vst;

namespace {

int g_failures = 0;

bool expect(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
    return ok;
}

void note(const char* fmt, ...) {
    std::printf("       ");
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
}

// ParamID de banco/preset no shell (PianoForgeVst3.hpp). Numeros fixos porque o
// estado salvo do usuario depende deles: mudar aqui quebra sessao, nao "evolui".
constexpr Vst::ParamID kBankParamId = 100;
constexpr Vst::ParamID kPresetParamId = 101;
constexpr int kStateMagic = 0x50465633;   // 'PFV3'
constexpr int kStateVersion = 1;

// ------------------------------------------------------------------ strings
// Titulo de parametro e UTF-16. So o necessario para imprimir: o BMP inteiro,
// que e onde os nomes em portugues vivem.
std::string utf16To8(const char16* s, size_t maxLen) {
    std::string out;
    for (size_t i = 0; i < maxLen && s[i]; ++i) {
        const uint32_t c = uint32_t(s[i]);
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// ------------------------------------------------------------------ streams
// O que o plugin usa de IBStream: ler, escrever, seek, tell. Sem isto o estado
// nao pode ser exercitado, e estado e metade do contrato de um plugin.
class MemStream final : public IBStream {
public:
    MemStream() = default;
    explicit MemStream(std::vector<char> bytes) : bytes_(std::move(bytes)) {}

    tresult PLUGIN_API queryInterface(const TUID, void** obj) override {
        if (obj) *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        const size_t at = std::min(pos_, bytes_.size());
        const size_t want = size_t(std::max(numBytes, int32(0)));
        const size_t n = std::min(want, bytes_.size() - at);
        if (n && buffer) std::memcpy(buffer, bytes_.data() + at, n);
        pos_ = at + n;
        if (numBytesRead) *numBytesRead = int32(n);
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override {
        if (!buffer || numBytes <= 0) {
            if (numBytesWritten) *numBytesWritten = 0;
            return kResultOk;
        }
        if (pos_ + size_t(numBytes) > bytes_.size()) bytes_.resize(pos_ + size_t(numBytes));
        std::memcpy(bytes_.data() + pos_, buffer, size_t(numBytes));
        pos_ += size_t(numBytes);
        if (numBytesWritten) *numBytesWritten = numBytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
        int64 target = pos;
        if (mode == kIBSeekCur) target = int64(pos_) + pos;
        else if (mode == kIBSeekEnd) target = int64(bytes_.size()) + pos;
        if (target < 0) return kResultFalse;
        pos_ = size_t(target);
        if (result) *result = target;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* pos) override {
        if (pos) *pos = int64(pos_);
        return kResultOk;
    }

    const std::vector<char>& bytes() const { return bytes_; }

private:
    std::vector<char> bytes_;
    size_t pos_ = 0;
};

// Lista de eventos de entrada: e por aqui que uma nota chega.
class EventList final : public Vst::IEventList {
public:
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override {
        if (obj) *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    int32 PLUGIN_API getEventCount() override { return int32(events_.size()); }
    tresult PLUGIN_API getEvent(int32 index, Vst::Event& e) override {
        if (index < 0 || size_t(index) >= events_.size()) return kInvalidArgument;
        e = events_[size_t(index)];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Vst::Event& e) override {
        events_.push_back(e);
        return kResultOk;
    }

    void clear() { events_.clear(); }
    Vst::Event& add() {
        events_.emplace_back();
        return events_.back();
    }

private:
    std::vector<Vst::Event> events_;
};

// ------------------------------------------------------------------ plugin
struct Plugin {
    void* lib = nullptr;
    IPluginFactory* factory = nullptr;
    FUnknown* obj = nullptr;
    Vst::IComponent* component = nullptr;
    Vst::IAudioProcessor* processor = nullptr;
    Vst::IEditController* controller = nullptr;
    bool initialized = false;

    ~Plugin() { unload(); }

    // O host cria o objeto pela fabrica e pergunta pelos tres papeis no MESMO
    // FUnknown: e isso que caracteriza o componente combinado. Um plugin que
    // devolvesse so IComponent compilaria igual e nao abriria em DAW nenhum.
    bool instantiate(IPluginFactory* f, const TUID classId) {
        factory = f;
        // TUID e char[16] e createInstance pede FIDString (const char*): a
        // conversao e implicita na decadencia do array, sem FUID no meio.
        TUID uid{};
        std::memcpy(uid, classId, sizeof(TUID));
        if (f->createInstance(uid, Vst::IComponent::iid, (void**)&obj) != kResultOk || !obj)
            return false;
        if (obj->queryInterface(Vst::IComponent::iid, (void**)&component) != kResultOk ||
            obj->queryInterface(Vst::IAudioProcessor::iid, (void**)&processor) != kResultOk ||
            obj->queryInterface(Vst::IEditController::iid, (void**)&controller) != kResultOk)
            return false;
        // O contexto pode ser nulo: o plugin so guarda o ponteiro do
        // IComponentHandler, que vem depois por setComponentHandler.
        if (component->initialize(nullptr) != kResultOk) return false;
        initialized = true;
        // Num componente combinado NAO existe classe de controlador: o host
        // checa isso para saber que processor e controller sao o mesmo objeto.
        TUID cid{};
        if (component->getControllerClassId(cid) != kNotImplemented) return false;
        return true;
    }

    void run() {
        if (!initialized) return;
        component->setActive(false);
        component->terminate();
        initialized = false;
    }

    void unload() {
        run();
        if (controller) controller->release(), controller = nullptr;
        if (processor) processor->release(), processor = nullptr;
        if (component) component->release(), component = nullptr;
        obj = nullptr;
        if (lib) {
            if (factory) factory->release();
            factory = nullptr;
            dlclose(lib);
            lib = nullptr;
        }
    }
};

// ------------------------------------------------------------------ render
struct Block {
    std::vector<float> l, r;
    float peak = 0.0f;
    bool finite = true;
    bool ok = false;
};

Block renderBlock(Plugin& p, EventList* events, int frames) {
    Block b;
    b.l.assign(size_t(frames), 0.0f);
    b.r.assign(size_t(frames), 0.0f);
    float* chans[2] = { b.l.data(), b.r.data() };

    Vst::AudioBusBuffers out{};
    out.numChannels = 2;
    out.silenceFlags = 0;
    out.channelBuffers32 = chans;

    Vst::ProcessData data{};
    data.symbolicSampleSize = Vst::kSample32;
    data.numSamples = int32(frames);
    data.numOutputs = 1;
    data.outputs = &out;
    data.inputEvents = events;

    const tresult rt = p.processor->process(data);
    b.ok = rt == kResultOk;
    for (int i = 0; i < frames; ++i) {
        const float a = b.l[size_t(i)], c = b.r[size_t(i)];
        if (!std::isfinite(a) || !std::isfinite(c)) b.finite = false;
        b.peak = std::max({b.peak, std::fabs(a), std::fabs(c)});
    }
    return b;
}

// Estado no formato do shell: magic, versao, 17 floats, banco, preset, caminho.
std::vector<char> buildState(const char* bankPath, float volume) {
    std::vector<char> out;
    auto put = [&out](const void* src, size_t n) {
        out.insert(out.end(), static_cast<const char*>(src),
                   static_cast<const char*>(src) + n);
    };
    const int magic = kStateMagic, version = kStateVersion;
    put(&magic, sizeof(magic));
    put(&version, sizeof(version));
    for (int i = 0; i < pianoforge::Param_Count; ++i) {
        const float v = i == pianoforge::Param_Volume ? volume : pianoforge::kParameterDefs[i].defaultValue;
        put(&v, sizeof(v));
    }
    const int bank = -1;   // o caminho e que manda; -1 = "nao vem do registro"
    const int preset = 0;
    put(&bank, sizeof(bank));
    put(&preset, sizeof(preset));
    const int len = bankPath ? int(std::strlen(bankPath)) : 0;
    put(&len, sizeof(len));
    if (len > 0) put(bankPath, size_t(len));
    return out;
}

// ------------------------------------------------------------------ bundle
// O alvo do probe pode ser a pasta .vst3 (o que o usuario instala) ou o binario
// dentro dela. Achar o binario e o que prova que o layout do bundle esta certo.
std::string resolveModule(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return path;
    // Contents/<arch>-<so>/<binario>: Resources nao tem binario, e o diretorio de
    // arquitetura e justamente o que o probe confere existir.
    const fs::path contents = fs::path(path) / "Contents";
    if (!fs::is_directory(contents, ec)) return {};
    for (const auto& arch : fs::directory_iterator(contents, ec)) {
        if (!arch.is_directory()) continue;
        if (arch.path().filename() == "Resources") continue;
        for (const auto& f : fs::directory_iterator(arch.path(), ec)) {
            if (f.is_regular_file()) return f.path().string();
        }
    }
    return {};
}

// ------------------------------------------------------------------ A/B/C/D/E
bool testFactory(void* lib, IPluginFactory** factoryOut, TUID classId) {
    auto getFactory = reinterpret_cast<IPluginFactory* (*)()>(dlsym(lib, "GetPluginFactory"));
    if (!expect(getFactory != nullptr, "A: GetPluginFactory exportada")) return false;

    // No Windows o host chama InitDll antes de tudo; o SDK tolera nao chamar,
    // mas chamar e o que um DAW faz e o que faz o caminho do modulo ser testado.
    using InitFn = bool (*)();
    if (auto init = reinterpret_cast<InitFn>(dlsym(lib, "InitDll"))) expect(init(), "A: InitDll");

    IPluginFactory* f = getFactory();
    if (!expect(f != nullptr, "A: fabrica criada")) return false;
    *factoryOut = f;

    PFactoryInfo info{};
    expect(f->getFactoryInfo(&info) == kResultOk, "A: getFactoryInfo");

    const int count = f->countClasses();
    if (!expect(count == 1, "A: uma classe registrada (componente combinado)")) return false;

    PClassInfo cls{};
    if (!expect(f->getClassInfo(0, &cls) == kResultOk, "A: getClassInfo(0)")) return false;
    note("classe: %s | categoria: %s", cls.name, cls.category);
    expect(std::strcmp(cls.category, kVstAudioEffectClass) == 0, "A: categoria kVstAudioEffectClass");
    expect(std::strcmp(cls.name, "PianoForge") == 0, "A: nome PianoForge");
    expect(cls.cardinality == PClassInfo::kManyInstances, "A: kManyInstances (aceita 2a instancia)");
    std::memcpy(classId, cls.cid, sizeof(TUID));
    return true;
}

bool testParameters(Plugin& p) {
    expect(p.component->getBusCount(Vst::kAudio, Vst::kOutput) == 1, "C: uma saida de audio");
    expect(p.component->getBusCount(Vst::kAudio, Vst::kInput) == 0, "C: nenhuma entrada de audio");
    expect(p.component->getBusCount(Vst::kEvent, Vst::kInput) == 1, "C: uma entrada de eventos");

    Vst::BusInfo bus{};
    if (p.component->getBusInfo(Vst::kAudio, Vst::kOutput, 0, bus) == kResultOk) {
        note("saida: \"%s\", %d canal(is)", utf16To8(bus.name, 128).c_str(), bus.channelCount);
        expect(bus.channelCount == 2, "C: saida estereo");
        expect(bus.flags & Vst::BusInfo::kDefaultActive, "C: saida ativa por default");
    } else {
        expect(false, "C: getBusInfo da saida");
    }

    const int32 n = p.controller->getParameterCount();
    if (!expect(n == pianoforge::Param_Count + 2, "C: 17 parametros do motor + banco + preset"))
        note("getParameterCount devolveu %d", int(n));

    bool titlesOk = true;
    bool bankFound = false, presetFound = false;
    for (int32 i = 0; i < std::min<int32>(n, 20); ++i) {
        Vst::ParameterInfo pi{};
        if (p.controller->getParameterInfo(i, pi) != kResultOk || pi.title[0] == 0) {
            titlesOk = false;
            break;
        }
        if (i < 3 || i >= pianoforge::Param_Count) {
            note("param %u: \"%s\" (%s) passo %d default %.2f flag 0x%x", pi.id,
                 utf16To8(pi.title, 128).c_str(), utf16To8(pi.shortTitle, 128).c_str(),
                 int(pi.stepCount), double(pi.defaultNormalizedValue), int(pi.flags));
        }
        bankFound = bankFound || pi.id == kBankParamId;
        presetFound = presetFound || pi.id == kPresetParamId;
        // Os 17 do motor sao automatizaveis; banco/preset existem mas escondidos
        // do painel generico do host (quem escolhe piano e o painel).
        if (i < pianoforge::Param_Count && !(pi.flags & Vst::ParameterInfo::kCanAutomate))
            titlesOk = false;
    }
    expect(titlesOk, "C: todo parametro tem titulo e os do motor sao automatizaveis");
    // Ids fixos: o estado salvo do usuario depende deles. Um dia que mudem,
    // sessao antiga abre com o banco errado -- e por isso sao conferidos aqui.
    expect(bankFound && presetFound, "C: parametros 100/101 (banco/preset) presentes");
    return titlesOk;
}

bool testState(Plugin& p, IPluginFactory* factory, const TUID classId, const char* bankPath) {
    // D: ida e volta. O valor escolhido nao e o default: um getState que
    // escrevesse sempre o default passaria com qualquer numero.
    const double want = 0.31;
    p.controller->setParamNormalized(pianoforge::Param_Volume, want);

    MemStream save;
    if (!expect(p.component->getState(&save) == kResultOk, "D: getState")) return false;
    if (!expect(save.bytes().size() > size_t(2 * sizeof(int) + 17 * sizeof(float)),
                "D: estado com cabecalho + 17 floats + banco/preset"))
        return false;

    int magic = 0;
    std::memcpy(&magic, save.bytes().data(), sizeof(magic));
    expect(magic == kStateMagic, "D: magic no inicio do estado");

    // Uma segunda instancia e o que um DAW faz ao abrir a sessao: o estado salvo
    // e entregue a um objeto novo, nao ao mesmo.
    Plugin second;
    if (!expect(second.instantiate(factory, classId), "D: segunda instancia")) return false;

    MemStream load(save.bytes());
    if (!expect(second.component->setState(&load) == kResultOk, "D: setState devolve ok"))
        return false;
    const double got = second.controller->getParamNormalized(pianoforge::Param_Volume);
    note("volume: pedido %.3f, restaurado %.3f", want, got);
    expect(std::fabs(got - want) < 0.01, "D: parametro restaurado pela segunda instancia");

    // Estado de outro plugin (magic errado) tem de ser RECUSADO: ler 17 floats
    // de um layout desconhecido e entregar como som e pior que nao restaurar.
    std::vector<char> bad = save.bytes();
    bad[0] = char(0x7f);
    MemStream badStream(bad);
    expect(second.component->setState(&badStream) != kResultOk, "D: estado com magic errado recusado");

    // Estado truncado (meio arquivo, como um save corrompido) tambem.
    std::vector<char> cut(save.bytes().begin(), save.bytes().begin() + 12);
    MemStream cutStream(cut);
    expect(second.component->setState(&cutStream) != kResultOk, "D: estado truncado recusado");

    second.run();
    (void)bankPath;
    return true;
}

bool testAudio(Plugin& p, const char* bankPath) {
    Vst::ProcessSetup setup{};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = 128;
    setup.sampleRate = 44100.0;
    if (!expect(p.processor->setupProcessing(setup) == kResultOk, "E: setupProcessing")) return false;

    EventList events;

    // Sem banco o plugin tem de ficar MUDO, e nao soltar ruido: o motor comeca
    // sem piano nenhum e nada carrega sem pedido.
    p.component->setActive(true);
    p.processor->setProcessing(true);
    Block silent = renderBlock(p, &events, 128);
    note("sem banco: pico %.6f", double(silent.peak));
    expect(silent.ok, "E: process devolve kResultOk");
    expect(silent.peak == 0.0f, "E: silencio absoluto antes de escolher um piano");
    p.processor->setProcessing(false);
    p.component->setActive(false);

    // Escolher o piano pelo ESTADO: e o caminho do DAW ao abrir a sessao, e o
    // unico que nao depende do registro global de soundfonts da maquina.
    MemStream state(buildState(bankPath, 0.85f));
    if (!expect(p.component->setState(&state) == kResultOk, "E: setState com o caminho do banco"))
        return false;
    note("banco pedido: %s", bankPath);

    p.component->setActive(true);
    p.processor->setProcessing(true);

    // A carga roda na thread do plugin: os primeiros blocos sao mudos de
    // proposito (RAM antes de I/O), e a nota mandada antes de o banco existir e
    // descartada. Por isso a nota e reenviada a cada 250 ms: um host de verdade so
    // toca depois de o usuario escolher o piano, e aqui quem escolhe e o estado que
    // acabou de chegar.
    auto sendNoteOn = [&events] {
        Vst::Event& on = events.add();
        on.type = Vst::Event::kNoteOnEvent;
        on.sampleOffset = 0;
        on.noteOn.channel = 0;
        on.noteOn.pitch = 60;
        on.noteOn.velocity = 0.8f;
        on.noteOn.noteId = -1;
    };
    sendNoteOn();

    // O teto e de 5 s de tempo de PAREDE, e nao de blocos, para nao medir a
    // velocidade do runner.
    float peak = 0.0f;
    bool finite = true;
    int blocks = 0;
    for (; blocks < 400 && peak < 0.01f; ++blocks) {
        Block b = renderBlock(p, &events, 128);
        peak = std::max(peak, b.peak);
        finite = finite && b.finite;
        events.clear();
        if (blocks % 25 == 0) sendNoteOn();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    note("som depois de %d blocos (~%d ms), pico %.4f", blocks, blocks * 10, double(peak));
    expect(peak > 0.01f, "E: o piano soa depois de o banco carregar");
    expect(finite, "E: saida sem NaN/Inf");

    p.component->setActive(false);
    p.processor->setProcessing(false);
    return peak > 0.01f;
}

// --------------------------------------------------------------- captura
#if defined(_WIN32)
// Blit do filho para um DIB e a faixa do canal verde. Uma janela criada e nunca
// pintada e uniforme (preta); um painel de verdade tem anel, texto e brasa.
struct Captura {
    int lo = 255, hi = 0, linhas = 0;
    bool pintou() const { return linhas > 0 && (hi - lo) > 8; }
};

Captura capturarPainel(HWND child) {
    Captura c;
    RECT cr{};
    GetClientRect(child, &cr);
    const int w = int(cr.right - cr.left), h = int(cr.bottom - cr.top);
    if (w <= 0 || h <= 0) return c;
    std::vector<uint32_t> pix(size_t(w) * size_t(h), 0);
    HDC childDc = GetDC(child);
    HDC memDc = CreateCompatibleDC(childDc);
    HBITMAP bmp = CreateCompatibleBitmap(childDc, w, h);
    HGDIOBJ old = SelectObject(memDc, bmp);
    BitBlt(memDc, 0, 0, w, h, childDc, 0, 0, SRCCOPY);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;      // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    c.linhas = GetDIBits(memDc, bmp, 0, UINT(h), pix.data(), &bi, DIB_RGB_COLORS);

    SelectObject(memDc, old);
    DeleteObject(bmp);
    DeleteDC(memDc);
    ReleaseDC(child, childDc);

    if (c.linhas <= 0) return c;
    for (uint32_t px : pix) {
        const int v = int((px >> 8) & 0xff);   // canal verde
        c.lo = std::min(c.lo, v);
        c.hi = std::max(c.hi, v);
    }
    return c;
}

// O primeiro filho do container e o painel: o backend cria uma janela so.
HWND painelDentroDe(HWND host) {
    HWND child = nullptr;
    EnumChildWindows(host, [](HWND h, LPARAM p) -> BOOL {
        *reinterpret_cast<HWND*>(p) = h;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&child));
    return child;
}

// Bombeia a fila do host de verdade: os timers do painel so disparam aqui.
void bombear(int ms) {
    for (int i = 0; i < ms / 10; ++i) {
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
#endif

// ------------------------------------------------------------------ F: view
bool testView(Plugin& p) {
    IPlugView* view = p.controller->createView(Vst::ViewType::kEditor);
    if (!expect(view != nullptr, "F: createView(kEditor)")) return false;

    const char* wanted =
#if defined(_WIN32)
        kPlatformTypeHWND;
#elif defined(__APPLE__)
        kPlatformTypeNSView;
#else
        kPlatformTypeX11EmbedWindowID;
#endif
    expect(view->isPlatformTypeSupported(wanted) == kResultTrue,
           "F: aceita o tipo de plataforma do sistema");
    expect(view->isPlatformTypeSupported("NaoExiste") == kResultFalse,
           "F: recusa um tipo de plataforma inventado");

    ViewRect rect{};
    if (expect(view->getSize(&rect) == kResultOk, "F: getSize")) {
        note("tamanho do painel: %dx%d", rect.right - rect.left, rect.bottom - rect.top);
        expect(rect.right - rect.left > 200 && rect.bottom - rect.top > 200,
               "F: tamanho plausivel para o desenho");
    }

#if defined(_WIN32)
    // O pai de teste: uma janela comum. O backend cria o painel como FILHO dela,
    // que e exatamente o que o DAW entrega em attached(). WS_VISIBLE importa:
    // janela escondida nao recebe WM_PAINT, e a captura devolveria so preto --
    // um falso negativo que parece "o painel nao desenha".
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "PianoForgeProbeHost";
    RegisterClassA(&wc);
    HWND host = CreateWindowExA(0, wc.lpszClassName, "probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1100, 640, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!expect(host != nullptr, "F: janela de teste do host")) {
        view->release();
        return false;
    }

    expect(view->attached(host, kPlatformTypeHWND) == kResultOk, "F: attached no HWND do host");
    UpdateWindow(host);

    // Deixa o painel pintar: WM_PAINT chega pelo bombeamento da fila.
    bombear(400);

    // A prova visual: o filho existe e os pixels nao sao uniformes. Uma janela
    // criada e nunca pintada e preta/uniforme -- e essa e a falha que aconteceu
    // de verdade no backend Win32 antes da correcao de thread.
    HWND child = painelDentroDe(host);
    expect(child != nullptr, "F: painel criado dentro da janela do host");

    if (child) {
        char cls[64]{};
        GetClassNameA(child, cls, sizeof(cls));
        RECT cr{};
        GetClientRect(child, &cr);
        note("painel: classe \"%s\", %ldx%ld", cls, long(cr.right - cr.left),
             long(cr.bottom - cr.top));
        expect(cr.right - cr.left > 200 && cr.bottom - cr.top > 200, "F: painel com tamanho real");

        const Captura cap = capturarPainel(child);
        note("pixels: canal verde de %d a %d (%d linha(s) lidas)", cap.lo, cap.hi, cap.linhas);
        expect(cap.pintou(), "F: o painel pintou (pixels nao uniformes)");
    }

    view->removed();
    DestroyWindow(host);
    view->release();
    return true;
#else
    note("painel: attached/pixels so no Windows neste probe (X11 pediria Xvfb)");
    view->release();
    return true;
#endif
}

// -------------------------------------------------- G: container apertado
// O REAPER 7.83 abre o editor num placeholder de 65x86 e so depois cresce o
// container. Um painel que adota o tamanho visto no attached() e para fica
// espremido num canto para sempre -- foi o que apareceu como "o plugin abriu
// minusculo". Aqui o host se comporta do mesmo jeito: container pequeno,
// attached, e o container cresce 400 ms depois.
bool testContainerApertado(Plugin& p) {
#if !defined(_WIN32)
    note("G: container apertado so no Win32 (no X11 o host redimensiona o filho");
    return true;
#else
    IPlugView* view = p.controller->createView(Vst::ViewType::kEditor);
    if (!expect(view != nullptr, "G: createView(kEditor) para o container apertado")) return false;

    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "PianoForgeProbeSqueeze";
    RegisterClassA(&wc);
    // WS_POPUP para o cliente ter exatamente 65x86, sem moldura: e a medida do
    // ui:parent que o REAPER entrega no Windows.
    HWND host = CreateWindowExA(0, wc.lpszClassName, "apertado", WS_POPUP, 0, 0, 65, 86,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!expect(host != nullptr, "G: container de 65x86")) {
        view->release();
        return false;
    }
    ShowWindow(host, SW_SHOWNA);

    expect(view->attached(host, kPlatformTypeHWND) == kResultOk, "G: attached no container pequeno");
    bombear(400);                    // dois tiques do relogio lento (200 ms)

    HWND child = painelDentroDe(host);
    if (!expect(child != nullptr, "G: painel criado dentro do container apertado")) {
        view->removed();
        DestroyWindow(host);
        view->release();
        return false;
    }
    RECT antes{};
    GetClientRect(child, &antes);
    note("G: painel num container de 65x86: %ldx%ld", long(antes.right - antes.left),
         long(antes.bottom - antes.top));

    // O host cresce o container. Para o painel e o mesmo que um WM_SIZE do
    // container do DAW: o filho NAO acompanha sozinho, e o conserto e o relogio
    // lento conferir o cliente do pai.
    SetWindowPos(host, nullptr, 0, 0, 1000, 520, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    bombear(600);                    // tres tiques

    RECT depois{};
    GetClientRect(child, &depois);
    const long w = depois.right - depois.left, h = depois.bottom - depois.top;
    note("G: depois de o host crescer para 1000x520, o painel ficou %ldx%ld", w, h);
    expect(w >= 900 && h >= 460, "G: o painel acompanhou o container que cresceu depois");

    const Captura cap = capturarPainel(child);
    note("G: pixels do painel grande: canal verde de %d a %d", cap.lo, cap.hi);
    expect(cap.pintou(), "G: o painel repintou no tamanho novo");

    view->removed();
    DestroyWindow(host);
    view->release();
    return true;
#endif
}

// --------------------------------------------------------- H: a pagina MOTOR
// Os sete parametros do motor deixaram de ser chip de leitura e viraram knob. O
// que este teste prova e o caminho INTEIRO: o clique na aba chega ao painel, o
// arrasto num disco do MOTOR sai pela porta 3+ParameterId e o HOST ve o valor
// novo. Sem ele, "agora da para editar" seria so desenho -- e desenho ja abriu
// sem fonte uma vez neste projeto.
bool testPaginaMotor(Plugin& p) {
#if !defined(_WIN32)
    note("H: clique e arrasto so no Win32 neste probe (o X11 pediria Xvfb)");
    return true;
#else
    IPlugView* view = p.controller->createView(Vst::ViewType::kEditor);
    if (!expect(view != nullptr, "H: createView(kEditor) para a pagina MOTOR")) return false;

    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "PianoForgeProbeMotor";
    RegisterClassA(&wc);
    // Cliente de exatamente 1000x520, o tamanho de desenho: as coordenadas abaixo
    // sao as do layout real e so valem neste tamanho.
    HWND host = CreateWindowExA(0, wc.lpszClassName, "motor", WS_POPUP, 0, 0, 1000, 520,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!expect(host != nullptr, "H: container de 1000x520")) {
        view->release();
        return false;
    }
    ShowWindow(host, SW_SHOWNA);
    expect(view->attached(host, kPlatformTypeHWND) == kResultOk, "H: attached no container");
    bombear(400);

    HWND child = painelDentroDe(host);
    if (!expect(child != nullptr, "H: painel criado")) {
        view->removed();
        DestroyWindow(host);
        view->release();
        return false;
    }
    RECT cr{};
    GetClientRect(child, &cr);
    if (!expect(cr.right - cr.left == 1000 && cr.bottom - cr.top == 520,
                "H: painel em 1000x520 (sem isso as coordenadas abaixo mentem)")) {
        note("H: painel veio %ldx%ld", long(cr.right - cr.left), long(cr.bottom - cr.top));
        view->removed();
        DestroyWindow(host);
        view->release();
        return false;
    }

    using Steinberg::Vst::ParamID;
    constexpr int kMotor = pianoforge::Param_ReverbType;   // o primeiro do MOTOR
    double antes[7] = {};
    for (int i = 0; i < 7; ++i) antes[i] = p.controller->getParamNormalized(ParamID(kMotor + i));
    double antesTimbre[10] = {};
    for (int i = 0; i < 10; ++i) antesTimbre[i] = p.controller->getParamNormalized(ParamID(i));

    // As abas: o cabecalho do painel e uma faixa de 38 px a partir de (253,8); as
    // duas abas de 62x18 comecam em 280 e 346. O centro da MOTOR e (377,27) --
    // numero lido do PNG de `pforge_ui_preview --page 1`, nao adivinhado.
    SendMessageA(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(377, 27));
    SendMessageA(child, WM_LBUTTONUP, 0, MAKELPARAM(377, 27));
    bombear(200);

    // O primeiro disco do MOTOR (Sala): centro em (355,160), tambem medido do
    // mesmo PNG. Arrastar 60 px para cima anda 1/3 do curso de um knob.
    SendMessageA(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(355, 160));
    for (int i = 1; i <= 6; ++i) {
        SendMessageA(child, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(355, 160 - i * 10));
        bombear(20);
    }
    SendMessageA(child, WM_LBUTTONUP, 0, MAKELPARAM(355, 100));
    bombear(200);

    int movedMotor = 0, movedTimbre = 0;
    for (int i = 0; i < 7; ++i)
        if (p.controller->getParamNormalized(ParamID(kMotor + i)) != antes[i]) ++movedMotor;
    for (int i = 0; i < 10; ++i)
        if (p.controller->getParamNormalized(ParamID(i)) != antesTimbre[i]) ++movedTimbre;
    note("H: %d parametro(s) do motor mudaram, %d do timbre", movedMotor, movedTimbre);
    expect(movedMotor == 1, "H: o arrasto num disco do MOTOR chega ao host");
    expect(movedTimbre == 0, "H: o arrasto do MOTOR nao mexe em disco do TIMBRE");

    const Captura cap = capturarPainel(child);
    expect(cap.pintou(), "H: o painel do motor tambem pinta");

    view->removed();
    DestroyWindow(host);
    view->release();
    return true;
#endif
}

// ------------------------------------------------------------------ main

} // namespace

int main(int argc, char** argv) {
    std::string target;
    std::string work = ".";
    bool wantUi = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--work" && i + 1 < argc) work = argv[++i];
        else if (a == "--ui") wantUi = true;
        else target = a;
    }
    if (target.empty()) {
        std::fprintf(stderr,
                     "uso: pforge_vst3_probe <PianoForge.vst3|pasta> [--work <dir>] [--ui]\n");
        return 2;
    }

    const std::string module = resolveModule(target);
    if (module.empty()) {
        std::fprintf(stderr, "nao achei o binario dentro de %s\n", target.c_str());
        return 2;
    }
    std::printf("modulo: %s\n", module.c_str());

    Plugin plugin;
    plugin.lib = dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!plugin.lib) {
        std::fprintf(stderr, "dlopen falhou: %s\n", dlerror() ? dlerror() : "?");
        return 2;
    }
    note("carregado");

    TUID classId{};
    if (!testFactory(plugin.lib, &plugin.factory, classId)) return 1;
    if (!expect(plugin.instantiate(plugin.factory, classId), "B: instanciado e inicializado")) {
        return 1;
    }
    note("IComponent + IAudioProcessor + IEditController no mesmo objeto");

    testParameters(plugin);

    // O banco sintetico: o CI nao pode baixar soundfont, e as amostras aqui sao
    // geradas na hora, dominio publico por construcao.
    //
    // O --work e criado aqui: os passos do CI apontam para pastas novas (vst3probe,
    // vst3probe_ui) que ainda nao existem, e gravar o SF2 dentro delas falhava com
    // "nao abriu ... para escrita" antes de qualquer teste rodar.
    {
        std::error_code ec;
        std::filesystem::create_directories(work, ec);
        if (ec && !std::filesystem::is_directory(work)) {
            std::fprintf(stderr, "nao consegui criar a pasta de trabalho %s: %s\n",
                         work.c_str(), ec.message().c_str());
            return 2;
        }
    }
    const std::string sf2 = (std::filesystem::path(work) / "probe_piano.sf2").string();
    std::string err;
    if (!psynth::buildIndexed(sf2, 0, err)) {
        std::fprintf(stderr, "nao consegui gerar o SF2 sintetico: %s\n", err.c_str());
        return 2;
    }
    note("SF2 sintetico: %s", sf2.c_str());

    testState(plugin, plugin.factory, classId, sf2.c_str());
    testAudio(plugin, sf2.c_str());
    if (wantUi) {
        testView(plugin);
        testContainerApertado(plugin);
        testPaginaMotor(plugin);
    }

    plugin.unload();

    std::printf("\n%s (%d falha(s))\n", g_failures == 0 ? "TUDO OK" : "FALHOU", g_failures);
    return g_failures == 0 ? 0 : 1;
}
