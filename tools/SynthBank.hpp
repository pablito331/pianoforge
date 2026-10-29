// SF2 sintetico para as ferramentas de verificacao.
//
// Existe porque as provas do CI (o `--pedalload`, o render do standalone e o
// pforge_vst3_probe) e as medidas de custo do
// pforge_dsp_stages precisam de um banco REAL passando pelo tsf (leitura hydra,
// conversao int16->float, alocacao de voz, o amortecedor progressivo do
// PIANOFORGE), e o CI nao pode baixar soundfont de terceiros: amostra de piano
// gravada tem licença, e nada de licenca duvidosa entra no repositorio nem no
// artefato. Aqui o conteudo e nosso: senoides com harmônicos inteiros, geradas
// na hora, dominio publico por construcao.
//
// O que este banco PROVA: o caminho de carga, o tempo de evento, a troca de
// banco, os codigos de status, o teto de vozes e o pedal. O que ele NAO prova:
// qualquer coisa de timbre -- martelo, feltro e ressonancia dependem de amostra
// gravada, e continuam sendo conferidos a ouvido.
//
// Formato: RIFF 'sfbk' com as LISTAS de sempre (INFO, sdta, pdta), lido por
// tsf_load (src/core/tsf.h:1555). Duas regras do leitor desse arquivo mandam no
// layout aqui:
//   * o leitor de chunk NAO consome o pad de paridade do RIFF
//     (`parent->size -= 8 + chunk->size`, tsf.h:513) -- todo corpo e par;
//   * os limites de cada bag vem do registro SEGUINTE (tsf.h:767, 781), entao
//     phdr/inst/ibag/pbag precisam de terminador e os indices deles tem de
//     bater exatamente com o fim dos arrays correspondentes.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace psynth {

struct Config {
    int presets = 2;         // 2 -> presetCount > 1, e a pilula do painel tem do que tratar
    int keys = 88;           // A0..C8, a tessitura de um piano
    int firstKey = 21;
    int layers = 4;          // regioes empilhadas por tecla: o que satura o teto de vozes
    double seconds = 0.12;   // duracao do LOOP. O ring-out vem do envelope injetado pelo
                             // PIANOFORGE (tsf_undamped_decay_seconds), nao da amostra.
    uint32_t rate = 44100;
    int variant = 0;         // faz o segundo banco soar DIFERENTE do primeiro
    double releaseSec = 0.55;  // como nos pianos gravados: soltou a tecla, 0,55 s de release
    // A amostra sintetica e de amplitude CONSTANTE (senoide com harmônicos, sem
    // decaimento gravado), entao sem estes dois campos toda tecla pedalizada fica no
    // MESMO nivel e o fundo acumulado iguala-se a nota que esta entrando: os testes
    // L e M, que medem por DIFERENCA o quanto uma tecla soma na sua propria janela,
    // dao ganho ~0 dB e ate negativo sem defeito nenhum no produto. E isso nao e
    // "o banco sintetico e pior": um piano gravado tem 30-40 dB entre o ataque de
    // uma tecla nova e o chao das que ja vao morrendo, e o modelo de envelope e o
    // unico lugar onde o SF2 pode reproduzir essa diferenca. `decaySec` e o tempo ate
    // o platô; `sustainDb` e a atenuacao do platô em dB (o tsf guarda o gen em
    // deciBel, clamp[0,1440], e faz ganho = 10^(-raw/10), tsf.h:730).
    double decaySec = 0.28;
    double sustainDb = 22.0;
};

// ---------------------------------------------------------------- ESCRITA RAW
inline void put16(std::string& o, int v) {
    const uint16_t u = uint16_t(v);
    o.push_back(char(u & 0xff));
    o.push_back(char(u >> 8));
}

inline void put32(std::string& o, uint32_t u) {
    for (int i = 0; i < 4; ++i) o.push_back(char((u >> (8 * i)) & 0xff));
}

inline void putTag(std::string& o, const char* id) { o.append(id, 4); }

// Campo de nome do hydra: n bytes fixos (20 para tsf_char20), zerado a direita.
inline void putName(std::string& o, const std::string& s, size_t n) {
    std::vector<char> b(n, '\0');
    std::memcpy(b.data(), s.data(), std::min(s.size(), n));
    o.append(b.data(), n);
}

inline void putChunk(std::string& o, const char* id, const std::string& body) {
    putTag(o, id);
    put32(o, uint32_t(body.size()));
    o += body;
    // Nao emito pad: preferimos gritar a escrever um arquivo que o tsf le
    // dessincronizado (ver o comentario do cabecalho).
    if (body.size() & 1u)
        std::fprintf(stderr, "[SynthBank] corpo de '%s' com tamanho IMPAR: o leitor do "
                             "tsf nao consome pad e o arquivo ficaria illegivel\n", id);
}

inline void putList(std::string& o, const char* type, const std::string& body) {
    putTag(o, "LIST");
    put32(o, uint32_t(4 + body.size()));
    putTag(o, type);
    o += body;
}

// Tempo de envelope em timecents. <= 0 devolve -12000, que o tsf le como zero
// segundos (tsf_region_envtosecs pinca tudo abaixo de -11950).
inline int16_t timecents(double sec) {
    if (sec <= 0.0) return -12000;
    const double v = 1200.0 * std::log2(sec);
    if (v <= -12000.0) return -12000;
    if (v >= 32000.0) return 32000;
    return int16_t(std::lround(v));
}

// Numero de gerador SF2 2.01, com a unidade que o tsf espera (tsf.h:626-660):
// tempos em timecents, pan em unidades de 0,1%, atenuacao em centi-dB -- os
// fatores .001/.01 sao aplicados na fusao de regioes, nao na leitura.
enum : int {
    kGenStartAddrs = 0,           // tambem usado como terminador de zona (valor 0)
    kGenPan = 17,
    kGenAttackVolEnv = 34,
    kGenDecayVolEnv = 36,
    kGenSustainVolEnv = 37,       // 0 = ganho 1,0 (envtosecs: decibelsToGain(-0/10))
    kGenReleaseVolEnv = 38,
    kGenInstrument = 41,
    kGenKeyRange = 43,
    kGenVelRange = 44,
    kGenInitialAttenuation = 48,
    kGenSampleID = 53,
    kGenSampleModes = 54,         // 1 = loop continuo
    kGenScaleTuning = 56,         // pitch_keytrack, 100 = semitom por tecla
};

inline void putGen(std::string& o, int op, int amount) { put16(o, op); put16(o, amount); }
inline void putRange(std::string& o, int op, int lo, int hi) {
    put16(o, op);
    put16(o, (lo & 0xff) | ((hi & 0xff) << 8));
}

// ------------------------------------------------------------------ BANDAS
// Um piano gravado separa dinamica por FAIXA DE VELOCIDADE: tecla moida puxa uma
// amostra macia, tecla forte puxa outra ~15 dB mais alta. O banco sintetico
// precisa disso pelo mesmo motivo pelo qual o Config precisa do platô: os testes
// L e M medem por DIFERENCA quanto uma tecla soma na janela dela, e com todas as
// camadas no mesmo nivel e no mesmo velRange 0..127 o fundo de uma passagem moida
// chega na altura da nota que esta entrando. Medido: ganho de tecla = -0,27 dB
// (negativo!) numa build sem defeito nenhum.
//
// A banda tem de ser FUNCAO DA CAMADA E NADA MAIS: a amostra e indexada por
// (tecla, camada) e os presets usam os primeiros N layers, entao uma camada que
// mudasse de banda conforme o preset herdaria a amplitude da outra banda. Camada
// par = forte, impar = piano, assim os dois presets (4 e 2 camadas) tem uma banda
// de cada.
enum : int { kBandPiano = 0, kBandForte = 1 };
inline int bandOfLayer(int layer) { return (layer % 2 == 0) ? kBandForte : kBandPiano; }
inline double bandPeak(int band) { return band == kBandForte ? 0.115 : 0.02; }
inline void bandVelRange(int band, int& lo, int& hi) {
    if (band == kBandForte) { lo = 80; hi = 127; }
    else { lo = 0; hi = 79; }
}

// ---------------------------------------------------------------- AMOSTRA
// Uma onda por (tecla, camada). Ciclos INTEIROS dentro do trecho do loop, para
// a volta do loop não dar descontinuidade: ciclos = round(h·f0·frames/rate) e a
// frequencia efetiva e ciclos·rate/frames. Sem isso cada volta do loop seria um
// clique, e clique por periodo e exatamente o artefato que estas medidas nao
// podem conter.
inline void renderSample(std::vector<int16_t>& out, int key, int layer, int variant,
                         uint32_t rate, size_t frames, double targetPeak) {
    struct Partial { int harm; double amp; };
    // Camada = timbre empilhado na MESMA tecla, dentro da FAIXA DE VELOCIDADE da
    // camada. Empilhar e o que faz um piano gravado ter ~3-4 vozes por tecla e o que
    // lota o teto; separar por velocidade e o que faz o fundo de uma passagem moida
    // ficar 30 dB abaixo de uma tecla forte -- sem essa diferca de nivel medida, os
    // testes L e M (que avaliam "quanto uma tecla soma na janela dela") perdem poder
    // e dao ganho ~0 dB num banco onde nada esta mudo.
    static const Partial sets[4][5] = {
        { {1, 1.00}, {2, 0.34}, {3, 0.11}, {4, 0.05}, {0, 0.00} },   // fundamental
        { {1, 0.90}, {2, 0.52}, {4, 0.20}, {6, 0.08}, {0, 0.00} },   // oitava extra
        { {1, 0.55}, {3, 0.46}, {5, 0.26}, {7, 0.12}, {9, 0.06} },   // corda metalica
        { {2, 0.90}, {3, 0.55}, {6, 0.28}, {8, 0.14}, {0, 0.00} },   // brilho do ataque
    };

    const int set = (layer + variant) % 4;
    const double f0 = 440.0 * std::pow(2.0, (key - 69) / 12.0);
    std::vector<float> buf(frames, 0.0f);
    double peak = 0.0;
    for (const Partial* p = sets[set]; p->amp > 0.0; ++p) {
        // variant desloca meio ciclo por camada entre os bancos gerados: os dois
        // arquivos tem de soar diferente, senao o teste D ("trocar na volta")
        // estaria trocando um banco por ele mesmo.
        long cycles = std::lround(double(p->harm) * f0 * double(frames) / double(rate));
        if (cycles < 1) cycles = 1;
        cycles += ((variant + p->harm + layer) % 2);
        const double hz = double(cycles) * double(rate) / double(frames);
        const double ph = 0.7 * double(layer) + 1.1 * double(p->harm);
        for (size_t i = 0; i < frames; ++i)
            buf[i] += float(p->amp * std::sin(2.0 * 3.14159265358979323846 * hz *
                                              double(i) / double(rate) + ph));
    }
    for (const float v : buf) peak = std::max(peak, std::fabs(double(v)));
    const double gain = peak > 1e-9 ? targetPeak / peak : 0.0;
    out.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        const double s = std::lround(double(buf[i]) * gain * 32767.0);
        out[i] = int16_t(s < -32768.0 ? -32768.0 : (s > 32767.0 ? 32767.0 : s));
    }
}

// true com o caminho escrito; false com o motivo em `err`. Nada de excecao: as
// ferramentas rodam no CI e um erro de escrita tem de sair como linha, nao como
// abort do harness inteiro.
inline bool build(const std::string& path, const Config& c, std::string& err) {
    if (c.keys < 1 || c.layers < 1 || c.presets < 1 || c.seconds <= 0.001 ||
        c.firstKey < 0 || c.firstKey + c.keys > 128) {
        err = "configuracao do banco sintetico sem sentido";
        return false;
    }
    const size_t frames = size_t(std::llround(c.seconds * double(c.rate)));

    // ---- camadas realmente usadas: o preset 2 empilha menos, pra troca de preset
    //      mudar o numero de vozes por tecla (e nao so o rótulo na tela) ----
    std::vector<int> presetLayers(size_t(c.presets));
    std::vector<double> presetRelease(size_t(c.presets));
    std::vector<std::string> presetName(size_t(c.presets));
    int maxLayers = 0;
    for (int p = 0; p < c.presets; ++p) {
        presetLayers[size_t(p)] = p == 0 ? c.layers : std::max(1, c.layers / 2);
        presetRelease[size_t(p)] = c.releaseSec * (p == 0 ? 1.0 : 2.2);
        presetName[size_t(p)] = "Synth Grand" + std::to_string(p + 1);
        maxLayers = std::max(maxLayers, presetLayers[size_t(p)]);
    }

    // ---- sdta + shdr: ordem do laço e camada-maior, tecla-menor ----
    // O indice de amostra usado pelas zonas tem de bater com esta ordem, senao
    // cada tecla apontaria para a amostra de outra tecla (silencio "funcionando").
    const auto shdrIndex = [&](int key, int layer) {
        return layer * c.keys + (key - c.firstKey);
    };
    std::string smpl;
    std::string shdr;
    {
        std::vector<int16_t> wave;
        for (int layer = 0; layer < maxLayers; ++layer)
            for (int k = 0; k < c.keys; ++k) {
                const int key = c.firstKey + k;
                renderSample(wave, key, layer, c.variant, c.rate, frames,
                             bandPeak(bandOfLayer(layer)));
                const uint32_t start = uint32_t(smpl.size() / 2);
                for (const int16_t v : wave) put16(smpl, v);
                const uint32_t end = uint32_t(smpl.size() / 2);
                putName(shdr, "s" + std::to_string(key) + "l" + std::to_string(layer), 20);
                put32(shdr, start);
                put32(shdr, end);          // exclusivo, como pede o SF2
                put32(shdr, start);
                put32(shdr, end);
                put32(shdr, c.rate);
                shdr.push_back(char(uint8_t(key)));   // originalPitch = a propria tecla;
                shdr.push_back(char(int8_t(0)));       // pitchCorrection em cents
                put16(shdr, 0);                          // sampleLink
                put16(shdr, 1);                          // sampleType = mono
            }
    }
    // terminador EOS (46 bytes zerados): nenhum instrumento o referencia, mas todo
    // leitor SF2 espera, e o tamanho do chunk tem de continuar par.
    putName(shdr, "EOS", 20);
    for (int i = 0; i < 5; ++i) put32(shdr, 0);
    put16(shdr, 0);
    put16(shdr, 0);
    shdr.push_back('\0');
    shdr.push_back('\0');

    std::string sdta;
    putChunk(sdta, "smpl", smpl);

    // ---- pdta: zonas por instrumento ----
    // Os dois indices sao os unicos campos de bagagem que existem, e cada um
    // aponta para o ARRAY DO OUTRO Chunk: ibag.instGenNdx e a posicao em igen,
    // inst.instBagNdx e a posicao em ibag. Errar um deles nao da excecao nem
    // aviso -- o leitor percorre a janela [bag[i], bag[i+1]) e passa a ler um
    // gen de cada vez, entao cada zona herda o generador SEGUINTE do arquivo
    // inteiro. O sintoma medido foi este: preset 0 com 528 regioes em vez de 352,
    // preset 1 com zero, keyRange 21 em zona da tecla 22, e loop_mode=0 (SampleModes
    // nunca chegava a ser aplicado) -> banco que "toque" mas nao sustenta nada.
    std::string ibag, igen, inst;
    for (int p = 0; p < c.presets; ++p) {
        putName(inst, presetName[size_t(p)], 20);
        put16(inst, int(ibag.size() / 4));               // instBagNdx: primeira bag deste instrumento
        for (int layer = 0; layer < presetLayers[size_t(p)]; ++layer)
            for (int k = 0; k < c.keys; ++k) {
                const int key = c.firstKey + k;
                const int band = bandOfLayer(layer);
                int vLo = 0, vHi = 127;
                bandVelRange(band, vLo, vHi);
                put16(ibag, int(igen.size() / 4));       // instGenNdx: primeiro gen desta zona
                put16(ibag, 0);                          // instModNdx: nada de modulator
                putRange(igen, kGenKeyRange, key, key);
                putRange(igen, kGenVelRange, vLo, vHi);
                putGen(igen, kGenAttackVolEnv, timecents(0.002));
                putGen(igen, kGenDecayVolEnv, timecents(c.decaySec));
                putGen(igen, kGenSustainVolEnv, int(c.sustainDb * 10.0));
                putGen(igen, kGenReleaseVolEnv, timecents(presetRelease[size_t(p)]));
                putGen(igen, kGenPan, (layer % 2) ? -90 : 90);
                putGen(igen, kGenInitialAttenuation, 100 * layer);
                putGen(igen, kGenSampleModes, 1);
                putGen(igen, kGenScaleTuning, 100);
                putGen(igen, kGenSampleID, shdrIndex(key, layer));
                putGen(igen, kGenStartAddrs, 0);        // terminador da zona
            }
    }
    const uint32_t ibagCount = uint32_t(ibag.size() / 4);
    put16(ibag, int(igen.size() / 4));   // ibag terminator: genNdx = fim dos igens
    put16(ibag, 0);
    putName(inst, "EOS", 20);
    put16(inst, int(ibagCount));         // inst terminator: bagNdx = fim das ibags

    // ---- pdta: presets ----
    // ---- pdta: presets ----
    // `presetBagNdx` e indice de BAGAGEM, nao de gen: com 2 pgens por preset o
    // valor 2*p faria o preset 0 engolir a bag do preset 1 (528 regioes) e o
    // ultimo preset ficar com janela vazia (0 regioes). A janela de cada record
    // de phdr/inst e sempre [ndx_deste, ndx_proximo), e o proximo vem do record
    // SEGUINTE do mesmo array -- por isso os terminadores acima importam.
    std::string pbag, pgen, phdr;
    for (int p = 0; p < c.presets; ++p) {
        put16(pbag, int(pgen.size() / 4));
        put16(pbag, 0);
        putGen(pgen, kGenInstrument, p);
        putGen(pgen, kGenStartAddrs, 0);
    }
    put16(pbag, int(pgen.size() / 4));
    put16(pbag, 0);
    for (int p = 0; p < c.presets; ++p) {
        putName(phdr, presetName[size_t(p)], 20);
        put16(phdr, p);            // preset number
        put16(phdr, 0);            // bank
        put16(phdr, int(p));       // presetBagNdx: UMA pbag por preset (2 pgens dentro dela)
        put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);
    }
    putName(phdr, "EOP", 20);
    put16(phdr, 0);
    put16(phdr, 0);
    put16(phdr, int(c.presets));   // bagNdx do terminador = numero de pbags reais
    put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);

    // pmod/imod: o tsf ignora modulators (tsf.h:875, 883) mas EXIGE os chunks
    // presentes (tsf.h:1617), entao um registro nulo de 10 bytes cada.
    std::string mod;
    for (int i = 0; i < 5; ++i) put16(mod, 0);

    std::string pdta;
    putChunk(pdta, "phdr", phdr);
    putChunk(pdta, "pbag", pbag);
    putChunk(pdta, "pmod", mod);
    putChunk(pdta, "pgen", pgen);
    putChunk(pdta, "inst", inst);
    putChunk(pdta, "ibag", ibag);
    putChunk(pdta, "imod", mod);
    putChunk(pdta, "igen", igen);
    putChunk(pdta, "shdr", shdr);

    // ---- INFO: opcional para o tsf, obrigatorio para qualquer outro leitor ----
    // Todos os corpos abaixo tem tamanho PAR por serem montados com putName(n par)
    // ou com put16/put32: o pad impar e o que dessincroniza o leitor do tsf.
    std::string info, ifil, isng, inam, isft, icrd, ieng;
    put16(ifil, 2);
    put16(ifil, 4);                       // SF2 2.4
    putName(isng, "PianoForge synth", 20);
    putName(inam, "PianoForge Synthetic", 24);
    putName(isft, "pianoforge", 24);
    putName(icrd, "2026 PianoForge", 16);
    putName(ieng, "tools/SynthBank.hpp", 20);
    putChunk(info, "ifil", ifil);
    putChunk(info, "isng", isng);
    putChunk(info, "INAM", inam);
    putChunk(info, "irom", std::string(2, '\0'));   // par, vazio
    putChunk(info, "iver", std::string(2, '\0'));
    putChunk(info, "isft", isft);
    putChunk(info, "ICRD", icrd);
    putChunk(info, "IENG", ieng);
    putChunk(info, "IMED", std::string(2, '\0'));
    putChunk(info, "IPRD", std::string(2, '\0'));
    putChunk(info, "ICOP", std::string(2, '\0'));
    putChunk(info, "ICMT", std::string(2, '\0'));
    putChunk(info, "ISOF", std::string(2, '\0'));

    std::string body;
    putList(body, "INFO", info);
    putList(body, "sdta", sdta);
    putList(body, "pdta", pdta);

    std::string file;
    putTag(file, "RIFF");
    put32(file, uint32_t(4 + body.size()));
    putTag(file, "sfbk");
    file += body;

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "nao abriu " + path + " para escrita"; return false; }
    const size_t wrote = std::fwrite(file.data(), 1, file.size(), f);
    if (std::fclose(f) != 0 || wrote != file.size()) {
        err = "escrita incompleta em " + path;
        return false;
    }
    return true;
}

// Banco sintetico numero `index` (0, 1, ...): conteudo tonal deslocado por indice
// e amostra mais CURTA a partir do segundo, para que os bancos gerados tenham
// tamanhos DIFERENTES. Isso importa para o teste J, que escolhe "o maior" e "o
// menor" pelo tamanho do arquivo: com dois bancos do mesmo tamanho nao ha troca de
// banco MENOR nenhuma, e o pedido estacionado deixaria de ser medido.
inline bool buildIndexed(const std::string& path, int index, std::string& err) {
    Config c;
    c.variant = index;
    c.seconds = index == 0 ? 0.12 : 0.05;
    return build(path, c, err);
}

} // namespace psynth
