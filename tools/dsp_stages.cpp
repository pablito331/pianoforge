// Medida do custo de cada estagio da cadeia de timbre, por chamada e por amostra.
//
// Por que isto existe: o teste G do harness antigo (o pforge_host_test, que so
// existia com o formato LV2) dizia que um bloco de 512 com
// automacao densa custa ~0,7 ms, e que quase tudo disso vem das ~32 FRONTIERAS de
// fatia, nao das vozes. "Fronteira" e uma chamada a renderSlice() com 16 amostras.
// Ou seja, o que importa e o custo FIXO de chamar cada estagio, que o numero por
// amostra esconde. Este tool mede as duas coisas no mesmo lugar onde o Engine as
// chama, na mesma ordem, com o mesmo estado.
//
// Nao e microbenchmark de compilador: cada estagio roda com sinal de verdade e o
// resultado e somado num dissipador que sai pelo stdout, senao -O2 apagaria o
// trabalho todo.

#include "core/dsp/Chorus.hpp"
#include "core/dsp/HammerModel.hpp"
#include "core/dsp/LidFilter.hpp"
#include "core/dsp/MasterStage.hpp"
#include "core/dsp/Reverb.hpp"
#include "core/dsp/SoundboardEQ.hpp"
#include "core/dsp/SympatheticResonance.hpp"

// O backend que o Engine chama uma vez POR FRONTEIRA de fatia. Medir so a cadeia
// de efeitos deu "fixo/fronteira = 0" e mesmo assim o bloco denso custa 0,7 ms:
// o que sobra e o tsf_render_float(), e ele nao estava nesta medida.
#define TSF_IMPLEMENTATION
#include "core/tsf.h"

#include "SynthBank.hpp"      // --makesf2: medir sem soundfont no CI

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// M_PI nao e padrao C++: o MSVC so o expoe com _USE_MATH_DEFINES antes de <cmath>.
// Aqui ele chegava por transitividade de Chorus.hpp, e uma medida que quebra com
// "undeclared identifier" quando alguem reordena dois includes nao e medida.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace pianoforge::dsp;

namespace {

constexpr size_t kBlock = 512;
constexpr size_t kSlice = 16;          // o kMinSlice do Engine
constexpr int kReps = 400;             // repeticoes por medicao
constexpr int kMaxVoices = 256;        // o mesmo teto do setupSf2() (Engine.cpp)

// Sinal de teste: um acorde baixo + ruido, para que nenhum estagio caia no
// "early-out de silencio" que o lid>=0.98 usa. Nivel de 0.05 para nao saturar o
// soft clip do master e medir o mesmo trabalho que o plugin faz.
void feed(float* l, float* r) {
    for (size_t i = 0; i < kBlock; ++i) {
        const double t = double(i);
        l[i] = float(0.05 * std::sin(2.0 * M_PI * 220.0 * t / 48000.0) +
                     0.01 * std::sin(2.0 * M_PI * 1760.0 * t / 48000.0));
        r[i] = float(0.05 * std::sin(2.0 * M_PI * 223.5 * t / 48000.0) +
                     0.01 * std::sin(2.0 * M_PI * 1810.0 * t / 48000.0));
    }
}

double sink = 0.0;

// Leitura de /proc/self/status, nao de um poll externo. Poll de fora (o loop que
// existia antes desta correcao) me deu "VmHWM 6 MB" para um render que o `ulimit -v`
// mostra precisar de >262 MB: a janela de amostragem perde o pico porque o load
// inteiro acontece entre dois sleeps, e VmHWM e por processo, nao por amostra.
// Medir de dentro e a unica forma de pegar o pico de verdade.
struct Mem {
    long vmHwm = -1, vmRss = -1, vmSize = -1;
    bool ok = false;      // false fora do Linux: sem /proc, e 0 nao e "zero kB"
};
Mem readMem() {
    Mem m;
#if defined(__linux__)
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return m;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        if (!std::strncmp(line, "VmHWM:", 6)) std::sscanf(line + 6, "%ld", &m.vmHwm);
        else if (!std::strncmp(line, "VmRSS:", 6)) std::sscanf(line + 6, "%ld", &m.vmRss);
        else if (!std::strncmp(line, "VmSize:", 7)) std::sscanf(line + 7, "%ld", &m.vmSize);
    }
    std::fclose(f);
    m.ok = true;
#endif
    return m;
}

struct Rows {
    std::vector<std::string> lines;
    void add(const char* name, double oneCall, double sliced, size_t nSlices) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "%-22s  1x512=%7.2f us   %zux%zu=%7.2f us   fixo/fronteira=%6.2f us",
                      name, oneCall, nSlices, kSlice, sliced,
                      nSlices ? (sliced - oneCall) / double(nSlices) : 0.0);
        lines.emplace_back(buf);
    }
};

} // namespace

// Medida da curva de decaimento de uma corda REAL deste banco.
//
// Por que isto existe: o pedal de sustain nao e um interruptor de voz -- e o
// levantamento dos abafadores. Uma corda sem abafador continua soando e
// DESCE: exponecial, com tempo que depende da nota (o grave de um cauda longa
// passa de 30 s, o agudo morre em ~2 s). O tinysonic, com o pedal pisado,
// congela o envelope em nivel-de-sustentacao para sempre (TSF_SEGMENT_SUSTAIN
// poe samplesUntilNextSegment = 0x7FFFFFFF), entao a corda NUNCA desce e as
// vozes nunca voltam para o pool. Antes de corrigir isso com um numero
// inventado, mede-se o que o banco faz de fato nos tres regimes.
//
// Regimes, por tecla: 'segurada' (tecla abaixada e nunca solta -- o decaimento
// que o proprio sample carrega), 'pedal' (solta com CC64=127 no ar -- hoje e o
// platô congelado) e 'abafada' (solta sem pedal -- o release do preset).
// Pico por bloco de 512 (10,7 ms), normalizado pelo maximo da propria execucao.
struct Envelope {
    std::vector<float> pico;      // por bloco
    double refDb = 0.0;           // pico de referencia, em dBFS
};

static Envelope renderEnvelope(tsf* s, int key, const char* regime, double seconds) {
    constexpr int kN = int(kBlock);
    std::vector<float> buf(kN * 2, 0.0f);
    const int blocos = int(seconds * 48000.0 / kN);

    // Um passo de ataque limpo antes de medir: nada de resto do nota anterior.
    tsf_channel_sounds_off_all(s, 0);
    for (int b = 0; b < 8; ++b) tsf_render_float(s, buf.data(), kN, 0);

    const bool pedal = regime[0] == 'p';
    const bool solta = regime[0] != 's';
    tsf_channel_set_sustain(s, 0, pedal ? 1 : 0);
    tsf_channel_note_on(s, 0, key, 80.0f / 127.0f);
    // Ataque de 250 ms antes do note-off, para o release cair no sustenido
    // e nao no meio do ataque (que e o que o musico faz ao soltar a tecla).
    const int antesOff = int(0.25 * 48000.0 / kN);
    Envelope e;
    e.pico.resize(size_t(blocos), 0.0f);
    for (int b = 0; b < blocos; ++b) {
        if (solta && b == antesOff) tsf_channel_note_off(s, 0, key);
        std::fill(buf.begin(), buf.end(), 0.0f);
        tsf_render_float(s, buf.data(), kN, 0);
        float p = 0.0f;
        for (float v : buf) { const float a = std::fabs(v); if (a > p) p = a; }
        e.pico[size_t(b)] = p;
    }
    tsf_channel_set_sustain(s, 0, 0);
    float mx = 0.0f;
    for (float v : e.pico) mx = std::max(mx, v);
    e.refDb = mx > 0.0f ? 20.0 * std::log10(mx) : -200.0;
    return e;
}

// Tempo (s) em que o envelope, medido do pico, cruza para sempre -X dB.
// "para sempre" importa: um pico solto no meio da cauda nao e decaimento.
static double cruzamento(const Envelope& e, double db) {
    const double lim = std::pow(10.0, db / 20.0);
    // varredura reversa: o ultimo bloco ACIMA do limiar define o fim real
    size_t ultimo = e.pico.size();
    while (ultimo > 0) {
        --ultimo;
        if (e.pico[ultimo] > lim) break;
    }
    // 'ultimo' e o ultimo bloco acima; o cruzamento e um bloco depois
    if (ultimo + 1 >= e.pico.size()) return -1.0;      // nunca cruzou
    return double(ultimo + 1) * double(kBlock) / 48000.0;
}

static double nivelFinal(const Envelope& e, int caudaMs = 500) {
    const size_t n = e.pico.size();
    const size_t janela = size_t(std::max(1, caudaMs) * 48.0 / kBlock);
    float p = 0.0f;
    for (size_t b = n - std::min(janela, n); b < n; ++b) p = std::max(p, e.pico[b]);
    return 20.0 * std::log10(std::max(1e-12f, p) / std::max(1e-12f, float(std::pow(10.0, e.refDb / 20.0))));
}

static int measureRing(const std::string& path, double seconds) {
    tsf* s = tsf_load_filename(path.c_str());
    if (!s) { std::fprintf(stderr, "nao abriu %s\n", path.c_str()); return 1; }
    tsf_set_output(s, TSF_STEREO_INTERLEAVED, 48000, 0.0f);
    // Pool folgado de proposito: nada aqui pode ser roubo de voz, senao o
    // "cruzamento" mediria o corte, nao o decaimento da corda.
    tsf_set_max_voices(s, 64);
    tsf_channel_set_presetindex(s, 0, 0);
    std::printf("curva de decaimento de %s (tecla segurada com vel 80, %.0f s por regime)\n",
                path.c_str(), seconds);
    std::printf("  tecla  freq    | segurada: -40 -60    | pedal: -40 -60  fim  | abafada: -40 -60  fim\n");
    for (const int key : {21, 27, 33, 39, 45, 51, 57, 63, 69, 75, 81, 87, 93, 99, 105}) {
        const double hz = 440.0 * std::pow(2.0, (key - 69) / 12.0);
        const Envelope seg = renderEnvelope(s, key, "segurada", seconds);
        const Envelope ped = renderEnvelope(s, key, "pedal", seconds);
        const Envelope aba = renderEnvelope(s, key, "abafada", seconds);
        auto fmt = [](double t) { return t < 0.0 ? std::string("  >") : [&] {
            char b[8]; std::snprintf(b, sizeof b, "%4.1f", t); return std::string(b); }(); };
        std::printf("  %3d  %6.1f Hz | %5s %5s        | %5s %5s %6.1f | %5s %5s %6.1f\n",
                    key, hz,
                    fmt(cruzamento(seg, -40)).c_str(), fmt(cruzamento(seg, -60)).c_str(),
                    fmt(cruzamento(ped, -40)).c_str(), fmt(cruzamento(ped, -60)).c_str(),
                    nivelFinal(ped),
                    fmt(cruzamento(aba, -40)).c_str(), fmt(cruzamento(aba, -60)).c_str(),
                    nivelFinal(aba));
    }
    tsf_close(s);
    return 0;
}

// Passagem pedalizada de verdade, para responder quantas vozes UM PEDAL USA.
//
// O --ring acima diz que uma corda grave deste banco soa 25-40 s com o abafador
// levantado. Entao a pergunta de projeto ("128 vozes basta?") nao e de gosto: e
// duracao-de-cauda x densidade-de-notas. Este modo toca uma passagem determinista
// (LCG com semente fixa -- mesmas notas toda execucao), com CC64=127 do inicio ao
// fim, e conta o que o pool fez: pico de vozes ativas e QUANTAS NOTAS ENTRARAM
// MUDAS -- o "corta o som" que o usuario ouviu.
// MUDAS e medido pelos contadores do proprio backend (pfMuteNotes), NAO pelo delta
// de tsf_active_voice_count(): uma nota que FURA a fila (mata uma voz e herda o
// slot) tambem nao move a contagem, entao o delta confunde roubo com silencio.
// Foi exatamente assim que a primeira versao deste modo acusou 26 notas mudas numa
// passagem onde nenhuma nota ficou sem som.
static auto wallNow() { return std::chrono::steady_clock::now(); }

struct PedalLoad {
    int notas = 0;          // notas tocadas
    int mudas = 0;          // notas que casaram com regiao e nao ganharam nenhuma voz
    int regioesPerdidas = 0;  // regioes do banco que ficaram sem slot
    int abafadas = 0;         // cordas seguradas derrubadas antes da hora
    double idadeAbafadas = 0.0;  // media de segundos que uma corda tocou antes de ser abafada
    int picoVozes = 0;      // ocupacao maxima do pool
    double usPorBloco = 0.0;
};

static PedalLoad measurePedalLoad(tsf* s, double seconds, unsigned seed) {
    std::vector<float> buf(kBlock * 2, 0.0f);
    const int blocos = int(seconds * 48000.0 / kBlock);
    struct Agend { int bloco; int key; int vel; bool on; };
    std::vector<Agend> fila;

    s->pfMuteNotes = s->pfDroppedRegions = s->pfDampedHeld = 0;   // medir ESTA passagem
    s->pfDampedAgeSamples = 0;
    uint32_t rng = seed;
    auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
    // ~3 eventos por segundo: as vezes nota sola, as vezes acorde de 2-4.
    for (double t = 0.5; t < seconds - 3.0; t += 0.28 + (rnd() % 220) / 1000.0) {
        const int vozes = 1 + int(rnd() % 4);
        const int base = 24 + int(rnd() % 72);
        for (int v = 0; v < vozes; ++v) {
            const int key = std::min(96, base + v);
            const int vel = 50 + int(rnd() % 61);
            fila.push_back({int(t * 48000.0 / kBlock), key, vel, true});
            const double dur = 0.35 + (rnd() % 900) / 1000.0;
            fila.push_back({int((t + dur) * 48000.0 / kBlock), key, vel, false});
        }
    }
    std::stable_sort(fila.begin(), fila.end(), [](const Agend& a, const Agend& b) {
        return a.bloco < b.bloco; });

    PedalLoad r;
    size_t ip = 0;
    tsf_channel_set_sustain(s, 0, 1);        // pedal no CHAO a passagem inteira
    auto t0 = wallNow();
    for (int b = 0; b < blocos; ++b) {
        for (; ip < fila.size() && fila[ip].bloco == b; ++ip) {
            if (fila[ip].on) {
                const unsigned antes = s->pfMuteNotes;
                tsf_channel_note_on(s, 0, fila[ip].key, float(fila[ip].vel) / 127.0f);
                if (s->pfMuteNotes != antes) ++r.mudas;   // nenhuma voz, de verdade
                ++r.notas;
            } else {
                tsf_channel_note_off(s, 0, fila[ip].key);
            }
        }
        r.picoVozes = std::max(r.picoVozes, tsf_active_voice_count(s));
        std::fill(buf.begin(), buf.end(), 0.0f);
        tsf_render_float(s, buf.data(), int(kBlock), 0);
    }
    // Solta o pedal no fim: e o que abafa as cordas, e o tempo que isso leva
    // depois e o que o teste K mede.
    tsf_channel_set_sustain(s, 0, 0);
    int cauda = 0;
    for (int b = 0; b < 48000 / kBlock * 3; ++b) {
        std::fill(buf.begin(), buf.end(), 0.0f);
        tsf_render_float(s, buf.data(), int(kBlock), 0);
        if (tsf_active_voice_count(s)) cauda = b;
    }
    r.usPorBloco = std::chrono::duration<double, std::micro>(wallNow() - t0).count() / double(blocos);
    r.regioesPerdidas = int(s->pfDroppedRegions);
    r.abafadas = int(s->pfDampedHeld);
    r.idadeAbafadas = r.abafadas
        ? double(s->pfDampedAgeSamples) / double(r.abafadas) / 48000.0 : 0.0;
    {
        char note[160];
        std::snprintf(note, sizeof note,
                      "  cauda apos soltar o pedal: %.2f s", double(cauda + 1) * kBlock / 48000.0);
        std::printf("%s\n", note);
    }
    return r;
}

static int measurePedalLoadMain(const std::string& path, double seconds, int pool) {
    tsf* s = tsf_load_filename(path.c_str());
    if (!s) { std::fprintf(stderr, "nao abriu %s\n", path.c_str()); return 1; }
    tsf_set_output(s, TSF_STEREO_INTERLEAVED, 48000, 0.0f);
    tsf_set_max_voices(s, pool);
    tsf_channel_set_presetindex(s, 0, 0);
    const PedalLoad r = measurePedalLoad(s, seconds, 0x12345678u);
    std::printf("passagem pedalizada %s: teto=%d | notas=%d MUDAS=%d | regioes sem slot=%d\n",
                path.c_str(), pool, r.notas, r.mudas, r.regioesPerdidas);
    std::printf("  abafadas antes da hora=%d, media de som segurado antes de abafar=%.2f s | pico de vozes=%d | %.1f us/bloco\n",
                r.abafadas, r.idadeAbafadas, r.picoVozes, r.usPorBloco);
    tsf_close(s);
    // Gate duro, e a unica prova NAO-diferencial de "nenhuma tecla muda": o teste M
    // mede pelo audio e por construcao nao distingue "nota descartada" de "nota que
    // entrou e derrubou outra" -- com o pool estourado, omitir uma tecla muda o
    // conjunto de vitimas do abafador e o ganho medido sai comprimido (0,4 dB com o
    // fundo a 0,0005). Aqui o proprio backend conta o evento decidido: pfMuteNotes
    // incrementa na nota que casou com regiao e nao conseguiu NENHUMA voz. Regioes
    // perdidas sao esperadas sob escassez (e o roubo funcionando); nota muda nao.
    if (r.mudas > 0) {
        std::fprintf(stderr, "PEDALLOAD FALHOU: %d de %d notas entraram mudas com o pedal no ar\n",
                     r.mudas, r.notas);
        return 1;
    }
    return 0;
}

// Em quanto tempo o feltro CAI, e nao apenas "caiu?".
//
// O teste K e a linha "cauda apos soltar o pedal" do --pedalload perguntam se a
// cauda DEPOIS do levantamento e menor que a cauda COM o pedal no ar. Isso vale
// tanto para um abafador de 500 ms quanto para um corte de 10 ms -- e o usuario
// ouviu a diferenca e chamou de "corta totalmente o audio". A pergunta que a
// oitiva faz e outra, e mede-se em milissegundos a partir do NIVEL EM QUE O SOM
// ESTAVA no instante em que o pe sai, nao do pico da execucao.
//
// Junto vai o diagnostico do porque: para cada voz segurada, o `release` do
// preset em segundos. Se o banco nao define release na regiao, tsf cai em
// TSF_FASTRELEASETIME (0,01 s) e o "abafador" e um corte digital -- que e
// exatamente o que o upstream faz ao soltar uma tecla normal, e que aqui passa a
// valer para 60 cordas de uma vez porque o pedal segurava todas.
static int measurePedalLift(const std::string& path, int pool, bool trace, bool chain) {
    // A cadeia do Engine, com o mesmo estado do bench: se o pe quebra a SAIDA mas
    // nao quebra o tsf, o corte esta num estagio e nao nas cordas.
    SympatheticResonance symp; SoundboardEQ eq; LidFilter lid; Chorus chorus;
    Reverb reverb; MasterStage master;
    if (chain) {
        symp.prepare(48000.0f); eq.prepare(48000.0f); lid.prepare(48000.0f);
        chorus.prepare(48000.0f); reverb.prepare(48000.0f);
        symp.setParameters(0.25f, 0.5f);
        eq.setGains(0.5f, 0.5f, 0.5f);
        lid.setLid(1.0f);
        chorus.setMix(0.0f);
        reverb.setType(1); reverb.setMix(0.1f);
    }
    // {} NAO serve aqui: vector<float> v{size_t(512)} escolhe o construtor de
    // initializer_list e cria UM elemento com o valor 512,0 -- que e o heap
    // corruption que deu "realloc(): invalid pointer" nesta ferramenta. Com
    // parenteses e o identificador sozinho (sem `int` na frente) e o construtor
    // de tamanho: e assim que o bench ja faz.
    std::vector<float> chL(kBlock), chR(kBlock);
    auto applyChain = [&](float* deint, int n) {
        for (int i = 0; i < n; ++i) { chL[size_t(i)] = deint[2 * i]; chR[size_t(i)] = deint[2 * i + 1]; }
        symp.process(chL.data(), chR.data(), size_t(n));
        eq.process(chL.data(), chR.data(), size_t(n));
        lid.process(chL.data(), chR.data(), size_t(n));
        chorus.process(chL.data(), chR.data(), size_t(n));
        reverb.process(chL.data(), chR.data(), size_t(n));
        master.process(chL.data(), chR.data(), size_t(n), 0.85f, 0.0f, 1.0f);
        for (int i = 0; i < n; ++i) { deint[2 * i] = chL[size_t(i)]; deint[2 * i + 1] = chR[size_t(i)]; }
    };
    tsf* s = tsf_load_filename(path.c_str());
    if (!s) { std::fprintf(stderr, "nao abriu %s\n", path.c_str()); return 1; }
    tsf_set_output(s, TSF_STEREO_INTERLEAVED, 48000, 0.0f);
    tsf_set_max_voices(s, pool);
    tsf_channel_set_presetindex(s, 0, 0);

    std::vector<float> buf(int(kBlock) * 2, 0.0f);
    auto blocos = [](double sec) { return int(sec * 48000.0 / kBlock); };
    // Pico E RMS por bloco. O pico de um bloco de um som de piano complexado
    // oscila +/- 3 dB de um bloco para o outro (depende de em que fase da forma
    // de onda ele corta), e isso e suficiente para fazer uma coluna de "-6 dB"
    // ler 21 ms numa corda que cai em 68 ms. O que decai suave e o RMS; o pico
    // fica so para dizer em que nivel (dBFS) a coisa estava.
    auto render = [&](int n, float* pico, double* rmsa) -> float {
        float mx = 0.0f;
        for (int b = 0; b < n; ++b) {
            std::fill(buf.begin(), buf.end(), 0.0f);
            tsf_render_float(s, buf.data(), int(kBlock), 0);
            if (chain) applyChain(buf.data(), int(kBlock));
            float p = 0.0f;
            double sq = 0.0;
            for (float v : buf) { const float a = std::fabs(v); if (a > p) p = a; sq += double(v) * double(v); }
            if (pico) pico[b] = p;
            if (rmsa) rmsa[b] = std::sqrt(sq / double(buf.size()));
            if (p > mx) mx = p;
        }
        return mx;
    };

    std::printf("levantamento do pedal em %s (teto=%d, vel 90, 1 s de corda no ar antes do pe, %s)\n",
                path.c_str(), pool, chain ? "CADEIA DO ENGINE" : "tsf sozinho");
    std::printf("  caso          | vozes | nivel ao pe (dBFS) | -6 dB   -20 dB  -40 dB | fim (2 s) | preset | tabela\n");
    // As colunas de tempo sao RMS cruzado com janela de 85 ms contra o nivel do pe.
    // Numa tecla SOLA isto nao e confiavel: as ~4 camadas do banco na mesma tecla
    // batem entre si e o bloco oscila +/- 6 dB sem nenhuma relacao com o abafador
    // (o -6 dB de "tecla 69" le 11 ms com o envelope caindo em 0,90 s). O caso que
    // interessa ao ouvido e o estrato, onde as batidas se medio; para tecla sola o
    // numero exato esta na coluna `tabela` (e no --trace, que le o envelope).
    std::printf("  (as colunas de ms so valem para o caso AGREGADO: numa tecla sola as camadas batem e o RMS oscila +/- 6 dB; use --trace)\n");

    struct Caso { const char* nome; std::vector<int> keys; };
    const std::vector<Caso> casos = {
        {"tecla 21 grave", {21}},
        {"tecla 45 medio", {45}},
        {"tecla 69 A4",    {69}},
        {"tecla 93 agudo", {93}},
        {"estrato 12",     {21, 27, 33, 39, 45, 51, 57, 63, 69, 75, 81, 93}},
    };
    for (const Caso& c : casos) {
        tsf_channel_sounds_off_all(s, 0);
        render(8, nullptr, nullptr);
        tsf_channel_set_sustain(s, 0, 1);
        for (int k : c.keys) tsf_channel_note_on(s, 0, k, 90.0f / 127.0f);
        render(blocos(0.25), nullptr, nullptr);
        for (int k : c.keys) tsf_channel_note_off(s, 0, k);
        render(blocos(1.0), nullptr, nullptr);

        // Nivel de referencia: os ultimos 8 blocos ANTES do pe sair.
        float ref[8]; double refRms[8];
        render(8, ref, refRms);
        float antes = 0.0f;
        double antesRms = 0.0, somaRms = 0.0;
        for (int i = 0; i < 8; ++i) { antes = std::max(antes, ref[i]); somaRms += refRms[i] * refRms[i]; }
        antesRms = std::sqrt(somaRms / 8.0);
        const int vozes = tsf_active_voice_count(s);
        // release do preset, em segundos, na primeira voz segurada (o --ring ja
        // mostrou que e o mesmo por registro nestes bancos)
        float presetRelease = -1.0f;
        for (unsigned int i = 0; i < s->voiceNum; ++i) {
            const struct tsf_voice* v = &s->voices[i];
            if (v->playingPreset != -1 && v->heldSustain) { presetRelease = v->ampenv.parameters.release; break; }
        }

        tsf_channel_set_sustain(s, 0, 0);            // <<-- o pe saindo
        if (trace) {
            // O que o abafador REALMENTE mandou para cada voz, lido antes de o
            // renderizar contar para tras: o envelope em RELEASE carrega em
            // samplesUntilNextSegment o numero de amostras ate -80 dB. e a
            // verificacao exata da tabela, imune a batida entre camadas que e o
            // que faz a coluna de RMS de uma tecla sola oscilar +/- 6 dB.
            std::printf("  -- vozes abafadas no pe (%s): tecla | feltro aplicado | tabela\n", c.nome);
            for (unsigned int i = 0; i < s->voiceNum; ++i) {
                const struct tsf_voice* v = &s->voices[i];
                if (v->playingPreset == -1) continue;
                std::printf("     key %3d | %6.3f s | %6.3f s | seg %d | loop %d\n",
                            v->playingKey,
                            double(v->ampenv.samplesUntilNextSegment) / 48000.0,
                            double(tsf_damped_release_seconds(v->playingKey)),
                            int(v->ampenv.segment), int(v->region->loop_mode));
            }
        }
        constexpr int kDepois = 190;                  // ~2 s de resolução de 10,7 ms
        std::vector<float> pico(kDepois, 0.0f);
        std::vector<double> rms(kDepois, 0.0);
        if (trace) {
            // O que cai em cada bloco: saida, vozes, e o ENVELOPE da primeira voz
            // segurada. As tres curvas separam "o feltro esta rapido" de "a amostra
            // acaba" (vozes caindo) de "o loop sustenido foi abandonado" (level alto,
            // saida zero). Sem isso so se sabe que cortou, nao o que cortou.
            // RMS por bloco (relativo ao nivel do pe). Numa tecla sola as ~4 camadas
            // do banco batem entre si e o bloco oscila +/- 6 dB sem nenhuma relacao
            // com o abafador -- por isso a leitura exata esta na linha "vozes
            // abafadas" acima. O caso que os ouvidos reclamam e o estrato, onde as
            // batidas se medio.
            std::printf("  -- trace %s: bloco | RMS dB | vozes | ampenv(min-max) | seg | loop | pos/loopEnd\n", c.nome);
            for (int b = 0; b < 60; ++b) {
                render(1, pico.data() + size_t(b), rms.data() + size_t(b));
                const struct tsf_voice* vit = nullptr;
                int nv = 0; double lo = -1.0, hi = -1.0;
                for (unsigned int i = 0; i < s->voiceNum; ++i) {
                    const struct tsf_voice* v = &s->voices[i];
                    if (v->playingPreset == -1) continue;
                    ++nv;
                    if (!vit) vit = v;
                    const double l = v->ampenv.level;
                    if (lo < 0.0 || l < lo) lo = l;
                    if (l > hi) hi = l;
                }
                char lvl[32];
                if (nv) std::snprintf(lvl, sizeof lvl, "%.3f-%.3f", lo, hi);
                else std::snprintf(lvl, sizeof lvl, "-");
                std::printf("  %3d | %7.1f | %3d | %13s | %d | %d | %.0f/%.0f\n",
                            b, antesRms > 0.0 ? 20.0 * std::log10(std::max(1e-12, rms[size_t(b)]) / antesRms) : -999.0,
                            nv, lvl, vit ? int(vit->ampenv.segment) : -1,
                            vit ? int(vit->region->loop_mode) : -1,
                            vit ? float(vit->sourceSamplePosition) : 0.0f, vit ? float(vit->loopEnd) : 0.0f);
            }
            render(kDepois - 60, pico.data() + 60, rms.data() + 60);
        } else {
            render(kDepois, pico.data(), rms.data());
        }

        const double refDb = antes > 0.0f ? 20.0 * std::log10(antes) : -200.0;
        // Acai em janela de 8 blocos (85 ms) antes de cruzar. Uma tecla destes
        // bancos tem ~4 regioes ao mesmo tempo; elas bateriam entre si e o RMS de
        // um bloco solteiro oscila +/- 6 dB por causa disso, nao por causa do
        // abafador (medido: tecla 69, blocos 10-12 = -8,6 / -8,6 / -14,3 dB).
        // O que se quer saber e a constante de tempo, e ela e da ordem de 100 ms a
        // 700 ms -- 85 ms de janela e o preco de tirar a batida sem achatar a curva.
        constexpr int kSuave = 8;
        std::vector<double> suave(rms.size(), 0.0);
        for (size_t i = 0; i < rms.size(); ++i) {
            double soma = 0.0; int n = 0;
            for (int d = -(kSuave / 2); d < kSuave / 2; ++d) {
                const long j = long(i) + d;
                if (j < 0 || j >= long(rms.size())) continue;
                soma += rms[size_t(j)] * rms[size_t(j)]; ++n;
            }
            suave[i] = n ? std::sqrt(soma / double(n)) : 0.0;
        }
        // Ultima bloco DEPOIS do qual o nivel nunca mais volta acima do limiar.
        auto cruza = [&](double dbRel) -> double {
            const double lim = antesRms * std::pow(10.0, dbRel / 20.0);
            size_t ultimo = suave.size();
            while (ultimo > 0) { --ultimo; if (suave[ultimo] > lim) break; }
            if (ultimo + 1 >= suave.size()) return -1.0;
            return double(ultimo + 1) * double(kBlock) * 1000.0 / 48000.0;
        };
        char rel[40];
        if (presetRelease < 0.0f) std::snprintf(rel, sizeof rel, "nenhuma segurada");
        else std::snprintf(rel, sizeof rel, "%.3f s%s", presetRelease,
                           presetRelease <= 0.011f ? " (FASTRELEASE!)" : "");
        // O que a tabela do abafador MANDA para as teclas deste caso, em segundos
        // ate -80 dB. E a previsao ao lado da medida: se a coluna -40 dB vale
        // metade disso, o feltro esta funcionando como modelado.
        char tab[32];
        {
            float tMin = 1e9f, tMax = 0.0f;
            for (int k : c.keys) {
                const float t = tsf_damped_release_seconds(k);
                if (t < tMin) tMin = t;
                if (t > tMax) tMax = t;
            }
            if (tMin == tMax) std::snprintf(tab, sizeof tab, "%.2f s", tMin);
            else std::snprintf(tab, sizeof tab, "%.2f-%.2f s", tMin, tMax);
        }
        char t[6][16];
        for (int i = 0; i < 3; ++i) {
            const double ms = cruza(-double(6 * (i + 1)));
            if (ms < 0.0) std::snprintf(t[i], sizeof t[i], "  >");
            else std::snprintf(t[i], sizeof t[i], "%.0fms", ms);
        }
        const double fim = (antesRms > 0.0 && suave[size_t(kDepois - 4)] > 0.0)
            ? 20.0 * std::log10(suave[size_t(kDepois - 4)] / antesRms) : -999.0;
        std::printf("  %-13s | %5d | %8.1f           | %6s %7s %7s | %7.1f dB | %s | %s\n",
                    c.nome, vozes, refDb, t[0], t[1], t[2], fim, rel, tab);
    }
    tsf_close(s);
    return 0;
}

// O que o tsf ENTENDEU do arquivo, linha por linha. Existe porque o banco
// sintetico dos testes sustenta 0 s com o pedal no ar enquanto um piano gravado
// sustenta 25 s, e nenhuma das medidas de audio diria se a culpa era do gerador
// (loop off? ampenv? shdr?) ou do modelo de sustain. Esta e a unica forma de
// separar as duas: imprime os campos que decidem o loop e o platô.
static int measureRegions(const std::string& path) {
    tsf* s = tsf_load_filename(path.c_str());
    if (!s) { std::fprintf(stderr, "nao abriu %s\n", path.c_str()); return 1; }
    std::printf("regioes entendidas de %s | presets=%d\n", path.c_str(), s->presetNum);
    for (int p = 0; p < s->presetNum; ++p) {
        const tsf_preset* pr = &s->presets[p];
        std::printf("  preset %d \"%s\": %d regiao(es)\n", p, pr->presetName, pr->regionNum);
        const int mostra = pr->regionNum < 3 ? pr->regionNum : 3;
        for (int r = 0; r < mostra; ++r) {
            const tsf_region& g = pr->regions[r];
            std::printf("    r%-4d loop_mode=%d loop[%u..%u] offset=%u end=%u sr=%u"
                        " key=%d..%d vel=%d..%d kc=%d"
                        " | ampenv A=%.4f D=%.4f S=%.4f R=%.4f"
                        " | att=%.2f fc=%d\n",
                        r, g.loop_mode, g.loop_start, g.loop_end, g.offset, g.end,
                        g.sample_rate, g.lokey, g.hikey, g.lovel, g.hivel,
                        g.pitch_keycenter, g.ampenv.attack, g.ampenv.decay,
                        g.ampenv.sustain, g.ampenv.release, g.attenuation,
                        g.initialFilterFc);
        }
    }
    tsf_close(s);
    return 0;
}

int main(int argc, char** argv) {
    int activeStrings = 0;
    bool hammer = false;
    std::string sf2Path;
    std::string sf2bPath;
    int heldNotes = 0;
    int poolVoices = kMaxVoices;      // o teto do Engine; --voices fala quanto ele custa
    bool ring = false;
    bool pedalload = false;
    bool pedallift = false;
    bool pedalliftTrace = false;
    bool pedalliftChain = false;
    bool regions = false;
    int makeSf2 = 0;
    double ringSeconds = 60.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--strings" && i + 1 < argc) activeStrings = std::atoi(argv[++i]);
        else if (a == "--sf2" && i + 1 < argc) sf2Path = argv[++i];
        else if (a == "--makesf2" && i + 1 < argc) makeSf2 = std::atoi(argv[++i]);
        else if (a == "--sf2b" && i + 1 < argc) sf2bPath = argv[++i];
        else if (a == "--notes" && i + 1 < argc) heldNotes = std::atoi(argv[++i]);
        else if (a == "--voices" && i + 1 < argc) poolVoices = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc) ringSeconds = std::atof(argv[++i]);
        else if (a == "--ring") ring = true;
        else if (a == "--pedalload") pedalload = true;
        else if (a == "--pedallift") pedallift = true;
        else if (a == "--trace") pedalliftTrace = true;
        else if (a == "--chain") pedalliftChain = true;
        else if (a == "--hammer") hammer = true;
        else if (a == "--regions") regions = true;
    }
    // Banco sintetico: as medidas de pedal/loop tem de rodar em maquina sem
    // soundfont nenhum (CI), entao quem nao recebeu --sf2 gera o seu. Nao e
    // substitute da medida com piano gravado -- um sample de 0,12 s em loop nao
    // tem o decaimento fisico de uma corda; e o instrumento certo para perguntar
    // "o arquivo diz loop?" e "o modelo sustenta?".
    // Com --makesf2 N --sf2 CAMINHO o arquivo gerado VAI PARA LA em vez do temp: e
    // o que permite ao CI renderizar com o pforge_standalone sem adivinhar onde o
    // temp_directory_path() escreve em cada sistema.
    if (makeSf2 > 0) {
        std::error_code ec;
        if (sf2Path.empty()) {
            const auto dir = std::filesystem::temp_directory_path(ec) / "pforge_synth";
            std::filesystem::create_directories(dir, ec);
            sf2Path = (dir / ("dsp_synth_" + std::to_string(makeSf2 - 1) + ".sf2")).string();
        }
        std::string berr;
        psynth::Config cfg;
        cfg.variant = makeSf2 - 1;
        cfg.seconds = makeSf2 == 1 ? 0.12 : 0.05;
        if (!psynth::build(sf2Path, cfg, berr)) {
            std::fprintf(stderr, "nao gerou banco sintetico: %s\n", berr.c_str());
            return 1;
        }
        std::printf("banco sintetico %s (%.1f MB)\n", sf2Path.c_str(),
                    double(std::filesystem::file_size(sf2Path, ec)) / (1024.0 * 1024.0));
    }
    if (regions) {
        if (sf2Path.empty()) { std::fprintf(stderr, "--regions pede --sf2 <banco> (ou --makesf2 1)\n"); return 1; }
        return measureRegions(sf2Path);
    }
    if (ring) {
        if (sf2Path.empty()) { std::fprintf(stderr, "--ring pede --sf2 <banco>\n"); return 1; }
        return measureRing(sf2Path, ringSeconds);
    }
    if (pedallift) {
        if (sf2Path.empty()) { std::fprintf(stderr, "--pedallift pede --sf2 <banco>\n"); return 1; }
        return measurePedalLift(sf2Path, poolVoices, pedalliftTrace, pedalliftChain);
    }
    if (pedalload) {
        if (sf2Path.empty()) { std::fprintf(stderr, "--pedalload pede --sf2 <banco>\n"); return 1; }
        return measurePedalLoadMain(sf2Path, ringSeconds, poolVoices);
    }

    std::vector<std::string> memLines;
    auto memRaw = [&](std::string s) { memLines.push_back(std::move(s)); };
    auto memAt = [&](const char* label) {
        const Mem m = readMem();
        char buf[160];
        if (m.ok)
            std::snprintf(buf, sizeof buf, "%-26s RSS=%8ld kB   HWM=%8ld kB   Size=%8ld kB",
                          label, m.vmRss, m.vmHwm, m.vmSize);
        else
            std::snprintf(buf, sizeof buf, "%-26s RAM: sem medida neste sistema", label);
        memLines.emplace_back(buf);
        return m;
    };
    memAt("inicio (sem nada)");

    SympatheticResonance symp;
    SoundboardEQ eq;
    LidFilter lid;
    Chorus chorus;
    Reverb reverb;
    MasterStage master;
    HammerModel hammerModel;
    symp.prepare(48000.0f);
    eq.prepare(48000.0f);
    lid.prepare(48000.0f);
    chorus.prepare(48000.0f);
    reverb.prepare(48000.0f);
    hammerModel.prepare(48000.0f);
    memAt("estagios preparados");

    // O estado que o teste G ve: nada tocando, pedal no meio, cadeia com cauda.
    symp.setParameters(0.25f, 0.5f);
    eq.setGains(0.5f, 0.5f, 0.5f);
    lid.setLid(1.0f);
    chorus.setMix(0.0f);
    reverb.setType(1);
    reverb.setMix(0.1f);
    for (int i = 0; i < activeStrings; ++i) symp.noteOn(40 + i, 0.4f);
    if (activeStrings > 0) {
        char lbl[64];
        std::snprintf(lbl, sizeof lbl, "+%d cordas simpaticas", activeStrings);
        memAt(lbl);
    }

    std::vector<float> refL(kBlock), refR(kBlock), L(kBlock), R(kBlock);
    feed(refL.data(), refR.data());

    Rows rows;
    std::string nanStage;

    auto bench = [&](const char* name, auto&& fn) {
        // Cada passagem TEM de ver a mesma entrada, inclusive as de repeticao de
        // tempo. Sem o memcpy por rep, symp/chorus/reverb -- que tem realimentacao
        // -- processavam a saidia da passagem anterior: a energia compunha, o
        // "dissipador" chegava a 1e26 e com 40 cordas virava NaN. O custo por
        // amostra desses estagios nao depende do sinal, entao medir com entrada
        // fixa nao muda o microsegundo; muda o que a ferramenta e capaz de acusar.
        const auto reset = [&] {
            std::memcpy(L.data(), refL.data(), kBlock * sizeof(float));
            std::memcpy(R.data(), refR.data(), kBlock * sizeof(float));
        };
        // 1x512: o que um bloco quieto custa.
        reset();
        fn(L.data(), R.data(), kBlock);
        double best1 = 1e30;
        for (int rep = 0; rep < kReps; ++rep) {
            reset();
            const auto t0 = wallNow();
            fn(L.data(), R.data(), kBlock);
            best1 = std::min(best1, std::chrono::duration<double, std::micro>(wallNow() - t0).count());
        }
        // 32x16: o MESMO bloco fatiado nas fronteiras do teste G.
        double bestS = 1e30;
        for (int rep = 0; rep < kReps; ++rep) {
            reset();
            const auto t0 = wallNow();
            for (size_t s = 0; s < kBlock; s += kSlice) fn(L.data() + s, R.data() + s, kSlice);
            bestS = std::min(bestS, std::chrono::duration<double, std::micro>(wallNow() - t0).count());
        }
        for (size_t i = 0; i < kBlock; ++i) sink += L[i] + R[i];
        // O dissipador existe para o -O2 nao apagar o trabalho; de quebra ele e o
        // detector de NaN da ferramenta, que antes so imprimia "-nan" sem dizer de
        // que estagio veio. Um estagio que cospe inf/NaN nao e "custo alto": e o
        // pior bug possivel numa cadeia de audio, e ele passa mudo no RMS.
        if (!std::isfinite(sink) && nanStage.empty()) nanStage = name;
        rows.add(name, best1, bestS, kBlock / kSlice);
    };

    bench("hammer", [&](float* l, float* r, size_t n) { hammerModel.process(l, r, n); });
    bench("sympResonance", [&](float* l, float* r, size_t n) { symp.process(l, r, n); });
    bench("soundboardEQ", [&](float* l, float* r, size_t n) { eq.process(l, r, n); });
    bench("lidFilter", [&](float* l, float* r, size_t n) { lid.process(l, r, n); });
    bench("chorus", [&](float* l, float* r, size_t n) { chorus.process(l, r, n); });
    bench("reverb", [&](float* l, float* r, size_t n) { reverb.process(l, r, n); });
    bench("masterStage", [&](float* l, float* r, size_t n) {
        master.process(l, r, n, 0.85f, 0.0f, 1.0f);
    });
    // A cadeia inteira, que e o que renderSlice() chama 32 vezes num bloco denso.
    bench("CADEIA", [&](float* l, float* r, size_t n) {
        hammerModel.process(l, r, n);
        symp.process(l, r, n);
        eq.process(l, r, n);
        lid.process(l, r, n);
        chorus.process(l, r, n);
        reverb.process(l, r, n);
        master.process(l, r, n, 0.85f, 0.0f, 1.0f);
    });

    // ---- o backend, medido do mesmo jeito -----------------------------------
    // Mesma chamada que renderSlice() faz: tsf_render_float(sf2, buf, n, 0), com
    // o buffer INTERCALADO (por isso o bench acima, em canais separados, nao o
    // cobre). `--notes N` segura N vozes tocando, que e o estado do teste G.
    if (!sf2Path.empty()) {
        const Mem beforeMem = readMem();
        tsf* s = tsf_load_filename(sf2Path.c_str());
        if (!s) {
            std::fprintf(stderr, "nao abriu %s\n", sf2Path.c_str());
        } else {
            const Mem loaded = memAt("banco 1 carregado");
            double diskMb = 0.0;
            std::error_code ec;
            if (const auto sz = std::filesystem::file_size(sf2Path, ec); !ec)
                diskMb = double(sz) / (1024.0 * 1024.0);
            {
                char note[160];
                const double ramMb =
                    (beforeMem.ok && loaded.ok) ? (loaded.vmRss - beforeMem.vmRss) / 1024.0 : 0.0;
                if (beforeMem.ok && loaded.ok)
                    std::snprintf(note, sizeof note,
                                  "%-26s arquivo=%7.1f MB   RAM que ele come=%7.1f MB",
                                  "  custo do banco:", diskMb, ramMb);
                else
                    std::snprintf(note, sizeof note,
                                  "%-26s arquivo=%7.1f MB   RAM: sem medida neste sistema",
                                  "  custo do banco:", diskMb);
                memRaw(note);
            }
            tsf_set_output(s, TSF_STEREO_INTERLEAVED, 48000, 0.0f);
            tsf_set_max_voices(s, poolVoices);
            tsf_channel_set_presetindex(s, 0, 0);
            std::vector<float> inter(kBlock * 2, 0.0f);
            for (int i = 0; i < heldNotes; ++i)
                tsf_channel_note_on(s, 0, 24 + i, float(70 + i % 40) / 127.0f);
            if (heldNotes > 0) {
                char lbl[64];
                std::snprintf(lbl, sizeof lbl, "+%d vozes alocadas", heldNotes);
                memAt(lbl);
            }
            auto t2 = [&](auto&& fn) {
                double best = 1e30;
                for (int rep = 0; rep < kReps; ++rep) {
                    const auto t0 = wallNow();
                    fn();
                    best = std::min(best, std::chrono::duration<double, std::micro>(wallNow() - t0).count());
                }
                return best;
            };
            const double one = t2([&] { tsf_render_float(s, inter.data(), int(kBlock), 0); });
            const double many = t2([&] {
                for (size_t off = 0; off < kBlock; off += kSlice)
                    tsf_render_float(s, inter.data() + off * 2, int(kSlice), 0);
            });
            for (float f : inter) sink += f;
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "%-22s  1x512=%7.2f us   %zux%zu=%7.2f us   fixo/fronteira=%6.2f us",
                          "tsf_render_float", one, kBlock / kSlice, kSlice, many,
                          (many - one) / double(kBlock / kSlice));
            rows.lines.insert(rows.lines.begin(), std::string(buf) +
                              "   [vozes ativas: " + std::to_string(tsf_active_voice_count(s)) +
                              " de um teto de " + std::to_string(poolVoices) + "]");
            tsf_close(s);
            memAt("apos tsf_close (banco 1)");
        }
        if (!sf2bPath.empty()) {
            // O pico da TROCA de banco: o Engine segura o banco velho ate a
            // primeira fronteira em que o run() nao o ve mais (fila de retire de
            // profundidade 1), entao os dois existem ao mesmo tempo.
            tsf* a = tsf_load_filename(sf2Path.c_str());
            if (a) memAt("troca: banco velho vivo");
            const Mem preMem = readMem();
            tsf* b = tsf_load_filename(sf2bPath.c_str());
            if (b) {
                const Mem both = memAt("troca: os dois vivos");
                char note[160];
                if (preMem.ok && both.ok)
                    std::snprintf(note, sizeof note,
                                  "%-26s banco 2 soma=%7.1f MB   PICO TOTAL=%7.1f MB (HWM)",
                                  "  pico da troca:", (both.vmRss - preMem.vmRss) / 1024.0,
                                  both.vmHwm / 1024.0);
                else
                    std::snprintf(note, sizeof note,
                                  "%-26s RAM: sem medida neste sistema (sem /proc/self/status)",
                                  "  pico da troca:");
                memRaw(note);
            } else std::fprintf(stderr, "nao abriu %s\n", sf2bPath.c_str());
            if (a) tsf_close(a);
            if (b) tsf_close(b);
        }
    }

    std::printf("custo por estagio (48 kHz, bloco %zu, fronteira %zu, strings=%d%s)\n",
                kBlock, kSlice, activeStrings, hammer ? " +hammer" : "");
    std::printf("\"fixo/fronteira\" = (32x16 - 1x512)/32: o que cada fronteira de\n"
                "                 fatia custa alem do trabalho por amostra.\n");
    for (const auto& line : rows.lines) std::printf("  %s\n", line.c_str());
    std::printf("\nmemoria (lida de /proc/self/status, nao por poll externo):\n");
    for (const auto& line : memLines) std::printf("  %s\n", line.c_str());
    std::printf("(dissipador %.6f)\n", sink);
    if (!nanStage.empty())
        std::printf("[FALHA] o estagio '%s' entregou amostra nao-finita (inf/NaN)\n",
                    nanStage.c_str());
    return 0;
}
