#pragma once
#include "Parameters.hpp"
#include "dsp/VelocityCurve.hpp"
#include "dsp/HammerModel.hpp"
#include "dsp/SympatheticResonance.hpp"
#include "dsp/SoundboardEQ.hpp"
#include "dsp/LidFilter.hpp"
#include "dsp/Chorus.hpp"
#include "dsp/Reverb.hpp"
#include "dsp/MasterStage.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct tsf;

// Handle opaco do sfizz. O header nao conhece sfz::Sfizz, e por isso o unico
// header de terceiros que Engine.cpp incluia saiu da assinatura publica do Bank:
// um build sem sfizz (o runner de CI, qualquer maquina sem a lib) nao precisa de
// SFIZZ_DIR nem de header nenhum. O unique_ptr de tipo incompleto e legal porque
// ~Bank() esta declarado aqui e definido no .cpp, que e onde o tipo fecha.
struct SfzHandle;

namespace pianoforge {

enum class SamplerBackend {
    None,
    NativeSF2,
    SfizzSFZ
};

// Um banco carregado. Construido inteiro fora da thread de audio (buildBank),
// publicado por troca de ponteiro e depois tratado como imutavel: nada aqui
// muda depois de install(), o que e o que torna o swap seguro.
struct Bank {
    SamplerBackend backend = SamplerBackend::None;
    tsf* sf2 = nullptr;
    std::unique_ptr<SfzHandle> sfz;
    std::string path;
    int presetCount = 0;

    Bank() = default;
    ~Bank();
    Bank(const Bank&) = delete;
    Bank& operator=(const Bank&) = delete;

    // Carrega o banco do disco, configura e pre-aloca o que o sampler faria lazy
    // dentro do render. Bloqueia segundos num SF2 grande: nunca no run().
    bool load(const std::string& bankPath, float sampleRate, size_t blockSize);
};

class Engine {
public:
    Engine();
    ~Engine();

    // So em contexto nao-RT (instantiate / standalone).
    void prepare(float sampleRate, size_t maxBlockSize);

    // --- troca de piano -------------------------------------------------
    // buildBank carrega em memoria e instala. Roda no worker do host (ou na
    // thread de load propria). Devolve nullptr em falha; o banco chamado nunca
    // e publicado, entao um caminho errado nao muda o som nem derruba o plugin.
    Bank* buildBank(const std::string& bankPath) noexcept;
    // Publica o banco novo (nullptr = nenhum piano) e apos o antigo para
    // aposentadoria. Nao-RT: mexe na fila de retire.
    void publishBank(Bank* next) noexcept;
    // Carregamento sincrono usado pelo standalone, onde nao existe thread de audio.
    bool loadSoundBankSync(const std::string& bankPath);
    bool loadSoundBank(const std::string& bankPath);   // alias do sincrono
    bool loadSf2(const std::string& sf2Path);
    bool loadSfz(const std::string& sfzPath);

    // RT: true se um bloco inteiro comecou depois do ultimo publish, ou seja, o
    // worker ja pode libertar os bancos aposentados. Nao limpa sozinho: limpar e
    // do worker depois de um schedule bem-sucedido, senao um host com fila cheia
    // perderia o aviso e o banco velho ficaria preso na RAM.
    bool retirePending() const noexcept { return retireReady.load(std::memory_order_acquire); }
    void clearRetirePending() noexcept { retireReady.store(false, std::memory_order_release); }
    // Nao-RT: ainda ha RAM de banco aposentado em poder do processo. E o que diz
    // ao worker se vale a pena alocar um banco novo agora (medido: um SF2 de
    // 612 MB ocupa 1,2 GB, entao velho + novo no ar = 1,7 GB).
    bool holdingRetired() const noexcept {
        std::lock_guard<std::mutex> lk(retireMx);
        return !retireList.empty();
    }
    // Nao-RT: liberta tudo que estava esperando o proximo bloco.
    void freeRetiredBanks() noexcept;
    // Nao-RT (deactivate/cleanup): idem, para quando o host garantiu que nao ha
    // run() concorrente e o transporte pode ate estar parado.
    void drainRetireNow() noexcept;

    // Primeira coisa dentro de run(), antes de qualquer evento MIDI.
    void beginBlock() noexcept;

    // --- estado do bloco atual ------------------------------------------
    bool isLoaded() const noexcept { return rtBank != nullptr; }
    SamplerBackend getBackend() const noexcept {
        return rtBank ? rtBank->backend : SamplerBackend::None;
    }

    int getPresetCount() const noexcept;
    std::string getPresetName(int index) const;
    bool setPresetIndex(int index) noexcept;

    // MIDI Event Handling. delayFrames e o tempo dentro do bloco que esta sendo
    // montado agora: o evento entra numa fila e so e aplicado pelo render() no
    // instante certo. Devem ser chamados antes do render() do mesmo bloco.
    void handleNoteOn(int delayFrames, int note, int rawVelocity) noexcept;
    void handleNoteOff(int delayFrames, int note, int velocity) noexcept;
    void handleCC(int delayFrames, int ccNumber, int ccValue) noexcept;
    // pitchValue em -8192..8191 (centro = 0).
    void handlePitchBend(int delayFrames, int pitchValue) noexcept;
    // Ignora a fila e age ja: usado fora do contexto de bloco (troca de piano).
    void allNotesOff() noexcept;

    // Audio Processing: aplica a fila do bloco em ordem de tempo e renderiza o
    // bloco todo, fatiado nos instantes em que algo mudou.
    void render(float* outL, float* outR, size_t numFrames) noexcept;

    ParameterManager& getParameters() noexcept { return params; }
    const ParameterManager& getParameters() const noexcept { return params; }

    float getSampleRate() const noexcept { return sampleRate; }

private:
    // Evento esperado pelo bloco em curso. Nada aqui aloca: a fila e um array
    // fixo, e o que o RT pode fazer dentro de run().
    struct PendingEvent {
        int delay;
        enum Kind { NoteOn, NoteOff, Control, Bend } kind;
        int a;   // nota, numero de CC ou nada
        int b;   // velocidade, valor de CC ou pitch bend
    };
    // Capacidade da fila por bloco. Um bloco de MIDI raramente passa de algumas
    // dezenas de eventos; o que exceder e aplicado na hora, adiantado no maximo
    // ate o proximo fatiamento, em vez de ser descartado ou de alocar.
    static constexpr int kMaxPendingEvents = 512;

    void stage(const PendingEvent& ev) noexcept;
    void dispatch(const PendingEvent& ev) noexcept;
    void applyNoteOn(int note, int rawVelocity) noexcept;
    void applyNoteOff(int note, int velocity) noexcept;
    void applyCC(int ccNumber, int ccValue) noexcept;
    void applyBend(int pitchValue) noexcept;

    // O corpo antigo do process(): renderiza exatamente um fatio, sem mexer no
    // smoothing dos parametros (que continua por bloco, nao por fatio).
    void renderSlice(float* outL, float* outR, size_t numFrames) noexcept;

    // Publicado pelo worker, lido pelo RT uma vez por bloco (ver beginBlock).
    std::atomic<Bank*> bank_{nullptr};
    Bank* rtBank = nullptr;                  // snapshot do bloco: so o RT le

    // Aposentadoria: o tsf_close de um SF2 de gigabyte e um munmap de centenas
    // de milissegundos, entao o velho nunca e liberado pelo RT. O worker encosta
    // o ponteiro aqui e so pode libera-lo depois que um bloco inteiro comecou
    // apos o publish (o RT ja nao aponta mais pro velho).
    std::vector<Bank*> retireList;           // exclusively worker/nao-RT
    mutable std::mutex retireMx;             // dois nao-RT podem libertar na mesma hora
    std::atomic<unsigned> blockSerial{0};
    std::atomic<unsigned> retireSerial{0};
    std::atomic<bool> retireArmed{false};
    std::atomic<bool> retireReady{false};

    ParameterManager params;

    dsp::HammerModel hammerModel;
    dsp::SympatheticResonance sympResonance;
    dsp::SoundboardEQ soundboardEQ;
    dsp::LidFilter lidFilter;
    dsp::Chorus chorus;
    dsp::Reverb reverb;
    dsp::MasterStage masterStage;

    std::vector<float> interleavedBuffer;
    PendingEvent pending[kMaxPendingEvents];
    int pendingCount = 0;
    float sampleRate = 44100.0f;
    size_t blockSize = 256;
};

} // namespace pianoforge
