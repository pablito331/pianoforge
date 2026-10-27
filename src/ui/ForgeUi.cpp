// Controlador portavel da UI. Ver ForgeUi.hpp para o criterio da divisao.
//
// Regras de produto que moram aqui (e nao na camada de janela):
//   - nada e escolhido por voce no abrir: current_ = -1, nenhum valor e escrito
//     na porta soundbank;
//   - um clique na lista escreve o indice UMA vez (o plugin carrega no worker);
//   - os 10 knobs visiveis escrevem nas portas 3..12; os 7 escondidos sao so
//     texto no rodape, nunca controle arrastavel;
//   - o nome no rodape e do piano que esta SOANDO (porta 22), nao do ultimo
//     clicado: um arquivo que nao abriu nao muda o que se esta ouvindo.

#include "ForgeUi.hpp"

#include "Bundle.hpp"
#include "Image.hpp"
#include "Parameters.hpp"
#include "SoundbankManager.hpp"
#include "StatusPorts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace pfui {

using namespace pforge;

namespace {
constexpr uint32_t kPortSoundbank = 20;
constexpr uint32_t kPortPreset = 21;        // preset do banco publicado
constexpr uint32_t kPortParamBase = 3;      // portas 3..19 = ParameterId 0..16
constexpr int kWinW = 1000;
constexpr int kWinH = 520;
} // namespace

void utf8Append(std::string& out, unsigned cp) {
    if (cp < 0x80) { out += char(cp); return; }
    if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
        return;
    }
    if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
        return;
    }
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
}

ForgeUi::ForgeUi(const std::string& bundlePath, LV2UI_Write_Function writeFn,
                 LV2UI_Controller controller)
    : write_(writeFn), controller_(controller), bundlePath_(bundlePath) {
    loadFonts(bundlePath);

    // O painel indexa knobs[] por ParameterId (kGroups decide o card e a posicao
    // dentro dele), e os dez visiveis sao exatamente as portas 3..12.
    int slot = 0;
    for (int pid = 0; pid < pianoforge::Param_Count && slot < kKnobCount; ++pid) {
        const auto& def = pianoforge::kParameterDefs[pid];
        if (!def.isVisible) continue;
        if (pid != slot) {
            std::fprintf(stderr,
                         "[PianoForge] knob %d espera ParameterId %d, veio %d: "
                         "o painel assume que os dez visiveis sao os dez primeiros\n",
                         slot, slot, pid);
        }
        KnobState& k = state_.knobs[pid];
        k.port = int(kPortParamBase + pid);
        k.label = def.shortName;
        k.fullLabel = def.name;
        k.tip = def.tip;
        k.symbol = def.symbol;
        k.minValue = def.minValue;
        k.maxValue = def.maxValue;
        k.defValue = def.defaultValue;
        k.value = def.defaultValue;
        k.bipolar = def.minValue < 0.0f;
        ++slot;
    }
    rebuildHiddenChips();
    rescan();
}

ForgeUi::~ForgeUi() = default;

int ForgeUi::preferredWidth() { return kWinW; }
int ForgeUi::preferredHeight() { return kWinH; }

void ForgeUi::log(const char* msg) { std::fprintf(stderr, "[PianoForge UI] %s\n", msg); }

// ---------------------------------------------------------------------------
// tamanho e apresentacao
// ---------------------------------------------------------------------------

void ForgeUi::adoptSize(int w, int h) {
    cfgW_ = w;
    cfgH_ = h;
    // Nao alargar ate kWinW: se o host deu menos, um blit maior que o drawable e
    // erro em todo frame (a tela volta a ficar preta). So limitar o teto ao nosso
    // design e o piso a um tamanho nao-absurdo.
    w = std::clamp(w, 64, kWinW);
    h = std::clamp(h, 64, kWinH);
    if (w == winW_ && h == winH_ && winW_ != 0) return;
    winW_ = w;
    winH_ = h;
    surf_.resize(winW_, winH_);
    if (!resizeBacking(winW_, winH_)) { winW_ = winH_ = 0; return; }
    needDraw_ = true;
}

// "O host mudou o meu window": adotar a dimensao dele e redesenhar SEMPRE (um
// reparent/resize invalida o conteudo ja pintado). O log sai so quando o tamanho
// muda de verdade, porque um WM manda ConfigureNotify a cada passo do drag.
void ForgeUi::onWindowGeometry(int w, int h) {
    if (w > 0 && h > 0 && (w != cfgW_ || h != cfgH_)) {
        std::fprintf(stderr, "[PianoForge UI] host mudou a janela para %dx%d\n", w, h);
        adoptSize(w, h);
    }
    needDraw_ = true;
}

void ForgeUi::drawLocked() {
    if (!winW_ || !winH_) return;
    needDraw_ = false;
    layout_ = layoutFor(winW_, winH_, artPlateH());
    drawPanel(surf_, fonts_, layout_, state_, float(elapsedSinceTouch()));
    present(surf_);
}

void ForgeUi::serviceFrame() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    tick();
    if (needDraw_) drawLocked();
}

void ForgeUi::wakeOrDraw() {
    if (hasEventThread_) requestRedraw();
    else { std::lock_guard<std::recursive_mutex> lk(mtx_); tick(); if (needDraw_) drawLocked(); }
}

bool ForgeUi::animating() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    return state_.loading || state_.sparkAge >= 0.0f || state_.toastAge >= 0.0f;
}

// Relogio do painel: fagulhas e brasa de carregamento vivem por tempo, nao por
// evento. Roda com mtx_ tomado.
void ForgeUi::tick() {
    // Fagulhas vivem ~1,5 s depois de soltar o knob: enquanto isso o painel
    // precisa de frame novo; depois, so redesenha se algo mudou.
    if (state_.sparkAge >= 0.0f) {
        state_.sparkAge = elapsedSinceTouch();
        needDraw_ = state_.sparkAge < 1.5f || animating();
    }
    if (state_.loading && elapsedSinceTouch() > loadSeconds_) {
        // Duas origens de verdade para o "forjando": a porta 22, quando o host
        // repassa port_event, e o teto de seguranca quando nao repassa (host
        // parado, host que nao manda eventos de entrada). Sem a segunda a
        // janela ficaria com brasa acesa para sempre depois de um load que o
        // plugin nunca recebeu.
        state_.loading = false;
        refreshStateSub();
        if (!state_.haveStatus) notify("o plugin nao respondeu ao pedido");
        needDraw_ = true;
    }
    if (state_.toastAge >= 0.0f) {
        state_.toastAge = float(elapsed(Clock::now() - toastClock_));
        if (state_.toastAge > 2.1f) {
            // Morto de verdade: toastAge < 0 e o que tira o painel do modo
            // animado. Deixar o valor crescendo faria a thread acordar para
            // redesenhar um aviso que nao esta mais na tela.
            state_.toast.clear();
            state_.toastAge = -1.0f;
            needDraw_ = true;
        } else {
            needDraw_ = true;                 // o .toast do CSS vive ~2 s
        }
    }
}

// ---------------------------------------------------------------------------
// ciclo de vida pedido pelo host
// ---------------------------------------------------------------------------

// Sem thread propria (host que bombeia idle), o idle() dele e o nosso pump. Com
// thread, idle() so da um chute no relogio: tocar em X/Win32/Cocoa daqui seria
// mexer numa conexao que pertence a outra thread.
int ForgeUi::idle() {
    if (!loggedIdle_) {
        loggedIdle_ = true;
        std::fprintf(stderr, "[PianoForge UI] primeiro idle() do host: %s\n",
                     hasEventThread_ ? "com thread de eventos propria"
                                     : "sem thread, o host e o nosso pump");
    }
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    tick();
    if (needDraw_ && !hasEventThread_) drawLocked();
    return closed_ ? 1 : 0;
}

int ForgeUi::show() {
    needDraw_ = true;
    mapWindow();
    wakeOrDraw();
    return 0;
}

int ForgeUi::hide() {
    unmapWindow();
    return 0;
}

// Toda port_event vem da thread principal do host. Ela mexe o estado sob lock e
// acorda a thread da UI, que e quem desenha.
void ForgeUi::portEvent(uint32_t port, uint32_t bufferSize, uint32_t format,
                        const void* buffer) {
    applyPortEvent(port, bufferSize, format, buffer);
    if (hasEventThread_) requestRedraw();
}

void ForgeUi::applyPortEvent(uint32_t port, uint32_t bufferSize, uint32_t format,
                             const void* buffer) {
    if (format != 0 || !buffer) return;
    // Uma portNotification de atom (ui:portSubscribe) chega com format != 0, mas
    // um host que manda formato 0 com outro tamanho nao e raro: ler 4 bytes de um
    // buffer menor e ler lixo, e de um maior e so metade do evento.
    if (bufferSize != sizeof(float)) return;
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    const float v = *static_cast<const float*>(buffer);
    // NaN de automacao quebrada travaria o knob para sempre: SmoothedValue nunca
    // escapa de um NaN, e double-click-para-default tambem nao conserta.
    if (!std::isfinite(v)) return;
    if (port == kPortSoundbank) {
        const int idx = int(std::lround(v));
        if (idx != state_.current) noteBankRequested(idx);
        return;
    }
    if (port == pianoforge::kPortBankState) { applyBankStatus(v); return; }
    if (port == pianoforge::kPortPresetCount) {
        const int total = int(std::lround(v));
        if (total != state_.presetTotal) {
            state_.presetTotal = total;
            if (state_.preset >= total) state_.preset = 0;
            syncSoundingName();
            needDraw_ = true;
        }
        return;
    }
    if (port == kPortPreset) {
        const int p = int(std::lround(v));
        // syncSoundingName(), nao so a linha do rodape: a arte do bundle e por
        // preset, e um preset que o host escolheu sozinho tem de trocar o
        // retrato junto com o nome.
        if (p != state_.preset) { state_.preset = p; syncSoundingName(); needDraw_ = true; }
        return;
    }
    if (port >= kPortParamBase && port < kPortParamBase + pianoforge::Param_Count) {
        const int pid = int(port - kPortParamBase);
        if (pianoforge::kParameterDefs[pid].isVisible && pid < kKnobCount) {
            state_.knobs[pid].value = v;
            needDraw_ = true;
        } else if (!pianoforge::kParameterDefs[pid].isVisible) {
            noteHiddenValue(pid, v);
        }
    }
}

// O plugin escreve na porta 22 a cada bloco: -3 enquanto o worker roi o
// arquivo, o indice publicado quando toca, -2 silencio, -4 recusa. Com isso o
// "FORJANDO" do painel e o que esta acontecendo, nao um chute de tempo.
void ForgeUi::applyBankStatus(float code) {
    const bool loading = pianoforge::bankLoading(code);
    const bool failed = pianoforge::bankFailed(code);
    const int published = pianoforge::bankPublishedIndex(code);
    if (loading == state_.plugLoading && failed == state_.plugFailed &&
        published == state_.published && state_.haveStatus) {
        return;                       // o host reenvia o mesmo valor todo bloco
    }
    state_.haveStatus = true;
    state_.plugLoading = loading;
    state_.plugFailed = failed;
    const bool changedBank = published != state_.published;
    state_.published = published;

    if (loading) state_.loading = true;
    else {
        state_.loading = false;
        if (failed) notify("o arquivo nao abriu - continua tocando o anterior");
    }
    if (changedBank && published >= 0) state_.preset = 0;   // o plugin tambem volta ao 0
    refreshStateSub();
    syncSoundingName();
    needDraw_ = true;
}

// A linha miuda do bloco de estado. Dizer DE ONDE vem o que a tela mostra e o
// ponto: com a porta 22 ativa, "CARREGANDO" e o plugin; sem ela, e a nossa
// estimativa por tamanho, e a linha tem de confessar isso.
void ForgeUi::refreshStateSub() { state_.stateSub = stateSubLine(state_); }

// ---------------------------------------------------------------------------
// gesto
// ---------------------------------------------------------------------------

// Codigos de hover da coluna esquerda, os mesmos que drawSidebar() le:
// -2 Procurar, -3 +Pasta, -4 Remover, -5 botao do veil, -6 campo de busca.
// Um codigo novo aqui sem um == correspondente la e hover invisivel.
void ForgeUi::updateHover(int x, int y) {
    const Hit h = hitTest(layout_, state_, x, y);
    int newHoverKnob = (h.kind == HitKind::Knob) ? h.index : -1;
    int newHoverBank = -1;
    if (h.kind == HitKind::BankRow) newHoverBank = h.index;
    else if (h.kind == HitKind::Browse) newHoverBank = -2;
    else if (h.kind == HitKind::AddFolder) newHoverBank = -3;
    else if (h.kind == HitKind::Remove) newHoverBank = -4;
    else if (h.kind == HitKind::VeilButton) newHoverBank = -5;
    else if (h.kind == HitKind::Search) newHoverBank = -6;
    const int newHoverPreset = h.kind == HitKind::PresetDec ? 0
                               : h.kind == HitKind::PresetInc ? 1 : -1;
    if (newHoverKnob != state_.hoverKnob || newHoverBank != state_.hoverBank ||
        newHoverPreset != state_.hoverPreset) {
        state_.hoverKnob = newHoverKnob;
        state_.hoverBank = newHoverBank;
        state_.hoverPreset = newHoverPreset;
        needDraw_ = true;
    }
}

void ForgeUi::onMotion(int x, int y, bool shift, bool dragging) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (dragKnob_ >= 0 && dragging) {
        KnobState& k = state_.knobs[dragKnob_];
        const float fine = shift ? 0.15f : 1.0f;
        const float range = k.maxValue - k.minValue;
        const float dy = float(dragStartY_ - y) / 180.0f;
        float v = dragStartVal_ + dy * range * fine;
        v = std::max(k.minValue, std::min(k.maxValue, v));
        if (std::fabs(v - k.value) > 1e-6f) {
            k.value = v;
            writePort(k.port, v);
            needDraw_ = true;
        }
        return;
    }
    updateHover(x, y);
}

void ForgeUi::onPress(int x, int y, double timeSec) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    const Hit h = hitTest(layout_, state_, x, y);
    const bool doubleKnob =
        h.kind == HitKind::Knob && h.index == lastPressKnob_ &&
        timeSec - lastPressSec_ < 0.45;
    touchClock_ = Clock::now();
    lastPressSec_ = timeSec;
    // O par (indice, instante) que define o duplo clique so vale para prensa EM
    // KNOB. Guardar o indice de uma linha da lista e prensar o knob seguinte
    // dentro de 450 ms leria "duplo clique" entre objetos diferentes e jogaria o
    // default na porta em vez de comecar o arrasto -- os dois numeros se
    // encontram porque linha 0 e knob 0 coexistem na mesma tela.
    lastPressKnob_ = (h.kind == HitKind::Knob) ? h.index : -1;
    // O foco da busca segue o navegador: qualquer clique fora do campo o
    // solta. Deixar aceso depois de clicar um banco mostra o cursor piscando
    // num campo que nao vai receber a proxima tecla.
    if (state_.searchFocus && h.kind != HitKind::Search) {
        state_.searchFocus = false;
        needDraw_ = true;
    }
    switch (h.kind) {
        case HitKind::Knob: {
            if (doubleKnob) {                       // duplo clique = default
                KnobState& k = state_.knobs[h.index];
                k.value = k.defValue;
                writePort(k.port, k.value);
                lastPressKnob_ = -1;
            } else {
                dragKnob_ = h.index;
                dragStartY_ = y;
                dragStartVal_ = state_.knobs[h.index].value;
                focusWindow();
            }
            needDraw_ = true;
            break;
        }
        case HitKind::BankRow: selectBank(h.index); break;
        case HitKind::Search:
            state_.searchFocus = true;
            // O campo e da UI, mas quem entrega a tecla e o host (o window e
            // filho dele). Pedir o foco e o unico jeito de ela chegar.
            focusWindow();
            needDraw_ = true;
            break;
        case HitKind::VeilButton: chooseFirstBank(); break;
        case HitKind::Browse: requestDialog("file"); break;
        case HitKind::AddFolder: requestDialog("dir"); break;
        case HitKind::Remove: removeCurrent(); break;
        case HitKind::PresetDec: stepPreset(-1); break;
        case HitKind::PresetInc: stepPreset(+1); break;
        default: break;
    }
}

void ForgeUi::onRelease(int, int) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (dragKnob_ >= 0) {
        state_.dragKnob = -1;
        dragKnob_ = -1;
        state_.sparkAge = 0.0f;
        touchClock_ = Clock::now();
        needDraw_ = true;
    }
}

// `up` e +1 para roda para cima em TODA plataforma. Em todo host de audio roda
// para cima sobe o valor, e o usuario compara os dois com o ouvido e com o
// habito. A lista e o unico caso contrario -- la "para cima" e voltar ao topo.
void ForgeUi::onWheel(int x, int y, int up) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    const Hit h = hitTest(layout_, state_, x, y);
    if (h.kind == HitKind::Knob) {
        KnobState& k = state_.knobs[h.index];
        const float step = (k.maxValue - k.minValue) / 50.0f;
        k.value = std::max(k.minValue, std::min(k.maxValue, k.value + up * step));
        writePort(k.port, k.value);
        state_.hoverKnob = h.index;
        needDraw_ = true;
        return;
    }
    if (h.kind == HitKind::PresetDec || h.kind == HitKind::PresetInc ||
        (presetSelectorVisible(state_) && layout_.presetBox.contains(x, y))) {
        stepPreset(up);
        return;
    }
    if (h.kind == HitKind::BankRow || layout_.list.contains(x, y)) {
        // O teto do scroll e do que PASSA NO FILTRO, contado pelo passo do
        // proprio renderer. Com 8 bancos e busca "p" sobrariam 3 linhas de nada
        // se eu usasse banks.size().
        const int shown = int(visibleBanks(state_).size());
        const int last = std::max(0, shown - listRowCapacity(layout_));
        state_.scroll = std::clamp(state_.scroll - up, 0, last);
        needDraw_ = true;
    }
}

void ForgeUi::onKey(UiKey key, const std::string& text) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    // A busca vem antes do resto: com o campo focado ate o Escape e da busca
    // (limpar o filtro; so fechar o foco quando ele ja esta vazio), nao da
    // janela. Fechar a UI por Backspace dentro de um DAW seria a pior surpresa
    // possivel.
    if (state_.searchFocus) {
        if (key == UiKey::Backspace) {
            popFilterChar();
        } else if (key == UiKey::Escape) {
            if (state_.filter.empty()) state_.searchFocus = false;
            else state_.filter.clear();
        } else if (key == UiKey::Return) {
            chooseFirstBank();
            wakeOrDraw();
            return;
        } else if (key == UiKey::Text) {
            appendFilterText(text);
        }
        clampScroll();       // filtro novo pode deixar o scroll alem do fim
        needDraw_ = true;
        return;
    }
    if (key == UiKey::Escape) { closed_ = true; return; }
    // Typeahead: num navegador de banco, digitar FILTRA sem antes clicar no
    // campo -- e o que faz o explorador de FX do REAPER, e o que se espera de uma
    // lista de 300 nomes. Sem isto a busca so existia depois do clique, e o
    // "y-a-m" digitado caia aqui embaixo, onde nenhuma tecla de texto tem ramo.
    if (key == UiKey::Text) {
        state_.searchFocus = true;
        focusWindow();
        appendFilterText(text);
        clampScroll();
        needDraw_ = true;
        return;
    }
    if (state_.banks.empty()) return;
    if (key == UiKey::Up) selectBank(std::max(0, state_.current - 1));
    else if (key == UiKey::Down)
        selectBank(std::min(int(state_.banks.size()) - 1,
                            state_.current < 0 ? 0 : state_.current + 1));
    else if (key == UiKey::Left) stepPreset(-1);
    else if (key == UiKey::Right) stepPreset(+1);
    // Return sem foco NAO abre o dialogo de arquivo: num DAW um Enter perdido
    // abriria um modal na thread da UI e o painel pararia de responder ate alguem
    // fechar o zenity. Dar foco a busca e o gesto honesto.
    else if (key == UiKey::Return) {
        state_.searchFocus = true;
        focusWindow();
        needDraw_ = true;
    }
}

// Backspace apaga UM glifo, nao um byte: "Pianao" tem o "ao" em dois bytes e
// pop_back() deixaria metade de um UTF-8 no filtro, desenhada como lixo.
void ForgeUi::popFilterChar() {
    if (state_.filter.empty()) return;
    size_t lead = state_.filter.size() - 1;
    while (lead > 0 && (uint8_t(state_.filter[lead]) & 0xC0) == 0x80) --lead;
    state_.filter.erase(lead);
}

// Um evento de tecla pode carregar mais de um codepoint (o Cocoa entrega uma
// string por comando de IME); o filtro aceita so graficos, e o DEL (0x7F) que o
// X devolve para alguns modifiers nao e um deles.
void ForgeUi::appendFilterText(const std::string& text) {
    for (const unsigned cp : utf8DecodeChars(text))
        if (cp >= 0x20 && cp != 0x7F) utf8Append(state_.filter, cp);
}

void ForgeUi::clampScroll() {
    const int last = std::max(0, int(visibleBanks(state_).size()) - listRowCapacity(layout_));
    state_.scroll = std::clamp(state_.scroll, 0, last);
}

// Bytes UTF-8 -> codepoints. O teclado entrega a sequencia ja codificada e o
// filtro precisa de codepoints inteiros para nao partir um acento ao reencodar.
std::vector<unsigned> ForgeUi::utf8DecodeChars(const std::string& s) {
    std::vector<unsigned> out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned cp = c;
        unsigned extra = 0;
        if (c >= 0xF0) { cp = c & 0x07u; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0Fu; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1Fu; extra = 1; }
        for (unsigned k = 1; k <= extra && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// portas, banco, manifest
// ---------------------------------------------------------------------------

void ForgeUi::writePort(uint32_t port, float value) {
    if (write_) write_(controller_, port, sizeof(float), 0, &value);
}

void ForgeUi::notify(const std::string& msg) {
    state_.toast = msg;
    state_.toastAge = 0.0f;
    toastClock_ = Clock::now();
    needDraw_ = true;
}

void ForgeUi::notifyExternal(const std::string& msg) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    notify(msg);
}

// Os sete escondidos nao viram controle arrastavel (PIANOFORGE.md §5): sao os
// chips monoespacados do rodape, com o VALOR ATUAL de cada um quando o host
// tiver mandado port_event daquela porta, e o default do TTL ate la.
void ForgeUi::rebuildHiddenChips() {
    state_.hidden.clear();
    // Ordem das portas CRESCENTE (13..19), que e a ordem do prototipo:
    // reverbType, trimDb, width, releaseDamp, hammerTone, sympTune, engineTune.
    for (int pid = pianoforge::Param_ReverbType; pid < pianoforge::Param_Count; ++pid) {
        const auto& def = pianoforge::kParameterDefs[pid];
        if (def.isVisible) continue;
        const int slot = pid - pianoforge::Param_ReverbType;
        const float v = hiddenLive_[slot] ? hiddenValue_[slot] : def.defaultValue;
        char buf[48];
        switch (pid) {
            case pianoforge::Param_ReverbType:
                std::snprintf(buf, sizeof(buf), "Sala %d", int(std::lround(v)));
                break;
            case pianoforge::Param_TrimDb:
                std::snprintf(buf, sizeof(buf), "%+.1f dB", v);
                break;
            case pianoforge::Param_EngineTune:
                std::snprintf(buf, sizeof(buf), "%.0f ct", v);
                break;
            default:
                std::snprintf(buf, sizeof(buf), "%.2f", v);
                break;
        }
        state_.hidden.push_back({def.symbol, buf});
    }
}

void ForgeUi::noteHiddenValue(int pid, float v) {
    const int slot = pid - pianoforge::Param_ReverbType;
    if (slot < 0 || slot >= 7) return;
    if (hiddenValue_[slot] == v && hiddenLive_[slot]) return;
    hiddenValue_[slot] = v;
    hiddenLive_[slot] = true;
    rebuildHiddenChips();
    needDraw_ = true;
}

// Chip de motor do cabecalho: qual dos tres leitores o arquivo pede. Um
// .pforge aponta um sf2 ou um sfz dentro de si, e e isso que conta.
std::string ForgeUi::engineNameOf(const std::string& path) const {
    if (path.empty()) return {};
    if (hasExt(path, ".sfz") || hasExt(path, ".sf3")) return "SFIZZ";
    if (hasExt(path, ".pforge")) {
        pianoforge::Bundle b;
        std::string err;
        if (b.open(path, err) && !b.presets().empty())
            return hasExt(b.presets().front().sf2, ".sfz") ||
                           hasExt(b.presets().front().sf2, ".sf3")
                       ? "SFIZZ" : "TINYSONIC";
        return "TINYSONIC";
    }
    return "TINYSONIC";
}

bool ForgeUi::hasExt(const std::string& path, const std::string& ext) {
    if (path.size() < ext.size()) return false;
    const size_t base = path.size() - ext.size();
    for (size_t i = 0; i < ext.size(); ++i) {
        // O dobrar e so ASCII de proposito: uma locale do host nao pode mudar o
        // veredito de qual backend o arquivo pede.
        const char c = path[base + i];
        const char lo = (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
        if (lo != ext[i]) return false;
    }
    return true;
}

// Nome de um .pforge e o que o manifest diz, nao o nome do arquivo. Le-se na
// thread da UI: Bundle::open so pega cabecalho, diretorio e o manifest (texto
// pequeno), e a arte sao alguns KB. Nada disso chega perto do run().
std::string ForgeUi::bundleDisplayName(const std::string& path, const std::string& fallback) {
    if (!hasExt(path, ".pforge")) return fallback;
    pianoforge::Bundle b;
    std::string err;
    if (!b.open(path, err)) return fallback;
    std::string name = b.productName();
    if (name.empty() && !b.presets().empty()) name = b.presets().front().name;
    return name.empty() ? fallback : name;
}

std::string ForgeUi::fileNameOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void ForgeUi::rescan() {
    auto& mgr = pianoforge::SoundbankManager::getInstance();
    mgr.scan();
    state_.banks.clear();
    for (const auto& info : mgr.list()) {
        BankRow b;
        b.name = bundleDisplayName(info.path, info.name);
        b.path = info.path;
        b.sizeBytes = info.sizeBytes;
        b.exists = info.exists;
        state_.banks.push_back(b);
    }
    if (state_.current >= int(state_.banks.size())) state_.current = -1;
    artKey_.clear();          // os nomes mudaram; a arte tem de ser relida
    needDraw_ = true;
}

void ForgeUi::selectBank(int index) {
    if (index < 0 || index >= int(state_.banks.size())) return;
    if (index == state_.current) return;
    noteBankRequested(index);
    writePort(kPortSoundbank, float(index));
}

// Um banco pedido: vale tanto para o clique na lista quanto para o host mexendo
// na porta 20 (automacao, recall de preset). O nome no "FORJANDO" sai daqui,
// senao o rodape mostraria a brasa sem dizer o que esta sendo forjado.
void ForgeUi::noteBankRequested(int index) {
    state_.current = index;
    state_.plugFailed = false;
    state_.loading = true;
    const bool known = index >= 0 && index < int(state_.banks.size());
    state_.loadingName = known ? state_.banks[index].name + " (" +
                                     formatSize(state_.banks[index].sizeBytes) + ")"
                               : "nenhum piano";
    loadSeconds_ = loadEstimate(known ? state_.banks[index].sizeBytes : 0);
    touchClock_ = Clock::now();
    // O prototipo avisa no toast ("Carregando <nome>...") em vez de escrever no
    // rodape: o rodape e o estado do plugin, o toast e o pedido do usuario.
    notify("solicitado: " + state_.loadingName);
    syncSoundingName();
    needDraw_ = true;
}

// O botao "escolher o primeiro" do veil. So faz sentido existir se houver um
// banco REGISTRADO que exista no disco; com a lista filtrada, o primeiro e o
// primeiro que passa no filtro -- clicar e o que a linha visivel mostra.
void ForgeUi::chooseFirstBank() {
    for (const int i : visibleBanks(state_)) {
        if (i >= 0 && i < int(state_.banks.size()) && state_.banks[i].exists) {
            selectBank(i);
            return;
        }
    }
    notify(state_.filter.empty() ? "nenhum banco registrado ainda" : "nada corresponde a busca");
}

// O nome no rodape e do piano QUE ESTA SOANDO, nao do ultimo clicado: um
// arquivo que nao abriu nao muda o que se esta ouvindo.
void ForgeUi::syncSoundingName() {
    const int pub = state_.haveStatus ? state_.published : state_.current;
    if (pub >= 0 && pub < int(state_.banks.size())) {
        state_.activeBankName = state_.banks[pub].name;
        presetFile_ = fileNameOf(state_.banks[pub].path);
        state_.engineChip = engineNameOf(state_.banks[pub].path);
        loadBundleArt(state_.banks[pub].path, state_.preset);
    } else {
        state_.activeBankName.clear();
        presetFile_.clear();
        state_.engineChip.clear();
        loadBundleArt(std::string(), 0);
    }
    refreshPresetLine();
    // A placa de arte muda a geometria da lista (layoutFor artH), nao so o
    // pixel: refazer o layout aqui e o que mantem clique == linha desenhada
    // entre o evento e o proximo frame.
    layout_ = layoutFor(winW_, winH_, artPlateH());
    refreshStateSub();
}

int ForgeUi::artPlateH() const { return artPlateHeight(state_.art.w, state_.art.h); }

// Arte e nomes de preset do bundle publicado. O indice que o plugin toca e o
// presetindex dentro do SF2; o presets[] do manifest e outro eixo (cada item
// pode apontar um banco diferente). Casar um com o outro so vale quando as
// contas batem -- e o caso do bundle de piano unico, que e o que existe.
void ForgeUi::loadBundleArt(const std::string& path, int preset) {
    const std::string key = path + "@" + std::to_string(preset);
    if (key == artKey_) return;
    artKey_ = key;
    state_.art.clear();
    bundlePresetNames_.clear();
    if (path.empty() || !hasExt(path, ".pforge")) return;

    pianoforge::Bundle b;
    std::string err;
    if (!b.open(path, err)) {
        std::fprintf(stderr, "[PianoForge UI] %s\n", err.c_str());
        return;
    }
    for (const pianoforge::Bundle::Preset& p : b.presets())
        bundlePresetNames_.push_back(p.name);
    if (b.presets().empty()) return;

    // Presets que apontam SF2 diferentes nao sao presets: sao uma coletanea, e o
    // plugin carrega um banco so (Bundle::bankEntry pega o primeiro "sf2" do
    // manifest). Anunciar nome e arte do outro seria a tela mentindo sobre o que
    // esta soando, entao o bundle vira banco unico na tela.
    std::string bankOf;
    for (const pianoforge::Bundle::Preset& p : b.presets()) {
        if (p.sf2.empty()) continue;
        if (bankOf.empty()) bankOf = p.sf2;
        else if (bankOf != p.sf2) {
            std::fprintf(stderr, "[PianoForge UI] %s: presets com sf2 diferentes "
                         "(%s e %s); so '%s' toca, painel mostra o preset 0\n",
                         path.c_str(), bankOf.c_str(), p.sf2.c_str(), bankOf.c_str());
            bundlePresetNames_.clear();
            preset = 0;
            break;
        }
    }

    const auto& pres = b.presets();
    const pianoforge::Bundle::Preset& pick =
        pres[size_t(std::clamp(preset, 0, int(pres.size()) - 1))];
    if (pick.image.empty()) return;
    std::vector<uint8_t> blob;
    if (!b.readBlob(pick.image, blob, err)) {
        std::fprintf(stderr, "[PianoForge UI] %s (%s)\n", err.c_str(), path.c_str());
        return;
    }
    if (!decodeImage(blob, state_.art)) {
        std::fprintf(stderr, "[PianoForge UI] arte '%s' em %s nao decodifica\n",
                     pick.image.c_str(), path.c_str());
        state_.art.clear();
    }
}

// A linha miuda da identidade: `<arquivo> - <tamanho>`, como no prototipo. O
// nome do preset nao chega ao plugin por port_event de float, entao o que se
// pode dizer e o indice, o arquivo e o tamanho de verdade -- menos quando o
// bundle da um nome por preset e a conta bate com a porta 23.
void ForgeUi::refreshPresetLine() {
    state_.instrumentFile = identityLine(state_, presetFile_, bundlePresetNames_);
}

void ForgeUi::stepPreset(int delta) {
    if (state_.presetTotal <= 1) return;
    const int next = std::clamp(state_.preset + delta, 0, state_.presetTotal - 1);
    if (next == state_.preset) return;
    state_.preset = next;
    writePort(kPortPreset, float(next));
    syncSoundingName();
    needDraw_ = true;
}

// Sem canal de status o "forjando" dura o tempo que o worker leva de verdade.
// Medido: ~35 MB/s (246 MB em 6,9 s num disco HDD desta maquina).
float ForgeUi::loadEstimate(uint64_t bytes) {
    const double mb = double(bytes) / (1024.0 * 1024.0);
    return float(std::min(90.0, 2.0 + mb / 30.0));
}

// ---------------------------------------------------------------------------
// registro de soundfonts e dialogo
// ---------------------------------------------------------------------------

// Um clique em [Procurar]/[+ Pasta] so marca o pedido: quem executa e a camada
// de janela, sem o lock na mao.
void ForgeUi::requestDialog(const char* kind) {
    pendingDialog_ = kind;
    needDraw_ = true;
}

bool ForgeUi::takeDialogRequest(std::string& kind) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (pendingDialog_.empty()) return false;
    kind = pendingDialog_;
    pendingDialog_.clear();
    return true;
}

void ForgeUi::runDialogRequest(const std::string& kind) {
    const bool fileMode = (kind == "file");
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        notify(fileMode ? "escolhendo arquivos..." : "escolhendo pasta...");
        tick();
        if (needDraw_) drawLocked();       // o aviso tem de aparecer ANTES de bloquear
    }
    std::vector<std::string> files;
    std::string dir;
    if (fileMode) files = browseFiles();
    else dir = browseFolder();
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (fileMode) applyBrowse(files);
    else applyAddFolder(dir);
}

void ForgeUi::applyBrowse(const std::vector<std::string>& paths) {
    if (paths.empty()) { notify("nada escolhido"); return; }
    auto& mgr = pianoforge::SoundbankManager::getInstance();
    int first = -1;
    for (const std::string& p : paths) {
        const int idx = mgr.addSoundfont(p);
        if (first < 0) first = idx;
    }
    rescan();
    if (first >= 0) selectBank(first);
    notify(std::to_string(paths.size()) + " arquivo(s) registrado(s)");
}

// Importa .sf2/.sf3/.sfz/.pforge de UM nivel da pasta (registro de uma vez, nao
// monitoramento recursivo: foi a varredura recursiva que puxou 26 bancos).
void ForgeUi::applyAddFolder(const std::string& dir) {
    if (dir.empty()) return;
    std::error_code ec;
    std::vector<std::string> found;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        std::string ext = e.path().extension().string();
        for (auto& c : ext) c = (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
        if (ext == ".sf2" || ext == ".sf3" || ext == ".sfz" || ext == ".pforge")
            found.push_back(e.path().string());
    }
    if (found.empty()) {
        notify("nenhum .sf2/.sf3/.sfz/.pforge nessa pasta");
        return;
    }
    auto& mgr = pianoforge::SoundbankManager::getInstance();
    const int first = mgr.addMany(found);
    rescan();
    if (first >= 0) selectBank(first);
    notify(std::to_string(found.size()) + " bancos adicionados");
}

void ForgeUi::removeCurrent() {
    if (state_.current < 0) { notify("selecione um banco para remover"); return; }
    auto& mgr = pianoforge::SoundbankManager::getInstance();
    const int idx = state_.current;
    const std::string name = idx < int(state_.banks.size()) ? state_.banks[idx].name : "";
    state_.current = -1;
    state_.loading = false;
    state_.plugLoading = false;
    state_.plugFailed = false;
    writePort(kPortSoundbank, -1.0f);       // plugin: publica banco vazio
    mgr.removeAt(idx);
    rescan();
    syncSoundingName();
    notify(name + " removido da lista");
}

std::vector<std::string> ForgeUi::splitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('\n', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// fontes
// ---------------------------------------------------------------------------
// Quatro arquivos, dez corpos de letra. O bundle leva as quatro DejaVu; se um
// host instalar so o .so, cai no diretorio de fontes do SO e, sem serif/mono, o
// Fonts::load degrada para o sans em vez de sumir com o texto.
void ForgeUi::loadFonts(const std::string& bundlePath) {
    auto firstExisting = [&](const char* name) -> std::string {
        std::vector<std::string> tries = {
            bundlePath + "/" + name,
            std::string("assets/ui/") + name,
        };
#if defined(__linux__)
        tries.push_back(std::string("/usr/share/fonts/truetype/") + name);
        tries.push_back(std::string("/usr/share/fonts/") + name);
#elif defined(__APPLE__)
        tries.push_back("/System/Library/Fonts/Supplemental/DejaVuSans.ttf");
#endif
        for (const std::string& p : tries) {
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) return p;
        }
        return {};
    };
    fonts_.load(firstExisting("DejaVuSans.ttf"), firstExisting("DejaVuSans-Bold.ttf"),
                firstExisting("DejaVuSerif.ttf"), firstExisting("DejaVuSansMono.ttf"));
}

} // namespace pfui
