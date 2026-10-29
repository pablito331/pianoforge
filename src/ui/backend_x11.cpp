// PianoForge - backend X11 da UI "Forja de Pianos".
//
// Isto e o que sobra depois de o controlador portavel (src/ui/ForgeUi.{hpp,cpp})
// levar embora estado, gesto, portas e desenho: criar o window filho do host,
// apresentar a Surface ARGB, bombear eventos nativos e abrir o dialogo de
// arquivo do desktop. Tres arquivos assim (X11, Win32, Cocoa) dao o mesmo painel
// nos tres sistemas, e o que o painel faz nao pode divergir entre eles -- por
// isso nada de regra de produto mora aqui.
//
// Uma regra de HOST que continua sendo deste arquivo, e a razao de ele ter uma
// thread: o REAPER 7.83 abre o editor, entrega o container X11, faz o handshake
// XEmbed e NUNCA chama idle() nem show(). Como pintar num window cujo ancestral
// ainda nao e viewable e descartado pelo servidor X, a UI abre sua propria conexao
// e bomba os eventos dela; sem isso a janela aparece preta dentro de um DAW de
// verdade.

#include "ForgeUi.hpp"

#include "ForgeTheme.hpp"
#include "UiFactory.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/select.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace pfui;
using namespace pforge;   // Surface, forge:: (a paleta), e o que o backend toca

// O window que o host nos deu e preferencia, nao garantia; o controlador tem os
// mesmos numeros para o layout e para os hints.
constexpr int kWinW = 1000;
constexpr int kWinH = 520;

// XEmbed: sem o handshake o host cria o socket e nao exibe nada (a UI "nao
// renderiza" mesmo desenhando certo).
constexpr long kXEmbedVersion = 0;
constexpr long kXEmbedMapped = 1L << 0;
constexpr long kXEmbedEmbeddedNotify = 0;
constexpr long kXEmbedRequestFocus = 1;
constexpr long kXEmbedWindowActivate = 17;

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Roda um helper de dialogo e devolve a saida (paths). Retorna "" se nenhum.
std::string runCapture(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

bool hasFileDialogHelper() {
    const char* candidates[] = {"qarma", "zenity", nullptr};
    for (int i = 0; candidates[i]; ++i) {
        const std::string probe = "command -v ";
        if (!runCapture(probe + candidates[i] + " >/dev/null 2>&1 && echo yes").empty())
            return true;
    }
    return false;
}

// Escolhe um helper de arquivo disponivel. kdialog nao aceita --multiple sem
// --separate-output, entao so zenity/qarma oferecem multi-selecao aqui.
std::string fileDialogCommand(const std::string& title) {
    const char* candidates[] = {"qarma", "zenity", nullptr};
    for (int i = 0; candidates[i]; ++i) {
        const std::string probe = "command -v ";
        if (!runCapture(probe + candidates[i] + " >/dev/null 2>&1 && echo yes").empty()) {
            const std::string bin = candidates[i];
            if (bin == "qarma")
                return "qarma --file-select --multiple --title " + shellQuote(title) +
                       " 2>/dev/null";
            return "zenity --file-selection --multiple --separator='\\n' --title " +
                   shellQuote(title) +
                   " --file-filter='Pianos | *.sf2 *.sf3 *.sfz *.pforge' 2>/dev/null";
        }
    }
    return "";
}

std::string dirDialogCommand(const std::string& title) {
    const char* candidates[] = {"qarma", "zenity", nullptr};
    for (int i = 0; candidates[i]; ++i) {
        const std::string probe = "command -v ";
        if (!runCapture(probe + candidates[i] + " >/dev/null 2>&1 && echo yes").empty()) {
            const std::string bin = candidates[i];
            if (bin == "qarma")
                return "qarma --directory-select --title " + shellQuote(title) + " 2>/dev/null";
            return "zenity --file-selection --directory --title " +
                   shellQuote(title) + " 2>/dev/null";
        }
    }
    return "";
}

// Handler temporario de erro X: XShmAttach devolve 1 mesmo quando o servidor
// recusa o segmento (BadAccess); so um XSync com este handler revela a recusa.
int g_sawXError = 0;
int quetXError(Display*, XErrorEvent* ev) {
    g_sawXError = ev->error_code;
    return 0;
}

// Uma linha de texto por caminho: e o separator que o zenity recebe, e o que o
// qarma devolve por omissao.
std::vector<std::string> splitPaths(const std::string& s) {
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

UiKey keyFromKeysym(KeySym ks) {
    switch (ks) {
        case XK_BackSpace: return UiKey::Backspace;
        case XK_Escape:    return UiKey::Escape;
        case XK_Return:
        case XK_KP_Enter:  return UiKey::Return;
        case XK_Up:        return UiKey::Up;
        case XK_Down:      return UiKey::Down;
        case XK_Left:      return UiKey::Left;
        case XK_Right:     return UiKey::Right;
        default:           return UiKey::Text;
    }
}

class X11ForgeUi final : public ForgeUi {
public:
    X11ForgeUi(Display* dpy, Window parent, const std::string& bundlePath,
               WriteFunction writeFn, void* controller)
        : ForgeUi(bundlePath, writeFn, controller), dpy_(dpy), parent_(parent) {
        const int screen = DefaultScreen(dpy_);
        // O host pode chamar attached() sem container (janela propria, modo
        // "flutuante" do probe). Parent 0 e BadWindow em XCreateWindow: sem isto a
        // UI morre calada e o host mostra o container vazio.
        if (!parent_) parent_ = RootWindow(dpy_, screen);
        const bool embedded = (parent_ != RootWindow(dpy_, screen));

        // NADA de usar DefaultVisual/DefaultDepth aqui: a janela filha herda o
        // visual do PAI (XCreateSimpleWindow passa CopyFromParent), e um host
        // toolkitizado entrega um container TrueColor de 32 bits. Um XImage de
        // depth 24 num window de depth 32 e BadMatch em todo XPutImage -- o
        // handler do host engole o erro e a tela fica no fundo do window, que e
        // o "abriu so uma tela preta".
        //
        // O background do window e um PIXEL, nao um ARGB: num TrueColor de 24
        // bits o X usa os 24 bits de baixo, e 0xFF13161C & 0xFFFFFF = 0x13161C
        // -- que e exatamente o ferro do prototipo. Daqui ate o primeiro draw()
        // (alguns ms num host, nenhum frame em quem nao bombeia idle) e isso que
        // se ve.
        win_ = XCreateSimpleWindow(dpy_, parent_, 0, 0, kWinW, kWinH, 0,
                                   forge::iron0, forge::iron1);
        XWindowAttributes wa{};
        if (XGetWindowAttributes(dpy_, win_, &wa)) {
            visual_ = wa.visual;
            depth_ = wa.depth;
        } else {
            visual_ = DefaultVisual(dpy_, screen);
            depth_ = DefaultDepth(dpy_, screen);
        }
        opaqueBlit_ = (depth_ == 32);

        // Segunda conexao pro painel, e a razao de ele funcionar num host que nao
        // bombeia idle(). Nada de XInitThreads aqui: cada thread usa a SUA
        // conexao, entao nao ha estado X compartilhado a proteger.
        gfx_ = XOpenDisplay(nullptr);
        if (!gfx_) {
            log("XOpenDisplay da thread falhou: painel sem bomba de eventos propria");
            gfx_ = dpy_;
        }
        ownThread_ = (gfx_ != dpy_);
        hasEventThread_ = ownThread_;

        // Eventos so na conexao da thread. Na do host a mascara vai a zero: senao
        // cada clique ficaria parado na fila do REAPER, que nunca a le.
        const long kEvents = ExposureMask | StructureNotifyMask | PropertyChangeMask |
                             FocusChangeMask | ButtonPressMask | ButtonReleaseMask |
                             PointerMotionMask | KeyPressMask | VisibilityChangeMask;
        XSelectInput(dpy_, win_, 0L);
        XSelectInput(gfx_, win_, kEvents);
        wmDelete_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dpy_, win_, &wmDelete_, 1);

        XSizeHints hints{};
        hints.flags = PMinSize | PMaxSize | PBaseSize;
        hints.base_width = hints.min_width = hints.max_width = kWinW;
        hints.base_height = hints.min_height = hints.max_height = kWinH;
        XSetWMNormalHints(dpy_, win_, &hints);

        gc_ = XCreateGC(gfx_, win_, 0, nullptr);
        XSetForeground(gfx_, gc_, forge::iron0);
        XClearWindow(gfx_, win_);       // fundo = o ferro mais escuro ate o primeiro draw()
        XStoreName(dpy_, win_, "PianoForge - Forja de Pianos");

        // Backing store + Surface no tamanho que o host deu (wa.width pode ja ser
        // o do container, e o ConfigureNotify traz o definitivo).
        adoptSize(wa.width > 0 ? wa.width : kWinW, wa.height > 0 ? wa.height : kWinH);

        embedHandshake(embedded);
        if (embedded) XMapRaised(dpy_, win_);
        else XMapWindow(dpy_, win_);

        // Primeiro paint SINCRONO: um host que so bombeia idle() quando quer (ou
        // que nem conhece idleInterface) nao pode deixar o painel no fundo do
        // window. needDraw_ continua true porque o servidor descarta o que
        // pintarmos enquanto o socket do host ainda esta desmapeado -- a thread
        // redesenha no MapNotify, que e quando o conteudo comeca a valer.
        needDraw_ = true;
        serviceFrame();
        XSync(dpy_, False);

        if (ownThread_) {
            // pipe2 em vez de pipe: sem O_NONBLOCK o write() de um segundo
            // port_event com o pipe cheio pararia a thread principal do host.
            if (pipe2(wake_, O_NONBLOCK | O_CLOEXEC) != 0) {
                wake_[0] = wake_[1] = -1;
                log("pipe2 falhou");
            }
            running_.store(true, std::memory_order_release);
            pump_ = std::thread([this] { pumpLoop(); });
        }
    }

    ~X11ForgeUi() override {
        running_.store(false, std::memory_order_release);
        wakePump();
        if (pump_.joinable()) pump_.join();
        for (int fd : wake_) if (fd >= 0) close(fd);
        destroyXImage();
        if (gc_) XFreeGC(gfx_, gc_);
        if (ownThread_ && gfx_) XCloseDisplay(gfx_);
        gfx_ = nullptr;
        if (win_) XDestroyWindow(dpy_, win_);
        XCloseDisplay(dpy_);
    }

    Window window() const { return win_; }

protected:
    // ---- apresentacao ----------------------------------------------------
    // O X server pode recusar o MIT-SHM (BadAccess: outro UID, namespace ou
    // acesso negado). XShmAttach devolve 1 mesmo quando o erro chega depois, entao
    // so confiamos no SHM se um XSync com handler proprio nao acusar nada.
    bool resizeBacking(int w, int h) override {
        destroyXImage();
        makeXImage(visual_, depth_, w, h);
        return ximg_ != nullptr;
    }

    void present(const pforge::Surface& surf) override {
        if (!win_ || !ximg_) return;
        const uint32_t* src = surf.px.data();
        uint32_t* dst = reinterpret_cast<uint32_t*>(ximg_->data);
        const size_t stride = size_t(ximg_->bytes_per_line) / 4;   // pixels por linha
        const int w = surf.w, h = surf.h;   // o Surface e o tamanho do backing
        if (!loggedDraw_) {
            loggedDraw_ = true;
            std::fprintf(stderr, "[PianoForge UI] primeiro frame: window %dx%d, imagem "
                                 "%dx%d bpl=%d shm=%d opaque=%d\n", winW_, winH_,
                         ximg_->width, ximg_->height, ximg_->bytes_per_line,
                         haveShm_ ? 1 : 0, opaqueBlit_ ? 1 : 0);
        }
        // Visual de 32 bits tem canal alfa de verdade e o compositor passa a
        // usalo: as bordas antialiasadas da Surface sairiam translucidas e o fundo
        // do host apareceria atraves delas. Nas janelas de 24 bits o byte alto e
        // padding.
        if (opaqueBlit_ || stride != size_t(w)) {
            for (int y = 0; y < h; ++y) {
                uint32_t* row = dst + size_t(y) * stride;
                const uint32_t* srow = src + size_t(y) * w;
                for (int x = 0; x < w; ++x) row[x] = srow[x] | 0xFF000000u;
            }
        } else {
            // bytes_per_line, NAO stride*4: stride ja esta em pixels, e copiar
            // "stride * altura" bytes entrega um quarto do frame ao X -- a janela
            // sai pintada so ate 1/4 da altura e preta embaixo.
            std::memcpy(ximg_->data, src,
                        std::min(size_t(ximg_->bytes_per_line) * size_t(h),
                                 surf.px.size() * sizeof(uint32_t)));
        }
        verifyPutPath();
        if (haveShm_)
            XShmPutImage(gfx_, win_, gc_, ximg_, 0, 0, 0, 0, w, h, False);
        else
            XPutImage(gfx_, win_, gc_, ximg_, 0, 0, 0, 0, w, h);
        XFlush(gfx_);
    }

    // Acorda o select() da thread. Sem isto, um port_event do host so aparece no
    // painel quando o timeout de 1 s vence -- e um host que so manda status e nao
    // mexe no mouse deixa o painel atrasado um segundo por mudanca.
    void requestRedraw() override { wakePump(); }

    void focusWindow() override {
        XSetInputFocus(gfx_, win_, RevertToParent, CurrentTime);
    }

    // show()/hide() do host: a propriedade _XEMBED_INFO e o que diz ao socket se
    // o filho quer ser visto; mapear sozinho nao basta num WM que respeita o
    // handshake.
    void mapWindow() override {
        setMappedProperty(true);
        if (win_) XMapRaised(dpy_, win_);
    }
    void unmapWindow() override {
        setMappedProperty(false);
        if (win_) XUnmapWindow(dpy_, win_);
    }

    std::vector<std::string> browseFiles() override {
        const std::string cmd = fileDialogCommand("Escolha soundfonts");
        if (cmd.empty()) {
            notifyExternal("sem zenity/qarma: edite ~/.config/pianoforge/soundfonts.list");
            return {};
        }
        return splitPaths(runCapture(cmd));
    }

    std::string browseFolder() override {
        const std::string cmd = dirDialogCommand("Pasta com soundfonts");
        if (cmd.empty()) {
            notifyExternal("sem zenity/qarma: edite ~/.config/pianoforge/soundfonts.list");
            return {};
        }
        std::string out = runCapture(cmd);
        while (!out.empty() && out.back() == '/') out.pop_back();
        return out;
    }

private:
    // ---- thread de eventos ------------------------------------------------
    void pumpLoop() {
        const int xfd = ConnectionNumber(gfx_);
        const int nfds = std::max(xfd, wake_[0]) + 1;
        while (running_.load(std::memory_order_acquire)) {
            serviceFrame();
            bool anim = animating();
            int handled = 0;
            while (running_.load(std::memory_order_acquire) && XPending(gfx_) &&
                   handled < 128) {
                XEvent ev;
                XNextEvent(gfx_, &ev);
                ++handled;
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                handle(ev);
            }
            if (!running_.load(std::memory_order_acquire)) break;
            // O dialogo de arquivo roda FORA do lock: zenity pode ficar aberto
            // minutos, e com o lock tomado o port_event do host (thread principal)
            // pararia junto -- o DAW inteiro, nao so o painel.
            std::string dialog;
            if (takeDialogRequest(dialog)) {
                runDialogRequest(dialog);
                continue;
            }
            if (XPending(gfx_) || needDraw_) continue;    // ainda ha trabalho
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(xfd, &fds);
            if (wake_[0] >= 0) FD_SET(wake_[0], &fds);
            timeval tv{};
            tv.tv_sec = anim ? 0 : 1;
            tv.tv_usec = anim ? 33000 : 0;
            select(nfds, &fds, nullptr, nullptr, &tv);
            if (wake_[0] >= 0 && FD_ISSET(wake_[0], &fds)) {
                char drain[64];
                while (read(wake_[0], drain, sizeof drain) > 0) {}
            }
        }
    }

    void wakePump() {
        if (wake_[1] < 0) return;
        const char c = 1;
        ssize_t r = write(wake_[1], &c, 1);
        (void)r;
    }

    void handle(const XEvent& ev) {
        switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) needDraw_ = true;
                break;
            case ConfigureNotify:
                // Um host que redimensionou o nosso window: adotar as dimensoes
                // dele (senao blitamos 1000x520 num buraco menor e o resto fica
                // preto). A regra e do controlador, nao daqui.
                onWindowGeometry(ev.xconfigure.width, ev.xconfigure.height);
                break;
            // Os tres abaixo sao exatamente o momento em que um host resolve o
            // tamanho/empacota a janela: sem redesenhar neles, a janela fica com
            // o fundo e o usuario ve "so uma tela preta".
            case MapNotify:
            case ReparentNotify:
                needDraw_ = true;
                break;
            case VisibilityNotify:
                if (ev.xvisibility.state != VisibilityPartiallyObscured) needDraw_ = true;
                break;
            case ClientMessage: {
                if (ev.xclient.message_type == xembed_ && ev.xclient.format == 32) {
                    const long msg = ev.xclient.data.l[1];
                    if (msg == kXEmbedRequestFocus) focusWindow();
                    else if (msg == kXEmbedWindowActivate) needDraw_ = true;
                    break;
                }
                if (static_cast<Atom>(ev.xclient.data.l[0]) == wmDelete_) closed_ = true;
                break;
            }
            case MotionNotify:
                onMotion(ev.xmotion.x, ev.xmotion.y, (ev.xmotion.state & ShiftMask) != 0,
                         (ev.xmotion.state & Button1Mask) != 0);
                break;
            case ButtonPress:
                if (ev.xbutton.button >= 4 && ev.xbutton.button <= 7) {
                    // X11 entrega "roda para cima" como botao 4 (6 no smooth
                    // scroll); o controlador quer +1 = para cima em toda plataforma.
                    const int up = (ev.xbutton.button == 4 || ev.xbutton.button == 6) ? 1 : -1;
                    onWheel(ev.xbutton.x, ev.xbutton.y, up);
                    break;
                }
                onPress(ev.xbutton.x, ev.xbutton.y, double(ev.xbutton.time) / 1000.0);
                break;
            case ButtonRelease:
                if (ev.xbutton.button >= 4 && ev.xbutton.button <= 7) break;
                onRelease(ev.xbutton.x, ev.xbutton.y);
                break;
            case KeyPress: {
                // XLookupKeysym nao sabe de Shift nem de layout: "a" e "A"
                // virariam o mesmo caractere. XLookupString aplica o modifier map e
                // devolve Latin-1; os bytes >= 0x80 viram o codepoint Unicode de
                // mesmo valor (e o que Latin-1 e) e sao reencodados em UTF-8, que e
                // o que o Font decodifica. Xutf8LookupString exige um XIC vivo, e
                // abrir XIM dentro de uma janela embedada e mais superficie de
                // falha do que a busca de um filtro merece.
                char buf[32];
                KeySym ks = NoSymbol;
                const int n = XLookupString(const_cast<XKeyEvent*>(&ev.xkey), buf,
                                            int(sizeof(buf) - 1), &ks, nullptr);
                std::string txt;
                for (int i = 0; i < n; ++i) {
                    const unsigned char c = static_cast<unsigned char>(buf[i]);
                    if (c < 0x80) txt += char(c);
                    else utf8Append(txt, c);   // Latin-1 == U+0080..U+00FF
                }
                onKey(keyFromKeysym(ks), txt);
                break;
            }
            default:
                break;
        }
    }

    // ---- XEmbed ----------------------------------------------------------
    void embedHandshake(bool embedded) {
        xembed_ = XInternAtom(dpy_, "_XEMBED", False);
        xembedInfo_ = XInternAtom(dpy_, "_XEMBED_INFO", False);
        if (!embedded) return;
        setMappedProperty(true);

        XEvent ce{};
        ce.xclient.type = ClientMessage;
        ce.xclient.window = parent_;
        ce.xclient.message_type = xembed_;
        ce.xclient.format = 32;
        ce.xclient.data.l[0] = static_cast<long>(CurrentTime);
        ce.xclient.data.l[1] = kXEmbedEmbeddedNotify;
        ce.xclient.data.l[2] = static_cast<long>(win_);
        ce.xclient.data.l[3] = static_cast<long>(parent_);
        ce.xclient.data.l[4] = kXEmbedVersion;
        XSendEvent(dpy_, parent_, False, StructureNotifyMask | SubstructureNotifyMask, &ce);
    }

    void setMappedProperty(bool mapped) {
        if (!win_ || !xembedInfo_) return;
        const long info[2] = { kXEmbedVersion, mapped ? kXEmbedMapped : 0L };
        XChangeProperty(dpy_, win_, xembedInfo_, xembedInfo_, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(info), 2);
    }

    // ---- XImage / SHM ----------------------------------------------------
    template <typename F>
    int withXErrorGuard(F body) {
        XErrorHandler old = XSetErrorHandler(quetXError);
        g_sawXError = 0;
        const int r = body();
        XSync(gfx_, False);
        XSetErrorHandler(old);
        return g_sawXError ? -1 : r;
    }

    void makeXImage(Visual* visual, int depth, int w, int h) {
        if (!visual || depth < 24) return;
        if (!noShm_ && XShmQueryExtension(gfx_)) {
            XImage* candidate = XShmCreateImage(gfx_, visual, depth, ZPixmap, nullptr,
                                                &shminfo_, w, h);
            if (candidate) {
                const size_t bytes = size_t(candidate->bytes_per_line) * h;
                void* addr = (char*)(-1);
                int shmid = -1;
                const int ok = withXErrorGuard([&]() -> int {
                    // 0666: o Xorg pode rodar com outro euid e um segmento 0600
                    // devolve BadAccess no XShmAttach.
                    shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0666);
                    if (shmid < 0) return 0;
                    addr = shmat(shmid, nullptr, 0);
                    if (addr == (char*)(-1)) return 0;
                    candidate->data = static_cast<char*>(addr);
                    shminfo_.shmaddr = static_cast<char*>(addr);
                    shminfo_.readOnly = False;
                    if (!XShmAttach(gfx_, &shminfo_)) return 0;
                    return 1;
                });
                if (ok == 1) {
                    ximg_ = candidate;
                    haveShm_ = true;
                    shmId_ = shmid;
                    return;
                }
                std::fprintf(stderr, "[PianoForge UI] MIT-SHM indisponivel "
                                     "(shmget=%d attach/recusa=%d): XPutImage\n",
                             shmid, g_sawXError);
                // Nada foi publicado no server: liberta sem XDestroyImage, que
                // chamaria XFree() sobre a nossa regiao de shm.
                if (addr != (char*)(-1)) shmdt(addr);
                if (shmid >= 0) shmctl(shmid, IPC_RMID, nullptr);
                XFree(candidate);
                noShm_ = true;      // nao insistir no SHM a cada resize
            }
        }
        // Fallback: XPutImage copia os pixels a cada frame. Mais lento, mas pinta.
        char* buf = static_cast<char*>(malloc(size_t(w) * size_t(h) * 4));
        if (!buf) return;
        ximg_ = XCreateImage(gfx_, visual, depth, ZPixmap, 0, buf, w, h, 32, 0);
        if (!ximg_) std::free(buf);
    }

    // XShmAttach devolve 1 e o BadAccess chega assincrono no primeiro put. Um put
    // de teste sob handler proprio e a unica forma de saber antes de o host mostrar
    // preto: se acusou erro, jogamos o SHM fora e ficamos no XPutImage. Roda so ate
    // o primeiro put aceito; depois disso o present() vai cru.
    void verifyPutPath() {
        if (putVerified_ || repairTries_ > 2) return;
        const int code = withXErrorGuard([&]() -> int {
            if (haveShm_)
                XShmPutImage(gfx_, win_, gc_, ximg_, 0, 0, 0, 0, winW_, winH_, False);
            else
                XPutImage(gfx_, win_, gc_, ximg_, 0, 0, 0, 0, winW_, winH_);
            return 0;
        });
        if (code == 0) { putVerified_ = true; return; }

        ++repairTries_;
        std::fprintf(stderr, "[PianoForge UI] put recusado pelo X (codigo %d), tentativa %d: %s\n",
                     code, repairTries_, haveShm_ ? "refazendo sem MIT-SHM"
                                                  : "readquirindo visual/depth do window");
        if (haveShm_) {
            // O servidor recusou o anexo: nao ha XShmDetach a fazer (daria outro
            // BadShmSeg), so desfazer o mapeamento local e liberar o segmento.
            haveShm_ = false;
            noShm_ = true;
            if (shminfo_.shmaddr) { shmdt(shminfo_.shmaddr); shminfo_.shmaddr = nullptr; }
            if (shmId_ >= 0) { shmctl(shmId_, IPC_RMID, nullptr); shmId_ = -1; }
            if (ximg_) { ximg_->data = nullptr; XDestroyImage(ximg_); ximg_ = nullptr; }
            makeXImage(visual_, depth_, winW_, winH_);
            return;
        }
        // Depth/visual nao batem: o servidor e a fonte de verdade, releia.
        XWindowAttributes wa{};
        if (XGetWindowAttributes(gfx_, win_, &wa)) {
            visual_ = wa.visual;
            depth_ = wa.depth;
            opaqueBlit_ = (wa.depth == 32);
        }
        destroyXImage();
        makeXImage(visual_, depth_, winW_, winH_);
    }

    void destroyXImage() {
        if (!ximg_) return;
        if (haveShm_) {
            XShmDetach(gfx_, &shminfo_);
            if (shminfo_.shmaddr) shmdt(shminfo_.shmaddr);
            if (shmId_ >= 0) shmctl(shmId_, IPC_RMID, nullptr);
            ximg_->data = nullptr;
        }
        XDestroyImage(ximg_);
        ximg_ = nullptr;
        haveShm_ = false;
    }

    // conexao do host (criar/mapear/propriedades, sempre na thread dele) e a NOSSA
    // conexao (blit e eventos, so tocadas pela pump_).
    Display* dpy_ = nullptr;
    Display* gfx_ = nullptr;
    bool ownThread_ = false;
    std::thread pump_;
    std::atomic<bool> running_{false};
    int wake_[2] = { -1, -1 };   // pipe que acorda o select() da thread
    Window parent_ = 0;
    Window win_ = 0;
    GC gc_ = nullptr;
    Atom wmDelete_ = 0;
    Atom xembed_ = 0;
    Atom xembedInfo_ = 0;

    XImage* ximg_ = nullptr;
    XShmSegmentInfo shminfo_{};
    int shmId_ = -1;
    bool haveShm_ = false;
    bool noShm_ = false;             // o servidor recusou MIT-SHM uma vez
    Visual* visual_ = nullptr;       // do NOSSO window, herdado do pai do host
    int depth_ = 24;
    bool opaqueBlit_ = false;        // visual de 32 bits: alpha tem de ir a 0xFF
    bool putVerified_ = false;
    bool loggedDraw_ = false;
    int repairTries_ = 0;
};

} // namespace      -- fechado so para o pfui abaixo ser o GLOBAL:
                   // um `namespace pfui` dentro do anonimo viraria
                   // <anon>::pfui e nao casaria com a declaracao de UiFactory.hpp.

// ---- fabrica do painel (UiFactory.hpp) --------------------------------

namespace pfui {

ForgeUi* createUi(void* parent, const char* bundlePath, WriteFunction write,
                  void* controller, void** widgetOut) {
    if (!hasFileDialogHelper())
        std::fprintf(stderr, "[PianoForge UI] nem zenity nem qarma no PATH: os botoes "
                             "Procurar/+ Pasta vao avisar, e o registro se editando "
                             "~/.config/pianoforge/soundfonts.list\n");
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::fprintf(stderr, "[PianoForge UI] XOpenDisplay falhou (DISPLAY='%s'): "
                             "nenhuma janela sera criada\n",
                     std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "(unset)");
        return nullptr;
    }

    auto* ui = new X11ForgeUi(dpy, static_cast<Window>(reinterpret_cast<uintptr_t>(parent)),
                              bundlePath ? bundlePath : "", write, controller);
    if (!ui->window()) {
        delete ui;
        return nullptr;
    }
    if (widgetOut)
        *widgetOut = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ui->window()));
    return ui;
}

void destroyUi(ForgeUi* ui) { delete static_cast<X11ForgeUi*>(ui); }

} // namespace pfui
