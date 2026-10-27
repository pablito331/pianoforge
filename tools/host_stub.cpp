// pforge_host_test: host LV2 minimo so para verificar a troca de piano.
//
//   A  instantiation com soundbank=-1  -> silencio absoluto, worker nunca chamado
//   B  muda soundbank 0                -> som aparece, load rodou FORA do run()
//   C  valor ja restaurado no activate -> continua mudo (nada e "puxado")
//   D  swap A -> B -> A                -> nenhum bloco estoura o orcamento
//   E  soundbank=-1 depois de tocar    -> cauda morre em silencio
//   F  delayFrames dentro do bloco     -> onset na amostra certa
//   G  bloco denso de eventos          -> o fatiamento nao estoura o orcamento
//   H  banco invalido escolhido        -> o piano em uso continua em uso
//   I  portas de saida 22/23            -> "carregando" aparece DURANTE o load, e
//      (bankState / presetCount)           vira o indice publicado depois
//
// Uso: pforge_host_test --so <pianoforge.so> [--bank-value N]
//        --sf2 <caminho> ...    banco real, repetivel (.sf2 ou .pforge)
//        --makesf2 <N>          N bancos SINTETICOS (tools/SynthBank.hpp) -- o que o
//                               CI usa, porque soundfont gravado tem licenca
//        --bad <caminho> ...    registrado depois dos validos (teste H)
//      Sem banco a suíte recusa rodar: 10 dos 13 testes precisam de um piano.
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/midi/midi.h>
#include <lv2/parameters/parameters.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include "Parameters.hpp"
#include "StatusPorts.hpp"
#include "SynthBank.hpp"      // banco sintetico: o CI nao tem (nem pode ter) soundfont

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include "Dl.hpp"                 // dlopen em tres SOs (o mingw nao tem <dlfcn.h>)
#include <filesystem>
#include <mutex>
#include <fstream>
#ifndef _WIN32
#include <sys/resource.h>         // getrusage, só para minorFaults()
#include <unistd.h>               // getpid
#else
#include <process.h>              // _getpid
#endif
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// ---------------- urid ----------------
std::unordered_map<std::string, LV2_URID> g_urids;
LV2_URID g_nextUrid = 1;

LV2_URID uridMap(void* /*handle*/, const char* uri) {
    auto it = g_urids.find(uri);
    if (it != g_urids.end()) return it->second;
    const LV2_URID id = g_nextUrid++;
    g_urids[uri] = id;
    return id;
}

LV2_URID_Map g_map = { nullptr, uridMap };

// ---------------- worker ----------------
struct WorkerStub {
    const LV2_Descriptor* desc = nullptr;
    LV2_Handle instance = nullptr;
    const LV2_Worker_Interface* iface = nullptr;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::vector<char>> inbox;
    std::deque<std::vector<char>> responses;
    bool stop = false;
    std::thread thread;

    std::atomic<int> workCalls{0};
    std::atomic<long long> totalWorkUs{0};
    std::atomic<bool> ranOffMainThread{false};
    std::atomic<bool> busy{false};
    std::thread::id rtThread{};
    std::thread::id mainThread{std::this_thread::get_id()};

    static LV2_Worker_Status schedule(void* handle, uint32_t size, const void* data) {
        auto* self = static_cast<WorkerStub*>(handle);
        std::lock_guard<std::mutex> lock(self->mutex);
        const auto* bytes = static_cast<const char*>(data);
        self->inbox.emplace_back(bytes, bytes + size);
        self->cv.notify_one();
        return LV2_WORKER_SUCCESS;
    }

    static LV2_Worker_Status respond(void* handle, uint32_t size, const void* data) {
        auto* self = static_cast<WorkerStub*>(handle);
        std::lock_guard<std::mutex> lock(self->mutex);
        const auto* bytes = static_cast<const char*>(data);
        self->responses.emplace_back(bytes, bytes + (size ? size : 0));
        return LV2_WORKER_SUCCESS;
    }

    void start() {
        iface = static_cast<const LV2_Worker_Interface*>(
            desc->extension_data(LV2_WORKER__interface));
        if (!iface) { std::fprintf(stderr, "plugin nao expoe lv2:worker\n"); return; }
        thread = std::thread([this] {
            ranOffMainThread.store(std::this_thread::get_id() != mainThread);
            for (;;) {
                std::vector<char> msg;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [this] { return stop || !inbox.empty(); });
                    if (stop && inbox.empty()) return;
                    msg = std::move(inbox.front());
                    inbox.pop_front();
                }
                const auto t0 = std::chrono::steady_clock::now();
                busy.store(true);
                iface->work(instance, &WorkerStub::respond, this,
                             static_cast<uint32_t>(msg.size()), msg.data());
                busy.store(false);
                totalWorkUs.store(totalWorkUs.load() + static_cast<long long>(
                    std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - t0).count()));
                workCalls.fetch_add(1);
            }
        });
    }

    void drain() {
        for (int spin = 0; spin < 8000; ++spin) {
            bool waiting;
            {
                std::lock_guard<std::mutex> lock(mutex);
                waiting = !inbox.empty();
            }
            if (!waiting && !busy.load()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void pumpResponses() {
        std::deque<std::vector<char>> local;
        {
            std::lock_guard<std::mutex> lock(mutex);
            local.swap(responses);
        }
        for (auto& r : local) {
            iface->work_response(instance, static_cast<uint32_t>(r.size()),
                                 r.empty() ? nullptr : r.data());
        }
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
            cv.notify_one();
        }
        if (thread.joinable()) thread.join();
    }
};

// ---------------- MIDI ----------------
std::vector<uint8_t> makeMidiBuffer(size_t bytes) { return std::vector<uint8_t>(bytes, 0); }

// Escreve um evento MIDI na atom sequence do bloco.
void putMidi(LV2_Atom_Forge& forge, LV2_URID midi_urid, int32_t frame,
             uint8_t b0, uint8_t b1, uint8_t b2) {
    uint8_t msg[3] = { b0, b1, b2 };
    lv2_atom_forge_frame_time(&forge, frame);
    lv2_atom_forge_atom(&forge, sizeof(msg), midi_urid);
    lv2_atom_forge_write(&forge, msg, sizeof(msg));
}

struct Result {
    double rms = 0.0;
    double rmsTail = 0.0;     // ultimo quarto da fase: o que soba depois da cauda
    double maxBlockMs = 0.0;
    // O orçamento e o p99.9, nao o maximo: um bloco isolado atrasado e page cache ou
    // outro processo disputando o disco, e nao o plugin. Foi a licao do teste I, que
    // reprovou uma execucao 0,155 ms em tudo o mais por causa de um unico bloco de
    // 2,192 ms. B/D/E/H estavam assentando no maximo puro e flacavam pelo mesmo
    // motivo: a mesma build, sem nenhuma mudanca, deu maxBlock 0,147 ms e 1,0+ ms em
    // execucoes seguidas.
    double p999BlockMs = 0.0;
    // Com poucas centenas de blocos, p99.9 E o pior bloco: o percentil sozinho nao diz
    // se o atraso e sistemico ou um evento unico. As tres linhas abaixo separam isso,
    // e as janelas de B/D/H foram alargadas (2000-3000 blocos) exatamente para que o
    // p99.9 tenha amostras suficientes para ser distribuicao e não moeda.
    double p50BlockMs = 0.0;
    double p90BlockMs = 0.0;
    double p99BlockMs = 0.0;
    int lateBlocks = 0;          // blocos acima de 1 ms
    int firstLateBlock = -1;     // posicao do pior: diz SE e a troca ou e ruina do disco
    // Falhas de pagina do proprio thread de audio no trecho. Depois de publicar um
    // banco de 1,2 GB o primeiro acorde toca pagina nunca vista, e cada fault e
    // tempo de kernel dentro do prazo do bloco -- o que separa "o plugin demora" de
    // "o kernel demora por causa do plugin".
    long minFaults = 0;
    int blocks = 0;
    // Saidas de estado (portas 22/23) visto no ultimo bloco da fase.
    float bankState = 0.0f;
    float presetCount = 0.0f;
    bool sawLoading = false;
};

} // namespace

// RSS de verdade, lida de dentro do processo. Poll externo perde o pico: o load
// inteiro de um banco cabe entre duas amostragens de um loop de 50 ms.
long memKb(const char* key) {
#if defined(__linux__)
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    const size_t klen = std::strlen(key);
    long value = 0;
    while (std::fgets(line, sizeof line, f))
        if (!std::strncmp(line, key, klen) && line[klen] == ':')
            std::sscanf(line + klen + 1, "%ld", &value);
    std::fclose(f);
    return value;
#else
    // Fora do Linux nao ha /proc/self/status. No macOS existe ru_maxrss (pico, em
    // BYTES) e mach_task_basic_info (RSS), mas a unidade e outra e nao ha como
    // conferir aqui: -1 diz "nao medido", e o teste assume o [SKIP]. Assentar em 0
    // faria a assercao de orcamento passar sem medir nada, que e o erro do
    // file_size de arquivo inexistente repetido em outro sitio.
    (void)key;
    return -1;
#endif
}

long rssKb() { return memKb("VmRSS"); }

// Pico do processo. Monotico, por isso so preza como assercao ABSOLUTA (no fim do
// main, com um orcamento de linha de base + 2x o maior arquivo): o delta dentro do
// teste J nao distingue as duas estrategias, porque os swaps dos testes A-I ja
// assentaram o HWM antes de J comecar.
long hwmKb() { return memKb("VmHWM"); }

// Page fault menores do processo. E DIAGNOSTICO, nao assercao: existe porque o teste
// D imprimia "faults=0/1" para matar a hipotese de "tempestade de page fault no
// run()" depois de publicar um banco de 1,2 GB. No Windows nao ha getrusage, e ai o
// numero vira -1 e a linha diz "n/d" em vez de mostrar um zero inventado.
long minorFaults() {
#if defined(_WIN32)
    return -1;
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt;
#endif
}


// Diretorio do sandbox de ~/.config. Nem mkdtemp nem "/tmp" existem nos tres SOs,
// e o CI roda esta suite em Windows e macOS tambem: temp_directory_path() + pid e
// portavel. Se o diretorio ja existe (pid reaproveitado) a suite reusa: o registro
// e reescrito a cada execucao, entao nada passa de uma para a outra.
std::string makeSandbox() {
    std::error_code ec;
    auto base = std::filesystem::temp_directory_path(ec);
    if (ec) base = std::filesystem::path(".");
#if defined(_WIN32)
    const unsigned pid = unsigned(_getpid());
#else
    const unsigned pid = unsigned(getpid());
#endif
    const std::filesystem::path dir = base / ("pforge_host_" + std::to_string(pid));
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir)) return {};
    return dir.string();
}

void setEnvVar(const char* key, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}

int main(int argc, char** argv) {
    std::string soPath = "build/pianoforge.lv2/pianoforge.so";
    int bankValue = -1;
    int makeBanks = 0;
    std::vector<std::string> sf2s;
    std::vector<std::string> bads;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--so" && i + 1 < argc) soPath = argv[++i];
        else if (a == "--bank-value" && i + 1 < argc) bankValue = std::stoi(argv[++i]);
        else if (a == "--sf2" && i + 1 < argc) sf2s.push_back(argv[++i]);
        else if (a == "--bad" && i + 1 < argc) bads.push_back(argv[++i]);
        // --makesf2 N: gera N bancos SINTETICOS (tools/SynthBank.hpp) e registra-os.
        // E o que o CI usa: soundfont gravado tem licenca, e nada de licenca duvidosa
        // entra no repositorio nem no artefato. Um banco a mais faz o teste D trocar
        // de verdade, e nao por ele mesmo.
        else if (a == "--makesf2" && i + 1 < argc) makeBanks = std::max(1, std::stoi(argv[++i]));
    }

    const std::string sandbox = makeSandbox();
    if (sandbox.empty()) {
        std::fprintf(stderr, "sem diretorio temporario para o sandbox de ~/.config\n");
        return 2;
    }
    const std::string cfgDir = sandbox + "/.config/pianoforge";
    {
        std::error_code ec;
        std::filesystem::create_directories(cfgDir, ec);
        if (!std::filesystem::is_directory(cfgDir)) {
            std::fprintf(stderr, "nao criou %s\n", cfgDir.c_str());
            return 2;
        }
    }
    for (int n = 0; n < makeBanks; ++n) {
        const std::string path = sandbox + "/synth_" + std::to_string(n) + ".sf2";
        std::string err;
        if (!psynth::buildIndexed(path, n, err)) {
            std::fprintf(stderr, "[makesf2] %s\n", err.c_str());
            return 2;
        }
        std::error_code ec;
        const auto sz = std::filesystem::file_size(path, ec);
        std::printf("[makesf2] %s  (%.1f MB, %d preset(s))\n", path.c_str(),
                    ec ? 0.0 : double(sz) / 1048576.0, psynth::Config{}.presets);
        sf2s.push_back(path);
    }
    // Sem banco a suíte para de existir: B/D/E/H/I/J/K/L/M precisam de um piano para
    // escolher. Rodar mesmo assim daria uma metade de [SKIP] e um "TUDO OK" no fim --
    // a mesma mentira de um [PASS] que nao testou nada, por isso e recusa, nao aviso.
    if (sf2s.empty()) {
        std::fprintf(stderr,
                     "nenhum banco registrado: passe --sf2 <caminho> (repetivel) ou "
                     "--makesf2 <N> para gerar N bancos sintetico\n");
        return 2;
    }

    // Registro: os bancos viram o sandbox de ~/.config (o teste nunca toca no config
    // real de quem roda).
    {
        std::ofstream list(cfgDir + "/soundfonts.list");
        if (!list) { std::fprintf(stderr, "nao escreveu o registro\n"); return 2; }
        for (const auto& s : sf2s) list << s << "\n";
        for (const auto& s : bads) list << s << "\n";
        list.close();
        setEnvVar("XDG_CONFIG_HOME", sandbox + "/.config");
        setEnvVar("XDG_DATA_HOME", sandbox + "/.local/share");
    }

    void* lib = dlopen(soPath.c_str(), RTLD_NOW);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto getDescriptor = reinterpret_cast<const LV2_Descriptor* (*)(uint32_t)>(
        dlsym(lib, "lv2_descriptor"));
    const LV2_Descriptor* desc = getDescriptor ? getDescriptor(0) : nullptr;
    if (!desc) { std::fprintf(stderr, "sem lv2_descriptor\n"); return 2; }

    // ---- portas ----
    static float outL[4096], outR[4096];
    static float controls[19];
    // 22 = bankState, 23 = presetCount. Sao saidas: o host so empresta memoria.
    static float statusPorts[2]{};   // 22 bankState, 23 presetCount (saidas)
    for (int i = 0; i < pianoforge::Param_Count; ++i)
        controls[i] = pianoforge::kParameterDefs[i].defaultValue;
    controls[pianoforge::Param_Count + 0] = static_cast<float>(bankValue);   // soundbank
    controls[pianoforge::Param_Count + 1] = 0.0f;                            // preset

    std::vector<uint8_t> midiBuf = makeMidiBuffer(8192);
    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &g_map);
    const LV2_URID midiUrid = uridMap(nullptr, LV2_MIDI__MidiEvent);
    const LV2_URID seqUrid = uridMap(nullptr, LV2_ATOM__Sequence);

    WorkerStub worker;
    LV2_Worker_Schedule scheduleFeature = { &worker, &WorkerStub::schedule };
    LV2_Feature featMap = { const_cast<char*>(LV2_URID__map), &g_map };
    LV2_Feature featWorker = { const_cast<char*>(LV2_WORKER__schedule), &scheduleFeature };
    const LV2_Feature* features[3] = { &featMap, &featWorker, nullptr };

    LV2_Handle inst = desc->instantiate(desc, 44100.0, "build/pianoforge.lv2", features);
    if (!inst) { std::fprintf(stderr, "instantiate falhou\n"); return 2; }
    // Linha de base do pico, ANTES de qualquer banco: o orcamento de memoria do fim
    // do processo e isso + 2x o maior arquivo. Um teto ABSOLUTO so em 2x o banco
    // funcionava com os pianos gravados (1,2 GB contra uma linha de base de ~4 MB) e
    // reprovaria com um banco sintetico de 4 MB, cujo 2x nao paga nem o processo.
    const long entryHwm = hwmKb();
    worker.instance = inst;
    worker.desc = desc;

    desc->connect_port(inst, 0, outL);
    desc->connect_port(inst, 1, outR);
    desc->connect_port(inst, 2, midiBuf.data());
    for (int i = 0; i < 19; ++i) desc->connect_port(inst, 3 + i, &controls[i]);
    desc->connect_port(inst, pianoforge::kPortBankState, &statusPorts[0]);
    desc->connect_port(inst, pianoforge::kPortPresetCount, &statusPorts[1]);

    worker.start();
    desc->activate(inst);
    const auto rtThread = std::this_thread::get_id();
    worker.rtThread = rtThread;

    auto runBlocks = [&](int count, bool withNotes, int startNote) -> Result {
        Result res;
        double sumSq = 0.0, sumSqTail = 0.0;
        size_t samples = 0, samplesTail = 0;
        const int tailFrom = count * 3 / 4;
        std::vector<double> tempos;
        tempos.reserve(size_t(count));
        const long faultsAntes = minorFaults();
        for (int b = 0; b < count; ++b) {
            lv2_atom_forge_set_buffer(&forge, midiBuf.data(), midiBuf.size());
            LV2_Atom_Forge_Frame frame;
            lv2_atom_forge_sequence_head(&forge, &frame, seqUrid);
            if (withNotes) {
                // Uma nota sustentada a cada 40 blocos (~0,46 s): notas de um bloco
                // so dariam RMS de transitorio, nao de piano.
                const int note = startNote + (b / 40 % 6) * 5;
                if (b % 40 == 0) putMidi(forge, midiUrid, 0, 0x90, uint8_t(note), 100);
                if (b % 40 == 39) putMidi(forge, midiUrid, 500, 0x80, uint8_t(note), 64);
            }
            lv2_atom_forge_pop(&forge, &frame);

            const auto t0 = std::chrono::steady_clock::now();
            desc->run(inst, 512);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count();
            res.maxBlockMs = std::max(res.maxBlockMs, ms);
            if (ms > 1.0) {
                ++res.lateBlocks;
                if (res.firstLateBlock < 0) res.firstLateBlock = b;
            }
            tempos.push_back(ms);
            ++res.blocks;

            // O load acontece AQUI, fora do trecho cronometrado: e assim que se
            // separa "o worker trabalhou 300 ms" de "o run() estourou o bloco".
            worker.drain();
            worker.pumpResponses();

            for (int i = 0; i < 512; ++i) {
                const double v = outL[i];
                sumSq += v * v;
                samples += 1;
                if (b >= tailFrom) { sumSqTail += v * v; samplesTail += 1; }
            }
        }
        res.rms = samples ? std::sqrt(sumSq / double(samples)) : 0.0;
        res.rmsTail = samplesTail ? std::sqrt(sumSqTail / double(samplesTail)) : 0.0;
        const long faultsDepois = minorFaults();
        res.minFaults = (faultsAntes < 0 || faultsDepois < 0)
                            ? -1
                            : faultsDepois - faultsAntes;
        std::sort(tempos.begin(), tempos.end());
        const auto pct = [&](double q) {
            return tempos.empty() ? 0.0 : tempos[size_t(tempos.size() * q)];
        };
        res.p999BlockMs = pct(0.999);
        res.p99BlockMs = pct(0.99);
        res.p90BlockMs = pct(0.90);
        res.p50BlockMs = pct(0.50);
        res.lateBlocks = int(std::count_if(tempos.begin(), tempos.end(),
                                           [](double ms) { return ms > 1.0; }));
        return res;
    };

    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s  %s\n", ok ? "[PASS]" : "[FALHA]", what);
        if (!ok) ++failures;
    };
    // Uma assercao que NAO PODE rodar e [SKIP], com o motivo -- nunca [PASS]. No CI
    // nao ha soundfont para registrar nem /proc para medir RAM, e um "TUDO OK"
    // construido com verdades que nao foram testadas e pior que um falho: da
    // licenca para publicar o binario sem olhar.
    auto skip = [&](const char* what, const char* why) {
        std::printf("[SKIP]  %s (%s)\n", what, why);
    };

    // Espera o plugin chegar num codigo da porta 22, dando tempo de PAREDE ao disco.
    // Um numero FIXO de blocos nao faz isso: com o cache frio, 400 blocos (~200 ms de
    // run) e menos que os 1,4 s que um SF2 de 49 MB leva, e entao D/H/B reprovavam por
    // velocidade do disco, nao pelo plugin (a mesma build passou com load de 246 ms).
    // Nem 40.000 blocos chegam para um SF2 de 611 MB -- o teto agora e em segundos, e
    // um load que nunca termina continua reprovando, so que depois de 90 s de parede.
    // O cochilo de 1 ms por iteracao existe porque sem ele o harness giria a 100% de
    // CPU em cima do proprio disco que o worker esta tentando ler.
    // A assercao de custo de bloco continua medindo a JANELA DO LOAD, separada da
    // medicao de som -- ver o comentario em runBlocks.
    auto aguardaEstado = [&](float want, double tetoSeg) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::duration<double>(tetoSeg));
        int blocos = 0;
        while (statusPorts[0] != want && std::chrono::steady_clock::now() < deadline) {
            runBlocks(1, false, 0);
            ++blocos;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return blocos;
    };

    // ---- Teste A: nada e puxado sozinho ----
    Result a = runBlocks(120, true, 48);
    std::printf("A: rms=%.9f workCalls=%d maxBlock=%.3fms\n",
                a.rms, worker.workCalls.load(), a.maxBlockMs);
    check(a.rms == 0.0, "com soundbank=-1 o plugin fica em silencio");
    check(worker.workCalls.load() == 0, "nenhum load foi agendado sem pedido do usuario");

    // ---- Teste B: escolher carrega ----
    // Fase 1 cronometra o run(); fase 2 espera o piano estar no ar e so allora mede
    // som. 3000 blocos, nao 600: com 600 o "p99,9" e a segunda pior amostra da serie,
    // ou seja, uma moeda -- reprovou uma execucao que estava 0,083 ms em tudo o mais.
    // E quem mede o run() COM O LOAD NO AR e o teste I (la o worker nao e drenado entre
    // blocos); aqui o load cai inteiro no drain, que esta fora do trecho cronometrado,
    // entao o que B afirma e "o custo de bloco normal nao degrada quando um load foi
    // pedido", com folga estatistica de 3 blocos.
    controls[pianoforge::Param_Count] = 0.0f;      // piano registrado 0
    Result bLoad = runBlocks(3000, false, 0);
    const int wB = aguardaEstado(0.0f, 90.0);
    Result b = runBlocks(600, true, 48);
    std::printf("B: rms=%.6f workCalls=%d loadMs=%.1f workerThread=%s "
                "p50=%.3f p90=%.3f p99=%.3f p99.9=%.3f maxBlock=%.3fms "
                "estourados=%d primeiroNoBlocos=%d blocosAtePublicar=%d\n",
                b.rms, worker.workCalls.load(), worker.totalWorkUs.load() / 1000.0,
                worker.ranOffMainThread.load() ? "separada" : "IGUAL AO RT",
                bLoad.p50BlockMs, bLoad.p90BlockMs, bLoad.p99BlockMs, bLoad.p999BlockMs,
                bLoad.maxBlockMs, bLoad.lateBlocks, bLoad.firstLateBlock, wB);
    check(b.rms > 1e-3, "depois de escolher, sai som");
    check(worker.workCalls.load() >= 1, "o load rodou no worker");
    check(worker.ranOffMainThread.load(), "o load rodou em OUTRA thread");
    check(bLoad.p999BlockMs < 1.0, "99,9% dos blocos cabaram em 1ms durante o load");

    // ---- Teste D: trocar vai e voltar ----
    const int workBefore = worker.workCalls.load();
    controls[pianoforge::Param_Count] = sf2s.size() > 1 ? 1.0f : 0.0f;
    const int wD1 = aguardaEstado(float(sf2s.size() > 1 ? 1 : 0), 90.0);
    Result d1 = runBlocks(2000, true, 60);
    controls[pianoforge::Param_Count] = 0.0f;
    const int wD2 = aguardaEstado(0.0f, 90.0);
    Result d2 = runBlocks(2000, true, 60);
    std::printf("D: rms=%.6f/%.6f p99.9=%.3fms/%.3fms maxBlock=%.3fms/%.3fms "
                "workCalls=%d->%d blocos=%d/%d faults=%ld/%ld\n",
                d1.rms, d2.rms, d1.p999BlockMs, d2.p999BlockMs, d1.maxBlockMs, d2.maxBlockMs,
                workBefore, worker.workCalls.load(), wD1, wD2,
                d1.minFaults, d2.minFaults);
    check(d1.rms > 1e-3 && d2.rms > 1e-3, "som continua depois de trocar na volta");
    check(std::max(d1.p999BlockMs, d2.p999BlockMs) < 1.0,
          "swap A->B->A nunca bloqueou o run()");

    // ---- Teste H: escolhe um banco invalido, o que esta tocando continua ----
    // O load que falha nao pode publicar nada nem silenciar o plugin: o usuario
    // clicou num arquivo quebrado, nao no botao de mudo.
    if (!bads.empty()) {
        const int badIndex = int(sf2s.size());
        controls[pianoforge::Param_Count] = float(badIndex);
        Result hLoad = runBlocks(2000, false, 0);
        const int wH = aguardaEstado(-4.0f, 30.0);
        Result h = runBlocks(400, true, 60);
        std::printf("H: rms=%.6f p99.9=%.3fms maxBlock=%.3fms workCalls=%d blocosAteFalha=%d\n",
                    h.rms, hLoad.p999BlockMs, hLoad.maxBlockMs, worker.workCalls.load(), wH);
        check(h.rms > 1e-3, "banco invalido nao troca o piano que esta tocando");
        check(hLoad.p999BlockMs < 1.0, "a recusa do banco invalido nao pesou no run()");
    }

    // ---- Teste K: pedal de sustain ----
    // "corta o som quando seguro o pedal" tem duas leituras, e o teste fecha as duas:
    // (a) com CC64=127 a nota TEM de continuar soando depois do note-off; (b) soltar o
    // pedal TEM de abafar o que estava segurado. Medir so (a) nao provaria nada, porque
    // reverb e cordas simpaticas dao cauda mesmo sem pedal -- o que se exige e que a
    // cauda com pedal seja muito maior que a cauda sem pedal, na MESMA nota, no MESMO
    // instante depois do note-off.
    {
        struct Ev { int bloco; int b0; int b1; int b2; };
        auto ciclo = [&](const std::vector<Ev>& eventos, int total, int desde) -> double {
            double sq = 0.0;
            long n = 0;
            for (int b = 0; b < total; ++b) {
                lv2_atom_forge_set_buffer(&forge, midiBuf.data(), midiBuf.size());
                LV2_Atom_Forge_Frame fr;
                lv2_atom_forge_sequence_head(&forge, &fr, seqUrid);
                for (const Ev& e : eventos)
                    if (e.bloco == b) putMidi(forge, midiUrid, 0, uint8_t(e.b0),
                                              uint8_t(e.b1), uint8_t(e.b2));
                lv2_atom_forge_pop(&forge, &fr);
                desc->run(inst, 512);
                worker.drain();
                worker.pumpResponses();
                if (b >= desde)
                    for (int i = 0; i < 512; ++i) { sq += double(outL[i]) * outL[i]; ++n; }
            }
            return n ? std::sqrt(sq / double(n)) : 0.0;
        };

        const int nota = 45;      // grave: e onde a cauda de um piano e longa
        const int desde = 60;     // comeca a medir 1,4 s depois, com a tecla ja solta
        const int janela = 400;   // ~4,6 s de janela

        // "o piano esta vivo" medido com a tecla ABAIXADA, nao na cauda: comparar
        // caudas de duas execucoes e comparar dois numeros pequenos, e isso nao prova
        // nada. A nota segurada no dedo e o mesmo nivel antes e depois de qualquer
        // pedal -- se ela cair, foi o pedal que estragou alguma coisa.
        auto notaSegurada = [&](int key) {
            return ciclo({{0, 0x90, key, 110}, {58, 0x80, key, 64}}, 60, 6);
        };

        // Estado limpo: pedal solto, nota solta, e espera a cauda morrer.
        ciclo({{0, 0xB0, 64, 0}, {1, 0x80, nota, 64}}, 900, 900);
        const double vivaAntes = notaSegurada(nota + 12);
        ciclo({{0, 0x80, nota + 12, 64}}, 300, 300);
        const double semPedal = ciclo({{0, 0x90, nota, 110}, {4, 0x80, nota, 64}},
                                      janela, desde);
        ciclo({{0, 0x80, nota, 64}, {1, 0xB0, 64, 0}}, 900, 900);
        const double comPedal = ciclo({{0, 0xB0, 64, 127}, {2, 0x90, nota, 110},
                                       {6, 0x80, nota, 64}}, janela, desde);
        const double solto = ciclo({{0, 0xB0, 64, 0}}, 120, 20);
        const double vivaDepois = notaSegurada(nota + 12);

        std::printf("K: nota %d, medindo do bloco %d -- sem pedal=%.6f com pedal=%.6f "
                    "(%.1fx) solto o pedal=%.6f  tecla abaixada antes=%.6f depois=%.6f\n",
                    nota, desde, semPedal, comPedal,
                    semPedal > 1e-9 ? comPedal / semPedal : 0.0, solto,
                    vivaAntes, vivaDepois);
        check(comPedal > semPedal * 2.0,
              "com o pedal pisado a nota continua depois de soltar a tecla");
        check(solto < comPedal * 0.5,
              "soltar o pedal abafa o que estava segurado");
        // Soltar o pedal nao pode calar o plugin inteiro: o que estava segurado
        // abafa, mas a nota TOCADA DEPOIS continua viva.
        check(vivaDepois > vivaAntes * 0.7,
              "soltar o pedal nao desliga o piano");
    }

    // ---- Teste L: polifonia com o pedal segurado ----
    // O pedal nao deixa a voz entrar em RELEASE: tsf_channel_note_off com sustain
    // so marca heldSustain (tsf.h:1962), e o unico criterio de roubo do tsf com teto
    // de vozes e "a voz mais avancada em release" (tsf.h:1580-1598). Se nenhuma esta
    // em release, o caminho e `continue`: a nota NOVA e descartada sem aviso. Com
    // kMaxVoices=128 e ~4 vozes por nota (pforge_dsp_stages --notes 32 entrega
    // [vozes ativas: 128]), o teto chega em ~32 notas pedalizadas -- e e exatamente
    // "corta o som quando seguro o pedal".
    //
    // Medida: a MESMA sequencia renderizada com e sem uma tecla. Uma janela que comeca
    // um bloco depois do ataque e termina antes do proximo ataque nao contem nenhum
    // outro evento, entao "nota que entra" = sinal diferente, e "nota descartada" =
    // sinal BIT A BIT igual. Ratios de RMS antes/depois nao servem: a textura decai
    // sozinha e o clipper comprime, e os dois mascaram o que se quer ver.
    {
        struct Nota { int on; int off; int key; };
        const int kNotas = 40;
        const int kPasso = 8;                 // ~0,085 s entre ataques
        std::vector<Nota> notas;
        notas.reserve(kNotas);
        for (int i = 0; i < kNotas; ++i)
            notas.push_back({4 + i * kPasso, 4 + i * kPasso + 6, 36 + i});
        const int total = notas.back().on + 30;
        const int kReset = 120;               // ~1,3 s: CC120 mata as vozes numa passada

        // Volume baixo e SEM cordas simpaticas, reverb, chorus e MARTETO: os tres
        // primeiros tem memoria longa (bancos de ressonador de Q alto e atrasos
        // realimentados) e um resto de uma renderizacao anterior nao e zerado por
        // CC120 -- foi isso que deu 0,076 dB de "piso" entre duas renderizacoes
        // identicas. O que se mede aqui e alocacao de voz no sampler, e os tres
        // estagios nao participam disso desligados.
        //
        // O martelo sai por outro motivo, e ele e uma licao do banco sintetico: o
        // clique e ruido SORTEADO por disparo (HammerModel rngTick) e ate aqui ele
        // era mascarado pelo corpo da nota. Com o platô do envelope 22 dB abaixo do
        // ataque (decaySec/sustainDb de tools/SynthBank.hpp), o clique passa a
        // DOMINAR a janela e o piso entre duas renderizacoes identicas foi de 0,08 a
        // 0,84 dB -- mais que o ganho de uma tecla inteira, ou seja: o metodo
        // differential parava de separar. Desligar o martelo nao e trapacear a
        // medida, e tirar da janela o unico estagio que sorteia ruido.
        const float volAntes = controls[pianoforge::Param_Volume];
        const float sympAntes = controls[pianoforge::Param_SympRes];
        const float revAntes = controls[pianoforge::Param_Reverb];
        const float chorAntes = controls[pianoforge::Param_Chorus];
        const float hammAntes = controls[pianoforge::Param_Hammer];
        controls[pianoforge::Param_Volume] = 0.20f;
        controls[pianoforge::Param_SympRes] = 0.0f;
        controls[pianoforge::Param_Reverb] = 0.0f;
        controls[pianoforge::Param_Chorus] = 0.0f;
        controls[pianoforge::Param_Hammer] = 0.0f;

        auto render = [&](int omit) {
            // Duas passadas de limpeza: a segunda comeca com o estado que a primeira
            // deixou, entao o que entra na sequencia e silencio de verdade nas duas.
            for (int passagem = 0; passagem < 2; ++passagem)
                for (int b = 0; b < kReset; ++b) {
                    lv2_atom_forge_set_buffer(&forge, midiBuf.data(), midiBuf.size());
                    LV2_Atom_Forge_Frame fr;
                    lv2_atom_forge_sequence_head(&forge, &fr, seqUrid);
                    if (b == 0) { putMidi(forge, midiUrid, 0, 0xB0, 120, 127);   // all sound off
                                  putMidi(forge, midiUrid, 1, 0xB0, 64, 0); }    // pedal solto
                    lv2_atom_forge_pop(&forge, &fr);
                    desc->run(inst, 512);
                    worker.drain();
                    worker.pumpResponses();
                }
            std::vector<double> v(size_t(total), 0.0);
            for (int b = 0; b < total; ++b) {
                lv2_atom_forge_set_buffer(&forge, midiBuf.data(), midiBuf.size());
                LV2_Atom_Forge_Frame fr;
                lv2_atom_forge_sequence_head(&forge, &fr, seqUrid);
                if (b == 0) putMidi(forge, midiUrid, 0, 0xB0, 64, 127);      // pisa e nao solta
                for (int i = 0; i < kNotas; ++i) {
                    if (i == omit) continue;
                    if (notas[i].on == b) putMidi(forge, midiUrid, 0, 0x90, uint8_t(notas[i].key), 72);
                    if (notas[i].off == b) putMidi(forge, midiUrid, 0, 0x80, uint8_t(notas[i].key), 64);
                }
                if (b == total - 1) putMidi(forge, midiUrid, 0, 0xB0, 64, 0);
                lv2_atom_forge_pop(&forge, &fr);
                desc->run(inst, 512);
                worker.drain();
                worker.pumpResponses();
                double sq = 0.0;
                for (int i = 0; i < 512; ++i) sq += double(outL[i]) * outL[i];
                v[size_t(b)] = std::sqrt(sq / 512.0);
            }
            return v;
        };

        const std::vector<double> ref = render(-1);
        const std::vector<double> ref2 = render(-1);
        // PISO DA MEDIDA: duas renderizacoes da MESMA sequencia nao sao bit-identicas,
        // e a causa e propria do modelo de martelo: rngTick avanca por disparo
        // (HammerModel.hpp:202) e o ruido de cada ataque depende dele. A segunda
        // renderizacao chega com a historia de disparos da primeira, entao todos os
        // cliques sao outra realizacao. O que se compara e a janela de cada tecla, e e
        // la que o piso tem que ser medido -- nao em bloco isolado, onde o fundo de um
        // silencio relativo daria um numero enorme e sem significado.
        double piorJanela = 0.0;
        int janelaPior = -1;
        for (int i = 0; i < kNotas; ++i) {
            double sqA = 0.0, sqB = 0.0;
            int n = 0;
            for (int b = notas[i].on + 1; b < notas[i].on + kPasso - 1; ++b) {
                sqA += ref[size_t(b)] * ref[size_t(b)];
                sqB += ref2[size_t(b)] * ref2[size_t(b)];
                ++n;
            }
            const double a = std::sqrt(sqA / n), c = std::sqrt(sqB / n);
            const double rel = a > 0.0 ? std::abs(a - c) / a : 0.0;
            if (rel > piorJanela) { piorJanela = rel; janelaPior = i + 1; }
        }
        const double pisoDb = 20.0 * std::log10(1.0 + piorJanela);
        // Limiar = 3x o piso, com um chao de 0,05 dB. Fixar um numero absoluto aqui
        // seria torcer para o ruido continuar pequeno; o piso medido e que decide.
        const double limiarDb = std::max(0.05, 3.0 * pisoDb);
        std::printf("   piso das janelas=%.3f dB (tecla %d) -> limiar=%.3f dB\n",
                    pisoDb, janelaPior, limiarDb);
        // Se o piso passar de 0,2 dB a diferenca entre duas renderizacoes ja e da ordem
        // do ganho de uma tecla e a medida perde poder de separar. Guarda do metodo, nao
        // do plugin.
        check(pisoDb < 0.2, "o metodo diferencial tem resolucao para separar uma tecla do ruido");

        std::vector<double> ganhoDb(size_t(kNotas), 0.0);
        int primeiraMuda = -1;
        double menorGain = 999.0;
        for (int i = 0; i < kNotas; ++i) {
            const std::vector<double> sem = render(i);
            double sqRef = 0.0, sqSem = 0.0;
            int n = 0;
            for (int b = notas[i].on + 1; b < notas[i].on + kPasso - 1; ++b) {
                sqRef += ref[size_t(b)] * ref[size_t(b)];
                sqSem += sem[size_t(b)] * sem[size_t(b)];
                ++n;
            }
            const double com = std::sqrt(sqRef / n), semN = std::sqrt(sqSem / n);
            ganhoDb[size_t(i)] = (com > 0.0 && semN > 0.0)
                                     ? 20.0 * std::log10(com / semN)
                                     : (com > semN ? 99.0 : 0.0);
            menorGain = std::min(menorGain, ganhoDb[size_t(i)]);
            // Tecla que entra soma decibeis acima do piso medido acima; tecla
            // descartada e o mesmo sinal, muda so o ruido do martelo das proximas.
            if (primeiraMuda < 0 && ganhoDb[size_t(i)] < limiarDb) primeiraMuda = i;
        }
        std::printf("L: pedal segurado, %d teclas -- ganho que cada uma soma (dB), limiar=%.2f dB:\n",
                    kNotas, limiarDb);
        for (int i = 0; i < kNotas; i += 5) {
            std::printf("   ");
            for (int j = i; j < std::min(i + 5, kNotas); ++j)
                std::printf(" t%d=%5.2f", j + 1, ganhoDb[size_t(j)]);
            std::printf("\n");
        }
        std::printf("   menor ganho=%.3f dB | primeira tecla muda: %s (teto de vozes do Engine, ~3 regioes por tecla)\n",
                    menorGain,
                    primeiraMuda < 0 ? "nenhuma" : std::to_string(primeiraMuda + 1).c_str());
        check(primeiraMuda < 0, "com o pedal segurado nenhuma tecla e descartada em silencio");

        // ---- Teste M: o pool SATURADO, e a nota nova mesmo assim ----
        //
        // L prova 40 teclas com o pool folgado, que e o caso do musico. M prova o
        // caso que o teto aperta de verdade: primeiro se enche o pool com ~250
        // teclas PEDALIZADAS e MOIDAS (velocidade 14/127), e so depois entram 12
        // teclas FORTE (110/127) em chaves nunca tocadas. A afirmacao e que cada
        // uma dessas 12 faz som -- o "corta o som quando seguro o pedal" e
        // exatamente a ausencia dele.
        //
        // Por que o desenho com fundo MOIDO e necessario: com o pool estourado e
        // tudo na mesma altura, omitir uma tecla muda QUAL corda o amortecedor
        // derruba nas teclas seguintes, e as duas renderizacoes deixam de ser "a
        // mesma textura com uma nota a menos". Medido numa versao anterior: o delta
        // da tecla 189 saiu NEGATIVO (-0,67 dB -- tirar a nota deixou a janela mais
        // alta). Com o fundo 30 dB mais baixo que as teclas testadas, a nota testada
        // domina a janela e a troca de vitimas do abafador nao move o nivel.
        //
        // A saturacao em si e provada por outra via: `pforge_dsp_stages --pedalload`
        // conta no proprio backend quantas regioes casaram sem conseguir slot
        // (pfDroppedRegions) e quantas notas entraram mudas (pfMuteNotes), e da
        // pico de vozes = teto em 128/192/256/384.
        //
        // A segunda afirmacao e do abafador: soltar o pedal tem de derrubar a
        // textura. Sem isso o "sustain correto" virou "o piano nunca mais para".
        {
            struct NotaM { int on; int off; int key; int vel; };
            const int kEncher = 250;              // teclas veladas que lotam o pool
            const int kTestes = 12;               // teclas forte que tem de soar
            std::vector<NotaM> notasM;
            notasM.reserve(kEncher + kTestes);
            for (int i = 0; i < kEncher; ++i) {
                const int on = 4 + i * 3;
                notasM.push_back({on, on + 2, 24 + 2 * (i % 37), 14});
            }
            const int kComecoTeste = notasM.back().on + 30;
            const int kPassoM = 12;               // ~0,128 s entre ataques fortes
            for (int i = 0; i < kTestes; ++i) {
                const int on = kComecoTeste + i * kPassoM;
                notasM.push_back({on, on + 4, 25 + 2 * i, 110});   // chaves impares, virgens
            }
            const int totalM = notasM.back().on + 700;   // ~15 s de cauda: ver as tres janelas
            const int kLevante = notasM.back().on + 40;  // bloco em que o pedal sobe

            auto renderM = [&](int omit, bool soltaPedal) {
                for (int passagem = 0; passagem < 2; ++passagem)
                    for (int b = 0; b < kReset; ++b) {
                        lv2_atom_forge_set_buffer(&forge, midiBuf.data(), midiBuf.size());
                        LV2_Atom_Forge_Frame fr;
                        lv2_atom_forge_sequence_head(&forge, &fr, seqUrid);
                        if (b == 0) { putMidi(forge, midiUrid, 0, 0xB0, 120, 127);
                                      putMidi(forge, midiUrid, 1, 0xB0, 64, 0); }
                        lv2_atom_forge_pop(&forge, &fr);
                        desc->run(inst, 512);
                        worker.drain();
                        worker.pumpResponses();
                    }
                std::vector<double> v(size_t(totalM), 0.0);
                for (int b = 0; b < totalM; ++b) {
                    lv2_atom_forge_set_buffer(&forge, midiBuf.data(), midiBuf.size());
                    LV2_Atom_Forge_Frame fr;
                    lv2_atom_forge_sequence_head(&forge, &fr, seqUrid);
                    if (b == 0) putMidi(forge, midiUrid, 0, 0xB0, 64, 127);
                    if (soltaPedal && b == kLevante) putMidi(forge, midiUrid, 0, 0xB0, 64, 0);
                    for (size_t i = 0; i < notasM.size(); ++i) {
                        if (int(i) == omit) continue;
                        if (notasM[i].on == b)
                            putMidi(forge, midiUrid, 0, 0x90, uint8_t(notasM[i].key), uint8_t(notasM[i].vel));
                        if (notasM[i].off == b)
                            putMidi(forge, midiUrid, 0, 0x80, uint8_t(notasM[i].key), 64);
                    }
                    lv2_atom_forge_pop(&forge, &fr);
                    desc->run(inst, 512);
                    worker.drain();
                    worker.pumpResponses();
                    double sq = 0.0;
                    for (int i = 0; i < 512; ++i) sq += double(outL[i]) * outL[i];
                    v[size_t(b)] = std::sqrt(sq / 512.0);
                }
                return v;
            };

            const std::vector<double> base = renderM(-1, true);
            const std::vector<double> repete = renderM(-1, true);

            // Janela de cada tecla testada: um bloco depois do ataque ate dois blocos
            // antes do proximo ataque forte (o fundo ja esta no teto, e dentro da
            // janela nao ha outro evento).
            auto janela = [&](const std::vector<double>& s, int i, double& soma) {
                soma = 0.0; int n = 0;
                for (int b = notasM[size_t(i)].on + 1; b < notasM[size_t(i)].on + kPassoM - 2; ++b) {
                    soma += s[size_t(b)] * s[size_t(b)]; ++n;
                }
                return n ? std::sqrt(soma / n) : 0.0;
            };

            // PISO: duas renderizacoes da MESMA sequencia, nas mesmas janelas. O
            // martelo sorteia ruido por disparo, entao bit-a-bit nao vale aqui.
            double pisoM = 0.0, fundoM = 0.0;
            for (int i = kEncher; i < kEncher + kTestes; ++i) {
                double sa = 0.0, sb = 0.0;
                const double a = janela(base, i, sa), bSem = janela(repete, i, sb);
                (void)bSem;
                if (a > 0.0) pisoM = std::max(pisoM, std::fabs(a - bSem) / a);
            }
            for (int b = kComecoTeste - 20; b < kComecoTeste; ++b) fundoM += base[size_t(b)] * base[size_t(b)];
            fundoM = std::sqrt(fundoM / 20.0);
            const double pisoDb = 20.0 * std::log10(1.0 + pisoM);
            const double limiarM = std::max(0.05, 3.0 * pisoDb);
            std::printf("M: fundo com o pool cheio=%.6f | piso=%.3f dB -> limiar=%.3f dB (%d teclas veladas + %d fortes)\n",
                        fundoM, pisoDb, limiarM, kEncher, kTestes);
            check(pisoDb < 0.5, "com o fundo moido o metodo diferencial separa uma tecla do ruido");

            int primeiraMudaM = -1;
            double menorGainM = 999.0;
            std::string ganhosM;
            for (int i = kEncher; i < kEncher + kTestes; ++i) {
                const std::vector<double> sem = renderM(i, true);
                double sa = 0.0, sb = 0.0;
                const double com = janela(base, i, sa), semN = janela(sem, i, sb);
                const double g = (com > 0.0 && semN > 0.0) ? 20.0 * std::log10(com / semN)
                                                            : (com > semN ? 99.0 : 0.0);
                menorGainM = std::min(menorGainM, g);
                char buf[32];
                std::snprintf(buf, sizeof buf, " t%d=%5.1f", i - kEncher + 1, g);
                ganhosM += buf;
                if (primeiraMudaM < 0 && g < limiarM) primeiraMudaM = i;
            }
            std::printf("M:  ganho de cada tecla forte sobre o fundo estourado (dB):%s\n", ganhosM.c_str());
            std::printf("M:  menor ganho=%.3f dB | tecla muda: %s\n", menorGainM,
                        primeiraMudaM < 0 ? "nenhuma" : std::to_string(primeiraMudaM - kEncher + 1).c_str());
            check(primeiraMudaM < 0,
                  "com o pool saturado toda tecla que entra faz som naquele instante");

            // O abafador cai -- e a pergunta tem DUAS metades, cada uma na janela
            // certa. Uma janela sozinha nao distingue as duas, e foi exatamente
            // assim que o abafador de 0,55 s (um degrau) passou durante uma rodada
            // inteira: a assercao antiga media "a cauda inteira depois do lift esta
            // 20x mais quieta", e tanto um feltro de 500 ms quanto um corte de 10 ms
            // passam nela.
            //   W1 (2..16 blocos depois do pe = 0,04-0,34 s): o feltro NAO pode ter
            //      derrubado a textura -- e aqui que um corte se denuncia.
            //   W2 (16..64 = 0,34-1,36 s): o meio da queda, impresso so para leitura.
            //   W3 (180..700 = 3,8-15 s): aqui ja tem de ser po -- e a prova de que
            //      soltar o pedal abafa, e nao de que o piano virou orgao.
            struct Janela { const char* nome; int a; int b; };
            auto rmsJanela = [&](const std::vector<double>& v, int a, int b) {
                double soma = 0.0; int n = 0;
                for (int i = a; i < b && i < int(v.size()); ++i) { soma += v[size_t(i)] * v[size_t(i)]; ++n; }
                return n ? std::sqrt(soma / double(n)) : 0.0;
            };
            const std::vector<double> semSoltar = renderM(-1, false);
            std::printf("M:  janelas depois do pe (razao pedal-segurado / pedal-solto):\n");
            for (const Janela& j : { Janela{"W1 0,04-0,34 s", kLevante + 2, kLevante + 16},
                                     Janela{"W2 0,34-1,36 s", kLevante + 16, kLevante + 64},
                                     Janela{"W3 3,8-15 s",   kLevante + 180, totalM} }) {
                const double seg = rmsJanela(semSoltar, j.a, j.b);
                const double sol = rmsJanela(base, j.a, j.b);
                // "20847712720,8x" nao e uma medida, e o 1e-12 do denominador. Quando
                // a janela do pedal-solto e silencio exato (todas as vozes liberadas
                // no fim do feltro), diz-se isso.
                char rz[32];
                if (sol <= 1e-12) std::snprintf(rz, sizeof rz, "silencio exato");
                else std::snprintf(rz, sizeof rz, "%.1fx, %.1f dB", seg / sol,
                                   20.0 * std::log10(seg / sol));
                std::printf("      %s: segurado=%.6f solto=%.6f  (%s)\n", j.nome, seg, sol, rz);
                if (std::string(j.nome).rfind("W1", 0) == 0) {
                    // Porta de saida do "corte". A assercao antiga (janela unica de
                    // 0,35-2,55 s a 20x mais quieta) NAO tinha dente nenhum: os dois
                    // lados deste arquivo mediram com isso, e o abafador de 0,55 s
                    // chato -- o que fez o usuario dizer "corta totalmente o audio" --
                    // passava. Medido com o codigo de volta ao release do banco, esta
                    // janela: 11,1 dB de queda. Com a tabela por registro: 3,7 dB. O
                    // limiar fica no meio geometrico dos dois (7,6 dB), com ~4 dB de
                    // folga para cada lado.
                    check(sol * 2.4 > seg,
                          "soltar o pedal nao e um corte: a textura ainda esta la nos primeiros 0,34 s (queda < 7,6 dB)");
                }
                if (std::string(j.nome).rfind("W3", 0) == 0) {
                    check(sol * 20.0 < seg,
                          "soltar o pedal abafa a textura que estava segurando (>=20x mais quieta aos 4 s)");
                }
            }
        }

        controls[pianoforge::Param_Volume] = volAntes;
        controls[pianoforge::Param_SympRes] = sympAntes;
        controls[pianoforge::Param_Reverb] = revAntes;
        controls[pianoforge::Param_Chorus] = chorAntes;
        controls[pianoforge::Param_Hammer] = hammAntes;
        // Estado limpo pro teste seguinte: sem pedal e sem corda segurando.
        for (int b = 0; b < 400; ++b) {
            lv2_atom_forge_set_buffer(&forge, midiBuf.data(), midiBuf.size());
            LV2_Atom_Forge_Frame fr;
            lv2_atom_forge_sequence_head(&forge, &fr, seqUrid);
            if (b == 0) { putMidi(forge, midiUrid, 0, 0xB0, 120, 127); putMidi(forge, midiUrid, 1, 0xB0, 64, 0); }
            lv2_atom_forge_pop(&forge, &fr);
            desc->run(inst, 512);
            worker.drain();
            worker.pumpResponses();
        }
    }

    // ---- -1 descarrega ----
    controls[pianoforge::Param_Count] = -1.0f;
    Result e = runBlocks(600, true, 60);
    std::printf("E: rms=%.6f rmsTail=%.9f maxBlock=%.3fms\n", e.rms, e.rmsTail, e.maxBlockMs);
    // rms da fase toda nao vale como teste: as cordas simpaticas e o reverb do
    // piano anterior ainda estao desligando. O que tem que ser silencio e o
    // estado estacionario no fim, e a cauda tem que tender a zero (sem fonte).
    check(e.rmsTail < 1e-6, "escolher -1 devolve ao silencio depois da cauda");

    // ---- Teste I: o que a UI ve (portas de saida 22/23) ----
    // Um host de verdade continua chamando run() enquanto o worker rói o disco,
    // entao aqui o worker NAO e drenado entre blocos: e assim que se confere que
    // "carregando" existe enquanto o load dura e que vira o indice publicado.
    {
        bool sawLoading = false, sawReady = false;
        std::vector<double> blockMs;
        blockMs.reserve(4096);
        int spinBlocks = 0;
        controls[pianoforge::Param_Count] = 0.0f;      // do silencio (E) de volta ao piano 0
        for (int b = 0; b < 60000 && !sawReady; ++b) {
            lv2_atom_forge_set_buffer(&forge, midiBuf.data(), midiBuf.size());
            LV2_Atom_Forge_Frame fr;
            lv2_atom_forge_sequence_head(&forge, &fr, seqUrid);
            lv2_atom_forge_pop(&forge, &fr);

            const auto t0 = std::chrono::steady_clock::now();
            desc->run(inst, 512);
            blockMs.push_back(double(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count()));
            ++spinBlocks;
            if (pianoforge::bankLoading(statusPorts[0])) sawLoading = true;
            else if (pianoforge::bankPublishedIndex(statusPorts[0]) == 0) sawReady = true;
            // Cadencia de host: nao martelar o CPU enquanto o disco trabalha.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        worker.drain();
        worker.pumpResponses();
        runBlocks(40, true, 60);
        const int published = pianoforge::bankPublishedIndex(statusPorts[0]);
        const float presets = statusPorts[1];
        // O pior bloco da serie inteira e ruido do sistema (page cache, outro
        // processo no disco): um unico bloco de 2 ms ja reprovou uma execucao que
        // estava 0,155 ms em tudo o mais. O que se pode afirmar e a distribuicao.
        std::vector<double> sorted = blockMs;
        std::sort(sorted.begin(), sorted.end());
        const double p999 = sorted.empty() ? 0.0 : sorted[size_t(sorted.size() * 0.999)];
        const double worstMs = sorted.empty() ? 0.0 : sorted.back();
        std::printf("I: carregando=%d publicado=%d presets=%.0f blocos=%d "
                    "p99.9=%.3fms maxBlock=%.3fms\n",
                    int(sawLoading), published, presets, spinBlocks, p999, worstMs);
        check(sawLoading, "a porta bankState mostra -3 (carregando) durante o load");
        check(sawReady && published == 0,
              "terminado o load, bankState passa a ser o indice publicado");
        check(presets >= 1.0f, "presetCount diz quantos presets o banco tem");
        check(p999 < 1.0, "99,9% dos blocos cabem em 1 ms com o load no disco");

        // Banco invalido: a porta reclama, mas nao silencia nem troca nada.
        if (!bads.empty()) {
            const int badIndex = int(sf2s.size());
            controls[pianoforge::Param_Count] = float(badIndex);
            bool sawFailed = false;
            for (int b = 0; b < 6000 && !sawFailed; ++b) {
                lv2_atom_forge_set_buffer(&forge, midiBuf.data(), midiBuf.size());
                LV2_Atom_Forge_Frame fr;
                lv2_atom_forge_sequence_head(&forge, &fr, seqUrid);
                lv2_atom_forge_pop(&forge, &fr);
                desc->run(inst, 512);
                sawFailed = pianoforge::bankFailed(statusPorts[0]);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            runBlocks(8, true, 60);
            std::printf("I: falha=%d bankState=%.0f publicado=%d\n",
                        int(sawFailed), statusPorts[0],
                        pianoforge::bankPublishedIndex(statusPorts[0]));
            check(sawFailed, "banco que nao abre aparece como -4 (falha) na saida");
            check(pianoforge::bankPublishedIndex(statusPorts[0]) != badIndex,
                  "a falha nunca e publicada como piano em uso");
        }

        controls[pianoforge::Param_Count] = -1.0f;
        runBlocks(20, false, 0);
        check(statusPorts[0] == pianoforge::kBankNone,
              "sem piano escolhido a saida diz -2 (nenhum piano)");
    }

    // ---- Teste C: sessao restaurada nao carrega ----
    {
        WorkerStub w2;
        LV2_Worker_Schedule f2 = { &w2, &WorkerStub::schedule };
        LV2_Feature featWorker2 = { const_cast<char*>(LV2_WORKER__schedule), &f2 };
        const LV2_Feature* feats2[3] = { &featMap, &featWorker2, nullptr };
        LV2_Handle i2 = desc->instantiate(desc, 44100.0, "build/pianoforge.lv2", feats2);
        w2.instance = i2; w2.desc = desc;
        static float ol[512], or_[512];
        std::vector<uint8_t> mb = makeMidiBuffer(8192);
        desc->connect_port(i2, 0, ol);
        desc->connect_port(i2, 1, or_);
        desc->connect_port(i2, 2, mb.data());
        for (int i = 0; i < 19; ++i) desc->connect_port(i2, 3 + i, &controls[i]);
        controls[pianoforge::Param_Count] = 0.0f;   // valor "salvo" na sessao
        w2.start();
        desc->activate(i2);                         // baseline captura 0
        double sq = 0.0;
        for (int b = 0; b < 200; ++b) {
            lv2_atom_forge_set_buffer(&forge, mb.data(), mb.size());
            LV2_Atom_Forge_Frame fr;
            lv2_atom_forge_sequence_head(&forge, &fr, seqUrid);
            putMidi(forge, midiUrid, 0, 0x90, 60, 110);
            lv2_atom_forge_pop(&forge, &fr);
            desc->run(i2, 512);
            w2.drain();
            w2.pumpResponses();
            for (int i = 0; i < 512; ++i) sq += double(ol[i]) * ol[i];
        }
        const double rms = std::sqrt(sq / (200.0 * 512.0));
        std::printf("C: rms=%.9f workCalls=%d\n", rms, w2.workCalls.load());
        check(rms == 0.0 && w2.workCalls.load() == 0,
              "valor restaurado pelo host nao puxa piano nenhum");
        desc->deactivate(i2);
        desc->cleanup(i2);
        w2.shutdown();
    }

    // ---- Teste F: delayFrames e tempo, nao enfeite ----
    // Uma nota com delay=200 num bloco de 512 tem que comecar ~200 amostras
    // depois da nota com delay=0. Mede a primeira amostra acima do piso do
    // silencio que precede o bloco, em uma instancia limpa (sem cauda antiga).
    {
        static float fOutL[512], fOutR[512];
        static float c3[19];
        for (int i = 0; i < pianoforge::Param_Count; ++i)
            c3[i] = pianoforge::kParameterDefs[i].defaultValue;
        c3[pianoforge::Param_Count] = -1.0f;
        c3[pianoforge::Param_Count + 1] = 0.0f;

        std::vector<uint8_t> mb3 = makeMidiBuffer(8192);
        WorkerStub w3;
        LV2_Worker_Schedule f3 = { &w3, &WorkerStub::schedule };
        LV2_Feature featWorker3 = { const_cast<char*>(LV2_WORKER__schedule), &f3 };
        const LV2_Feature* feats3[3] = { &featMap, &featWorker3, nullptr };
        LV2_Handle i3 = desc->instantiate(desc, 44100.0, "build/pianoforge.lv2", feats3);
        w3.instance = i3; w3.desc = desc;
        desc->connect_port(i3, 0, fOutL);
        desc->connect_port(i3, 1, fOutR);
        desc->connect_port(i3, 2, mb3.data());
        for (int i = 0; i < 19; ++i) desc->connect_port(i3, 3 + i, &c3[i]);
        w3.start();
        desc->activate(i3);                       // baseline = -1 (mudo)

        auto block = [&](int delay, int cmd, int note, int vel) {
            lv2_atom_forge_set_buffer(&forge, mb3.data(), mb3.size());
            LV2_Atom_Forge_Frame fr;
            lv2_atom_forge_sequence_head(&forge, &fr, seqUrid);
            if (cmd) putMidi(forge, midiUrid, delay, uint8_t(cmd), uint8_t(note), uint8_t(vel));
            lv2_atom_forge_pop(&forge, &fr);
            desc->run(i3, 512);
            w3.drain();
            w3.pumpResponses();
        };
        // Silencio ate a cauda morrer; devolve o maior modulo do ultimo bloco.
        auto quiet = [&](int blocks) -> double {
            double floorMax = 0.0;
            for (int b = 0; b < blocks; ++b) {
                block(0, 0, 0, 0);
                if (b == blocks - 1)
                    for (int i = 0; i < 512; ++i)
                        floorMax = std::max(floorMax, double(std::fabs(fOutL[i])));
            }
            return floorMax;
        };
        auto onset = [&](int delay, int note, double thr) -> int {
            block(delay, 0x90, note, 110);
            int at = -1;
            for (int i = 0; i < 512; ++i)
                if (std::fabs(fOutL[i]) > thr) { at = i; break; }
            block(0, 0x80, note, 64);
            return at;
        };

        c3[pianoforge::Param_Count] = 0.0f;       // pedido explicito: piano 0
        quiet(4);                                 // o worker publica nesse meio-tempo
        const int notes[3] = { 45, 50, 55 };
        const int delays[3] = { 0, 200, 400 };
        int at[3] = { -1, -1, -1 };
        double floors[3] = { 0.0, 0.0, 0.0 };
        for (int k = 0; k < 3; ++k) {
            floors[k] = quiet(4000);
            const double thr = std::max(5e-4, floors[k] * 8.0);
            at[k] = onset(delays[k], notes[k], thr);
            std::printf("F: delay=%d onset=%d piso=%.7f limiar=%.7f\n",
                        delays[k], at[k], floors[k], thr);
        }
        const bool medido = at[0] >= 0 && at[1] >= 0 && at[2] >= 0;
        check(medido, "as tres notas apareceram no bloco");
        if (medido) {
            check(at[0] < 64, "delay=0 comeca no comeco do bloco");
            check(std::abs((at[1] - at[0]) - 200) <= 32, "delay=200 comeca 200 amostras depois");
            check(std::abs((at[2] - at[0]) - 400) <= 32, "delay=400 comeca 400 amostras depois");
        }

        // G: custo do fatiamento. Cada evento cria uma fronteira de fatia, entao
        // um bloco denso (acorde grande + CC de automacao) e o pior caso do run().
        //
        // O `spread` separa as DUAS coisas que um bloco denso paga e que antes
        // vinham misturadas num numero so: com spread=7 os 64 eventos criam ~32
        // fronteiras de fatia; com spread=0 caem todos no delay 0 e o bloco tem
        // UMA fronteria com as mesmas 64 vozes. A diferenca e o custo de fatiar;
        // o que sobra em spread=0 e o custo de abrir vozes no backend.
        auto stress = [&](int noteOns, int ccs, int spread) -> double {
            lv2_atom_forge_set_buffer(&forge, mb3.data(), mb3.size());
            LV2_Atom_Forge_Frame fr;
            lv2_atom_forge_sequence_head(&forge, &fr, seqUrid);
            for (int i = 0; i < noteOns; ++i)
                putMidi(forge, midiUrid, spread ? i * spread % 512 : 0,
                        0x90, uint8_t(24 + i), uint8_t(70 + i % 40));
            for (int i = 0; i < ccs; ++i)
                putMidi(forge, midiUrid, spread ? i * 5 % 512 : 0, 0xB0, 64, i % 2 ? 127 : 0);
            lv2_atom_forge_pop(&forge, &fr);
            const auto t0 = std::chrono::steady_clock::now();
            desc->run(i3, 512);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count();
            w3.drain();
            w3.pumpResponses();
            for (int i = 0; i < 512; ++i) block(0, 0x80, uint8_t(24 + i % std::max(1, noteOns)), 64);
            return ms;
        };
        quiet(300);
        // Um so bloco cronometrado e uma moeda: nesta maquina a mesma build deu
        // 0,706 e 1,036 ms para o MESMO trabalho, porque o vizinho do disco e o
        // page cache mudam entre chamadas. O que se quer saber e o custo
        // deterministico do plugin, e esse e o MINIMO de varias execucoes; o
        // maximo continua impresso, porque um spike real de uma execucao tambem
        // importa -- so nao pode ser a assercao.
        auto stressFloor = [&](int noteOns, int ccs, int spread, int reps) {
            double lo = 1e30, hi = 0.0;
            for (int r = 0; r < reps; ++r) {
                quiet(120);
                const double ms = stress(noteOns, ccs, spread);
                lo = std::min(lo, ms);
                hi = std::max(hi, ms);
            }
            return std::pair<double, double>(lo, hi);
        };
        constexpr int kReps = 8;
        const auto g64 = stressFloor(64, 0, 7, kReps);
        const auto g64cc = stressFloor(64, 100, 7, kReps);
        const auto gBunched = stressFloor(64, 0, 0, kReps);
        const auto gCCOnly = stressFloor(0, 100, 5, kReps);
        // Orcamento: o bloco tem de caber numa FRACAO do proprio periodo, nao num
        // numero magico em milissegundos. Com bloco de 512 em 44,1 kHz o periodo e
        // 11,6 ms e o teto sai 2,32 ms; em 48 kHz sai 2,13 ms. As outros 80% ficam
        // para o resto do projeto do host. O caso tipico (load, troca de banco,
        // notas reais) continua asserrado em p99.9 < 1 ms nos testes B/D/I -- aqui e
        // o PIOR CASO sintetico: 64 ataques no mesmo bloco, que e mais acorde do
        // que dois manos seguram, e 100 CC de automacao por cima.
        constexpr double kPeriodoMs = 512.0 / 44100.0 * 1000.0;
        const double orcamentoMs = kPeriodoMs / 5.0;
        std::printf("G: 64 notas=%.3f..%.3fms | 64 notas + 100 CC=%.3f..%.3fms  (min..max de %d)\n",
                    g64.first, g64.second, g64cc.first, g64cc.second, kReps);
        std::printf("   decomposto: 64 notas num so delay=%.3f..%.3fms (vozes) | "
                    "100 CC espalhados=%.3f..%.3fms (fronteiras)\n",
                    gBunched.first, gBunched.second, gCCOnly.first, gCCOnly.second);
        std::printf("   orcamento: %.3f ms = 1/5 do periodo de %.3f ms (bloco 512 @ 44,1 kHz)\n",
                    orcamentoMs, kPeriodoMs);
        check(std::max(g64.first, g64cc.first) < orcamentoMs,
              "bloco denso de eventos nao come 1/5 do periodo");

        desc->deactivate(i3);
        desc->cleanup(i3);
        w3.shutdown();
    }

    // ---- Teste J: host SEM lv2:worker (a thread de reserva faz tudo) -------
    // O outro modo do plugin: sem LV2_Worker_Schedule no instantiate, o load e a
    // liberacao do banco velho acontecem na loaderThread. Tres coisas aqui:
    // (a) o som chega mesmo assim; (b) nenhum bloco estoura 1 ms -- o tsf_close de
    // um banco grande e um munmap de dezenas de ms, e libertar dentro de run() era
    // exatamente o que o fallback antigo fazia no ping do retire; (c) ao trocar
    // para um banco MENOR a RSS cai de verdade, que e o proposito do pedido
    // estacionado: nenhum banco novo e alocado com o velho ainda na RAM.
    if (sf2s.size() >= 2) {
        static float jOutL[512], jOutR[512], jCtl[19];
        // O chao da porta 22 nao pode ser 0: float() zero-init significa "indice 0
        // publicado", e a primeira espera do teste passaria antes de o plugin escrever
        // qualquer coisa -- dava blocos=0, rms=0 e duas falhas falsas. Comeca em -2,
        // que e o que o plugin realmente escreve no primeiro bloco de uma instancia
        // sem piano.
        static float jStatus[2]{pianoforge::kBankNone, 0.0f};
        std::error_code ec;
        const auto szOf = [&](int i) { return std::filesystem::file_size(sf2s[size_t(i)], ec); };
        const int big = szOf(0) >= szOf(1) ? 0 : 1;
        const int small = big == 0 ? 1 : 0;
        for (int i = 0; i < pianoforge::Param_Count; ++i)
            jCtl[i] = pianoforge::kParameterDefs[i].defaultValue;
        jCtl[pianoforge::Param_Count] = -1.0f;
        jCtl[pianoforge::Param_Count + 1] = 0.0f;

        const LV2_Feature* featsJ[2] = { &featMap, nullptr };   // sem worker, de caso pensado
        LV2_Handle iJ = desc->instantiate(desc, 44100.0, "build/pianoforge.lv2", featsJ);
        if (!iJ) {
            check(false, "instantiate sem LV2_Worker_Schedule");
        } else {
            std::vector<uint8_t> mbJ = makeMidiBuffer(8192);
            desc->connect_port(iJ, 0, jOutL);
            desc->connect_port(iJ, 1, jOutR);
            desc->connect_port(iJ, 2, mbJ.data());
            for (int i = 0; i < 19; ++i) desc->connect_port(iJ, 3 + i, &jCtl[i]);
            desc->connect_port(iJ, pianoforge::kPortBankState, &jStatus[0]);
            desc->connect_port(iJ, pianoforge::kPortPresetCount, &jStatus[1]);
            desc->activate(iJ);

            double maxMs = 0.0;
            std::vector<double> temposJ;
            auto blocosJ = [&](int count) {
                double sq = 0.0;
                for (int b = 0; b < count; ++b) {
                    lv2_atom_forge_set_buffer(&forge, mbJ.data(), mbJ.size());
                    LV2_Atom_Forge_Frame fr;
                    lv2_atom_forge_sequence_head(&forge, &fr, seqUrid);
                    const int note = 48 + ((b / 40) % 6) * 5;
                    if (b % 40 == 0) putMidi(forge, midiUrid, 0, 0x90, note, 100);
                    if (b % 40 == 39) putMidi(forge, midiUrid, 500, 0x80, note, 64);
                    lv2_atom_forge_pop(&forge, &fr);
                    const auto t0 = std::chrono::steady_clock::now();
                    desc->run(iJ, 512);
                    const double ms = std::chrono::duration<double, std::milli>(
                                            std::chrono::steady_clock::now() - t0).count();
                    maxMs = std::max(maxMs, ms);
                    temposJ.push_back(ms);
                    for (int i = 0; i < 512; ++i) sq += double(jOutL[i]) * jOutL[i];
                    // 200 us de cochilo por bloco: free-wheeling de verdade, e o
                    // tempo de parede que a thread de reserva precisa para rolar.
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                return std::sqrt(sq / (double(count) * 512.0));
            };
            auto esperaPublicado = [&](int want) {
                int blocos = 0;
                // Mesmo criterio do aguardaEstado: teto em segundos de parede, nao em
                // blocos. Um SF2 de 611 MB nao publica em 40.000 blocos de 200 us.
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(90);
                // do/while: sempre da ao menos um bloco, para a porta 22 ter sido
                // escrita pelo run() antes de qualquer julgamento.
                do {
                    blocosJ(1);
                    ++blocos;
                } while (std::chrono::steady_clock::now() < deadline &&
                         pianoforge::bankPublishedIndex(jStatus[0]) != want);
                return blocos;
            };

            jCtl[pianoforge::Param_Count] = float(big);
            const int w1 = esperaPublicado(big);
            const double rmsBig = blocosJ(300);
            const long rssBig = rssKb();
            const long hwmBig = hwmKb();
            jCtl[pianoforge::Param_Count] = float(small);
            const int w2 = esperaPublicado(small);
            const double rmsSmall = blocosJ(300);
            const long rssSmall = rssKb();
            const long hwmSmall = hwmKb();

            // O pico dentro da troca e informativo, mas nao e assercao possivel aqui:
            // VmHWM e monotico por processo, entao os swaps dos testes A-I ja o
            // assentaram antes do J (mutando o codigo para o jeito ingenuo, hwmBig e
            // hwmSmall deram ambos 144.700 kB e qualquer delta passaria em falso). A
            // assercao absoluta do pico esta no fim do main, junto do `memoria:`.
            std::sort(temposJ.begin(), temposJ.end());
            const double p999J =
                temposJ.empty() ? 0.0 : temposJ[size_t(temposJ.size() * 0.999)];
            const bool memMedida = rssBig >= 0 && rssSmall >= 0;
            std::printf("J: sem worker | blocos ate o grande=%d ate o pequeno=%d  "
                        "rms=%.6f/%.6f p99.9=%.3fms maxBlock=%.3fms  ",
                        w1, w2, rmsBig, rmsSmall, p999J, maxMs);
            if (memMedida)
                std::printf("RSS %ld->%ld kB  HWM %ld->%ld kB", rssBig, rssSmall, hwmBig,
                            hwmSmall);
            else
                std::printf("RSS/HWM: sem medida neste sistema");
            std::printf("  (arquivos %zu/%zu MB)\n", size_t(szOf(big) / 1048576),
                        size_t(szOf(small) / 1048576));
            check(rmsBig > 0.001 && rmsSmall > 0.001,
                  "host sem worker: a thread de reserva entrega os dois pianos");
            check(p999J < 1.0, "host sem worker: libertar o banco velho nao pesou no run()");
            // "A RAM do banco grande voltou" e uma assercao de ESCALA, nao de caminho:
            // o malloc so devolve paginas ao kernel quando o bloco e grande o bastante
            // para ir por mmap. Medido com os pianos gravados: 1 256 800 -> 509 416 kB
            // (611 MB de banco devolvidos). Com o banco sintetico de 3 MB o RSS nao se
            // move um kB e a assercao reprovaria um codigo correto. Abaixo de 64 MB o
            // que vale e a assercao absoluta do pico (linhas abaixo), nao esta.
            const bool escalaDevolve = memMedida && szOf(big) >= size_t(64) * 1048576;
            if (escalaDevolve)
                check(rssSmall < rssBig - long(szOf(big) / 1024),
                      "host sem worker: a RAM do banco grande voltou antes de o menor entrar");
            else if (memMedida)
                skip("host sem worker: a RAM do banco grande voltou antes de o menor entrar",
                     "banco abaixo de 64 MB: o malloc nao devolve paginas ao kernel e o "
                     "RSS nao se move -- a assercao so tem poder na escala dos pianos "
                     "gravados (611 MB); o orcado absoluto abaixo continua valendo");
            else
                skip("host sem worker: a RAM do banco grande voltou antes de o menor entrar",
                     "sem /proc/self/status");

            desc->deactivate(iJ);
            desc->cleanup(iJ);
        }
    }

    desc->deactivate(inst);
    desc->cleanup(inst);
    worker.shutdown();

    // Pico de RAM do processo inteiro, que e a unica forma de conferir o
    // "devolve a RAM do banco velho antes de gastar no novo": um swap A->B->A
    // deveria pedir max(A,B), nao A+B. O pico do jeito antigo (alocar com o
    // outro no ar) e exatamente o que tools/pforge_dsp_stages --sf2 A --sf2b B
    // mede, e os dois numeros tem de bater com este aqui.
    {
        const long hwm = hwmKb(), rss = rssKb();
        // Orçamento: linha de base do processo (medida antes de qualquer banco) + 2x
        // o maior arquivo, porque o tsf converte int16->float32 ao carregar. A folga de
        // 8 MB e para o transitorio do load. Um pico acima disso significa dois bancos
        // no ar ao mesmo tempo, que e justamente o que o estacionamento existe para
        // evitar. Mutando loadBankNow() para alocar com o velho publicado, com os pianos
        // gravados a linha deu 146.328 kB contra 106.032 kB e reprovou.
        //
        // A escala MUDA o poder da asserção: com um banco sintetico de 4 MB o jeito
        // ingenuo custa +8 MB, e e o que a folga cobre por pouco. O numero de referencia
        // do estacionamento continua sendo o da execucao com os pianos gravados (pico
        // 1,23 GB, folga real de ~5 MB sobre 2x o maior); no CI a linha vale como "nada
        // explodiu", nao como a medida.
        size_t maior = 0;
        int contados = 0;
        for (const std::string& s : sf2s) {
            std::error_code ec;
            const auto n = std::filesystem::file_size(s, ec);
            // `file_size` de arquivo que nao existe devolve (uintmax_t)-1 com ec posto,
            // e esse lixo é MAIOR que qualquer banco: o orcado viria 1,8e7 MB e a
            // asserção de memoria passaria sem provar nada. So entra na conta quem abriu.
            if (ec) continue;
            ++contados;
            maior = std::max(maior, size_t(n));
        }
        if (hwm < 0) {
            std::printf("memoria: [SKIP] sem /proc/self/status neste sistema: o pico de "
                        "RAM do processo nao foi medido, e a assercao de estacionamento "
                        "nao roda\n");
        } else if (maior && contados == int(sf2s.size())) {
            const long base = entryHwm < 0 ? 0 : entryHwm;
            const long budget = base + long(2 * maior / 1024) + 8 * 1024;
            std::printf("memoria: pico=%ld kB agora=%ld kB  (%.1f MB no pico, orcado %.1f MB "
                        "= linha de base %.1f MB + 2x o maior banco de %.1f MB + 8 MB)\n",
                        hwm, rss, hwm / 1024.0, budget / 1024.0, base / 1024.0,
                        maior / 1048576.0);
            check(hwm < budget, "a troca de banco nunca empilha os dois no pico do processo");
        } else {
            std::printf("memoria: pico=%ld kB agora=%ld kB  (%.1f MB no pico) "
                        "-- SEM orcado: %d de %d caminhos de banco abriram\n",
                        hwm, rss, hwm / 1024.0, contados, int(sf2s.size()));
        }
    }

    std::printf("\n%s (%d falhas)\n", failures ? "FALHOU" : "TUDO OK", failures);
    dlclose(lib);
    return failures ? 1 : 0;
}
