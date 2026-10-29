#pragma once
// Controlador portavel da UI "Forja de Pianos".
//
// Ele e tudo o que nao e janela: o PanelState, o hit-test, os gestos (clique,
// arrasto, roda, tecla), o protocolo das portas LV2, o registro de soundfonts e
// o render do painel numa Surface ARGB. O que sobra para a camada de plataforma
// e pouco: criar/adoptar o window filho, traduzir eventos nativos em chamadas
// daqui, apresentar a Surface e abrir o dialogo de arquivo nativo.
//
// Por que isso existe: o desenho ja era compartilhado com o pforge_ui_preview
// (src/core/ui/), mas a janela era X11 puro e nao havia como levar o painel a
// Win32/Cocoa sem arrastar Xlib junto. A divisao e a mesma do preview: quem
// conhece o SO fica num arquivo pequeno.
//
// Trava: mtx_ e recursivo porque write_() pode voltar em port_event na mesma
// thread, com o lock ja tomado. Toda entrada publica que mexe estado trava; os
// hooks protegidos assumem o lock JA tomado pela camada (e ela o toma fora dele
// quando chama um dialogo bloqueante).

#include "ForgePanel.hpp"

#include <lv2/ui/ui.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace pfui {

// Teclas que o painel entende, nos tres SOs. Um backend traduz o seu codigo
// nativo (X keysym, VK_*, keycode do Cocoa) para ca; o que nao estiver aqui
// chega como UiKey::Text com o texto UTF-8 correspondente, se houver.
enum class UiKey { Text, Backspace, Escape, Return, Up, Down, Left, Right };

class ForgeUi {
public:
    ForgeUi(const std::string& bundlePath, LV2UI_Write_Function writeFn,
            LV2UI_Controller controller);
    virtual ~ForgeUi();

    static int preferredWidth();
    static int preferredHeight();

    // ------------------------------------------------------------------
    // Chamadas do host LV2
    // ------------------------------------------------------------------
    // port_event vem da thread principal do host e NUNCA desenha: pintar de
    // aqui seria um blit numa conexao que nao e da thread da UI.
    void portEvent(uint32_t port, uint32_t bufferSize, uint32_t format, const void* buffer);
    int idle();
    int show();
    int hide();
    bool closed() const { return closed_; }

    // ------------------------------------------------------------------
    // Chamadas da camada de janela (ja traduzidas)
    // ------------------------------------------------------------------
    // Tamanho real do window que o host nos deu. kWinW/kWinH sao preferencia,
    // nao garantia: um host que ignora os hints recebe o painel refeito.
    void adoptSize(int w, int h);
    // O evento de "o host mexeu no meu tamanho". Vive aqui e nao em cada backend
    // porque os tres precisam da mesma regra: redesenharsempre, e logar so quando
    // a dimensao muda de fato (um WM manda ConfigureNotify a cada passo de drag).
    void onWindowGeometry(int w, int h);
    void onMotion(int x, int y, bool shift, bool dragging);
    void onPress(int x, int y, double timeSec);
    void onRelease(int x, int y);
    void onWheel(int x, int y, int up);      // +1 = roda para cima
    void onKey(UiKey key, const std::string& utf8);
    void onVisibilityChanged() { needDraw_ = true; }

    // Um frame, se algo mudou. E o que a thread de eventos chama antes de dormir.
    void serviceFrame();
    // Estado novo que chegou por fora (port_event): marcar frame pendente e dar o
    // empurrao que o backend souber dar. Sem thread propria o empurrao e invalidar
    // a janela; com thread, e acordar o laco.
    virtual void nudge() {
        needDraw_ = true;
        requestRedraw();
    }
    // Gancho para formato de plugin SEM idleInterface: o VST3 nao tem a chamada
    // periodica do LV2 (ui:idleInterface), entao automacao do host e estado do
    // banco nao tem por onde entrar no painel. Quem tem o que empurrar instala
    // aqui, e tick() chama ~6x por segundo, na thread da UI.
    std::function<void()> onHostPoll;
    // true quando ha animacao viva (fagulhas, brasa de load, toast): a camada usa
    // isso para escolher o intervalo do proximo frame.
    bool animating();
    // Pedido de dialogo feito por um clique. Rodar popen/NSOpenPanel com o lock
    // tomado pararia o host inteiro, entao quem executa e a camada, sem lock.
    bool takeDialogRequest(std::string& kind);
    void runDialogRequest(const std::string& kind);

    // ------------------------------------------------------------------
    // Hooks da plataforma
    // ------------------------------------------------------------------
protected:
    // Backing store do tamanho dado (XImage+SHM, DIB secao, CGContext).
    virtual bool resizeBacking(int w, int h) = 0;
    // Copia a Surface para a tela. Chamada com mtx_ tomado.
    virtual void present(const pforge::Surface& surf) = 0;
    // Acorda a thread de eventos (pipe no X11, PostMessage no Win32,
    // dispatch_async no Cocoa). Padrao: nada, quem nao tem thread desenha em idle().
    virtual void requestRedraw() {}
    // Foco de teclado (a busca nao funciona sem pedir ao host/window).
    virtual void focusWindow() {}
    // show()/hide() do host. Mapear o window e da plataforma; o que o painel faz
    // com isso (armar o redraw, dar o primeiro frame) esta em ForgeUi::show.
    virtual void mapWindow() {}
    virtual void unmapWindow() {}
    // Dialogos nativos. browseFiles devolve os caminhos escolhidos (vazio = nada
    // escolhido); browseFolder devolve a pasta ou "".
    virtual std::vector<std::string> browseFiles() { return {}; }
    virtual std::string browseFolder() { return {}; }
    // Mensagem para o stderr da plataforma (o X11 usa para logar o fallback de SHM).
    virtual void log(const char* msg);

    void drawLocked();                 // layout + drawPanel + present
    void wakeOrDraw();                 // requestRedraw(), ou desenha se nao ha thread
    // Linha curta de feedback no toast. Protegida porque o backend tambem tem o
    // que dizer ("nao ha zenity/qarma neste sistema"), e o rodape nao e lugar de
    // aviso: ele diz o que esta SOANDO.
    void notify(const std::string& msg);
    // Aviso de fora do gesto (o backend descobriu que nao ha dialog de arquivo no
    // sistema). Toma o lock por conta propria: quem chama pode estar entre dois
    // trechos criticos, como no corpo de um browseFiles() bloqueante.
    void notifyExternal(const std::string& msg);

    std::recursive_mutex mtx_;
    pforge::Surface surf_;
    pforge::Fonts fonts_;
    pforge::PanelState state_;
    pforge::Layout layout_;
    int winW_ = 0, winH_ = 0;

    LV2UI_Write_Function write_ = nullptr;
    LV2UI_Controller controller_ = nullptr;
    std::atomic<bool> needDraw_{true};
    bool closed_ = false;
    bool hasEventThread_ = false;      // false: o idle() do host e o nosso pump

private:
    // ---- estado do painel ----
    void applyPortEvent(uint32_t port, uint32_t bufferSize, uint32_t format,
                        const void* buffer);
    void applyBankStatus(float code);
    void rebuildHiddenChips();
    void noteHiddenValue(int pid, float v);
    void refreshStateSub();
    void requestDialog(const char* kind);
    void tick();

    // ---- banco ----
    void rescan();
    void selectBank(int index);
    void noteBankRequested(int index);
    void chooseFirstBank();
    void removeCurrent();
    void syncSoundingName();
    void loadBundleArt(const std::string& path, int preset);
    void refreshPresetLine();
    int artPlateH() const;
    std::string engineNameOf(const std::string& path) const;

    // ---- gesto ----
    void updateHover(int x, int y);
    void stepPreset(int delta);
    void popFilterChar();
    void appendFilterText(const std::string& text);
    void clampScroll();
    void writePort(uint32_t port, float value);

    // ---- importacao (o dialogo nativo roda sem lock; a aplica vem depois) ----
    void applyBrowse(const std::vector<std::string>& paths);
    void applyAddFolder(const std::string& dir);

    static std::string bundleDisplayName(const std::string& path, const std::string& fallback);
    static bool hasExt(const std::string& path, const std::string& ext);
    static std::string fileNameOf(const std::string& path);
    static std::vector<std::string> splitLines(const std::string& s);
    static std::vector<unsigned> utf8DecodeChars(const std::string& text);
    static float loadEstimate(uint64_t bytes);
    void loadFonts(const std::string& bundlePath);

    using Clock = std::chrono::steady_clock;
    double elapsedSinceTouch() const { return elapsed(Clock::now() - touchClock_); }
    template <typename D> static double elapsed(D d) {
        return std::chrono::duration<double>(d).count();
    }

    std::string bundlePath_;
    float hiddenValue_[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    bool hiddenLive_[7] = {false, false, false, false, false, false, false};
    Clock::time_point toastClock_ = Clock::now();
    Clock::time_point touchClock_ = Clock::now();
    // Ultimo chute no onHostPoll. E por TEMPO, e nao por frame: um laco a 60 Hz nao
    // deve perguntar ao plugin sessenta vezes por segundo se a automacao mudou.
    Clock::time_point pollClock_ = Clock::now();
    // Momento do ultimo clique, em segundos de `onPress`. E o backend que traz o
    // relogio (X usa timestamp do evento, Win32 GetDoubleClickTime nao serve ao
    // ajuste fino), por isso e um double e nao um time_point.
    double lastPressSec_ = 0.0;
    int lastPressKnob_ = -1;
    int dragKnob_ = -1;
    int dragStartY_ = 0;
    float dragStartVal_ = 0.0f;
    float loadSeconds_ = 3.0f;
    std::string pendingDialog_;        // "file"/"dir"
    std::string presetFile_;           // arquivo do banco soando (linha do rodape)
    std::string artKey_;               // path@preset ja lido, pra nao reabrir o bundle
    std::vector<std::string> bundlePresetNames_;
    bool loggedIdle_ = false;
    int cfgW_ = 0, cfgH_ = 0;          // ultimo tamanho visto, pra nao logar o mesmo duas vezes
};

// Codepoint -> UTF-8. O teclado nativo entrega um caractere por evento (Latin-1
// no X, UTF-16 no Cocoa, wchar no Win32) e o rasterizador quer UTF-8: sem essa
// ponte um "cedilha" desenharia dois glifos errados em vez de um certo.
void utf8Append(std::string& out, unsigned cp);

} // namespace pfui
