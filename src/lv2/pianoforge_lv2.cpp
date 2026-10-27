#include "Engine.hpp"
#include "SoundbankManager.hpp"
#include "StatusPorts.hpp"

#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/worker/worker.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#define PIANOFORGE_URI "https://pianoforge.audio/plugin"

namespace {

enum PortIndex {
    PORT_AUDIO_OUT_L = 0,
    PORT_AUDIO_OUT_R = 1,
    PORT_MIDI_IN     = 2,

    // Controls (10 visiveis)
    PORT_VOLUME      = 3,
    PORT_HAMMER      = 4,
    PORT_VEL_CURVE   = 5,
    PORT_SYMP_RES    = 6,
    PORT_LID         = 7,
    PORT_BASS_EQ     = 8,
    PORT_MID_EQ      = 9,
    PORT_HIGH_EQ     = 10,
    PORT_CHORUS      = 11,
    PORT_REVERB      = 12,

    // Controls ocultos (7)
    PORT_REVERB_TYPE  = 13,
    PORT_TRIM_DB      = 14,
    PORT_WIDTH        = 15,
    PORT_RELEASE_DAMP = 16,
    PORT_HAMMER_TONE  = 17,
    PORT_SYMP_TUNE    = 18,
    PORT_ENGINE_TUNE  = 19,

    // Selecao de banco e preset. -1 em soundbank = nenhum piano.
    PORT_SOUNDBANK    = 20,
    PORT_PRESET       = 21,

    // Saidas de estado (StatusPorts.hpp): a UI precisa saber o que o worker fez.
    PORT_BANK_STATE   = static_cast<int>(pianoforge::kPortBankState),
    PORT_PRESET_COUNT = static_cast<int>(pianoforge::kPortPresetCount),

    PORT_TOTAL_COUNT  = static_cast<int>(pianoforge::kPortTotal)
};

// Bloco maximo: prepara o buffer intercalado folgado para o run() nunca
// realocar na thread de audio (LV2 so garante n_samples <= max block length).
constexpr uint32_t kMaxBlockSize = 4096;

enum MsgKind : int32_t {
    MSG_LOAD_BANK  = 1,
    MSG_FREE_OLD   = 2
};

struct WorkerMsg {
    int32_t kind;
    int32_t bankIndex;
};

struct URIs {
    LV2_URID atom_Sequence;
    LV2_URID midi_Event;

    void map(LV2_URID_Map* m) {
        atom_Sequence = m->map(m->handle, LV2_ATOM__Sequence);
        midi_Event    = m->map(m->handle, LV2_MIDI__MidiEvent);
    }
};

class PianoForgeLV2 {
public:
    PianoForgeLV2(double rate, const LV2_Feature* const* features)
        : sampleRate(rate) {

        for (int i = 0; features && features[i]; ++i) {
            if (!std::strcmp(features[i]->URI, LV2_URID__map)) {
                uris.map(static_cast<LV2_URID_Map*>(features[i]->data));
            } else if (!std::strcmp(features[i]->URI, LV2_WORKER__schedule)) {
                worker = static_cast<const LV2_Worker_Schedule*>(features[i]->data);
            }
        }

        engine.prepare(static_cast<float>(sampleRate), kMaxBlockSize);

        // Rodar o load no run() nao e opcao: um SF2 de 641 MB bloqueia segundos.
        // A thread existe sempre, mesmo com worker no host -- ela e a reserva para
        // o host que aceita o schedule e nunca roda work().
        loaderRun.store(true);
        loaderThread = std::thread([this] { loaderLoop(); });
    }

    ~PianoForgeLV2() {
        loaderRun.store(false);
        if (loaderThread.joinable()) loaderThread.join();
        engine.drainRetireNow();
    }

    void connectPort(uint32_t port, void* data) {
        if (port < PORT_TOTAL_COUNT) ports[port] = data;
    }

    void activate() {
        // O valor que o host restaurou e linha de base, nao um pedido: e isso que
        // impede uma sessao salva de carregar um piano sozinha.
        baselineBank = currentBankValue();
        lastRequestedBank = baselineBank;
        engine.clearRetirePending();
    }

    void deactivate() {
        engine.allNotesOff();
        engine.drainRetireNow();
    }

    void run(uint32_t n_samples) {
        float* outL = static_cast<float*>(ports[PORT_AUDIO_OUT_L]);
        float* outR = static_cast<float*>(ports[PORT_AUDIO_OUT_R]);
        const LV2_Atom_Sequence* midiSeq = static_cast<const LV2_Atom_Sequence*>(ports[PORT_MIDI_IN]);
        if (!outL || !outR) return;

        // Estado pro UI primeiro: se o bloco adiante estourar, "carregando" ainda
        // e a verdade que a janela mostra.
        writeStatusPorts();

        // 1. Snapshot do banco deste bloco + aviso de liberacao pro worker.
        engine.beginBlock();
        pingFreeOld();

        // 2. Troca de piano: so uma mudanca observada depois do activate() carrega.
        const int bankValue = currentBankValue();
        if (bankValue != lastRequestedBank) {
            lastRequestedBank = bankValue;
            requestBank(bankValue);
        }

        // 3. Troca de preset (barato: so escreve no canal ja alocado no install).
        const float* presetPtr = static_cast<const float*>(ports[PORT_PRESET]);
        if (presetPtr) {
            const int wanted = static_cast<int>(std::round(*presetPtr));
            if (wanted != currentPresetIndex) {
                if (engine.setPresetIndex(wanted)) currentPresetIndex = wanted;
            }
        }

        // 4. Ports de controle.
        for (int p = 0; p < pianoforge::Param_Count; ++p) {
            const float* valPtr = static_cast<const float*>(ports[PORT_VOLUME + p]);
            if (valPtr) engine.getParameters().setValue(p, *valPtr);
        }

        // 5. MIDI.
        if (midiSeq) {
            LV2_ATOM_SEQUENCE_FOREACH(midiSeq, ev) {
                if (ev->body.type != uris.midi_Event) continue;
                const uint8_t* const msg = reinterpret_cast<const uint8_t*>(ev + 1);
                if (msg[0] == 0xF0 || msg[0] >= 0xF0) continue;   // System: ignorado
                const uint8_t status = msg[0] & 0xF0;
                int delay = static_cast<int>(ev->time.frames);
                if (delay < 0) delay = 0;
                if (delay >= static_cast<int>(n_samples)) delay = static_cast<int>(n_samples) - 1;

                switch (status) {
                    case 0x90: engine.handleNoteOn(delay, msg[1], msg[2]); break;
                    case 0x80: engine.handleNoteOff(delay, msg[1], msg[2]); break;
                    case 0xB0: engine.handleCC(delay, msg[1], msg[2]); break;
                    case 0xE0: engine.handlePitchBend(delay, ((int(msg[2]) << 7) | int(msg[1])) - 8192); break;
                    default: break;
                }
            }
        }

        // 6. Audio.
        std::memset(outL, 0, n_samples * sizeof(float));
        std::memset(outR, 0, n_samples * sizeof(float));
        engine.render(outL, outR, n_samples);
    }

    // ---- worker -------------------------------------------------------
    LV2_Worker_Status work(LV2_Worker_Respond_Function respond,
                           LV2_Worker_Respond_Handle handle,
                           uint32_t size, const void* data) {
        // Prova de vida do host no caminho nao-RT: e o que diz a nossa thread de
        // reserva que aqui alguem rói o trabalho. REAPER 7.83 entrega o schedule e
        // nunca chama work() com o transporte parado.
        workerAlive.store(true, std::memory_order_release);
        if (!data || size != sizeof(WorkerMsg)) return LV2_WORKER_ERR_UNKNOWN;
        const WorkerMsg msg = *static_cast<const WorkerMsg*>(data);

        if (msg.kind == MSG_FREE_OLD) {
            engine.freeRetiredBanks();
            engine.clearRetirePending();
            // Um pedido estacionado esperava exatamente esta liberacao (ver
            // loadBankNow): agora ha RAM para o banco novo sem os dois no ar.
            const int parked = parkedLoad.exchange(-1);
            if (parked >= 0) loadBankNow(parked);
            return LV2_WORKER_SUCCESS;
        }

        if (msg.kind == MSG_LOAD_BANK) {
            loadBankNow(msg.bankIndex);
            if (respond) respond(handle, 0, nullptr);
            return LV2_WORKER_SUCCESS;
        }
        return LV2_WORKER_ERR_UNKNOWN;
    }

    LV2_Worker_Status workResponse(uint32_t /*size*/, const void* /*body*/) {
        return LV2_WORKER_SUCCESS;
    }

private:
    int currentBankValue() const {
        const float* p = static_cast<const float*>(ports[PORT_SOUNDBANK]);
        return p ? static_cast<int>(std::round(*p)) : -1;
    }

    void writeStatusPorts() {
        if (float* st = static_cast<float*>(ports[PORT_BANK_STATE]))
            *st = static_cast<float>(pianoforge::bankStateCode(
                loadInFlight.load(std::memory_order_relaxed),
                loadFailed.load(std::memory_order_relaxed),
                publishedBank.load(std::memory_order_relaxed)));
        if (float* pc = static_cast<float*>(ports[PORT_PRESET_COUNT]))
            *pc = static_cast<float>(publishedPresets.load(std::memory_order_relaxed));
    }

    void requestBank(int index) {
        loadFailed.store(false, std::memory_order_relaxed);
        requestedIndex.store(index, std::memory_order_relaxed);
        loadInFlight.store(index != publishedBank.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
        if (worker) {
            const WorkerMsg msg{MSG_LOAD_BANK, index};
            if (worker->schedule_work(worker->handle, sizeof(msg), &msg) != LV2_WORKER_SUCCESS) {
                lastRequestedBank = baselineBank;   // re-tenta no proximo bloco
                return;
            }
            return;
        }
        loaderRequest.store(index, std::memory_order_release);   // thread de reserva
    }

    void pingFreeOld() {
        if (!engine.retirePending()) return;
        if (worker) {
            const WorkerMsg msg{MSG_FREE_OLD, 0};
            if (worker->schedule_work(worker->handle, sizeof(msg), &msg) == LV2_WORKER_SUCCESS) {
                // O worker limpa o sinal quando tratar a mensagem.
                return;
            }
            return;   // fila do host cheia: avisa de novo no proximo bloco
        }
        // Sem worker: libertar um banco e um munmap de centenas de MB (deletar o
        // tsf de 1,2 GB), jamais dentro de run(). A thread de reserva assume.
        freeRequested.store(true, std::memory_order_release);
    }

    // Roda no thread do worker (ou na loaderThread): I/O de disco e alocação.
    // publishedBank/publishedPresets sao o chao da verdade do que esta soando;
    // loadInFlight/loadFailed so valem para o pedido em curso.
    // esperarLiberar = true (o normal): devolve a RAM do banco velho antes de
    // alocar o novo. = false e o caminho do timeout, que aceita o pico.
    void loadBankNow(int index, bool esperarLiberar = true) {
        // Worker do host e thread de reserva podem bater no mesmo pedido: quem
        // pega o sinal primeiro rói, o outro sai sem duplicar 250 MB de I/O.
        bool livre = false;
        if (!loadingNow.compare_exchange_strong(livre, true)) return;
        struct Soltar {
            std::atomic<bool>& flag;
            ~Soltar() { flag.store(false, std::memory_order_release); }
        } soltar{loadingNow};

        if (index >= 0 && index == publishedBank.load(std::memory_order_relaxed) &&
            !loadFailed.load(std::memory_order_relaxed)) {
            // Ja esta rolando: o worker do host chegou depois da reserva. Roido de
            // novo seria 250 MB de I/O e dois bancos na RAM por nada.
            loadInFlight.store(false, std::memory_order_relaxed);
            return;
        }
        if (index < 0) {
            engine.publishBank(nullptr);
            currentPresetIndex = 0;
            publishedBank.store(-1, std::memory_order_relaxed);
            publishedPresets.store(0, std::memory_order_relaxed);
            loadInFlight.store(false, std::memory_order_relaxed);
            loadFailed.store(false, std::memory_order_relaxed);
            std::cerr << "[PianoForge] Nenhum piano selecionado (silencio).\n";
            return;
        }

        auto refused = [&](const std::string& what) {
            loadInFlight.store(false, std::memory_order_relaxed);
            loadFailed.store(true, std::memory_order_relaxed);
            std::cerr << "[PianoForge] " << what << " (indice " << index << ")\n";
        };

        pianoforge::SoundbankManager& mgr = pianoforge::SoundbankManager::getInstance();
        mgr.scan();

        pianoforge::SoundbankInfo info;
        if (!mgr.getBankAt(index, info)) {
            refused("indice de banco inexistente, " + std::to_string(mgr.getBankCount()) +
                    " registrados");
            return;
        }
        if (!info.exists) {
            refused("arquivo nao encontrado: " + info.path);
            return;
        }

        // ---- RAM antes de I/O ------------------------------------------------
        // Medido em tools/pforge_dsp_stages: um SF2 de 612 MB pede 1,2 GB aqui,
        // porque o tsf converte o chunk smpl de int16 em float32 (2x). Carregar o
        // novo com o velho publicado colocaria os dois no ar ao mesmo tempo --
        // 1,7 GB no par que existe no registro, numa maquina de 6,7 GB. Entao:
        // silencia, devolve a RAM do velho, e so depois pega o disco. O preco e
        // um bloco de silencio a mais numa troca que ja ia trocar de piano.
        //
        // Depois das duas guardas acima, de caso pensado: o contrato do teste H e
        // "clicar num banco que nao abre nao silencia o que esta tocando". Um
        // caminho apagado ou digitado errado e recusado sem tocar no piano publicado.
        // O que ainda silencia e o arquivo presente com conteudo quebrado, porque
        // a unica forma de saber antes seria ler os ~250 MB dele -- e esse e
        // exatamente o custo que o estacionamento existe para pagar.
        const bool tinhaBank = publishedBank.load(std::memory_order_relaxed) >= 0;
        if (tinhaBank || engine.holdingRetired()) {
            if (tinhaBank) {
                engine.allNotesOff();
                engine.publishBank(nullptr);
                publishedBank.store(-1, std::memory_order_relaxed);
                publishedPresets.store(0, std::memory_order_relaxed);
                currentPresetIndex = 0;
            }
            if (engine.retirePending()) {
                engine.freeRetiredBanks();
                engine.clearRetirePending();
            } else if (esperarLiberar) {
                // Falta o run() passar por um bloco para a liberacao ser segura.
                // O aviso vem por pingFreeOld(); o pedido fica estacionado aqui
                // com loadInFlight verdadeiro -- a UI segue lendo "forjando", que
                // e a verdade: nada soa, nada falhou.
                parkedLoad.store(index, std::memory_order_relaxed);
                return;
            }
        }

        pianoforge::Bank* next = engine.buildBank(info.path);
        if (!next) {
            refused("falha ao carregar: " + info.path);
            return;
        }

        engine.allNotesOff();
        engine.publishBank(next);
        currentPresetIndex = 0;
        publishedBank.store(index, std::memory_order_relaxed);
        publishedPresets.store(next->presetCount, std::memory_order_relaxed);
        loadInFlight.store(false, std::memory_order_relaxed);
        loadFailed.store(false, std::memory_order_relaxed);
        std::cerr << "[PianoForge] Piano carregado: " << info.name
                  << " (" << next->presetCount << " presets, "
                  << (info.sizeBytes / (1024 * 1024)) << " MB)\n";
    }

    // Fallback para hosts sem lv2:worker E para hosts que oferecem o schedule mas
    // nunca rodam work() (REAPER 7.83 so processa FX com o transporte tocando: o
    // pedido ficaria "carregando" para sempre e o piano nunca chegaria).
    void loaderLoop() {
        int vigiado = -2;
        int parkedTicks = 0;
        auto desde = std::chrono::steady_clock::now();
        while (loaderRun.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // O trabalho que o worker do host faria: libertar o banco aposentado
            // (munmap de centenas de MB, nunca dentro de run()).
            if (freeRequested.exchange(false)) {
                engine.freeRetiredBanks();
                engine.clearRetirePending();
            }
            // Retoma o pedido estacionado. A liberacao do banco velho tem UM dono
            // por modo de host: com worker e o work() (MSG_FREE_OLD), sem worker
            // sou eu tratando freeRequested. Esta thread jamais chama
            // freeRetiredBanks no caminho do park -- dois libertando a mesma Bank
            // e double free, e foi o "corrupted size vs. prev_size" desta versao.
            // Se em 300 ms a RAM nao voltou (host com transporte parado, que nao
            // passa bloco nenhum e portanto nao arma o aviso) assume o pior caso
            // de antes: velho + novo no ar. Pedido preso em "forjando" para sempre
            // pesa mais que pico de RAM.
            if (const int parked = parkedLoad.load(std::memory_order_relaxed); parked >= 0) {
                if (engine.holdingRetired() && ++parkedTicks < 3) continue;
                parkedTicks = 0;
                if (parkedLoad.exchange(-1) != parked) continue;   // outro assumiu
                loadBankNow(parked, false);
                vigiado = -2;
                continue;
            }
            parkedTicks = 0;
            const int pedido = loaderRequest.exchange(-2);
            if (pedido != -2) {
                loadBankNow(pedido);
                vigiado = -2;
                continue;
            }
            if (!worker || workerAlive.load(std::memory_order_acquire)) continue;
            const int want = requestedIndex.load(std::memory_order_relaxed);
            if (want != vigiado) {
                vigiado = want;
                desde = std::chrono::steady_clock::now();
                continue;
            }
            if (!loadInFlight.load(std::memory_order_relaxed)) continue;
            const double espera = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - desde).count();
            if (espera > 2.0) {
                std::cerr << "[PianoForge] host aceitou o pedido mas nao rodou work() em "
                          << int(espera) << " s: a thread de reserva assume o banco "
                          << want << ".\n";
                loadBankNow(want);
                vigiado = -2;
            }
        }
    }

    double sampleRate = 44100.0;
    URIs uris{};
    const LV2_Worker_Schedule* worker = nullptr;
    pianoforge::Engine engine;
    void* ports[PORT_TOTAL_COUNT]{};

    int baselineBank = -1;
    int lastRequestedBank = -1;
    int currentPresetIndex = 0;

    // O que esta publicado de verdade, nao o que a porta pede: e isso que a UI
    // mostra, e o que separa "voce escolheu" de "esta soando".
    std::atomic<int> publishedBank{-1};
    std::atomic<int> publishedPresets{0};
    std::atomic<bool> loadInFlight{false};
    std::atomic<bool> loadFailed{false};

    // Pedido de load esperando a RAM do banco velho voltar (loadBankNow), e o
    // aviso do RT de que um banco aposentado ja pode ser libertado fora do run().
    std::atomic<int> parkedLoad{-1};
    std::atomic<bool> freeRequested{false};

    // Reserva de load: a thread existe sempre; loaderRequest e a fila dela e
    // requestedIndex/workerAlive sao o que ela vigia para descobrir um host que
    // aceita o schedule e nunca roda work(). loadingNow impede os dois roerem o
    // mesmo arquivo ao mesmo tempo.
    std::thread loaderThread;
    std::atomic<bool> loaderRun{false};
    std::atomic<int> loaderRequest{-2};   // -2 = nenhum pedido
    std::atomic<int> requestedIndex{-1};
    std::atomic<bool> workerAlive{false};
    std::atomic<bool> loadingNow{false};
};

LV2_Handle instantiate(const LV2_Descriptor* /*descriptor*/,
                       double rate,
                       const char* /*bundle_path*/,
                       const LV2_Feature* const* features) {
    return new PianoForgeLV2(rate, features);
}

void connect_port(LV2_Handle instance, uint32_t port, void* data_location) {
    static_cast<PianoForgeLV2*>(instance)->connectPort(port, data_location);
}

void activate(LV2_Handle instance) { static_cast<PianoForgeLV2*>(instance)->activate(); }
void deactivate(LV2_Handle instance) { static_cast<PianoForgeLV2*>(instance)->deactivate(); }

void run(LV2_Handle instance, uint32_t sample_count) {
    static_cast<PianoForgeLV2*>(instance)->run(sample_count);
}

void cleanup(LV2_Handle instance) { delete static_cast<PianoForgeLV2*>(instance); }

const void* extension_data(const char* uri) {
    static const LV2_Worker_Interface workerInterface = {
        [](LV2_Handle inst, LV2_Worker_Respond_Function respond,
           LV2_Worker_Respond_Handle handle, uint32_t size, const void* data) {
            return static_cast<PianoForgeLV2*>(inst)->work(respond, handle, size, data);
        },
        [](LV2_Handle inst, uint32_t size, const void* body) {
            return static_cast<PianoForgeLV2*>(inst)->workResponse(size, body);
        },
        nullptr
    };
    if (!std::strcmp(uri, LV2_WORKER__interface)) return &workerInterface;
    return nullptr;
}

static const LV2_Descriptor descriptor = {
    PIANOFORGE_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

} // namespace

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return (index == 0) ? &descriptor : nullptr;
}
