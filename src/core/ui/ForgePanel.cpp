#include "ForgePanel.hpp"
#include "ForgeTheme.hpp"
#include "../Parameters.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace pianoforge;   // ParameterId: os knobs sao 1:1 com as portas do TTL

namespace pforge {

namespace {

// ---------------------------------------------------------------- medidas ---
// Tudo abaixo e o numero do CSS do prototipo, em pixels de janela. Se o host
// redimensionar a janela, o que flexiona e o que flexionaria num navegador: as
// colunas fracionarias e a lista; as alturas fixas (head 38, group-head 31,
// botao 28, linha 31, statusbar 76) continuam as mesmas.
constexpr int kPadWin = 7;      // .workarea padding
constexpr int kGapWin = 7;      // .workarea gap
constexpr int kSideW = 238;     // .workarea grid-template-columns: 238px
constexpr int kStatusH = 76;    // .window grid-template-rows: 76px
constexpr int kHeadH = 38;      // .panel-head
constexpr int kGroupHeadH = 31; // .group-head
constexpr int kCtlPad = 11;     // .control-area padding
constexpr int kCtlGap = 10;     // .control-area gap
constexpr int kBtnH = 28;       // .ui-button / input
constexpr int kRowH = 31;       // .bank-row min-height
constexpr int kRowGap = 3;      // .bank-list gap
constexpr int kKnobBox = 78;    // .knob-wrap
constexpr int kSidePadX = 12, kSidePadTop = 13, kSidePadBot = 11;

constexpr float colFr(int g) {
    // .control-area: .78 .88 1.64 .92 fr (coluna 1 carrega dois cards empilhados)
    switch (g) { case 0: return 0.78f; case 1: return 0.88f; case 2: return 1.64f;
                 default: return 0.92f; }
}

float norm(const KnobState& k) {
    const float span = k.maxValue - k.minValue;
    return span > 0.0f ? (k.value - k.minValue) / span : 0.0f;
}

// Rótulo cortado com reticencias pra caber na coluna.
std::string fit(const Font& f, const std::string& text, int maxW) {
    if (f.width(text) <= maxW) return text;
    // Passo por CODEPOINT, nao por byte: "Medios" com acento cortado ao meio sai
    // como dois glifos de lixo, e isso no lugar das reticencias e o pior tipo de
    // truncamento -- parece fonte quebrada, nao texto comprido.
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 :
                   (c & 0xF8) == 0xF0 ? 4 : 1;
        if (i + n > text.size()) break;
        const std::string ch = text.substr(i, n);
        if (f.width(out + ch + "...") > maxW) break;
        out += ch;
        i += n;
    }
    return out + "...";
}

// Texto em caixa alta com tracking, como o CSS faz com letter-spacing em `em`.
void drawTracked(Surface& s, const Font& f, int x, int baseline, const std::string& text,
                 float emSpacing, uint32_t color) {
    f.drawSpaced(s, x, baseline, text, emSpacing, color);
}

void hgradRect(Surface& s, const Rect& r, uint32_t top, uint32_t bottom) {
    s.vgrad(r.x, r.y, r.w, r.h, top, bottom);
}

} // namespace

// Ordem de EXIBICAO do prototipo: coluna 1 tem MASTER em cima e RESSONANCIA
// embaixo; coluna 2 DINAMICA; coluna 3 CORPO (grade 2x2); coluna 4 ESPACO.
//
// Isso muda o que o checkpoint anterior havia fixado (a grade 5x2 que VOCE
// ditou, na qual Volume e Dinamica dividiam a coluna 1). O conceito v2 agrupa
// por categoria e da a Ressonancia o lugar de honra abaixo do Volume. A
// mudanca inteira mora nesta tabela: para voltar atras, e so reordenar ids.
const GroupDef kGroups[5] = {
    {"MASTER",       {Param_Volume},                                        1, true},
    {"RESSONÂNCIA", {Param_SympRes},                                       1, true},
    {"DINÂMICA",     {Param_Hammer, Param_VelCurve},                        2, false},
    {"CORPO",        {Param_Lid, Param_BassEQ, Param_MidEQ, Param_HighEQ},  4, false},
    {"ESPAÇO",       {Param_Chorus, Param_Reverb},                          2, false},
};

std::string formatSize(uint64_t bytes) {
    char buf[48];
    const double gb = double(bytes) / (1024.0 * 1024.0 * 1024.0);
    const double mb = double(bytes) / (1024.0 * 1024.0);
    if (gb >= 1.0) std::snprintf(buf, sizeof(buf), "%.1f GB", gb);
    else if (mb >= 1.0) std::snprintf(buf, sizeof(buf), "%.0f MB", mb);
    else std::snprintf(buf, sizeof(buf), "%.0f kB", double(bytes) / 1024.0);
    return buf;
}

std::string formatValue(const KnobState& k) {
    char buf[32];
    if (k.bipolar) {
        std::snprintf(buf, sizeof(buf), "%+.1f", k.value);
    } else if (k.maxValue - k.minValue <= 1.0001f) {
        std::snprintf(buf, sizeof(buf), "%.2f", k.value);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f", k.value);
    }
    return buf;
}

std::string identityLine(const PanelState& s, const std::string& fileName,
                         const std::vector<std::string>& manifestPresets) {
    // "O que soa" e o nome do preset do manifest SO quando a conta dele bate com o
    // que a porta 23 informou: um bundle com 3 presets num host que diz 62 nao
    // pode exibir o nome do preset 7.
    std::string what = fileName;
    if (s.presetTotal > 1 && int(manifestPresets.size()) == s.presetTotal &&
        s.preset >= 0 && s.preset < int(manifestPresets.size()) &&
        !manifestPresets[size_t(s.preset)].empty())
        what = manifestPresets[size_t(s.preset)];
    std::string line = what;
    const int pub = s.haveStatus ? s.published : s.current;
    if (pub >= 0 && pub < int(s.banks.size()) && s.banks[size_t(pub)].sizeBytes)
        line += " \u00b7 " + formatSize(s.banks[size_t(pub)].sizeBytes);
    return line;
}

std::string stateSubLine(const PanelState& s) {
    // A funcao desta linha e confessar a ORIGEM do que o badge diz, nao repetir o
    // indice publicado (a identidade ja mostra o banco, e o badge mostra o
    // estado). Comeca pela contagem de presets, como no prototipo.
    std::string out = std::to_string(s.presetTotal) + " preset" +
                      (s.presetTotal == 1 ? "" : "s");
    if (s.haveStatus) out += " \u00b7 estado pela porta 22";
    else if (s.loading) out += " \u00b7 estimativa por tamanho";
    else out += " \u00b7 sem audio conectado";
    return out;
}

// ------------------------------------------------------------------ fontes ---

bool Fonts::load(const std::string& sansPath, const std::string& boldPath,
                 const std::string& serifPath, const std::string& monoPath) {
    bool any = false;
    if (!sansPath.empty()) {
        // Os tamanhos sao os do prototipo: 11 na linha do banco, 9 no tamanho,
        // 8 nos rotulos em caixa alta, 7 na dica, 16 serifado no nome.
        any = small.load(sansPath, 9.0f) || any;
        name.load(sansPath, 11.0f);
        sub.share(small, 10.0f);
    }
    if (!boldPath.empty()) {
        brand.load(boldPath, 14.0f);
        // "DE PIANOS" e os titulos de grupo sao negrito miudo no CSS (font-weight
        // 750/800 em 8-10 px): mesmo blob do negrito, outros tamanhos.
        if (brand.ok()) {
            sub.share(brand, 10.0f);
            any = true;
        }
    }
    const Font& base = brand.ok() ? brand : small;
    label.share(base, 8.0f);
    tiny.share(base, 7.0f);
    value.share(base, 8.0f);

    // Sem serif/mono no bundle nao ha tela preta nem tofu: os dois corpos "de
    // luxo" do prototipo viram o sans que ja esta carregado, e o rodape segue
    // legivel. E uma degradação deliberada, nao um fallback de emergencia.
    if (!serifPath.empty()) serifBig.load(serifPath, 17.0f);
    if (!serifBig.ok() && base.ok()) serifBig.share(base, 17.0f);
    serif.share(serifBig, 16.0f);

    if (!monoPath.empty()) mono.load(monoPath, 8.0f);
    if (!mono.ok()) mono.share(base, 8.0f);
    return any;
}

// ----------------------------------------------------------------- geometria ---

Layout layoutFor(int w, int h, int artH) {
    Layout l;
    l.width = w;
    l.height = h;

    const int workBottom = h - kStatusH;
    const int workH = std::max(60, workBottom - kPadWin - 5);
    l.sidebar = Rect{kPadWin, kPadWin, kSideW, workH};
    l.panel = Rect{kPadWin + kSideW + kGapWin, kPadWin,
                   std::max(120, w - (kPadWin + kSideW + kGapWin) - kPadWin), workH};
    l.statusbar = Rect{kPadWin, workBottom, w - 2 * kPadWin, std::max(40, h - workBottom - kPadWin)};

    l.head = Rect{l.panel.x + 1, l.panel.y + 1, l.panel.w - 2, kHeadH};
    l.control = Rect{l.panel.x + 1, l.head.y + kHeadH + 1, l.panel.w - 2,
                     std::max(40, l.panel.y + l.panel.h - 2 - (l.head.y + kHeadH + 1))};

    // Coluna esquerda: marca, regua da biblioteca, busca, lista, botoes, ajuda.
    int y = l.sidebar.y + kSidePadTop;
    y += 35 + 10;                                   // .brand + margin-bottom
    const int labelH = 6 + 10 + 7;                  // padding 6/7 em cima de 8 px
    y += labelH;
    l.search = Rect{l.sidebar.x + kSidePadX, y, l.sidebar.w - 2 * kSidePadX, kBtnH};
    y += kBtnH + 2 + 7;                             // .searchbox margin 2/7
    const int actionsH = kBtnH + 5 + kBtnH;         // primary + gap + linha de dois
    const int noteH = 8 + 22;                       // margin-top 8 + duas linhas
    const int artPlate = artH > 0 ? std::min(artH, 156) : 0;   // placa do manifest
    const int listBottom = l.sidebar.y + l.sidebar.h - kSidePadBot - noteH - 9 - actionsH - 1
                         - (artPlate ? artPlate + 8 : 0);
    l.list = Rect{l.sidebar.x + kSidePadX, y, l.sidebar.w - 2 * kSidePadX,
                  std::max(kRowH, listBottom - y)};
    const int by = l.list.y + l.list.h + 9 + 1 + artPlate + (artPlate ? 8 : 0);
    if (artPlate)
        l.artBox = Rect{l.sidebar.x + kSidePadX, l.list.y + l.list.h + 8,
                        l.sidebar.w - 2 * kSidePadX, artPlate};
    l.btnBrowse = Rect{l.sidebar.x + kSidePadX, by, l.sidebar.w - 2 * kSidePadX, kBtnH};
    l.btnFolder = Rect{l.sidebar.x + kSidePadX, by + kBtnH + 5,
                       (l.sidebar.w - 2 * kSidePadX - 5) / 2, kBtnH};
    l.btnRemove = Rect{l.btnFolder.x + l.btnFolder.w + 5, by + kBtnH + 5,
                       l.sidebar.w - 2 * kSidePadX - l.btnFolder.w - 5, kBtnH};

    // Colunas de cards. A coluna 1 empilha MASTER sobre RESSONANCIA; o CORPO e a
    // unica com grade 2x2 (quatro knobs), e por isso leva o maior fr.
    const int innerX = l.control.x + kCtlPad;
    const int innerY = l.control.y + kCtlPad;
    const int innerW = std::max(60, l.control.w - 2 * kCtlPad);
    const int innerH = std::max(60, l.control.h - 2 * kCtlPad);
    const float sum = colFr(0) + colFr(1) + colFr(2) + colFr(3);
    const int usable = innerW - kCtlGap * 3;
    int colX[4], colW[4];
    {
        int acc = 0;
        for (int c = 0; c < 4; ++c) {
            const int cw = (c == 3) ? usable - acc : int(float(usable) * colFr(c) / sum);
            colX[c] = innerX + c * kCtlGap + (c == 0 ? 0 : 0);
            colW[c] = cw;
            acc += cw;
        }
        // os vao entre colunas somam 3 gaps; redistribui o troco para a ultima
        colX[1] = colX[0] + colW[0] + kCtlGap;
        colX[2] = colX[1] + colW[1] + kCtlGap;
        colX[3] = colX[2] + colW[2] + kCtlGap;
        colW[3] = std::max(60, innerX + usable + kCtlGap * 3 - colX[3]);
    }
    const int stackH = (innerH - kCtlGap) / 2;
    l.groupCard[0] = Rect{colX[0], innerY, colW[0], stackH};
    l.groupCard[1] = Rect{colX[0], innerY + stackH + kCtlGap, colW[0], innerH - stackH - kCtlGap};
    l.groupCard[2] = Rect{colX[1], innerY, colW[1], innerH};
    l.groupCard[3] = Rect{colX[2], innerY, colW[2], innerH};
    l.groupCard[4] = Rect{colX[3], innerY, colW[3], innerH};
    for (int g = 0; g < 5; ++g) {
        const Rect& c = l.groupCard[g];
        l.groupHead[g] = Rect{c.x + 1, c.y + 1, c.w - 2, kGroupHeadH};
    }

    // Knobs: .knob-stack (coluna, space-evenly) para 1-2 por card, .body-grid
    // (2x2) para o CORPO. O box do knob e 78 px no prototipo; se a coluna for
    // mais estreita que isso ele encolhe junto, porque host LV2 redimensiona.
    for (int g = 0; g < 5; ++g) {
        const GroupDef& def = kGroups[g];
        const Rect& card = l.groupCard[g];
        const int areaY = card.y + 1 + kGroupHeadH + 1;
        const int areaH = std::max(20, card.y + card.h - 2 - areaY);
        // .knob-stack empilha um embaixo do outro (MASTER/RESSONANCIA/DINAMICA/
        // ESPACO); .body-grid e 2x2 so para o CORPO. Antes o "1 coluna" valia para
        // 1 E para 2 knobs, e o segundo saia pela porta do card: DINAMICA e ESPACO
        // mostravam um disco e meio.
        const int cols = def.count > 2 ? 2 : 1;
        const int rows = (def.count + cols - 1) / cols;
        const bool grid = cols > 1;
        // .knob-stack/.body-grid: borda 1 px + padding lateral 3 px, e os vao de
        // 1-2 px entre celulas. O que flexiona e a coluna, nunca o padding.
        const int areaWd = card.w - 2 - 2 * 3;
        const int cellW = (areaWd - 2 * (cols - 1)) / cols;
        for (int i = 0; i < def.count; ++i) {
            const int id = def.ids[i];
            const int r = grid ? i / cols : i;
            const int c = grid ? i % cols : 0;
            const int cellH = (areaH - 2 * (rows - 1)) / rows;
            const int cellX = card.x + 1 + 3 + c * (cellW + 2);
            const int cellY = areaY + r * (cellH + 2);
            const int box = std::max(30, std::min({kKnobBox, cellW - 2, cellH - 26}));
            l.knobBox[id] = Rect{cellX + (cellW - box) / 2, cellY + (cellH - 26 - box) / 2,
                                 box, box};
            l.knobCx[id] = l.knobBox[id].cx();
            l.knobCy[id] = l.knobBox[id].cy();
            l.knobU[id] = float(box) / 80.0f;   // o SVG do prototipo e um 80x80
        }
    }

    // Rodape: identidade | estado (com o preset) | chips informativos. As
    // medidas sao as do CSS (.statusbar grid 1.05fr 1fr 1.7fr, gap 12, pad 12) e
    // sao RECALKULADAS em drawStatusBlock -- aqui só interessa a geometria que o
    // hit-test precisa: a pilula de preset. Por isso o bloco nao tem fonte: o
    // rotulo "Preset 8 / 62" e centralizado ENTRE os dois botoes, entao a largura
    // do texto nao entra na conta.
    {
        const int pad = 12, gap = 12;
        const int innerW = std::max(60, l.statusbar.w - 2 * pad);
        const float a = 1.05f, b = 1.0f, c = 1.7f;
        const int usableS = std::max(60, innerW - 2 * gap);
        const int wA = int(float(usableS) * a / (a + b + c));
        const int xA = l.statusbar.x + pad;
        const int xB = xA + wA + gap;
        // Linha 2 do bloco de estado, alinhada a esquerda com a badge. A pílula
        // NAO divide a linha 1 com o texto do estado: com as frases portuguesas
        // (badge + "piano publicado" + pílula) os 253 px do bloco nao chegam e o
        // estado virava "pi...".
        const int sx = xB + 11;
        const int pw = 19, ph = 18, labelW = 76;
        const int boxW = labelW + 2 * pw + 8;
        const int maxRight = xB + int(float(usableS) * b / (a + b + c)) - 2;
        l.presetBox = Rect{sx, l.statusbar.y + 30,
                           std::max(0, std::min(boxW, maxRight - sx)), ph + 6};
        l.presetPrev = Rect{l.presetBox.x, l.presetBox.y + 3, pw, ph};
        l.presetNext = Rect{l.presetBox.x + l.presetBox.w - pw, l.presetBox.y + 3, pw, ph};
    }

    // O card do veil e geometria pura (max-width 280, altura do conteudo): o
    // hit-test do botao precisa dele ANTES de qualquer desenho, e a UI consulta o
    // Layout que layoutFor devolveu -- nao uma copia que drawPanel mexeu.
    //
    // A altura e a do conteudo do prototipo, nao um chute: padding 18 + icone 24
    // + titulo + TRES linhas de copy (10px/1.5) + botao. Com 132 a terceira linha
    // ("automaticamente.") era rejeitada pelo guarda do botao e a frase perdia a
    // palavra que da o sentido ao estado "nada e carregado sozinho".
    {
        const Rect& area = l.control;
        const int cw = std::min(280, area.w - 24), ch = 156;
        l.veilCard = Rect{area.cx() - cw / 2, area.cy() - ch / 2, cw, ch};
        const int bw = std::min(150, cw - 36);
        l.veilButton = Rect{l.veilCard.cx() - bw / 2, l.veilCard.y + l.veilCard.h - 30, bw, 22};
    }
    return l;
}

std::vector<int> visibleBanks(const PanelState& s) {
    std::vector<int> out;
    const std::string needle = s.filter;
    // ASCII minusculo manual: tolower e macro de funcao neste TU (a cadeia X11
    // inclui ctype de outro jeito) e std::tolower nao compila aqui.
    auto lower = [](std::string x) {
        for (char& ch : x)
            if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
        return x;
    };
    const std::string f = lower(needle);
    for (size_t i = 0; i < s.banks.size(); ++i) {
        if (f.empty()) { out.push_back(int(i)); continue; }
        const std::string n = lower(s.banks[i].name);
        const std::string p = lower(s.banks[i].path);
        if (n.find(f) != std::string::npos || p.find(f) != std::string::npos)
            out.push_back(int(i));
    }
    return out;
}

int listRowCapacity(const Layout& l) {
    return std::max(1, (l.list.h + kRowGap) / (kRowH + kRowGap));
}

int artPlateHeight(int imgW, int imgH) {
    if (imgW <= 0 || imgH <= 0) return 0;
    // Contida na LARGURA: a placa e a largura util da sidebar e a altura vem da
    // proporcao. O teto e 156 px para a placa nao comer a lista inteira, e o piso
    // de 44 px e o ponto em que ela para de valer o espaco que toma.
    const int usableW = kSideW - 2 * kSidePadX;
    const int dh = std::min(156, std::max(1, int(float(usableW) * float(imgH) / float(imgW))));
    return dh < 44 ? 0 : dh;
}

bool presetSelectorVisible(const PanelState& s) {
    if (s.presetTotal <= 1) return false;
    if (s.plugLoading) return false;          // nada a escolher enquanto forja
    const int pub = s.haveStatus ? s.published : s.current;
    return pub >= 0;
}

bool veilVisible(const PanelState& s) {
    // O veil e o estado "a forja esta fria": sem banco PUBLICADO. Enquanto o
    // plugin responde, e ele que diz; sem resposta vale o clique.
    return s.haveStatus ? (s.published < 0) : (s.current < 0);
}

Hit hitTest(const Layout& l, const PanelState& s, int x, int y) {
    if (l.btnBrowse.contains(x, y)) return {HitKind::Browse, -1};
    if (l.btnFolder.contains(x, y)) return {HitKind::AddFolder, -1};
    if (l.btnRemove.contains(x, y)) return {HitKind::Remove, -1};
    if (l.search.contains(x, y)) return {HitKind::Search, -1};
    // A lista VEM ANTES do veil: no prototipo o .veil e `position:absolute;
    // inset:38px 0 0` dentro do .panel, ou seja, cobre os knobs e nunca a sidebar.
    // Com o guarda do veil primeiro, clicar numa linha com a forja fria nao fazia
    // nada -- sobrava so o botao "Escolher primeiro banco" para começar, e a lista
    // inteira, que e justamente o controle de escolha, ficava morta.
    if (l.list.contains(x, y)) {
        const std::vector<int> vis = visibleBanks(s);
        const int row = (y - l.list.y) / (kRowH + kRowGap);
        if (row >= 0 && row < int(vis.size()) && row + s.scroll < int(vis.size()))
            return {HitKind::BankRow, vis[size_t(row + s.scroll)]};
        return {};
    }
    if (veilVisible(s)) {
        if (l.veilButton.contains(x, y)) return {HitKind::VeilButton, -1};
        return {};
    }
    if (presetSelectorVisible(s) && l.presetBox.contains(x, y)) {
        if (l.presetPrev.contains(x, y)) return {HitKind::PresetDec, -1};
        if (l.presetNext.contains(x, y)) return {HitKind::PresetInc, -1};
        return {};
    }
    for (int k = 0; k < kKnobCount; ++k) {
        if (l.knobBox[k].w <= 0) continue;
        if (l.knobBox[k].contains(x, y)) return {HitKind::Knob, k};
    }
    return {};
}

// ------------------------------------------------------------------ desenho ---

namespace {

// O filete de brasa da esquerda: 2 px, gradiente vertical ember -> #824419 nos
// grupos "acesos" do prototipo (MASTER e RESSONANCIA), ferro nos outros.
void drawGroupCard(Surface& s, const Font& title, const Font& idx, const Rect& card,
                   const Rect& head, const GroupDef& def, int number) {
    s.rrect(card.x, card.y, card.w, card.h, 5, forge::groupLo);
    hgradRect(s, Rect{card.x + 1, card.y + 1, card.w - 2, card.h - 2}, forge::groupHi,
              forge::groupLo);
    s.rrectStroke(card.x, card.y, card.w, card.h, 5, 1, forge::line3);
    // box-shadow: 0 5px 14px #0002 -- uma sombra embaixo do card, nao um relevo.
    s.fillRect(card.x + 3, card.y + card.h, card.w - 6, 2, withAlpha(0xFF000000u, 0.10f));
    const int barH = card.h - 2;
    if (def.hot) {
        for (int j = 0; j < barH; ++j) {
            const float t = float(j) / float(std::max(1, barH - 1));
            s.blendPix(card.x + 1, card.y + 1 + j, mix(forge::ember, forge::accent, t), 1.0f);
        }
    } else {
        s.fillRect(card.x + 1, card.y + 1, 2, barH, forge::barIdle);
    }
    s.hline(head.x + 2, head.y + head.h, head.w - 4, forge::line);
    const std::string num = (number < 10 ? "0" : "") + std::to_string(number);
    const int tw = title.widthSpaced(def.title, 1.3f);
    const int nw = idx.width(num);
    const int total = tw + nw + 7;
    int tx = head.cx() - total / 2;
    idx.draw(s, tx, head.cy() + 3, num, forge::indexTx);
    tx += nw + 7;
    drawTracked(s, title, tx, head.cy() + 3, def.title, 1.3f, forge::groupTx);
}

void drawKnob(Surface& s, const Fonts& f, const Layout& l,
              const PanelState& st, int k, bool live, float time) {
    const KnobState& knob = st.knobs[k];
    const float u = l.knobU[k];
    const int cx = l.knobCx[k], cy = l.knobCy[k];
    const float t = norm(knob);
    const bool active = live && (st.hoverKnob == k || st.dragKnob == k);
    const bool modified = std::fabs(knob.value - knob.defValue) > 1e-4f;

    // Angulos: o prototipo mede d a partir do 12h e gira para a direita; este
    // rasterizador mede a partir do 3h com Y para baixo. d=-135 -> 0,75pi e
    // d=+135 -> 2,25pi, o mesmo trilho de 270 graus de sempre.
    const float a0 = float(M_PI) * 0.75f;
    const float span = float(M_PI) * 1.5f;
    const float stop = a0 + span * t;
    const float rTrack = 34.0f * u;

    // .track: stroke 3.7, linecap round.
    s.arc(cx, cy, rTrack - 1.85f * u, rTrack + 1.85f * u, a0, a0 + span,
          live ? forge::track : forge::line, true);

    if (live && t > 0.004f) {
        // .active-arc: stroke 4 + drop-shadow(0 0 3px #ff7a1844). A sombra e o
        // mesmo arco, mais largo e a 27% -- um glow radial centralizado nao serve,
        // a brasa tem de seguir o caminho do arco.
        s.arc(cx, cy, rTrack - 4.6f * u, rTrack + 4.6f * u, a0, stop,
              withAlpha(forge::ember, 0.27f), true);
        s.arc(cx, cy, rTrack - 2.0f * u, rTrack + 2.0f * u, a0, stop, forge::ember, true);
    }

    // .face: gradiente diagonal 0% / 58% / 100%, contorno 1 px.
    s.faceDisc(cx, cy, 26.0f * u, forge::faceHi, forge::faceMid, forge::faceLo);
    s.arc(cx, cy, 26.0f * u - 0.5f, 26.0f * u + 0.5f, 0.0f, 2.0f * float(M_PI),
          live ? forge::faceEdge : forge::line2);
    // .face-shine: um anel de branco a 7% um pouco dentro da borda.
    s.arc(cx, cy, 23.5f * u - 0.6f, 23.5f * u + 0.6f, float(M_PI) * 1.05f,
          float(M_PI) * 1.95f, withAlpha(0xFFFFFFFFu, 0.07f));

    // .needle: de r=12 a r=20 (dentro da face), 1.6 px, #ffd7aa com brilho.
    if (live) {
        const float px = std::cos(stop), py = std::sin(stop);
        const float x1 = cx + px * 12.0f * u, y1 = cy + py * 12.0f * u;
        const float x2 = cx + px * 20.0f * u, y2 = cy + py * 20.0f * u;
        // drop-shadow(0 0 3px #ff7a1880) sao 3 px DE BLUR gaussiano na tela do
        // navegador, onde o knob inteiro tem ~64 px; aqui o knob tem ~110 px e o
        // nosso "blur" e um segmento de borda dura. Uma camada de 5,4u virava uma
        // barra de 8,6 px que encostava nos digitos do valor. Duas camadas finas
        // perto do nucleo imitam a queda do gaussiano sem invadir o texto.
        s.segment(x1, y1, x2, y2, 3.2f * u, withAlpha(forge::ember, 0.12f));
        s.segment(x1, y1, x2, y2, 2.2f * u, withAlpha(forge::ember, 0.20f));
        s.segment(x1, y1, x2, y2, 1.5f * u, forge::ember3);
    } else {
        const float px = std::cos(stop), py = std::sin(stop);
        s.segment(cx + px * 12.0f * u, cy + py * 12.0f * u,
                  cx + px * 20.0f * u, cy + py * 20.0f * u, 1.6f * u, forge::pin);
    }
    s.disc(cx, cy, 2.2f * u, forge::pin);

    // O valor mora DENTRO da face, 16 px abaixo do centro no viewBox de 80.
    const std::string val = formatValue(knob);
    f.value.drawCentered(s, cx, cy + int(16.0f * u), val,
                         !live ? forge::muted : (modified ? forge::ember2 : forge::text));

    // Nome e dica embaixo do disco (.knob-name 9 px, .knob-tip 7 px).
    const int nameY = l.knobBox[k].y + l.knobBox[k].h + 9;
    f.small.drawCentered(s, cx, nameY, fit(f.small, knob.label, l.knobBox[k].w + 16),
                         active ? forge::text : forge::knobName);
    if (!knob.tip.empty())
        f.tiny.drawCentered(s, cx, nameY + 10, fit(f.tiny, knob.tip, l.knobBox[k].w + 20),
                            forge::knobTip);
    if (live && modified)
        s.disc(cx, nameY + 18, 1.5f, forge::ember);

    // Fagulhas: so no knob tocado/sob o mouse, e por ~1,5 s depois de soltar.
    const bool sparks = live && (active || (st.sparkAge >= 0.0f && st.sparkAge < 1.5f &&
                                            st.dragKnob == k));
    if (sparks) {
        const float life = active ? 0.35f : std::max(0.0f, 1.0f - st.sparkAge / 1.5f);
        for (int i = 0; i < 14; ++i) {
            const float seed = float(i) * 12.9898f + float(k) * 4.1f;
            const float r1 = std::fabs(std::sin(seed));
            const float r2 = std::fabs(std::cos(seed * 1.7f + time));
            const float a = float(r1 * 6.28318f);
            const float dist = rTrack + 4.0f + r2 * rTrack * 0.9f * (0.4f + life);
            const int sxp = cx + int(std::cos(a) * dist);
            const int syp = cy + int(std::sin(a) * dist) - int(r2 * 8.0f * (1.0f - life));
            s.fillRect(sxp, syp, 2, 2, withAlpha(mix(forge::ember, forge::ember3, r2),
                                                 life * (0.5f + 0.5f * r1)));
        }
    }
}

void drawButton(Surface& s, const Font& f, const Rect& r, const std::string& label,
                bool enabled, bool hovered, bool danger = false) {
    if (hovered && enabled) {
        s.rrect(r.x, r.y, r.w, r.h, 4, danger ? forge::errBg : forge::btnHover);
        s.rrectStroke(r.x, r.y, r.w, r.h, 4, 1, danger ? forge::errLine : 0xFF885021u);
    } else {
        s.rrect(r.x, r.y, r.w, r.h, 4, forge::btnTop);
        hgradRect(s, Rect{r.x + 1, r.y + 1, r.w - 2, r.h - 2}, forge::btnTop, forge::btnBot);
        s.rrectStroke(r.x, r.y, r.w, r.h, 4, 1, forge::line3);
    }
    const uint32_t col = !enabled ? forge::dim
                         : (hovered ? (danger ? 0xFFFFAAA0u : 0xFFF2D0A8u)
                                    : (label.size() > 12 ? forge::btnTxHi : forge::btnTx));
    f.drawCentered(s, r.cx(), r.cy() + 4, label, col);
}

// Chevron de capsulas (nada de glifo de fonte: se faltasse no meio de um banco
// de 592 presets o rodape viraria tofu).
void drawArrow(Surface& s, int cx, int cy, int r, int dir, uint32_t col) {
    const float tip = float(cx + dir * r), back = float(cx - dir * r);
    s.segment(tip, float(cy), back, float(cy - r), 1.6f, col);
    s.segment(tip, float(cy), back, float(cy + r), 1.6f, col);
}

void drawMark(Surface& s, const Rect& box) {
    // A marca do prototipo e um glifo de brasa dentro de um quadrinho de 25 px
    // com borda bronze. Um glifo de fonte poderia faltar; uma bigorna desenhada
    // nao. Mesmo fundo em gradiente e mesma borda do CSS.
    s.rrect(box.x, box.y, box.w, box.h, 5, mix(0xFF332519u, 0xFF1C1A18u, 0.5f));
    s.rrectStroke(box.x, box.y, box.w, box.h, 5, 1, 0xFF70411Eu);
    const int cx = box.cx(), cy = box.cy();
    s.fillRect(cx - 6, cy - 4, 12, 3, forge::ember);      // cara
    s.fillRect(cx + 4, cy - 3, 3, 2, forge::ember);       // chifre
    s.fillRect(cx - 2, cy - 1, 4, 4, forge::ember);       // cintura
    s.fillRect(cx - 5, cy + 3, 10, 2, forge::ember);      // base
}

void drawSearchIcon(Surface& s, int x, int y, uint32_t col) {
    s.arc(x + 4, y + 4, 3.0f, 4.1f, 0.0f, 2.0f * float(M_PI), col);
    s.segment(float(x + 7), float(y + 7), float(x + 10), float(y + 10), 1.4f, col);
}

void drawSidebar(Surface& s, const Fonts& f, const Layout& l, const PanelState& st) {
    const Rect& sb = l.sidebar;
    s.rrect(sb.x, sb.y, sb.w, sb.h, 3, forge::card);
    hgradRect(s, Rect{sb.x + 1, sb.y + 1, sb.w - 2, sb.h - 2}, forge::card, forge::cardLo);
    s.rrectStroke(sb.x, sb.y, sb.w, sb.h, 3, 1, forge::line4);

    const int x = sb.x + kSidePadX;
    const int innerW = sb.w - 2 * kSidePadX;
    drawMark(s, Rect{x, sb.y + kSidePadTop, 25, 25});
    drawTracked(s, f.brand, x + 34, sb.y + kSidePadTop + 13, "FORJA", 14 * 0.16f,
                forge::ember);
    drawTracked(s, f.sub, x + 34, sb.y + kSidePadTop + 27, "DE PIANOS", 10 * 0.17f,
                forge::brandSub);

    const int labelY = sb.y + kSidePadTop + 35 + 10 + 6;
    s.hline(x, labelY - 7, innerW, forge::line6);
    const std::string count = st.banks.size() < 10
        ? "0" + std::to_string(int(st.banks.size())) : std::to_string(int(st.banks.size()));
    drawTracked(s, f.label, x + 3, labelY + 3, "BIBLIOTECA DE BANCOS", 8 * 0.16f, 0xFF77818Du);
    f.label.drawRight(s, x + innerW - 3, labelY + 3, count, forge::countTx);

    // Campo de busca: fundo po, borda, e o anel de brasa quando esta focado.
    const Rect& se = l.search;
    s.rrect(se.x, se.y, se.w, se.h, 4, forge::well);
    s.rrectStroke(se.x, se.y, se.w, se.h, 4, 1,
                  st.searchFocus ? 0xFF985021u : (st.hoverBank == -6 ? 0xFF3A424Eu
                                                                      : 0xFF303742u));
    if (st.searchFocus)
        s.rrectStroke(se.x - 1, se.y - 1, se.w + 2, se.h + 2, 5, 1, withAlpha(forge::ember, 0.09f));
    drawSearchIcon(s, se.x + 8, se.y + se.h / 2 - 5, 0xFF75808Bu);
    if (st.filter.empty()) {
        f.name.draw(s, se.x + 28, se.cy() + 4, "Filtrar bancos...",
                    st.searchFocus ? forge::dim : 0xFF6B7580u);
    } else {
        f.name.draw(s, se.x + 28, se.cy() + 4, fit(f.name, st.filter, se.w - 34),
                    forge::text);
        if (st.searchFocus)
            s.fillRect(se.x + 28 + f.name.width(fit(f.name, st.filter, se.w - 34)) + 1,
                       se.y + 6, 1, se.h - 12, forge::ember);
    }

    // Linhas de banco. "active" = publicado (soando); "selected" = clicado. Sao
    // estados diferentes e a tela tem de mostrar os dois.
    const std::vector<int> vis = visibleBanks(st);
    const int rows = listRowCapacity(l);
    if (vis.empty()) {
        f.small.draw(s, l.list.x + 7, l.list.y + 14,
                     st.filter.empty() ? "nenhum banco registrado" :
                     "Nenhum banco corresponde a busca.", 0xFF76808Au);
        if (st.filter.empty())
            f.small.draw(s, l.list.x + 7, l.list.y + 28, "use Procurar soundfont...",
                         forge::dim);
    }
    for (int row = 0; row < rows; ++row) {
        const int slot = row + st.scroll;
        if (slot >= int(vis.size())) break;
        const int i = vis[size_t(slot)];
        const BankRow& b = st.banks[i];
        const int y = l.list.y + row * (kRowH + kRowGap);
        const bool selected = (i == st.current);
        const bool sounding = st.haveStatus && i == st.published;
        const bool hov = (i == st.hoverBank);
        Rect r{l.list.x, y, l.list.w, kRowH};
        if (sounding) {
            s.rrect(r.x, r.y, r.w, r.h, 3, forge::rowOn);
            s.rrectStroke(r.x, r.y, r.w, r.h, 3, 1, forge::rowEdge);
            s.fillRect(r.x + 3, r.cy() - 9, 2, 18, forge::ember);
        } else if (hov) {
            s.rrect(r.x, r.y, r.w, r.h, 3, forge::rowHover);
        }
        const int dotX = r.x + 8 + 2, dotY = r.cy();
        if (selected) {
            s.glow(dotX, dotY, 6, forge::ember, 0.45f);
            s.disc(dotX, dotY, 2.5f, forge::ember);
        } else {
            s.disc(dotX, dotY, 2.5f, forge::idleDot);
        }
        const std::string shown = fit(f.name, b.name, r.w - 78);
        f.name.draw(s, r.x + 25, r.cy() + 4, shown,
                    !b.exists ? forge::red : (sounding ? forge::text : forge::rowText));
        if (!b.exists) {
            const int tw = f.name.width(shown);
            for (int px = 0; px < tw; ++px)
                s.blendPix(r.x + 25 + px, r.cy() - 1, forge::red, 0.75f);
        }
        f.small.drawRight(s, r.x + r.w - 8, r.cy() + 4, formatSize(b.sizeBytes),
                          b.exists ? forge::sizeTx : forge::red);
    }

    // A placa da arte do manifest, no vazio que a lista nao usou. O prototipo nao
    // tem isso (navegador sem bundle), entao o desenho e nosso: moldura de bronze
    // sobre ferro, a imagem contida e centrada.
    if (!st.art.empty() && l.artBox.h >= 44) {
        const Rect& ab = l.artBox;
        s.rrect(ab.x - 3, ab.y - 3, ab.w + 6, ab.h + 6, 4, forge::bronzePlate);
        s.rrect(ab.x - 1, ab.y - 1, ab.w + 2, ab.h + 2, 3, forge::iron0);
        const float sc = std::min(float(ab.w) / float(st.art.w), float(ab.h) / float(st.art.h));
        const int dw = std::max(1, int(float(st.art.w) * sc));
        const int dh = std::max(1, int(float(st.art.h) * sc));
        s.blendImage(st.art, ab.x + (ab.w - dw) / 2, ab.y + (ab.h - dh) / 2, dw, dh);
    }

    drawButton(s, f.name, l.btnBrowse, "Procurar soundfont...", true, st.hoverBank == -2, false);
    drawButton(s, f.name, l.btnFolder, "+ Pasta", true, st.hoverBank == -3, false);
    drawButton(s, f.name, l.btnRemove, "Remover", st.current >= 0, st.hoverBank == -4, true);

    const int ny = l.btnRemove.y + l.btnRemove.h + 8;
    s.hline(l.btnBrowse.x - 0, l.btnBrowse.y - 10, l.btnBrowse.w, forge::line6);
    f.label.draw(s, x, ny + 6, "Bancos locais · SF2 / SF3 / SFZ / PFORGE", forge::noteTx);
    f.label.draw(s, x, ny + 17, "Escolha um item para iniciar o carregamento.",
                 forge::noteTx);
}

void drawStatusBlock(Surface& s, const Fonts& f, const Layout& l, const PanelState& st) {
    const Rect& bar = l.statusbar;
    s.rrect(bar.x, bar.y, bar.w, bar.h, 4, forge::headHi);
    hgradRect(s, Rect{bar.x + 1, bar.y + 1, bar.w - 2, bar.h - 2}, forge::groupHi,
              forge::groupLo);
    s.rrectStroke(bar.x, bar.y, bar.w, bar.h, 4, 1, forge::barEdge);

    const int pad = 12;
    const int inner = bar.w - 2 * pad;
    const float a = 1.05f, b = 1.0f, c = 1.7f;
    const int gap = 12;
    const int usableS = std::max(60, inner - 2 * gap);
    const int wA = int(float(usableS) * a / (a + b + c));
    const int wB = int(float(usableS) * b / (a + b + c));
    const int xA = bar.x + pad;
    const int xB = xA + wA + gap;
    const int xC = xB + wB + gap;

    const int sounding = st.haveStatus ? st.published : st.current;
    const bool failed = st.haveStatus && st.plugFailed;
    const bool forging = st.loading || (st.haveStatus && st.plugLoading);

    // Bloco 1: identidade. A marca vertical de brasa e o serifado de 16 px sao o
    // que da cara de instrumento ao rodape.
    s.rrect(xA, bar.y + 10, 3, bar.h - 20, 2,
            withAlpha(sounding >= 0 ? forge::ember : forge::line2, 0.88f));
    const std::string instName =
        forging && !st.loadingName.empty() ? st.loadingName
        : (sounding >= 0 ? st.activeBankName : "Nenhum piano");
    f.serif.draw(s, xA + 13, bar.y + bar.h / 2 - 1,
                 fit(f.serif, instName, wA - 23), sounding >= 0 ? 0xFFEEF0F2u : forge::muted);
    f.small.draw(s, xA + 13, bar.y + bar.h / 2 + 15,
                 fit(f.small, st.instrumentFile.empty()
                                 ? std::string("Escolha um banco na biblioteca")
                                 : st.instrumentFile, wA - 23),
                 0xFF929CA7u);

    // Bloco 2: badge de estado + preset.
    s.vline(xB - gap / 2, bar.y + 10, bar.h - 20, forge::div);
    const int sx = xB + 11;
    const char* badge = "SEM BANCO";
    uint32_t badgeTx = forge::muted, badgeBg = forge::iron2, badgeLn = forge::line2;
    if (forging) { badge = "CARREGANDO"; badgeTx = forge::warnTx; badgeBg = forge::warnBg;
                   badgeLn = forge::warnLine; }
    else if (failed) { badge = "FALHA"; badgeTx = forge::errTx; badgeBg = forge::errBg;
                       badgeLn = forge::errLine; }
    else if (sounding >= 0) { badge = "BANCO PRONTO"; badgeTx = forge::okTx;
                              badgeBg = forge::okBg; badgeLn = forge::okLine; }
    const int badgeW = f.tiny.widthSpaced(badge, 0.8f) + 10;
    const int topY = bar.y + 14;
    s.rrect(sx, topY - 8, badgeW, 16, 3, badgeBg);
    s.rrectStroke(sx, topY - 8, badgeW, 16, 3, 1, badgeLn);
    drawTracked(s, f.tiny, sx + 5, topY + 3, badge, 0.8f, badgeTx);
    const int stateTxX = sx + badgeW + 6;
    const std::string stateTx =
        forging ? (st.haveStatus ? "forjando banco" : st.loadingName)
        : failed ? "não abriu"
        : sounding >= 0 ? "piano publicado" : "Selecione um piano";
    const bool showPreset = presetSelectorVisible(st);
    // A pílula de preset mora na LINHA 2 (layoutFor), nao nesta: com as strings
    // portuguesas, badge + "piano publicado" + pílula nao cabem em 253 px e o
    // estado virava "pi...". O texto do estado tem a largura toda do bloco.
    const int stateRoom = xB + wB - stateTxX;
    f.small.draw(s, stateTxX, topY + 3, fit(f.small, stateTx, std::max(20, stateRoom)),
                 0xFFC5CCD4u);
    // Linha 3: de onde vem o estado. Limitada ao BLOCO, nao a "bar.w - 60" -- o
    // texto do estado atravessar a divisoria e entrar no dos chips e como as
    // etiquetas truncadas de antes: vira "texto desorganizado".
    f.label.draw(s, sx, bar.y + bar.h - 12,
                 fit(f.label, st.stateSub.empty() ? std::string("sem audio conectado")
                                                  : st.stateSub, wB - 22),
                 0xFF77828Eu);

    if (showPreset) {
        const Rect& pb = l.presetBox;
        for (int i = 0; i < 2; ++i) {
            const Rect& r = i == 0 ? l.presetPrev : l.presetNext;
            const bool can = i == 0 ? (st.preset > 0) : (st.preset + 1 < st.presetTotal);
            const bool hov = st.hoverPreset == i;
            s.rrect(r.x, r.y, r.w, r.h, 3, forge::presetBg);
            s.rrectStroke(r.x, r.y, r.w, r.h, 3, 1,
                          hov && can ? 0xFF9A5524u : forge::div);
            drawArrow(s, r.cx(), r.cy(), 3, i == 0 ? -1 : +1,
                      !can ? withAlpha(0xFFAAB3BEu, 0.35f)
                           : (hov ? 0xFFFFD5A5u : 0xFFAAB3BEu));
        }
        char buf[40];
        std::snprintf(buf, sizeof(buf), "Preset %d / %d", st.preset + 1, st.presetTotal);
        f.small.draw(s, l.presetPrev.x + l.presetPrev.w + 4, pb.cy() + 4, buf, 0xFFC5CCD4u);
    }

    // Bloco 3: os sete parametros que nao viram controle, em chips mono.
    s.vline(xC - gap / 2, bar.y + 10, bar.h - 20, forge::div);
    const int hx = xC + 11;
    drawTracked(s, f.tiny, hx, bar.y + 16, "PARÂMETROS DO PRESET · INFORMATIVOS", 0.96f,
                0xFF717C88u);
    int cxx = hx, cyy = bar.y + 30;
    const int lineH = 12;
    for (const HiddenChip& chip : st.hidden) {
        const std::string head = chip.key + " ";
        const int wChip = f.mono.width(head) + f.mono.width(chip.value) + 9;
        if (cxx + wChip > bar.x + bar.w - pad) {
            cxx = hx;
            cyy += lineH;
        }
        if (cyy > bar.y + bar.h - 4) break;
        f.mono.draw(s, cxx, cyy, head, forge::chipTx);
        f.mono.draw(s, cxx + f.mono.width(head), cyy, chip.value, forge::chipVal);
        cxx += wChip;
    }
}

// O veil do prototipo: um veu escuro sobre a area de controles com um card no
// meio. E o que a forja mostra antes de voce escolher -- nao e um erro, e o
// estado inicial.
void drawVeil(Surface& s, const Fonts& f, const Layout& l, const PanelState& st) {
    const Rect& area = l.control;
    // #0c0f13aa com backdrop-filter: blur(1px). Sem compositor nao ha blur; o que
    // da o efeito e escurecer a area com 67% de opacidade e nao pintar nada por
    // cima dos cards -- os cards ficam atras do veu porque o veu desenha depois.
    s.fillRect(area.x, area.y, area.w, area.h, forge::veil);

    const Rect& card = l.veilCard;
    s.rrect(card.x, card.y, card.w, card.h, 5, forge::veilCard);
    s.rrectStroke(card.x, card.y, card.w, card.h, 5, 1, forge::veilEdge);
    // o icone do CSS e "dotted circle": um anel interrompido desenhado a mao.
    const int icx = card.cx(), icy = card.y + 28;
    for (int i = 0; i < 12; ++i) {
        const float a = float(i) / 12.0f * 2.0f * float(M_PI);
        s.disc(icx + int(std::cos(a) * 10.0f), icy + int(std::sin(a) * 10.0f), 1.3f,
               forge::veilIcon);
    }
    f.serifBig.drawCentered(s, card.cx(), icy + 27,
                           st.veilTitle.empty() ? "A forja está fria" : st.veilTitle,
                           forge::text);
    const std::string copy = st.veilCopy.empty()
        ? "Selecione um banco à esquerda para mostrar os controles. "
          "Nenhum banco será carregado automaticamente." : st.veilCopy;
    // Quebra de linha por palavras, e no maximo duas: o card tem altura fixa e o
    // botao mora no pe dele. Texto que nao cabe e cortado com reticencias, nao
    // empurrado para fora da chapa.
    const int maxW = card.w - 36;
    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : copy) {
            if (c == ' ') { if (!cur.empty()) words.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
    }
    int lineY = icy + 45;
    std::string line;
    for (size_t i = 0; i < words.size(); ++i) {
        const std::string tryLine = line.empty() ? words[i] : line + " " + words[i];
        if (f.small.width(tryLine) > maxW && !line.empty()) {
            f.small.drawCentered(s, card.cx(), lineY, line, 0xFF929CA7u);
            line = words[i];
            lineY += 13;
            if (lineY > l.veilButton.y - 8) { line.clear(); break; }
        } else line = tryLine;
    }
    if (!line.empty()) {
        if (lineY <= l.veilButton.y - 8)
            f.small.drawCentered(s, card.cx(), lineY, fit(f.small, line, maxW), 0xFF929CA7u);
    }

    const Rect& bt = l.veilButton;
    const bool hov = st.hoverBank == -5;
    s.rrect(bt.x, bt.y, bt.w, bt.h, 4, hov ? mix(forge::veilBtn, forge::ember, 0.12f)
                                           : forge::veilBtn);
    s.rrectStroke(bt.x, bt.y, bt.w, bt.h, 4, 1, forge::veilBtnLn);
    f.small.drawCentered(s, bt.cx(), bt.cy() + 4, "Escolher primeiro banco",
                         forge::veilBtnTx);
}

void drawToast(Surface& s, const Fonts& f, const Layout& l, const PanelState& st) {
    if (st.toast.empty() || st.toastAge < 0.0f) return;
    const float life = st.toastAge < 0.2f ? st.toastAge / 0.2f
                       : (st.toastAge > 1.9f ? std::max(0.0f, (2.1f - st.toastAge) / 0.2f) : 1.0f);
    if (life <= 0.0f) return;
    const int w = f.small.width(st.toast) + 26, h = 26;
    // O CSS diz bottom:24px, mas la o toast e position:fixed no VIEWPORT de uma
    // pagina com 16 px de padding: na pratica ele cai 8 px acima da borda inferior
    // da .window, em cima da .statusbar. No navegador ninguem ve porque o prototipo
    // so dispara notify() em caminhos que a nossa UI nao tem. Aqui a pílula tapava
    // o "dampTune" da coluna de parametros -- texto de estado coberto por texto de
    // feedback nao e flutuante, e colisao. Ancora real: logo acima da statusbar.
    const Rect r{l.width / 2 - w / 2, l.statusbar.y - h - 6, w, h};
    s.rrect(r.x, r.y, r.w, r.h, 5, withAlpha(forge::toastBg, 0.92f * life));
    s.rrectStroke(r.x, r.y, r.w, r.h, 5, 1, withAlpha(forge::toastLn, life));
    f.small.drawCentered(s, r.cx(), r.cy() + 4, st.toast, withAlpha(forge::toastTx, life));
}

} // namespace

void drawPanel(Surface& surf, const Fonts& f, const Layout& l, const PanelState& st,
               float time) {
    // A janela do prototipo e o .window: #171b22 com moldura de 1 px em #424954,
    // raio 5 e um fio de luz em cima (box-shadow inset 0 1px 0 #ffffff0b). Dentro
    // de um host a moldura e o que separa a placa de ferro do resto da janela.
    surf.vgrad(0, 0, l.width, l.height, forge::iron1, forge::iron0);
    surf.rrectStroke(0, 0, l.width, l.height, 5, 1, forge::edge);
    surf.hline(6, 1, l.width - 12, withAlpha(0xFFFFFFFFu, 0.043f));

    drawSidebar(surf, f, l, st);

    // Painel de controles.
    const Rect& pn = l.panel;
    surf.rrect(pn.x, pn.y, pn.w, pn.h, 3, forge::card);
    surf.rrectStroke(pn.x, pn.y, pn.w, pn.h, 3, 1, forge::line5);
    hgradRect(surf, Rect{l.head.x, l.head.y, l.head.w, l.head.h}, forge::headHi,
              forge::groupLo);
    surf.hline(l.head.x + 1, l.head.y + l.head.h, l.head.w - 2, forge::line7);
    const bool live = !veilVisible(st);
    const int ledX = l.head.x + 13;
    if (live) {
        surf.glow(ledX + 3, l.head.cy(), 7, forge::ember, 0.5f);
        surf.disc(ledX + 3, l.head.cy(), 3.0f, forge::ember);
    } else {
        surf.disc(ledX + 3, l.head.cy(), 3.0f, 0xFF515B67u);
    }
    drawTracked(surf, f.label, ledX + 14, l.head.cy() + 3, "CONTROLES DE TIMBRE", 9 * 0.15f,
                forge::headTx);
    {
        // Os dois chips da direita: .model-chip (bronze) e .engine-chip.
        const std::string eng = st.engineChip.empty() ? "tsf" : st.engineChip;
        const std::string chipE = "ENGINE " + eng;
        const int wE = f.label.width(chipE) + 14;
        const Rect rE{l.head.x + l.head.w - wE - 13, l.head.cy() - 9, wE, 18};
        surf.rrect(rE.x, rE.y, rE.w, rE.h, 3, forge::card);
        surf.rrectStroke(rE.x, rE.y, rE.w, rE.h, 3, 1, forge::line3);
        f.label.drawCentered(surf, rE.cx(), rE.cy() + 3, chipE, 0xFFA8B1BBu);
        const std::string chipM = "FORJA DSP";
        const int wM = f.label.width(chipM) + 14;
        const Rect rM{rE.x - wM - 8, rE.y, wM, rE.h};
        surf.rrect(rM.x, rM.y, rM.w, rM.h, 3, forge::card);
        surf.rrectStroke(rM.x, rM.y, rM.w, rM.h, 3, 1, 0xFF5A432Du);
        f.label.drawCentered(surf, rM.cx(), rM.cy() + 3, chipM, 0xFFB28352u);
    }

    // Luz de brasa no topo da area de controles (o radial-gradient do CSS).
    surf.glow(l.control.x + int(l.control.w * 0.53f), l.control.y + int(l.control.h * 0.15f),
              std::max(40, l.control.w / 2), 0xFF242931u, 0.32f);

    for (int g = 0; g < 5; ++g)
        drawGroupCard(surf, f.sub, f.tiny, l.groupCard[g], l.groupHead[g], kGroups[g], g + 1);
    for (int k = 0; k < kKnobCount; ++k) {
        if (l.knobBox[k].w <= 0) continue;
        drawKnob(surf, f, l, st, k, live, time);
    }

    if (veilVisible(st)) drawVeil(surf, f, l, st);
    drawStatusBlock(surf, f, l, st);
    drawToast(surf, f, l, st);
}

} // namespace pforge
