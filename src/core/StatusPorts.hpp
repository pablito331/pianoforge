#pragma once
// As duas portas de SAIDA de estado, criadas porque a UI nao tem como saber o que
// o plugin esta fazendo: "FORJANDO" era um chute do tamanho do arquivo.
//
// "Porta" aqui e o numero do EVENTO que o shell do VST3 entrega ao painel por
// ForgeUi::portEvent: 3..21 sao os controles, 22/23 sao estas duas saidas
// (format 0, float, como qualquer controle).
//
// Indices sao gravados em sessao de host -- nunca reordenar, so acrescentar depois.

#include <cstdint>

namespace pianoforge {

constexpr std::uint32_t kPortBankState   = 22;   // -4..255, ver codigos abaixo
constexpr std::uint32_t kPortPresetCount = 23;   // 0..1024 presets do banco publicado
constexpr std::uint32_t kPortTotal       = 24;

constexpr float kBankFailed  = -4.0f;   // o ultimo pedido nao virou som
constexpr float kBankLoading = -3.0f;   // worker/carregador em cima do arquivo
constexpr float kBankNone    = -2.0f;   // publicado: nenhum piano (silencio)
// >= 0: indice do banco que esta de fato publico e soando

inline int bankStateCode(bool loading, bool failed, int published) {
    if (loading) return static_cast<int>(kBankLoading);
    if (failed) return static_cast<int>(kBankFailed);
    return published < 0 ? static_cast<int>(kBankNone) : published;
}

inline bool bankLoading(float code) { return code == kBankLoading; }
inline bool bankFailed(float code) { return code == kBankFailed; }
inline int bankPublishedIndex(float code) {
    return code >= 0.0f ? static_cast<int>(code) : -1;
}

}  // namespace pianoforge
