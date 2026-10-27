#pragma once
#include "BiquadFilter.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pianoforge {
namespace dsp {

// xorshift32: duas sequencias independentes por voz, uma por canal. Ruido
// identico nos dois lados e o que faz um transiente stereo virar um "pipoco" no
// centro da imagem.
inline float hammerNoise(uint32_t& s) noexcept {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return static_cast<float>(static_cast<int32_t>(s)) * (1.0f / 2147483648.0f);
}

// O ruido acima e uniforme em [-1,1), entao tem desvio padrao 1/sqrt(3) e nao 1. A
// normalizacao por resposta ao impulso assume entrada de variancia 1 -- sem este
// fator o clique sairia 5,5 dB mais alto do que foi medido.
inline constexpr float kHammerNoiseStd = 0.57735027f;

// Soma h[n]^2 da resposta ao impulso de um estagio (ou da cascata a->b). Por Parseval
// isso e a variancia na saida quando entra ruido branco de variancia 1: o fator de
// normalizacao exato, que nao depende de em que fase o ruido caiu.
inline float irEnergy(BiquadFilter a) noexcept {
    a.reset();
    float acc = 0.0f;
    for (int i = 0; i < 256; ++i) {
        const float h = a.process(i == 0 ? 1.0f : 0.0f);
        acc += h * h;
    }
    return std::sqrt(acc);
}

inline float irEnergy(BiquadFilter a, BiquadFilter b) noexcept {
    a.reset();
    b.reset();
    float acc = 0.0f;
    for (int i = 0; i < 256; ++i) {
        const float h = b.process(a.process(i == 0 ? 1.0f : 0.0f));
        acc += h * h;
    }
    return std::sqrt(acc);
}

struct HammerVoice {
    bool active = false;
    float envClick = 0.0f;
    float envThump = 0.0f;
    float decayClick = 0.99f;
    float decayThump = 0.999f;
    float gainClickL = 0.0f, gainClickR = 0.0f;
    float gainThump = 0.0f;
    BiquadFilter clickL;
    BiquadFilter clickR;
    BiquadFilter thump;
    uint32_t rngA = 0x9e3779b9u;
    uint32_t rngB = 0x85ebca6bu;
    uint32_t rngC = 0x1b873593u;
};

// O ataque de um piano nao e um clique: sao duas coisas acontecendo em tempos
// diferentes. O "click" e o feltro saindo da corda (800 Hz..5 kHz, ~10 ms) e o
// "thump" e a massa do martelo batendo no fundo da caixa (graves, ~50 ms), que e
// o que faz o ouvinte sentir a nota antes de ouvir. Antes so existia o primeiro,
// com ganho cru de banda estreita -- medido, o knob todo no maximo mudava 1,8% da
// energia do render, e o ouvido nao separa isso do sample.
class HammerModel {
public:
    void prepare(float sampleRate) {
        sr = sampleRate > 1000.0f ? sampleRate : 44100.0f;
        // O bandpass do clique e de 2a ordem (12 dB/oitava) e vaza agudo: medido no
        // FFT, quase um terco da energia saia em 8-22 kHz, que e assobio de filtro e
        // nao feltro. Este lowpass e o teto. Ele e FIXO, por isso mora no modelo e nao
        // na voz: filtrar a soma dos cliques e o mesmo que filtrar cada clique e somar
        // (o filtro e linear), e custa 2 biquads por bloco em vez de 32 por bloco.
        clickTail_[0].setup(BiquadFilter::Lowpass, kTailHz, sr, 0.7071f);
        clickTail_[1].setup(BiquadFilter::Lowpass, kTailHz, sr, 0.7071f);
        clickTail_[0].reset();
        clickTail_[1].reset();
        for (auto& v : voices) {
            v.active = false;
            v.envClick = 0.0f;
            v.envThump = 0.0f;
            v.clickL.reset();
            v.clickR.reset();
            v.thump.reset();
        }
        buildCal();
    }

    // Nivel de cada caminho, por nota, medido uma vez e guardado. trigger() roda na
    // thread de audio e 768 estagios de filtro por nota (0,012 ms) viram 0,8 ms numa
    // rajada de 64 notas -- o suficiente para o teste G esbarrar no teto de 1 ms.
    // Aqui e 128 notas de uma vez, no prepare(), fora do tempo real.
    void buildCal() {
        const float tone = toneValue();
        for (int n = 0; n < 128; ++n) {
            const float noteNorm = static_cast<float>(n) / 127.0f;
            const float toneFactor = std::pow(2.0f, (tone - 0.5f) * 2.0f);
            const float clickFreq = std::fmin(700.0f + noteNorm * 3800.0f, sr * 0.45f) * toneFactor;
            const float thumpFreq = 90.0f + noteNorm * 140.0f;
            BiquadFilter bp, lp = clickTail_[0], th;
            bp.setup(BiquadFilter::Bandpass, clickFreq, sr, 1.4f);
            th.setup(BiquadFilter::Lowpass, thumpFreq, sr, 0.7071f);
            cal_[n].rmsC = irEnergy(bp, lp) * kHammerNoiseStd;
            cal_[n].rmsT = irEnergy(th) * kHammerNoiseStd;
        }
        calSr = sr;
    }

    // So o `hammerTone` muda a tabela, e ele e uma das portas escondidas (nem no
    // painel ha knob pra ele). Um host automatizando-brilho paga 1 ms uma vez por
    // passo de 0,02 -- por isso o degrau, senao cada amostra de controle re-fazia tudo.
    // O degrau e um inteiro de proposito: comparar o float quantizado (`round(t*50)*0,02`)
    // com o valor guardado nunca dava igual, e o primeiro trigger() de cada nota
    // refazia a tabela inteira dentro do bloco de audio.
    void refreshCalFor(float hammerTone) {
        int step = static_cast<int>(std::lround(hammerTone * 50.0f));
        step = std::min(50, std::max(0, step));
        if (calSr == sr && calToneStep == step) return;
        calToneStep = step;
        buildCal();
    }

    float toneValue() const { return static_cast<float>(calToneStep) * 0.02f; }

    void trigger(int note, int velocity, float hammerAmount, float hammerTone) noexcept {
        if (hammerAmount <= 0.001f || velocity <= 0) return;

        int slot = -1;
        float minEnv = 2.0f;
        for (size_t i = 0; i < voices.size(); ++i) {
            if (!voices[i].active) { slot = static_cast<int>(i); break; }
            if (voices[i].envClick < minEnv) { minEnv = voices[i].envClick; slot = int(i); }
        }
        if (slot < 0) slot = 0;

        auto& v = voices[slot];
        const float noteNorm = static_cast<float>(note & 127) / 127.0f;
        refreshCalFor(hammerTone);

        // Click: feltro. Grave bate mais fundo e mais longo, agudo e seco. O tom e o
        // da tabela (quantizado), senao o filtro ao vivo e o nivel medido seriam de
        // duas notas diferentes.
        const float toneFactor = std::pow(2.0f, (toneValue() - 0.5f) * 2.0f);
        const float clickFreq = std::fmin(700.0f + noteNorm * 3800.0f, sr * 0.45f) * toneFactor;
        const float clickSec = 0.020f - noteNorm * 0.011f;       // 20 ms -> 9 ms
        // Thump: a caixa. Segue a nota de leve, mas e banda larga e grave sempre.
        const float thumpFreq = 90.0f + noteNorm * 140.0f;       // 90 Hz -> 230 Hz
        // 75 ms virou 32 ms: com a envelope longa, o thump somava mais ENERGIA que o
        // click mesmo com menos amplitude (a envelope decide o tempo integrado), e o
        // resultado era um "tum" de caixa de som. Um martelo encosta e sai.
        const float thumpSec = 0.032f - noteNorm * 0.014f;       // 32 ms -> 18 ms

        v.clickL.setup(BiquadFilter::Bandpass, clickFreq, sr, 1.4f);
        v.clickR.setup(BiquadFilter::Bandpass, clickFreq, sr, 1.4f);
        v.thump.setup(BiquadFilter::Lowpass, thumpFreq, sr, 0.7071f);
        v.decayClick = std::exp(-1.0f / (clickSec * sr));
        v.decayThump = std::exp(-1.0f / (thumpSec * sr));

        // O nivel vem da tabela (ver buildCal): medir a resposta ao impulso aqui, por
        // nota, e o que colocava 0,8 ms de filtro dentro de um bloco de 512 amostras.
        //
        // Medir a ENERGIA DA RESPOSTA AO IMPULSO, e nao uma RMS amostrada em janela de
        // ruido, por Parseval: soma h[n]^2 e exatamente a variancia que um ruido branco
        // unitario sairia a ter. Motivos:
        //   - amostragem: um lowpass de 90 Hz tem periodo de 11 ms; numa janela de 256
        //     amostras (5,8 ms) a RMS era sorte, e cada nota saia com um nivel de thump
        //     diferente conforme a fase em que o ruido caia;
        //   - custo: para a estimativa por ruido convergir a janela teria de ser ~1024
        //     amostras por nota, e 64 notas num bloco estouravam o orcamento (1,18 ms
        //     medidos no teste G).
        const float rmsC = cal_[note & 127].rmsC;
        const float rmsT = cal_[note & 127].rmsT;
        if (rmsC < 1e-6f || rmsT < 1e-6f) return;

        // O ruido do feltro cresce menos que a amplitude da corda: numa nota
        // pianissima o clique e a coisa mais alta que se ouve. vel^2 (o antigo)
        // fazia o contrario -- sumia o ataque justo onde ele mais importa.
        const float velNorm = static_cast<float>(velocity) / 127.0f;
        // Os dois niveis sao medidos, nao chutados. Como cada caminho e normalizado
        // pela propria RMS, o que decide o que se escuta e a duracao da envelope: com
        // 0.045 sobre 75 ms o thump levava 78% da energia adicionada, um "toc" de
        // caixa de som em vez de piano. O par aqui divide o ataque em ~metade corpo,
        // metade feltro -- que e o que sobe de um martelo de cauda gravado de perto.
        const float click = std::pow(velNorm, 0.55f) * hammerAmount * 0.115f / rmsC;
        const float thump = std::pow(velNorm, 0.80f) * hammerAmount * 0.034f / rmsT;

        // Numa cauda de cauda, o grave fica a esquerda e o agudo a direita. E pouco
        // (0,3) e so no click: o thump e grave demais para ter direcao.
        const float pan = (noteNorm - 0.5f) * 0.6f;
        v.gainClickL = click * (1.0f - pan) * 0.5f;
        v.gainClickR = click * (1.0f + pan) * 0.5f;
        v.gainThump = thump;
        v.envClick = 1.0f;
        v.envThump = 1.0f;
        // Semente por nota e por disparo: duas notas do mesmo intervalo nao podem
        // cuspir o mesmo ruido, senao o ataque vira um sample repetido.
        rngTick += 0x2545F491u;
        v.rngA = 0x9e3779b9u ^ (uint32_t(note) * 0x6c078965u) ^ rngTick;
        v.rngB = 0x85ebca6bu ^ (uint32_t((note + 41) & 127) * 0x6c078965u) ^ rngTick;
        v.rngC = 0x1b873593u ^ (uint32_t((note + 89) & 127) * 0x85ebca6bu) ^ (rngTick * 3u);
        if (v.rngA == 0) v.rngA = 0x6d2b79f5u;
        if (v.rngB == 0) v.rngB = 0x3c3ef461u;
        if (v.rngC == 0) v.rngC = 0x27d4eb2fu;
        v.active = true;
    }

    void process(float* left, float* right, size_t numFrames) noexcept {
        for (size_t i = 0; i < numFrames; ++i) {
            float sumL = 0.0f, sumR = 0.0f, thump = 0.0f;
            for (auto& v : voices) {
                if (!v.active) continue;
                const float e = v.envClick, t = v.envThump;
                // Tres fontes de ruido e tres filtros: o mesmo ruido pelo mesmo
                // filtro nos dois lados colaria o clique e o transiente -- que e o
                // que da a sensacao de caixa -- viraria "mono".
                thump += v.thump.process(hammerNoise(v.rngA)) * t * v.gainThump;
                sumL += v.clickL.process(hammerNoise(v.rngB)) * e * v.gainClickL;
                sumR += v.clickR.process(hammerNoise(v.rngC)) * e * v.gainClickR;
                v.envClick *= v.decayClick;
                v.envThump *= v.decayThump;
                if (v.envClick < 0.0005f && v.envThump < 0.0005f) v.active = false;
            }
            // O teto de 9,5 kHz entra AQUI e nao dentro da voz: o filtro e linear,
            // entao aplicar na soma dos cliques da o mesmo numero com 2 biquads por
            // bloco em vez de 2 por voz por amostra. Com 16 vozes vivas num bloco de
            // 512 isso era ~0,2 ms -- o suficiente para estourar o teste G.
            left[i] += clickTail_[0].process(sumL) + thump;
            right[i] += clickTail_[1].process(sumR) + thump;
        }
    }

private:
    static constexpr float kTailHz = 9500.0f;
    float sr = 44100.0f;
    uint32_t rngTick = 0;
    BiquadFilter clickTail_[2];
    // Nivel medido por nota (ver buildCal). Nao e mutable state de voz: e so a
    // constante de ganho daquela nota naquele sample rate / brilho.
    struct Cal { float rmsC = 1.0f, rmsT = 1.0f; };
    std::array<Cal, 128> cal_;
    float calSr = 0.0f;
    int calToneStep = 25;
    std::array<HammerVoice, 16> voices;
};

} // namespace dsp
} // namespace pianoforge
