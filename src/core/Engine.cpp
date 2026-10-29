#include "Engine.hpp"
#include "Bundle.hpp"

#ifdef PF_HAVE_SFIZZ
#include "sfizz.hpp"
#endif

#define TSF_IMPLEMENTATION
#include "tsf.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <iostream>

// O sfizz por tras do handle opaco que o Bank declara.
//
// Com PF_HAVE_SFIZZ o SfzHandle E o sfizz: heranca simples, sem wrapper, e os
// ~10 pontos de chamada deste arquivo ficam identicos nos dois modos.
//
// Sem ele o tipo vira um casulo vazio com as mesmas assinaturas. Isso nao e uma
// segunda implementação do SFZ e nada aqui roda: o unico lugar onde uma instancia
// nasce e loadSfzInto(), que na build sem sfizz avisa no stderr e devolve false,
// entao Bank::sfz fica nullptr e toda a branca `else if (b->sfz)` do RT e morta.
// O casulo existe só para o codigo compilar sem o header da dependencia.
#ifdef PF_HAVE_SFIZZ
struct SfzHandle : sfz::Sfizz {};
#else
struct SfzHandle {
    void setSampleRate(float) {}
    void setSamplesPerBlock(int) {}
    bool loadSfzFile(const std::string&) { return false; }
    void noteOn(int, int, int) {}
    void noteOff(int, int, int) {}
    void cc(int, int, int) {}
    void pitchWheel(int, int) {}
    void allSoundOff() {}
    void renderBlock(float**, size_t, int) {}
};
#endif

namespace pianoforge {

namespace {

// Teto de vozes: 256, e nao 128, por medida e nao por gosto.
//
// Uma corda deste piano soa 25-28 s (-40 dB) no grave e 4,8 s em A4 com o pedal no
// ar; no agudo extremo sao 0,4 s (`pforge_dsp_stages --ring`). Cada tecla acende ~3
// regioes. `--pedalload` (60 s de passagem com CC64=127 o tempo todo, 357 notas)
// pede ~1086 vozes para nenhuma corda ser abafada antes da hora: nenhum teto
// razoavel chega la.
//
// Desde que o abafamento progressivo foi ligado (tsf.h) NENHUM teto descarta nota
// em silencio -- `--pedalload` conta pfMuteNotes/pfDroppedRegions no proprio
// backend e da zero em 128, 192, 256 e 384. O que o teto compra e o tempo que uma
// corda pedalizada fica no ar antes de o abafador cair, medido na mesma passagem:
//
//   teto   us/bloco (piscina cheia)   idade media ao abafar
//   128          313                          3,05 s
//   192          465                          5,20 s
//   256          623                          7,62 s
//   384          956                         13,86 s
//
// 256 e o ponto em que todo o medio e agudo ja passou dos -40 dB quando o abafador
// cai (A4 chega la em 4,8 s; abaixo disso a corda some dentro da textura) e apenas o
// grave ainda seria ouvido. Os 623 us sao por bloco de 512 com a piscina LOTADA, e
// um bloco de 512 leva 10,7 ms para ser tocado em 48 kHz (11,6 ms em 44,1) -- em
// bloco menor o trabalho e proporcional e a fracao nao muda. Na suíte, G assere o
// pior bloco sintetico abaixo de 1/5 do periodo (2,3 ms) e mede 1,00-1,10 ms no
// minimo deterministico; B/D/I medem p99.9 < 0,2 ms em uso real. RAM de voz e
// estruturinha (~200 B), nao o banco; o banco e o mesmo, o teto nao muda a RSS.
constexpr int kMaxVoices = 256;
constexpr size_t kMinSlice = 16;
// CC e bend NAO sao ataque: sao controle continuo, e o proprio Engine ja quantiza
// o valor deles uma vez por bloco (params.updateBlock). Fronteira de fatia custa
// ~6 us no tsf com o piano cheio de vozes (medido: tools/pforge_dsp_stages), e um
// host automatizando amostra a amostra pagaria isso 32 vezes num bloco de 512.
// Quantizar controle continuo em 64 amostras (1,3 ms) corta isso pela metade do
// caminho sem que nenhum ouvido separe o ganho 1,3 ms adiantado.
constexpr size_t kCcSlice = 64;

bool endsWithCi(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])));
        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
        if (a != b) return false;
    }
    return true;
}

bool isSf2Name(const std::string& s) {
    return endsWithCi(s, ".sf2");
}
bool isSfzName(const std::string& s) {
    // .sf3 e o mesmo caminho: sfizz sabe decodificar ogg, o tsf nao.
    return endsWithCi(s, ".sfz") || endsWithCi(s, ".sf3");
}

// tsf le por stream, entao um SF2 gravado dentro de um .pforge pode ser lido no
// lugar certo do arquivo sem copiar o banco inteiro para a memoria primeiro.
struct Sf2Range {
    std::FILE* f;
    uint64_t left;
};

int rangeRead(void* data, void* ptr, unsigned int size) {
    auto* r = static_cast<Sf2Range*>(data);
    if (uint64_t(size) > r->left) size = unsigned(r->left);
    const size_t got = std::fread(ptr, 1, size, r->f);
    r->left -= got;
    return int(got);
}

int rangeSkip(void* data, unsigned int count) {
    auto* r = static_cast<Sf2Range*>(data);
    if (uint64_t(count) > r->left) return 0;
    if (std::fseek(r->f, long(count), SEEK_CUR) != 0) return 0;
    r->left -= count;
    return 1;
}

// Tudo que o RT faria sob demanda no primeiro acorde: alocar canais, alocar
// vozes, escolher preset. Roda no worker, uma vez, antes de publicar.
bool setupSf2(tsf* s, float rate) {
    if (!s) return false;
    tsf_set_output(s, TSF_STEREO_INTERLEAVED, static_cast<int>(rate), 0.0f);
    if (!tsf_set_max_voices(s, kMaxVoices)) return false;
    if (!tsf_channel_set_presetindex(s, 0, 0)) return false;
    // Aquecimento: tsf cria o array de canais no primeiro acesso a um canal
    // (tsf_channel_init, TSF_MALLOC) e uma nota com vel 0 vira note-off e nao
    // aloca voz. Sem isto, o primeiro acorde depois do swap alocaria na thread
    // de audio.
    tsf_channel_note_on(s, 0, 60, 0.001f);
    tsf_channel_note_off(s, 0, 60);
    tsf_channel_sounds_off_all(s, 0);
    return true;
}

#ifdef PF_HAVE_SFIZZ
// SFZ dentro de bundle: o sfizz so abre por caminho, entao o conteudo do bundle
// vai para o cache. Nome do arquivo + hash do tamanho, para dois bundles com o
// mesmo mes nao se escreverem por cima.
std::string bundleCacheDir(const Bundle& b) {
    // Mesma regra do registro de soundfonts: num host grafico de Windows nao ha
    // HOME nem XDG, e o cache do SFZ extraido iria para o diretorio de trabalho do
    // host. %LOCALAPPDATA% e o par do %APPDATA% para o que e regeneravel.
#if defined(_WIN32)
    const char* loc = std::getenv("LOCALAPPDATA");
    std::filesystem::path root = (loc && *loc)
        ? std::filesystem::path(loc)
        : std::filesystem::path(std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : ".") /
              "AppData" / "Local";
#else
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path root = (xdg && *xdg)
        ? std::filesystem::path(xdg)
        : std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".cache";
#endif
    std::error_code ec;
    auto canon = std::filesystem::canonical(b.path(), ec);
    const std::string tag = canon.empty() ? std::filesystem::path(b.path()).stem().string()
                                          : canon.filename().stem().string();
    return (root / "pianoforge" / "bundles" / tag).string();
}

bool setupSfz(SfzHandle& s, const std::string& file, float rate, size_t block) {
    s.setSampleRate(rate);
    s.setSamplesPerBlock(static_cast<int>(block));
    return s.loadSfzFile(file);
}
#endif

// Unica porta de entrada do backend SFZ: o arquivo solto e o SFZ dentro de um
// bundle entram pelos dois lados daqui, e tudo que existe so porque o sfizz existe
// mora dentro do recorte. Assim o Bank::load nao tem #ifdef no meio, e a build sem
// a dependencia recusa com uma frase no stderr antes de qualquer I/O em vez de um
// generico "formato nao suportado".
bool loadSfzInto(Bank& bank, const std::string& name, const Bundle* fromBundle,
                 float rate, size_t block) {
#ifdef PF_HAVE_SFIZZ
    std::string file = name;
    if (fromBundle) {
        const std::string dir = bundleCacheDir(*fromBundle);
        std::string err;
        if (!fromBundle->unpackAll(dir, err)) {
            std::cerr << "[PianoForge] falha ao desempacotar " << fromBundle->path()
                      << ": " << err << "\n";
            return false;
        }
        file = (std::filesystem::path(dir) / name).string();
    }
    auto inst = std::make_unique<SfzHandle>();
    if (!setupSfz(*inst, file, rate, block)) {
        std::cerr << "[PianoForge] sfizz recusou " << file << "\n";
        return false;
    }
    bank.sfz = std::move(inst);
    bank.backend = SamplerBackend::SfizzSFZ;
    bank.presetCount = 1;   // SFZ nao tem biblioteca de presets
    return true;
#else
    (void)bank;
    (void)fromBundle;
    (void)rate;
    (void)block;
    std::cerr << "[PianoForge] build sem sfizz: " << name
              << " (SFZ/SF3) nao abre. SF2 e .pforge com SF2 dentro funcionam.\n";
    return false;
#endif
}

} // namespace

// ---------------------------------------------------------------- Bank ---

Bank::~Bank() {
    if (sf2) {
        tsf_close(sf2);
        sf2 = nullptr;
    }
}

bool Bank::load(const std::string& bankPath, float rate, size_t block) {
    path = bankPath;

    // ------------------------------------------------ bundle .pforge ---------
    if (endsWithCi(bankPath, ".pforge")) {
        Bundle bundle;
        std::string err;
        if (!bundle.open(bankPath, err)) {
            std::cerr << "[PianoForge] bundle invalido: " << err << "\n";
            return false;
        }
        const Bundle::Entry* e = bundle.bankEntry();
        if (!e) {
            std::cerr << "[PianoForge] bundle sem banco (nem manifest.sf2, nem .sf2/.sfz): "
                      << bankPath << "\n";
            return false;
        }

        if (isSf2Name(e->name)) {
            if (e->type == Bundle::kRaw) {
                if (!bundle.verifyEntry(*e, err)) {
                    std::cerr << "[PianoForge] " << err << "\n";
                    return false;
                }
                std::FILE* f = bundle.openEntry(*e);
                if (!f) {
                    std::cerr << "[PianoForge] nao abriu de novo " << bankPath << "\n";
                    return false;
                }
                Sf2Range range{f, e->size};
                struct tsf_stream st = { &range, &rangeRead, &rangeSkip };
                sf2 = tsf_load(&st);
                std::fclose(f);
            } else {
                std::vector<uint8_t> bytes;
                if (!bundle.readEntry(*e, bytes, err)) {
                    std::cerr << "[PianoForge] " << err << "\n";
                    return false;
                }
                if (bytes.size() > size_t(std::numeric_limits<int>::max())) {
                    std::cerr << "[PianoForge] SF2 dentro do bundle grande demais para o tsf\n";
                    return false;
                }
                sf2 = tsf_load_memory(bytes.data(), int(bytes.size()));
            }
            if (!sf2) {
                std::cerr << "[PianoForge] o " << e->name << " dentro de " << bankPath
                          << " nao e um SF2 legivel\n";
                return false;
            }
            backend = SamplerBackend::NativeSF2;
            presetCount = tsf_get_presetcount(sf2);
            if (!setupSf2(sf2, rate)) return false;
            return true;
        }

        if (isSfzName(e->name))
            return loadSfzInto(*this, e->name, &bundle, rate, block);

        std::cerr << "[PianoForge] entry de banco com formato estranho: " << e->name << "\n";
        return false;
    }

    // ------------------------------------------------------------ SF2 --------
    if (isSf2Name(bankPath)) {
        sf2 = tsf_load_filename(bankPath.c_str());
        if (!sf2) return false;
        backend = SamplerBackend::NativeSF2;
        presetCount = tsf_get_presetcount(sf2);
        if (!setupSf2(sf2, rate)) return false;
        return true;
    }

    // ------------------------------------------------------------ SFZ --------
    if (isSfzName(bankPath))
        return loadSfzInto(*this, bankPath, nullptr, rate, block);

    std::cerr << "[PianoForge] Formato nao suportado: " << bankPath << "\n";
    return false;
}

// --------------------------------------------------------------- Engine ---

Engine::Engine() {
    prepare(44100.0f, 256);
}

Engine::~Engine() {
    drainRetireNow();
    if (Bank* b = bank_.exchange(nullptr)) delete b;
    rtBank = nullptr;
}

void Engine::prepare(float newSampleRate, size_t maxBlockSize) {
    sampleRate = newSampleRate > 1000.0f ? newSampleRate : 44100.0f;
    blockSize = maxBlockSize > 0 ? maxBlockSize : 256;

    params.prepare(sampleRate);
    hammerModel.prepare(sampleRate);
    sympResonance.prepare(sampleRate);
    soundboardEQ.prepare(sampleRate);
    lidFilter.prepare(sampleRate);
    chorus.prepare(sampleRate);
    reverb.prepare(sampleRate);

    interleavedBuffer.assign(blockSize * 2, 0.0f);
}

Bank* Engine::buildBank(const std::string& bankPath) noexcept {
    if (bankPath.empty()) return nullptr;
    Bank* b = new Bank();
    if (!b->load(bankPath, sampleRate, blockSize)) {
        delete b;
        return nullptr;
    }
    return b;
}

void Engine::publishBank(Bank* next) noexcept {
    // next == nullptr significa "nenhum piano" (silencio), nao e falha.
    Bank* old = bank_.exchange(next, std::memory_order_seq_cst);

    // Snapshot depois do exchange: qualquer bloco que ainda enxerga 'old'
    // incrementou blockSerial antes deste ponto.
    retireSerial.store(blockSerial.load(std::memory_order_seq_cst), std::memory_order_seq_cst);

    if (old) {
        {
            std::lock_guard<std::mutex> lk(retireMx);
            retireList.push_back(old);
        }
        retireArmed.store(true, std::memory_order_release);
    }
}

void Engine::beginBlock() noexcept {
    const unsigned s = blockSerial.fetch_add(1, std::memory_order_seq_cst);
    rtBank = bank_.load(std::memory_order_seq_cst);

    // Nenhum evento pode sobreviver ao bloco em que foi escrito: se um host
    // enfileirou MIDI e nao renderizou, o atraso dele e relativo aquele bloco.
    pendingCount = 0;

    // O bloco cujo serial e retireSerial foi o primeiro depois do publish. Quando
    // um bloco estritamente posterior comeca, aquele ja terminou e ninguem mais
    // aponta para o banco velho: o worker pode libera-lo.
    if (retireArmed.load(std::memory_order_relaxed) && s > retireSerial.load(std::memory_order_relaxed)) {
        retireArmed.store(false, std::memory_order_relaxed);
        retireReady.store(true, std::memory_order_release);
    }
}

void Engine::freeRetiredBanks() noexcept {
    // A fila e roubada antes de deletar: dois caminhos nao-RT podem chegar aqui
    // no mesmo instante (o work() do host via MSG_FREE_OLD e, no fallback sem
    // worker, a thread de reserva via freeRequested; e o deactivate() do host
    // contra o ultimo tick da reserva). Sem o roubo + mutex os dois deletariam a
    // mesma Bank, e foi isso que apareceu como "corrupted size vs. prev_size".
    std::vector<Bank*> mine;
    {
        std::lock_guard<std::mutex> lk(retireMx);
        mine.swap(retireList);
    }
    for (Bank* b : mine) delete b;
}

void Engine::drainRetireNow() noexcept {
    freeRetiredBanks();
    retireArmed.store(false, std::memory_order_relaxed);
    retireReady.store(false, std::memory_order_relaxed);
}

bool Engine::loadSoundBankSync(const std::string& bankPath) {
    if (bankPath.empty()) return false;
    Bank* next = buildBank(bankPath);
    if (!next) {
        std::cerr << "[PianoForge] Falha ao carregar banco: " << bankPath << "\n";
        return false;
    }
    allNotesOff();
    publishBank(next);
    drainRetireNow();
    rtBank = bank_.load(std::memory_order_seq_cst);
    std::cout << "[PianoForge] Banco carregado: " << bankPath << " ("
              << next->presetCount << " presets)\n";
    return true;
}

bool Engine::loadSoundBank(const std::string& path) { return loadSoundBankSync(path); }
bool Engine::loadSf2(const std::string& p) { return loadSoundBankSync(p); }
bool Engine::loadSfz(const std::string& p) { return loadSoundBankSync(p); }

int Engine::getPresetCount() const noexcept {
    return rtBank ? rtBank->presetCount : 0;
}

std::string Engine::getPresetName(int index) const {
    Bank* b = rtBank;
    if (!b) return "Nenhum piano";
    if (b->backend == SamplerBackend::NativeSF2 && b->sf2) {
        const char* name = tsf_get_presetname(b->sf2, index);
        return name ? std::string(name) : "Default";
    }
    return std::filesystem::path(b->path).stem().string();
}

bool Engine::setPresetIndex(int index) noexcept {
    Bank* b = rtBank;
    if (!b) return false;
    if (b->backend == SamplerBackend::NativeSF2 && b->sf2) {
        if (index < 0 || index >= b->presetCount) return false;
        return tsf_channel_set_presetindex(b->sf2, 0, index) != 0;
    }
    return false;   // SFZ: um unico "preset", nada para trocar
}

void Engine::stage(const PendingEvent& ev) noexcept {
    if (pendingCount >= kMaxPendingEvents) {
        dispatch(ev);   // fila cheia: age ja em vez de perder o evento
        return;
    }
    pending[pendingCount++] = ev;
}

void Engine::dispatch(const PendingEvent& ev) noexcept {
    switch (ev.kind) {
        case PendingEvent::NoteOn:   applyNoteOn(ev.a, ev.b); break;
        case PendingEvent::NoteOff:   applyNoteOff(ev.a, ev.b); break;
        case PendingEvent::Control:   applyCC(ev.a, ev.b); break;
        case PendingEvent::Bend:      applyBend(ev.b); break;
    }
}

void Engine::handleNoteOn(int delayFrames, int note, int rawVelocity) noexcept {
    if (rawVelocity <= 0) {
        handleNoteOff(delayFrames, note, 0);
        return;
    }
    stage({delayFrames, PendingEvent::NoteOn, note, rawVelocity});
}

void Engine::handleNoteOff(int delayFrames, int note, int velocity) noexcept {
    stage({delayFrames, PendingEvent::NoteOff, note, velocity});
}

void Engine::handleCC(int delayFrames, int ccNumber, int ccValue) noexcept {
    stage({delayFrames, PendingEvent::Control, ccNumber, ccValue});
}

void Engine::handlePitchBend(int delayFrames, int pitchValue) noexcept {
    stage({delayFrames, PendingEvent::Bend, 0, pitchValue});
}

void Engine::applyNoteOn(int note, int rawVelocity) noexcept {
    Bank* b = rtBank;
    if (!b) return;   // sem piano escolhido: MIDI algum produz som, nem o martelo

    float curve = params.getValue(Param_VelCurve);
    int curvedVel = dsp::VelocityCurve::process(rawVelocity, curve);

    // O martelo somava demais: 100% do knob passa a valer o que 10% valia. A
    // escala fica AQUI, no encontro do controle com o modelo, e nao dentro do
    // HammerModel: o modelo e o transiente medido (as tabelas de cal_ saem de
    // irEnergy), o controle e gosto -- e gosto muda de numero sem obrigar a
    // recalibrar tabela nenhuma.
    constexpr float kHammerEscala = 0.10f;
    float hammerAmt = params.getValue(Param_Hammer) * kHammerEscala;
    float hammerTone = params.getValue(Param_HammerTone);
    if (hammerAmt > 0.001f) {
        hammerModel.trigger(note, curvedVel, hammerAmt, hammerTone);
    }

    sympResonance.noteOn(note, curvedVel);

    if (b->backend == SamplerBackend::NativeSF2 && b->sf2) {
        tsf_channel_note_on(b->sf2, 0, note, static_cast<float>(curvedVel) / 127.0f);
    } else if (b->sfz) {
        b->sfz->noteOn(0, note, curvedVel);   // o fatiamento ja cuidou do tempo
    }
}

void Engine::applyNoteOff(int note, int velocity) noexcept {
    Bank* b = rtBank;
    if (!b) return;
    sympResonance.noteOff(note);

    if (b->backend == SamplerBackend::NativeSF2 && b->sf2) {
        tsf_channel_note_off(b->sf2, 0, note);
    } else if (b->sfz) {
        b->sfz->noteOff(0, note, velocity);
    }
}

void Engine::applyCC(int ccNumber, int ccValue) noexcept {
    Bank* b = rtBank;
    if (!b) return;
    if (ccNumber == 64) {
        sympResonance.setSustainPedal(ccValue >= 64);   // CC 64 = pedal de sustain
    }

    if (b->backend == SamplerBackend::NativeSF2 && b->sf2) {
        tsf_channel_midi_control(b->sf2, 0, ccNumber, ccValue);
    } else if (b->sfz) {
        b->sfz->cc(0, ccNumber, ccValue);
    }
}

void Engine::applyBend(int pitchValue) noexcept {
    Bank* b = rtBank;
    if (!b) return;
    if (b->backend == SamplerBackend::NativeSF2 && b->sf2) {
        tsf_channel_set_pitchwheel(b->sf2, 0, std::clamp(pitchValue + 8192, 0, 16383));
    } else if (b->sfz) {
        b->sfz->pitchWheel(0, pitchValue);
    }
}

void Engine::allNotesOff() noexcept {
    Bank* b = rtBank;
    if (b) {
        if (b->backend == SamplerBackend::NativeSF2 && b->sf2) {
            tsf_channel_sounds_off_all(b->sf2, 0);
        } else if (b->sfz) {
            b->sfz->allSoundOff();
        }
    }
    sympResonance.setSustainPedal(false);
}

void Engine::render(float* outL, float* outR, size_t numFrames) noexcept {
    if (numFrames == 0) { pendingCount = 0; return; }

    if (numFrames * 2 > interleavedBuffer.size()) {
        interleavedBuffer.resize(numFrames * 2, 0.0f);
    }

    // O smoothing e por bloco, nao por fatiamento: avanca as amostras do bloco
    // inteiro de uma vez. Fatiar mais fino nao acelera (nem deve acelerar) a rampa
    // dos parametros.
    params.updateBlock(numFrames);

    // Ordena por tempo preservando a ordem de chegada nos empates (o stable
    // insertion sort faz isso e sai quase linear quando o host ja entrega em
    // ordem, que e o caso normal).
    for (int i = 1; i < pendingCount; ++i) {
        const PendingEvent ev = pending[i];
        int j = i - 1;
        while (j >= 0 && pending[j].delay > ev.delay) { pending[j + 1] = pending[j]; --j; }
        pending[j + 1] = ev;
    }

    const int frames = static_cast<int>(numFrames);
    const auto noteKind = [](const PendingEvent& e) {
        return e.kind == PendingEvent::NoteOn || e.kind == PendingEvent::NoteOff;
    };
    size_t pos = 0;
    int next = 0;
    while (pos < numFrames) {
        // Tudo que vence aqui age antes das amostras do fatio.
        while (next < pendingCount && pending[next].delay <= static_cast<int>(pos)) {
            dispatch(pending[next]);
            ++next;
        }
        size_t stop = numFrames;
        size_t minSlice = kMinSlice;
        bool nextIsNote = false;
        if (next < pendingCount) {
            stop = static_cast<size_t>(std::clamp(pending[next].delay, 0, frames));
            nextIsNote = noteKind(pending[next]);
            minSlice = nextIsNote ? kMinSlice : kCcSlice;
        }
        // Cada frontia custa ~6 us medidas no tsf com o piano cheio de vozes
        // (tools/pforge_dsp_stages: 1x512 = 330 us, 32x16 = 527 us). Um host que
        // manda automacao amostra a amostra teria 512 fronteiras num bloco de 512;
        // aqui elas viram no maximo numFrames/kMinSlice para nota e
        // numFrames/kCcSlice para controle continuo. Nota e ataque; CC e bend sao
        // ganho/estado, e o valor que o PROPRIO Engine tira deles ja e quantizado
        // por bloco (params.updateBlock roda uma vez aqui, nao por fatio).
        if (stop < numFrames && stop - pos < minSlice) {
            size_t target = pos + minSlice;
            // A fusao de CC aproxima a fronteira da proxima nota, nunca passa dela.
            for (int i = next; i < pendingCount; ++i) {
                if (noteKind(pending[i])) {
                    target = std::min(target,
                                      static_cast<size_t>(std::clamp(pending[i].delay, 0, frames)));
                    break;
                }
            }
            // ...mas nunca abaixo do piso de nota. Sem este teto, um CC chegando
            // pouco antes de uma nota densa PARAVA na nota (a 7 amostras) e
            // criava mais fronteiras do que o merge uniforme antigo: 64 notas +
            // 100 CC deu 1,115 ms com a regra do "para na nota" e 0,72 ms com o
            // piso aqui. O adiantamento de nota continua limitado a kMinSlice-1.
            target = std::max(target, pos + kMinSlice);
            stop = std::min(target, numFrames);
            while (next < pendingCount && pending[next].delay < static_cast<int>(stop)) {
                dispatch(pending[next]);
                ++next;
            }
        }
        renderSlice(outL + pos, outR + pos, stop - pos);
        pos = stop;
    }
    // Eventos atrasados alem do bloco (host pode mandar tempo > n_samples):
    // aplicados no fim para nao perder estado (pedal, bend) para o proximo bloco.
    while (next < pendingCount) { dispatch(pending[next]); ++next; }
    pendingCount = 0;
}

void Engine::renderSlice(float* outL, float* outR, size_t numFrames) noexcept {
    if (numFrames == 0) return;

    // 1. Render raw samples from the active backend
    Bank* b = rtBank;
    if (b && b->backend == SamplerBackend::NativeSF2 && b->sf2) {
        tsf_render_float(b->sf2, interleavedBuffer.data(), static_cast<int>(numFrames), 0);
        for (size_t i = 0; i < numFrames; ++i) {
            outL[i] = interleavedBuffer[i * 2 + 0];
            outR[i] = interleavedBuffer[i * 2 + 1];
        }
    } else if (b && b->sfz) {
        float* channels[2] = { outL, outR };
        b->sfz->renderBlock(channels, numFrames, 1);
    } else {
        std::memset(outL, 0, numFrames * sizeof(float));
        std::memset(outR, 0, numFrames * sizeof(float));
    }

    // 2. Physical hammer transient (subtle, adds attack definition)
    hammerModel.process(outL, outR, numFrames);

    // 3. Sympathetic string resonance (rich harmonic wash on held notes & pedal)
    float sympRes = params.getSmoothed(Param_SympRes);
    float sympTune = params.getValue(Param_SympTune);
    sympResonance.setParameters(sympRes, sympTune);
    sympResonance.process(outL, outR, numFrames);

    // 4. 3-Band Soundboard EQ
    float bass = params.getSmoothed(Param_BassEQ);
    float mid = params.getSmoothed(Param_MidEQ);
    float high = params.getSmoothed(Param_HighEQ);
    soundboardEQ.setGains(bass, mid, high);
    soundboardEQ.process(outL, outR, numFrames);

    // 5. Lid Acoustic Filter (transparent when 1.0)
    float lid = params.getSmoothed(Param_Lid);
    lidFilter.setLid(lid);
    lidFilter.process(outL, outR, numFrames);

    // 6. Stereo Chorus (off by default)
    chorus.setMix(params.getSmoothed(Param_Chorus));
    chorus.process(outL, outR, numFrames);

    // 7. Concert Reverb
    reverb.setType(static_cast<int>(params.getValue(Param_ReverbType)));
    reverb.setMix(params.getSmoothed(Param_Reverb));
    reverb.process(outL, outR, numFrames);

    // 8. Master Stage: Stereo Width, Trim dB, Volume, and Soft Clipping
    masterStage.process(outL, outR, numFrames,
                        params.getSmoothed(Param_Volume),
                        params.getValue(Param_TrimDb),
                        params.getValue(Param_Width));
}

} // namespace pianoforge
