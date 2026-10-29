// PianoForge - backend Win32 da UI "Forja de Pianos".
//
// E o mesmo painel do backend X11: estado, gesto, portas, registro e desenho
// moram em src/ui/ForgeUi.{hpp,cpp} e em src/core/ui/. Aqui so ha o que o SO
// manda: criar o window filho do host, apresentar a Surface ARGB, bombear
// mensagens nativas e abrir o dialogo de arquivo do Windows.
//
// Por que NAO ha thread propria, que e a decisao estrutural deste arquivo (e a
// oposta do backend X11): um window pertence a thread que o criou, e no Windows
// criar OU destruir um filho de uma janela de outra thread exige que essa outra
// thread bombeie mensagens. O host chama instantiate() na thread dele e fica parado
// ali esperando o retorno, entao a criacao travava: o probe pegou a thread da UI
// presa em NtUserCreateWindowEx -- e, depois de WS_EX_NOPARENTNOTIFY, presa em
// NtUserDestroyWindow no cleanup -- enquanto o instantiate nunca voltava para
// devolver o HWND (o widget saia NULL e o painel nao abria de jeito nenhum).
//
// Aqui tudo roda na thread que chama instantiate(), que e a dona do HWND recebido
// em ui:parent: desenho no WM_PAINT e no idle(), input direto no WndProc,
// ShowWindow/SetFocus diretos. Um host que nunca chame idle() continua atendido,
// porque o WM_PAINT chega pelo laco de mensagens dele; o que so um laco proprio
// daria (animacao) e coberto por um timer de 33 ms armado apenas enquanto ha algo
// animando e desarmado assim que o painel fica em repouso.
//
// Tres coisas que este arquivo NAO faz de proposito:
//   - nao chama SetProcessDPIAware nem trata WM_DPICHANGED. O processo e do host;
//     mudar a conscience de DPI dele por causa do nosso painel afeta todas as
//     outras janelas. Consequencia honesta: num host DPI-unaware somos escalados
//     pelo sistema junto com ele, e e o comportamento que o usuario ja ve no resto
//     do DAW.
//   - nao usa CS_DBLCLKS nem GetDoubleClickTime. O duplo clique e do painel (450 ms
//     sobre o MESMO knob, em ForgeUi::onPress) e o relogio dele aqui e
//     GetMessageTime, que da duas janelas de tempo diferentes das do Windows.
//   - nao desenha no WM_PAINT com GDI de texto: a Surface ja esta pronta e o blit e
//     o mesmo caminho de todo frame, senao teriamos dois renderizadores.

#include "ForgeUi.hpp"

#include "ForgeTheme.hpp"
#include "UiFactory.hpp"

// UNICODE/_UNICODE antes do <windows.h>: este arquivo so chama a familia W
// (CreateWindowExW, DefWindowProcW, ...), mas sem UNICODE os macros IDC_*/IDI_*
// expandem para a variante A e o LoadCursorW recusa um LPSTR. Quem ja definiu
// (host, toolchain) nao e atropelado.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <windowsx.h>       // GET_X_LPARAM / GET_Y_LPARAM
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

using namespace pfui;
using namespace pforge;   // Surface, forge:: (a paleta)

// Os mesmos numeros do design do painel; o controlador tem os mesmos para o layout
// e para os hints, e um host que der menos recebe o painel no tamanho dele.
constexpr int kWinW = 1000;
constexpr int kWinH = 520;

// Id do timer do relogio do painel. Fagulha e brasa andam a 30 Hz -- o mesmo passo
// que o laco proprio do backend X11 usa -- e so existem enquanto ha animacao.
constexpr UINT_PTR kFrameTimer = 1;

// Relogio lento do formato sem idleInterface (VST3). Ver syncTimer().
constexpr UINT_PTR kPollTimer = 2;

const wchar_t* kPanelClass = L"PianoForgeForgePanel";

// ---- UTF-16 -> UTF-8 -----------------------------------------------------
// O registro de soundfonts e UTF-8 (o mesmo arquivo nos tres SOs) e o Windows so
// fala UTF-16 nos dialogos: esta e a unica traducao do backend.

std::string toUtf8(const wchar_t* w, int n) {
    if (!w || n <= 0) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(len), '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, n, &s[0], len, nullptr, nullptr);
    return s;
}

UiKey keyFromVk(WPARAM vk) {
    switch (vk) {
        case VK_BACK:   return UiKey::Backspace;
        case VK_ESCAPE: return UiKey::Escape;
        case VK_RETURN: return UiKey::Return;
        case VK_UP:     return UiKey::Up;
        case VK_DOWN:   return UiKey::Down;
        case VK_LEFT:   return UiKey::Left;
        case VK_RIGHT:  return UiKey::Right;
        default:        return UiKey::Text;
    }
}

inline BYTE gdiByte(uint32_t argb, int shift) {
    return static_cast<BYTE>((argb >> shift) & 0xFFu);
}

} // namespace

namespace pfui {

class Win32ForgeUi final : public ForgeUi {
public:
    Win32ForgeUi(HWND parent, const std::string& bundlePath, LV2UI_Write_Function writeFn,
                 LV2UI_Controller controller)
        : ForgeUi(bundlePath, writeFn, controller), parent_(parent) {
        // Sem thread propria: a janela nasce e morre na thread que chama
        // instantiate(), que e a dona do HWND de ui:parent. Qualquer outra escolha
        // trava o host na criacao do filho (ver o comentario do topo do arquivo).
        // hasEventThread_ = false diz a ForgeUi que quem desenha e o idle()/WM_PAINT
        // desta thread, e nao um laco nosso.
        hasEventThread_ = false;
        // Os dialogos comuns e o shell namespace exigem COM nesta thread. Se o host
        // ja inicializou (o normal), a chamada devolve S_FALSE e continua sendo nossa
        // a obrigacao de balancear com CoUninitialize.
        coMine_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        createWindow();
    }

    ~Win32ForgeUi() override {
        destroyWindow();             // mesma thread que criou: caminho seguro
        if (coMine_) CoUninitialize();
    }

    HWND widget() const { return win_; }

    // O host repassou valor de porta (na thread dele, que agora e a nossa): marcar
    // frame pendente e rearmar o relogio, senao um estado que chega por fora --
    // "FORJANDO", contagem de presets -- so apareceria no proximo redesenho.
    void nudge() override {
        ForgeUi::nudge();        // needDraw_ + InvalidateRect
        syncTimer();
    }

protected:
    // ---- apresentacao ----------------------------------------------------
    // O backing store e uma DIB section: 32 bits por pixel e linhas de cima para
    // baixo (biHeight negativo), que e byte a byte o formato da Surface (0xAARRGGBB
    // em little-endian = B,G,R,x).
    bool resizeBacking(int w, int h) override {
        if (!memDc_) memDc_ = CreateCompatibleDC(nullptr);
        if (!memDc_) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP created = CreateDIBSection(memDc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!created || !bits) return false;
        // Nao se deleta um bitmap que esta selecionado num DC: primeiro entra o novo,
        // depois se solta o que NOS criamos (nunca o stock de 1x1 do DC recem-aberto,
        // por isso o prev e o nosso dib_, nao o retorno do SelectObject).
        SelectObject(memDc_, created);
        if (dib_) DeleteObject(dib_);
        dib_ = created;
        dibBits_ = static_cast<uint32_t*>(bits);
        dibW_ = w;
        dibH_ = h;
        return true;
    }

    void present(const pforge::Surface& surf) override {
        if (!win_ || !dibBits_ || !memDc_) return;
        if (!loggedDraw_) {
            loggedDraw_ = true;
            std::fprintf(stderr, "[PianoForge UI] primeiro frame Win32: janela %dx%d, "
                                 "DIB %dx%d\n", winW_, winH_, dibW_, dibH_);
        }
        // stride do DIB em PIXELS = dibW_ (32 bits nunca tem padding de linha) e o
        // memcpy come em BYTES: cols*sizeof. No X11 eu troquei um pelo outro e a
        // janela pintou so ate 1/4 da altura, preto embaixo.
        const int rows = std::min(surf.h, dibH_);
        const int cols = std::min(surf.w, dibW_);
        const size_t copyBytes = size_t(cols) * sizeof(uint32_t);
        for (int y = 0; y < rows; ++y)
            std::memcpy(dibBits_ + size_t(y) * size_t(dibW_),
                        surf.px.data() + size_t(y) * size_t(surf.w), copyBytes);
        HDC dc = GetDC(win_);
        if (dc) {
            BitBlt(dc, 0, 0, dibW_, dibH_, memDc_, 0, 0, SRCCOPY);
            ReleaseDC(win_, dc);
            GdiFlush();
        }
    }

    void requestRedraw() override {
        if (win_) InvalidateRect(win_, nullptr, FALSE);
    }

    // Tudo desta thread agora: sem AttachThreadInput, sem SetFocus atravessando
    // fila de input de outra thread.
    void focusWindow() override {
        if (win_) SetFocus(win_);
    }

    // show()/hide() do host. Antes o pedido ia por PostMessage porque ShowWindow tem
    // de rodar na dona da janela; agora a dona e esta thread, entao e chamada direta.
    void mapWindow() override {
        if (!win_) return;
        ShowWindow(win_, SW_SHOW);
        needDraw_ = true;
    }

    void unmapWindow() override {
        if (win_) ShowWindow(win_, SW_HIDE);
    }

    std::vector<std::string> browseFiles() override {
        std::vector<wchar_t> buf(32768, L'\0');
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = win_;
        ofn.lpstrFilter = L"Pianos\0*.sf2;*.sf3;*.sfz;*.pforge\0"
                          L"Todos os arquivos\0*.*\0";
        ofn.lpstrFile = buf.data();
        ofn.nMaxFile = DWORD(buf.size());
        ofn.lpstrTitle = L"Escolha soundfonts";
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                    OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
        if (!GetOpenFileNameW(&ofn)) {
            const DWORD err = CommDlgExtendedError();
            if (err)
                notifyExternal("dialogo de arquivo falhou (erro Win32 " + std::to_string(err) +
                               "); edite o registro na linha de comando");
            return {};
        }
        // Com OFN_EXPLORER + multisselecao o buffer e "pasta\0nome1\0nome2\0\0"; e se
        // a pasta tiver espaco no nome o Windows devolve cada item ja como caminho
        // completo. A regra e por token (relativo?), nao por contagem de tokens.
        std::vector<std::wstring> tok;
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) tok.emplace_back(p);
        std::vector<std::string> out;
        if (tok.empty()) return out;
        if (tok.size() == 1) return { toUtf8(tok[0].c_str(), int(tok[0].size())) };
        const std::wstring& dir = tok[0];
        for (size_t i = 1; i < tok.size(); ++i) {
            const std::wstring& item = tok[i];
            const std::wstring full =
                PathIsRelativeW(item.c_str()) ? dir + L"\\" + item : item;
            out.push_back(toUtf8(full.c_str(), int(full.size())));
        }
        return out;
    }

    std::string browseFolder() override {
        BROWSEINFOW bi{};
        bi.hwndOwner = win_;
        bi.lpszTitle = L"Pasta com soundfonts";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (!pidl) return {};
        std::wstring path(MAX_PATH * 4, L'\0');
        std::string out;
        if (SHGetPathFromIDListW(pidl, &path[0])) out = toUtf8(path.c_str(), int(path.size()));
        CoTaskMemFree(pidl);
        while (out.size() > 1 && (out.back() == '\\' || out.back() == '/')) out.pop_back();
        return out;
    }

    void log(const char* msg) override {
        std::fprintf(stderr, "[PianoForge UI] %s\n", msg);
        OutputDebugStringA("[PianoForge UI] ");
        OutputDebugStringA(msg);
        OutputDebugStringA("\n");
    }

private:
    // ---- relogio do painel -------------------------------------------------
    // Fagulha, brasa de carregamento e toast vivem por TEMPO, nao por evento. Enquanto
    // algum estiver aceso o painel precisa de frame a 30 Hz; em repouso nenhum timer
    // fica armado -- o laco de mensagens agora e o do host, e um painel parado nao
    // deve acordar a thread do DAW trinta vezes por segundo.
    void syncTimer() {
        if (!win_) return;
        // O timer de 5 Hz e o unico caminho do VST3: IPlugView nao tem idle() e
        // nenhum DAW chama nada no painel por conta propria, entao automacao e
        // estado do banco chegariam so no proximo clique. Custa um serviceFrame
        // que nao desenha quando nada mudou.
        SetTimer(win_, kPollTimer, 200, nullptr);
        if (animating()) SetTimer(win_, kFrameTimer, 33, nullptr);
        else KillTimer(win_, kFrameTimer);
    }

    // O pedido de dialogo nasce num handler de mouse/tecla e tem de ser executado
    // FORA do lock: GetOpenFileName fica minutos aberto e roda o proprio laco de
    // mensagens, e com a trava tomada todo port_event vindo do plugin pararia junto.
    // Antes quem chamava isto era o laco da thread propria; agora e o fim de cada
    // mensagem nossa, na thread do host -- que e o que um plugin Win32 faz.
    void runPendingDialog() {
        std::string kind;
        if (!takeDialogRequest(kind)) return;
        runDialogRequest(kind);
        syncTimer();
    }

    // Roda na thread que chama instantiate(), que e a dona de parent_.
    void createWindow() {
        HINSTANCE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Win32ForgeUi::panelProc), &mod);
        background_ = CreateSolidBrush(RGB(gdiByte(forge::iron1, 16), gdiByte(forge::iron1, 8),
                                           gdiByte(forge::iron1, 0)));
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        // Sem CS_PARENTDC: o painel e um DIB blitado por nos, e um DC emprestado do
        // pai traz a origem do cliente do PAI -- o painel sairia deslocado dentro do
        // container do host.
        wc.style = DWORD(CS_HREDRAW | CS_VREDRAW);
        wc.lpfnWndProc = &Win32ForgeUi::panelProc;
        wc.hInstance = mod;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = background_;
        wc.lpszClassName = kPanelClass;
        RegisterClassExW(&wc);      // ERROR_CLASS_ALREADY_EXISTS e ok: 2a instancia

        // Sem ui:parent (host "flutuante") o painel precisa de janela propria: filho
        // de janela nula nao aparece em lugar nenhum. Nesse caso kWinW/kWinH sao o
        // CLIENTE (o painel), entao a moldura entra por fora -- pedir 1000x520 de
        // janela daria um cliente menor e o layout sairia apertado.
        const bool embedded = (parent_ != nullptr);
        int windowW = kWinW, windowH = kWinH;
        if (!embedded) {
            RECT frame{0, 0, kWinW, kWinH};
            if (AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0)) {
                windowW = int(frame.right - frame.left);
                windowH = int(frame.bottom - frame.top);
            }
        }
        // Se ui:parent vier de outra thread, criar o filho daqui trava o host (o
        // Windows espera a thread dona bombear, e ela esta dentro de instantiate()).
        // Nao ha o que consertar do nosso lado: registrar para o diagnostico acabar
        // em um segundo, e nao em um DAW congelado sem pista.
        if (embedded && GetWindowThreadProcessId(parent_, nullptr) != GetCurrentThreadId())
            log("ui:parent pertence a outra thread: a criacao do painel pode travar");
        win_ = CreateWindowExW(0, kPanelClass, L"PianoForge - Forja de Pianos",
                               embedded ? DWORD(WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN)
                                        : DWORD(WS_OVERLAPPEDWINDOW),
                               0, 0, windowW, windowH, embedded ? parent_ : nullptr,
                               nullptr, mod, this);
        if (!win_) {
            log("CreateWindowExW falhou: nenhuma janela sera criada");
            return;
        }

        // Adotar o tamanho que o container do host ja tem ANTES de mostrar: pintamos
        // uma vez, e so entao a janela aparece. Mostrar primeiro daria um flick de
        // fundo de classe, e pintar depois do ShowWindow no laco valeria o mesmo.
        RECT rc{};
        if (embedded && GetClientRect(parent_, &rc) && rc.right > 0 && rc.bottom > 0)
            adoptSize(int(rc.right), int(rc.bottom));
        else
            adoptSize(kWinW, kWinH);
        serviceFrame();
        ShowWindow(win_, embedded ? SW_SHOW : SW_SHOWNORMAL);
        needDraw_ = true;
        syncTimer();
    }

    // Roda na thread que criou tudo: janela, DIB e DC.
    void destroyWindow() {
        if (win_) {
            KillTimer(win_, kFrameTimer);
            KillTimer(win_, kPollTimer);
            DestroyWindow(win_);
        }
        win_ = nullptr;
        if (memDc_) DeleteDC(memDc_);
        memDc_ = nullptr;
        if (dib_) DeleteObject(dib_);
        dib_ = nullptr;
        dibBits_ = nullptr;
        if (background_) DeleteObject(background_);
        background_ = nullptr;
    }

    static LRESULT CALLBACK panelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) {
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(
                                  reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
            return DefWindowProcW(h, m, w, l);
        }
        auto* self = reinterpret_cast<Win32ForgeUi*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
    }

    // Todo handler que mexe estado toma a trava: o port_event do host chega pela
    // mesma thread, mas o lock continua aqui porque write_() pode voltar em
    // port_event no meio do gesto (trava recursiva) e porque uma segunda instancia
    // do painel no mesmo processo tem a propria.
    LRESULT handle(HWND h, UINT m, WPARAM w, LPARAM l) {
        const LRESULT r = handleOne(h, m, w, l);
        // Dialogo no fim da mensagem, nunca dentro do switch: runDialogRequest abre
        // janela modal com laco de mensagens proprio.
        runPendingDialog();
        return r;
    }

    LRESULT handleOne(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
            case WM_SIZE: {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                onWindowGeometry(LOWORD(l), HIWORD(l));
                syncTimer();
                return 0;
            }
            case WM_TIMER: {
                if (w == kPollTimer) {
                    serviceFrame();      // 5 Hz: automacao/estado do VST3
                    return 0;
                }
                if (w != kFrameTimer) return DefWindowProcW(h, m, w, l);
                serviceFrame();
                syncTimer();
                return 0;
            }
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(h, &ps);
                needDraw_ = true;
                serviceFrame();
                EndPaint(h, &ps);
                syncTimer();
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;            // pintamos o cliente inteiro: apagar e flicker
            case WM_SHOWWINDOW: {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                onVisibilityChanged();
                return 0;
            }
            case WM_LBUTTONDBLCLK:    // cai no mesmo caminho: quem decide o par e o painel
            case WM_LBUTTONDOWN: {
                SetFocus(h);
                SetCapture(h);        // arrastar continua saindo do painel
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                onPress(GET_X_LPARAM(l), GET_Y_LPARAM(l), double(GetMessageTime()) / 1000.0);
                return 0;
            }
            case WM_LBUTTONUP: {
                ReleaseCapture();
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                onRelease(GET_X_LPARAM(l), GET_Y_LPARAM(l));
                return 0;
            }
            case WM_MOUSEMOVE: {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                onMotion(GET_X_LPARAM(l), GET_Y_LPARAM(l), (w & MK_SHIFT) != 0,
                         (w & MK_LBUTTON) != 0);
                return 0;
            }
            case WM_MOUSEWHEEL: {
                // O lParam da roda vem em coordenadas de TELA; o painel so conhece as
                // suas. Um "dente" sao 120 unidades e o sinal da a direcao -- o
                // controlador quer +1 = para cima em toda plataforma, como no X11.
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ScreenToClient(h, &pt);
                const int delta = GET_WHEEL_DELTA_WPARAM(w);
                if (!delta) return 0;
                const int up = delta > 0 ? 1 : -1;
                int notches = std::max(1, int(std::abs(delta)) / WHEEL_DELTA);
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                while (notches-- > 0) onWheel(pt.x, pt.y, up);
                return 0;
            }
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: {
                const UiKey k = keyFromVk(w);
                if (k == UiKey::Text) return 0;   // texto e o WM_CHAR que vem depois
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                onKey(k, std::string());
                return 0;
            }
            case WM_CHAR: {
                const wchar_t ch = wchar_t(w);
                // Controle nao e texto: Backspace (0x08), Enter (0x0D) e Escape (0x1B)
                // chegam aqui depois de tratados no WM_KEYDOWN, e o que nao e grafico
                // nao pode virar caractere de filtro.
                if (ch < 0x20 || ch == 0x7F) return 0;
                if (ch >= 0xD800 && ch <= 0xDBFF) { pendingSurrogate_ = ch; return 0; }
                unsigned cp = ch;
                if (ch >= 0xDC00 && ch <= 0xDFFF) {
                    if (!pendingSurrogate_) return 0;
                    cp = 0x10000u + ((unsigned(pendingSurrogate_) - 0xD800u) << 10) +
                         (unsigned(ch) - 0xDC00u);
                    pendingSurrogate_ = 0;
                }
                std::string txt;
                utf8Append(txt, cp);
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                onKey(UiKey::Text, txt);
                return 0;
            }
            case WM_CLOSE: {
                // Fechar a janela flutuante nao pode matar o processo: o host ainda vai
                // chamar cleanup(). closed_ e o sinal para o idle() do host devolver 1.
                closed_ = true;
                KillTimer(h, kFrameTimer);
                DestroyWindow(h);
                win_ = nullptr;              // nao blitar mais num HWND morto
                return 0;
            }
            case WM_DESTROY:
                // NUNCA PostQuitMessage aqui: esta fila agora e a do HOST, e um
                // WM_QUIT encerraria o laco de mensagens do DAW inteiro.
                return 0;
            default:
                return DefWindowProcW(h, m, w, l);
        }
    }

    HWND parent_ = nullptr;
    HWND win_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    uint32_t* dibBits_ = nullptr;
    int dibW_ = 0, dibH_ = 0;
    HBRUSH background_ = nullptr;
    wchar_t pendingSurrogate_ = 0;
    bool coMine_ = false;        // o CoInitializeEx desta thread foi nosso
    bool loggedDraw_ = false;
};

} // namespace pfui

// ---- fabrica host-agnostica (UiFactory.hpp) ----------------------------
// O LV2 e o VST3 entram por aqui: um caminho so para o painel existir.

namespace pfui {

ForgeUi* createUi(void* parent, const char* bundlePath, LV2UI_Write_Function write,
                  LV2UI_Controller controller, void** widgetOut) {
    auto* ui = new Win32ForgeUi(reinterpret_cast<HWND>(parent), bundlePath ? bundlePath : "",
                                write, controller);
    if (!ui->widget()) {
        // Devolver um objeto sem HWND daria ao host uma area vazia para embutir e
        // um cleanup que passa pelo DestroyWindow de uma janela que nao existe.
        delete ui;
        return nullptr;
    }
    if (widgetOut)
        *widgetOut = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ui->widget()));
    return ui;
}

void destroyUi(ForgeUi* ui) { delete static_cast<Win32ForgeUi*>(ui); }

} // namespace pfui

// ---- LV2 UI descriptor -------------------------------------------------

namespace {

pfui::ForgeUi* uiFromHandle(LV2UI_Handle handle) {
    return static_cast<pfui::ForgeUi*>(handle);
}

LV2UI_Handle uiInstantiate(const LV2UI_Descriptor*,
                           const char*,
                           const char* bundlePath,
                           LV2UI_Write_Function writeFunction,
                           LV2UI_Controller controller,
                           LV2UI_Widget* widget,
                           const LV2_Feature* const* features) {
    HWND parent = nullptr;
    const LV2UI_Resize* resize = nullptr;
    for (int i = 0; features && features[i]; ++i) {
        if (!std::strcmp(features[i]->URI, LV2_UI__parent) && features[i]->data) {
            // ui.ttl, classe ui:WindowsUI: "the LV2_Widget is not a pointer to a HWND
            // but should be interpreted as an HWND itself" -- e o mesmo valor que o
            // host entrega em ui:parent.
            parent = reinterpret_cast<HWND>(reinterpret_cast<uintptr_t>(features[i]->data));
        } else if (!std::strcmp(features[i]->URI, LV2_UI__resize) && features[i]->data) {
            resize = static_cast<const LV2UI_Resize*>(features[i]->data);
        }
    }

    void* native = nullptr;
    pfui::ForgeUi* ui = pfui::createUi(parent, bundlePath, writeFunction, controller, &native);
    if (!ui) return nullptr;
    *widget = reinterpret_cast<LV2UI_Widget>(reinterpret_cast<uintptr_t>(native));
    // Sem pedir o tamanho, um host que hospeda a UI numa aba nao tem como saber
    // 1000x520 e deixa o container do tamanho dele.
    if (resize && resize->ui_resize)
        resize->ui_resize(resize->handle, pfui::ForgeUi::preferredWidth(),
                          pfui::ForgeUi::preferredHeight());
    return ui;
}

void uiCleanup(LV2UI_Handle handle) { pfui::destroyUi(uiFromHandle(handle)); }

// A assinatura do typedef e (handle, port_index, BUFFER_SIZE, FORMAT, buffer)
// -- third_party/lv2/lv2/include/lv2/ui/ui.h:200. Os tres primeiros sao inteiros,
// entao nomear ao contrario compila igual e faz o guarda `format != 0` ver o tamanho
// do buffer (4): todo port_event do host seria descartado em silencio. Conferir
// ordem no header, nunca na memoria.
void uiPortEvent(LV2UI_Handle handle, uint32_t port, uint32_t bufferSize, uint32_t format,
                 const void* buffer) {
    auto* ui = uiFromHandle(handle);
    ui->portEvent(port, bufferSize, format, buffer);
    // Sem thread propria, ForgeUi::portEvent so registra o estado -- quem acorda o
    // desenho e a janela. Sem este empurrao, um host que nunca chama idle() (o
    // REAPER nunca chama) mostraria "FORJANDO" e a contagem de presets so no proximo
    // clique, porque nada invalidou a area.
    ui->nudge();
}

int uiIdle(LV2UI_Handle handle) { return uiFromHandle(handle)->idle(); }
int uiShow(LV2UI_Handle handle) { return uiFromHandle(handle)->show(); }
int uiHide(LV2UI_Handle handle) { return uiFromHandle(handle)->hide(); }

const LV2UI_Idle_Interface kIdleInterface = { uiIdle };
const LV2UI_Show_Interface kShowInterface = { uiShow, uiHide };

const void* uiExtensionData(const char* uri) {
    if (!std::strcmp(uri, LV2_UI__idleInterface)) return &kIdleInterface;
    if (!std::strcmp(uri, LV2_UI__showInterface)) return &kShowInterface;
    return nullptr;
}

const LV2UI_Descriptor kDescriptor = {
    "https://pianoforge.audio/plugin#ui",
    uiInstantiate,
    uiCleanup,
    uiPortEvent,
    uiExtensionData,
};

} // namespace

LV2_SYMBOL_EXPORT
const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index) {
    return (index == 0) ? &kDescriptor : nullptr;
}
