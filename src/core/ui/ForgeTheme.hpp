#pragma once
// Paleta da forja, copiada do conceito v2 ("ideia para ui/PianoForge-Concept-v2/
// index.html", bloco :root + as regras de cada componente). Nao e re-invencao:
// cada valor abaixo existe literalmente no CSS do prototipo, e o prototipo e a
// referencia visual aprovada -- quando este arquivo divergir do HTML, o HTML
// ganha.
//
// O ferro do prototipo e mais quente e mais claro que o primeiro: #13161c de
// janela, #171b22 de chapa, e a brasa e sempre a MESMA (#ff7a18) -- o desenho
// nao esquenta o knob com o valor, ele acende a brasa e apaga o resto.

#include "Surface.hpp"

namespace pforge {

namespace forge {

// --- ferros (CSS: --iron0..--iron4) -----------------------------------------
constexpr uint32_t iron0   = 0xFF0D0F13u;  // fundo do gradiente da pagina
constexpr uint32_t iron1   = 0xFF13161Cu;  // fundo da janela
constexpr uint32_t iron2   = 0xFF191D24u;  // po de ferro
constexpr uint32_t iron3   = 0xFF20252Du;  // relevo alto
constexpr uint32_t iron4   = 0xFF282E37u;  // relevo mais alto ainda
constexpr uint32_t card     = 0xFF171B22u;  // sidebar e painel (linear-gradient topo)
constexpr uint32_t cardLo   = 0xFF14171Du;  // sidebar (linear-gradient fundo)
constexpr uint32_t headHi   = 0xFF1D222Au;  // panel-head / statusbar (gradiente topo)
constexpr uint32_t groupHi  = 0xFF20252Du;  // card de grupo (gradiente 155deg)
constexpr uint32_t groupLo  = 0xFF191D24u;  // card de grupo (fim)
constexpr uint32_t rowHover = 0xFF212630u;  // .bank-row:hover
constexpr uint32_t rowOn    = 0xFF292E37u;  // .bank-row.active
constexpr uint32_t well     = 0xFF101318u;  // fundo do campo de busca
constexpr uint32_t btnTop   = 0xFF242A33u;  // .ui-button (gradiente 180deg)
constexpr uint32_t btnBot   = 0xFF1C2128u;
constexpr uint32_t btnHover = 0xFF2A2928u;
constexpr uint32_t presetBg = 0xFF262C34u;  // .preset-control button

// --- linhas e bordas ---------------------------------------------------------
constexpr uint32_t line    = 0xFF303743u;   // --line
constexpr uint32_t line2   = 0xFF3B4551u;   // --line2
constexpr uint32_t line3   = 0xFF333B46u;   // borda de card de grupo e de botao
constexpr uint32_t line4   = 0xFF252B34u;   // borda da sidebar
constexpr uint32_t line5   = 0xFF252C36u;   // borda do painel
constexpr uint32_t line6   = 0xFF272E38u;   // regua interna da sidebar
constexpr uint32_t line7   = 0xFF2B323Du;   // barra do panel-head
constexpr uint32_t edge    = 0xFF424954u;   // moldura da janela inteira
constexpr uint32_t rowEdge = 0xFF414956u;   // linha ativa
constexpr uint32_t barEdge = 0xFF343D49u;   // statusbar
constexpr uint32_t div     = 0xFF343C47u;   // divisor dos blocos do rodape

// --- texto (CSS: --text/--muted/--dim) ---------------------------------------
constexpr uint32_t text   = 0xFFECEEF1u;
constexpr uint32_t muted  = 0xFF9AA3AEu;
constexpr uint32_t dim    = 0xFF68727Fu;
constexpr uint32_t rowText = 0xFFD2D6DCu;   // nome do banco
constexpr uint32_t sizeTx  = 0xFF8B95A0u;   // tamanho a direita da linha
constexpr uint32_t btnTx   = 0xFFAEB6C0u;   // rotulo do botao
constexpr uint32_t btnTxHi = 0xFFD8DDE3u;   // botao primario
constexpr uint32_t knobName = 0xFFC3CAD2u;  // nome debaixo do knob
constexpr uint32_t knobTip  = 0xFF64707Du;  // dica miuda
constexpr uint32_t chipTx   = 0xFF84909Cu;  // chave do chip monoespacado
constexpr uint32_t chipVal  = 0xFFB3BDC7u;  // valor do chip
constexpr uint32_t groupTx  = 0xFF8E99A6u;  // titulo do card de grupo
constexpr uint32_t headTx   = 0xFF929BA7u;  // "CONTROLES DE TIMBRE"
constexpr uint32_t brandSub = 0xFFA2A9B1u;  // "DE PIANOS"
constexpr uint32_t noteTx   = 0xFF616B77u;  // ajuda do rodape da sidebar
constexpr uint32_t countTx  = 0xFF626C78u;  // contador de bancos

// --- brasa (CSS: --ember/--ember2/--ember3) ----------------------------------
constexpr uint32_t ember   = 0xFFFF7A18u;
constexpr uint32_t ember2  = 0xFFFFB05Au;
constexpr uint32_t ember3  = 0xFFFFDCAEu;   // ponteiro
constexpr uint32_t accent  = 0xFF824419u;   // fim do filete dos grupos acesos
constexpr uint32_t indexTx = 0xFF70523Cu;   // "01" do titulo do grupo

// --- estados -----------------------------------------------------------------
constexpr uint32_t green   = 0xFF76B58Bu;
constexpr uint32_t okTx    = 0xFFA7D1ADu;
constexpr uint32_t okBg    = 0xFF1E3027u;
constexpr uint32_t okLine  = 0xFF34523Du;
constexpr uint32_t warnTx  = 0xFFFFBD75u;
constexpr uint32_t warnBg  = 0xFF342617u;
constexpr uint32_t warnLine= 0xFF785126u;
constexpr uint32_t red     = 0xFFDB6D60u;
constexpr uint32_t errTx   = 0xFFFF9A8Eu;
constexpr uint32_t errBg   = 0xFF321D1Bu;
constexpr uint32_t errLine = 0xFF71403Bu;

// --- knob (as classes do SVG do prototipo) -----------------------------------
constexpr uint32_t track    = 0xFF3B4552u;  // .track
constexpr uint32_t faceHi   = 0xFF343B45u;  // .face gradiente 0%
constexpr uint32_t faceMid  = 0xFF242A33u;  // 58%
constexpr uint32_t faceLo   = 0xFF171B21u;  // 100%
constexpr uint32_t faceEdge = 0xFF414A56u;  // contorno da face
constexpr uint32_t pin      = 0xFF57616Du;  // .center
constexpr uint32_t idleDot  = 0xFF4E5863u;  // .bank-dot sem selecao
constexpr uint32_t barIdle  = 0xFF3C4653u;  // filete esquerdo dos grupos inertes
constexpr uint32_t bronzePlate = 0xFF6B5B4Au;  // moldura da placa de arte (nosso, nao do CSS)

// --- veil / toast ------------------------------------------------------------
constexpr uint32_t veil     = 0xAA0C0F13u;  // #0c0f13aa
constexpr uint32_t veilCard = 0xEF171B22u;  // #171b22eF
constexpr uint32_t veilEdge = 0xFF3C444Fu;
constexpr uint32_t veilIcon = 0xFF9D6033u;
constexpr uint32_t veilBtn  = 0xFF322519u;
constexpr uint32_t veilBtnLn= 0xFF78471Fu;
constexpr uint32_t veilBtnTx= 0xFFF6C18Cu;
constexpr uint32_t toastBg  = 0xFF242D29u;
constexpr uint32_t toastLn  = 0xFF49614Eu;
constexpr uint32_t toastTx  = 0xFFD8EADB;


inline uint32_t withA(uint32_t argb, float a) { return withAlpha(argb, a); }

} // namespace forge

} // namespace pforge
