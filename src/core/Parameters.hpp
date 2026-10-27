#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <string>

namespace pianoforge {

enum ParameterId : int {
    // 10 Visible Knobs
    Param_Volume = 0,
    Param_Hammer = 1,
    Param_VelCurve = 2,
    Param_SympRes = 3,
    Param_Lid = 4,
    Param_BassEQ = 5,
    Param_MidEQ = 6,
    Param_HighEQ = 7,
    Param_Chorus = 8,
    Param_Reverb = 9,

    // 7 Hidden Parameters
    Param_ReverbType = 10,
    Param_TrimDb = 11,
    Param_Width = 12,
    Param_ReleaseDamp = 13,
    Param_HammerTone = 14,
    Param_SympTune = 15,
    Param_EngineTune = 16,

    Param_Count = 17
};

struct ParameterDef {
    const char* symbol;
    const char* name;
    float minValue;
    float maxValue;
    float defaultValue;
    bool isVisible;
    // Nome curto para debaixo do knob. O `name` e o que o HOST mostra (la ha
    // espaco); na tela "Ataque do Martelo" virava "Ataque do Mart...", e cinco
    // rotulos truncados e exatamente o que se le como "botoes desorganizados".
    const char* shortName;
    // A linha miuda debaixo do nome, do prototipo v2 (.knob-tip): UMA palavra, nao
    // uma frase. Com frase, oito dos dez discos saem com "..." embaixo e a tela
    // volta a ler-se como coisa truncada -- o `title=` do SVG (a frase cheia) so
    // existe porque o navegador tem tooltip; painel não tem.
    const char* tip;
};

// Nomes em português para os controles
static const ParameterDef kParameterDefs[Param_Count] = {
    // Controles Visíveis (10 Knobs Principais)
    { "volume",      "Volume",                  0.0f,   1.0f,   0.85f, true,  "Volume",      "nivel" },
    { "hammer",      "Ataque do Martelo",       0.0f,   1.0f,   0.35f, true,  "Martelo",     "" },
    { "velCurve",    "Curva de Dinâmica",       0.0f,   1.0f,   0.5f,  true,  "Dinâmica",    "velocity" },
    { "sympRes",     "Ressonância das Cordas",  0.0f,   1.0f,   0.25f, true,  "Ressonância", "" },
    { "lid",         "Abertura da Tampa",       0.0f,   1.0f,   1.0f,  true,  "Tampa",       "abertura" },
    { "bassEQ",      "Graves (EQ)",             0.0f,   1.0f,   0.5f,  true,  "Graves",      "tom" },
    { "midEQ",       "Médios (EQ)",             0.0f,   1.0f,   0.5f,  true,  "Médios",      "tom" },
    { "highEQ",      "Agudos (EQ)",             0.0f,   1.0f,   0.5f,  true,  "Agudos",      "tom" },
    { "chorus",      "Chorus",                  0.0f,   1.0f,   0.0f,  true,  "Chorus",      "" },
    { "reverb",      "Reverb (Ambiência)",      0.0f,   1.0f,   0.10f, true,  "Reverb",      "" },

    // Controles de Ajuste Fino
    { "reverbType",  "Tipo de Sala",            0.0f,   3.0f,   1.0f,  false, "Sala",        "" },
    { "trimDb",      "Ganho de Saída (Trim dB)", -12.0f, 12.0f,  0.0f,  false, "Trim",       "" },
    { "width",       "Largura Estéreo",         0.0f,   2.0f,   1.0f,  false, "Largura",     "" },
    { "releaseDamp", "Amortecimento de Soltura", 0.0f,  1.0f,   0.35f, false, "Abafar",      "" },
    { "hammerTone",  "Brilho do Martelo",       0.0f,   1.0f,   0.5f,  false, "Brilho",      "" },
    { "sympTune",    "Afinação da Ressonância", 0.0f,   1.0f,   0.5f,  false, "Afin. res.",  "" },
    { "engineTune",  "Afinação Global (Cents)", -100.0f, 100.0f, 0.0f,  false, "Afin. geral", "" }
};

class SmoothedValue {
public:
    void reset(float initialValue, float sampleRate, float smoothTimeSec = 0.02f) {
        current = initialValue;
        target = initialValue;
        // Guardar a constante de tempo por amostra, nao um coeficiente "por
        // chamada": quem chama e o bloco, e o tamanho do bloco muda.
        invTauPerSample = sampleRate > 1000.0f
            ? 1.0f / (smoothTimeSec * sampleRate)
            : 0.01f;
    }

    void setTarget(float newTarget) noexcept {
        target = newTarget;
    }

    float get() const noexcept {
        return current;
    }

    float getTarget() const noexcept {
        return target;
    }

    // Avanca o bloco inteiro de uma vez. Chamar isto uma vez por bloco com o
    // coeficiente de uma amostra (o bug antigo) transformava os 20 ms em ~5 s num
    // bloco de 256: EQ, Reverb, Chorus, Tampa e Volume pareciam mortos. O resultado
    // e o mesmo decaimento exponencial de uma passada por amostra, avaliado no fim
    // do bloco -- que e o instante em que o valor volta a ser lido.
    void advance(size_t frames) noexcept {
        if (frames == 0) return;
        const float f = float(frames > 100000 ? 100000 : frames);
        const float alpha = 1.0f - std::exp(-f * invTauPerSample);
        current += (target - current) * alpha;
    }

private:
    float current = 0.0f;
    float target = 0.0f;
    float invTauPerSample = 0.01f;
};

class ParameterManager {
public:
    ParameterManager() {
        for (int i = 0; i < Param_Count; ++i) {
            values[i].store(kParameterDefs[i].defaultValue, std::memory_order_relaxed);
            smoothed[i].reset(kParameterDefs[i].defaultValue, 44100.0f);
        }
    }

    void prepare(float sampleRate) {
        for (int i = 0; i < Param_Count; ++i) {
            float val = values[i].load(std::memory_order_relaxed);
            smoothed[i].reset(val, sampleRate, 0.02f);
        }
    }

    void setValue(int paramId, float value) noexcept {
        if (paramId >= 0 && paramId < Param_Count) {
            // Nao e higiene: fmin/fmax devolvem o operando NAO-NaN, entao um NaN que
            // venha de automacao quebrada nao "passa" pelo clamp -- ele vira
            // maxValue. No volume, isso e um host com um glitch mandando o piano a
            // fundo. Descartar e o unico comportamento seguro.
            if (!std::isfinite(value)) return;
            const auto& def = kParameterDefs[paramId];
            float clamped = std::fmax(def.minValue, std::fmin(def.maxValue, value));
            values[paramId].store(clamped, std::memory_order_relaxed);
            smoothed[paramId].setTarget(clamped);
        }
    }

    float getValue(int paramId) const noexcept {
        if (paramId >= 0 && paramId < Param_Count) {
            return values[paramId].load(std::memory_order_relaxed);
        }
        return 0.0f;
    }

    float getSmoothed(int paramId) const noexcept {
        if (paramId >= 0 && paramId < Param_Count) {
            return smoothed[paramId].get();
        }
        return 0.0f;
    }

    void updateBlock(size_t frames) noexcept {
        for (int i = 0; i < Param_Count; ++i) {
            smoothed[i].setTarget(values[i].load(std::memory_order_relaxed));
            smoothed[i].advance(frames);
        }
    }

private:
    std::array<std::atomic<float>, Param_Count> values;
    std::array<SmoothedValue, Param_Count> smoothed;
};

} // namespace pianoforge
