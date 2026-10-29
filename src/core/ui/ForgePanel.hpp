#pragma once
// Painel da forja: geometria e desenho compartilhados entre o X11UI do plugin
// e o pforge_ui_preview. E isso que me deixa conferir o visual renderizando
// PNG offscreen, sem host LV2 instalado na maquina.
//
// O layout e a paleta sao o conceito v2 ("ideia para ui/PianoForge-Concept-v2"):
// cada medida aqui tem um numero equivalente no CSS do prototipo, e o prototipo
// e a referencia. Quando os dois divergirem, o prototipo ganha.

#include "Font.hpp"
#include "Surface.hpp"
#include "../Parameters.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pforge {

// 17 = Param_Count: os dez do TIMBRE e os sete do MOTOR numa array so, indexada
// por ParameterId. E isso que faz arrasto, roda e duplo-clique (= default)
// valerem para as duas paginas sem nenhum codigo novo -- muda so qual conjunto
// de ids o hit-test aceita.
constexpr int kKnobCount = pianoforge::Param_Count;

struct KnobState {
    int port = 0;
    std::string label;      // curto: cabem debaixo do knob sem reticencias
    std::string fullLabel;  // o nome do host, exibido no tooltip/toast quando se passa o mouse
    std::string tip;        // a dica miuda do prototipo ("nivel", "abertura", "tom", "velocity")
    std::string symbol;
    float value = 0.0f;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defValue = 0.0f;
    bool bipolar = false;   // display em dB/cents: arco cresce dos dois lados
};

struct BankRow {
    std::string name;
    std::string path;
    uint64_t sizeBytes = 0;
    bool exists = true;
};

// Os sete parametros que o §3 nao expoe como controle tambem tem chip no rodape
// ("reverbType  Sala Estudio"), como no prototipo. O chip e LEITURA: quem
// escreve neles e a pagina MOTOR, com os mesmos knobs do TIMBRE.
struct HiddenChip {
    std::string key;
    std::string value;
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    int cx() const { return x + w / 2; }
    int cy() const { return y + h / 2; }
    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

// Os dez corpos de letra do prototipo, em quatro arquivos. Font::share() copia o
// blob em vez de reler o TTF, entao dez tamanhos custam dez caches de glifo e nao
// dez leituras de disco. Font::share() mexe no destino: um Fonts e unico por UI
// (nao copia), e e passado por referencia ao renderer.
struct Fonts {
    Font brand;      // 14 negrito  "FORJA"
    Font sub;        // 10 negrito  "DE PIANOS", titulos de grupo
    Font label;      // 8           rotulos miudos em caixa alta
    Font name;       // 11          linhas de banco, botoes
    Font small;      // 9           tamanho do banco, texto secundario
    Font tiny;       // 7           dica debaixo do nome do knob, badge
    Font value;      // 8           o numero dentro da face do knob
    Font serif;      // 16          nome do instrumento no rodape
    Font serifBig;   // 17          titulo do veil
    Font mono;       // 8           chips dos parametros informativos

    // serifPath/monoPath vazios sao tolerados: o painel cai no sans e continua
    // desenhando (um bundle sem a DejaVu Serif nao pode ser tela preta).
    bool load(const std::string& sansPath, const std::string& boldPath,
              const std::string& serifPath, const std::string& monoPath);
    bool ok() const { return name.ok() && small.ok(); }
};

struct PanelState {
    std::vector<BankRow> banks;
    int current = -1;          // -1 = nenhum piano escolhido (o que VOCE pediu)
    int hoverBank = -1;        // negativo: -2..-5 sao os botoes/busca da coluna esquerda
    int hoverPreset = -1;      // 0 = "<<", 1 = ">>" (so quando o seletor aparece)
    int scroll = 0;
    int page = 0;              // 0 = TIMBRE (as cinco chapas), 1 = MOTOR (os sete)
    std::array<KnobState, kKnobCount> knobs;
    int hoverKnob = -1;
    int hoverTab = -1;         // 0/1: a aba sob o mouse
    int dragKnob = -1;
    float sparkAge = -1.0f;    // >=0: fagulhas vivas (tempo desde o ultimo toque)
    bool loading = false;
    std::string loadingName;
    std::string activeBankName;  // nome do banco PUBLICADO (nao do ultimo clicado)
    Image art;                   // arte do manifest.presets[].imagem, se houver

    // O que o PLUGIN disse (portas de saida 22/23). `haveStatus` e o que separa
    // o feedback real do chute de tempo por tamanho de arquivo: num host que
    // nunca repassa port_event a estimativa continua sendo o unico palpite.
    bool haveStatus = false;
    bool plugLoading = false;
    bool plugFailed = false;
    int published = -1;        // >=0: indice do banco que esta de fato soando
    int presetTotal = 0;       // presets do banco publicado (porta 23)
    int preset = 0;            // indice do preset em uso (porta 21)

    // --- novos, todos vindos do conceito v2 ---
    std::string filter;        // o "Filtrar bancos..." da busca
    bool searchFocus = false;
    std::vector<HiddenChip> hidden;
    std::string instrumentFile;  // "Grand Piano.sf2 - 246 MB", a linha miuda da identidade
    std::string stateSub;        // a linha miuda do bloco de estado
    std::string engineChip;      // "SFIZZ" / "TINYSONIC" / "tsf"
    std::string toast;
    float toastAge = -1.0f;      // >=0: toast vivo (segundos desde que apareceu)
    std::string veilTitle;
    std::string veilCopy;
};

enum class HitKind {
    None, BankRow, Browse, AddFolder, Remove, Knob, PresetDec, PresetInc, Search, VeilButton,
    Page                       // index = a pagina pedida (0 TIMBRE, 1 MOTOR)
};

struct Hit {
    HitKind kind = HitKind::None;
    int index = -1;
};

struct Layout {
    // As tres chapas do prototipo: .workarea (sidebar + painel) e .statusbar.
    Rect sidebar, panel, statusbar, head, control;
    Rect search, list;
    Rect btnBrowse, btnFolder, btnRemove;
    // A placa da arte do manifest. O prototipo nao a tem (nao ha bundle no
    // navegador): quando existe, ela comesce o mesmo vazio que a lista deixava,
    // e por isso Entra NA CONTA da altura da lista -- ver layoutFor(artH).
    Rect artBox;
    // Cards de grupo, em ORDEM DE EXIBICAO (01 MASTER, 02 RESSONANCIA,
    // 03 DINAMICA, 04 CORPO, 05 ESPACO).
    Rect groupCard[5], groupHead[5];
    std::array<Rect, kKnobCount> knobBox;
    std::array<int, kKnobCount> knobCx{}, knobCy{};
    std::array<float, kKnobCount> knobU{};   // escala: o SVG do prototipo e um 80x80
    Rect presetBox, presetPrev, presetNext;
    // As duas abas do cabecalho (TIMBRE | MOTOR). Largura FIXA: o layout nao tem
    // fonte para medir texto, e um botao cuja largura seguisse a fonte do sistema
    // sairia do lugar num bundle sem a DejaVu.
    Rect tabTimbre, tabMotor;
    // A chapa unica da pagina MOTOR: ocupa a mesma area que as cinco do TIMBRE
    // dividem entre si -- so uma das duas e desenhada por vez.
    Rect engineCard, engineHead;
    Rect veilCard, veilButton;
    int width = 0, height = 0;
};

// Geometria da janela. `artH` e a altura da placa de arte do manifest (0 = sem
// arte): ela mora entre a lista e os botoes, e quem reserva o espaco e ESTA
// funcao -- se a lista nao encolhesse junto, a arte cobriria a ultima linha e o
// hit-test continuaria achando que aquela linha esta ali.
Layout layoutFor(int w, int h, int artH = 0);

// Altura da placa para uma arte de imgW x imgH, contida na largura util da
// sidebar. UI e preview calculam por AQUI: a placa muda o layout (encolhe a
// lista), e duas contas diferentes fariam o clique acertar outra linha.
// Devolve 0 quando nao ha placa (sem arte, ou arte baixa demais pra valer o
// espaco que ela tomaria da lista).
int artPlateHeight(int imgW, int imgH);

// Indices de banks[] que passam pelo filtro, na ordem da lista. UI, desenho e
// hit-test tem de usar ESTA funcao -- se cada um filtrar do seu jeito, a linha
// clicada deixa de ser a linha mostrada.
std::vector<int> visibleBanks(const PanelState& s);

// Quantas linhas de banco cabem na lista, contadas com o MESMO passo do
// hit-test e do desenho. A UI limita o scroll por aqui: se ela contar o passo
// por conta propria, a ultima linha rolada sai do hit-test e vira area morta.
int listRowCapacity(const Layout& l);

// O seletor de preset so tem o que mostrar quando existe banco publicado E ele
// tem mais de um preset (SFZ e sempre um; SF2 grande pode ter centenas).
bool presetSelectorVisible(const PanelState& s);

// O veil ("A forja esta fria") cobre a area de controles enquanto nao ha piano
// publicado -- e o estado em que o plugin abre, por decisao sua. Ele cobre a
// pagina TIMBRE: a do MOTOR nao depende de banco nenhum e fica editavel com a
// forja fria (afinar o motor antes de escolher o piano e trabalho legitimo).
bool veilVisible(const PanelState& s);

// Pagina de um parametro: os dez visiveis sao o TIMBRE, os outros sete o MOTOR.
// Hit-test, desenho e a UI usam ESTA funcao -- se cada um decidisse por conta
// propria, um disco clicavel apareceria na pagina errada.
int pageDoKnob(int paramId);

Hit hitTest(const Layout& l, const PanelState& s, int x, int y);

// `time` s6o movem as fagulhas; passe 0 para um frame deterministico.
void drawPanel(Surface& surf, const Fonts& f, const Layout& l, const PanelState& s,
               float time);

std::string formatSize(uint64_t bytes);
std::string formatValue(const KnobState& k);

// Os dois textos do rodape, montados AQUI e nao em cada consumidor. Duplicar
// regra de texto entre a UI e o preview ja divergiu (o "--presets 1" mostrava
// "preset 1/1" na prova e nada na tela), e quando o preview diverge ele para de
// ser prova.
//
// Linha miuda da identidade: "<arquivo> - <tamanho>". O nome do preset do
// manifest entra no lugar do arquivo SO quando a contagem do manifest bate com o
// que a porta 23 informou -- senao a tela afirmaria um preset que o plugin nao
// esta usando.
std::string identityLine(const PanelState& s, const std::string& fileName,
                         const std::vector<std::string>& manifestPresets);

// A linha que diz DE ONDE vem o estado shown. Com a porta 22 viva, o rodape e o
// plugin; sem ela, e estimativa por tamanho; sem nada, o painel esta olhando para
// uma porta fechada.
std::string stateSubLine(const PanelState& s);

// Grupos fixos do PIANOFORGE.md §3, agora por ParameterId (o layout do
// prototipo agrupa por categoria, nao por posicao de tela).
struct GroupDef {
    const char* title;
    int ids[4];
    int count;
    bool hot;    // filete de brasa a esquerda (MASTER e RESSONANCIA no prototipo)
};
// Ordem de EXIBICAO, com o indice impresso no titulo (01..05).
extern const GroupDef kGroups[5];

} // namespace pforge
