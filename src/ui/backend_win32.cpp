// PianoForge - backend Win32 da UI "Forja de Pianos".
//
// E o mesmo painel do backend X11: estado, gesto, portas, registro e desenho
// moram em src/ui/ForgeUi.{hpp,cpp} e em src/core/ui/. Aqui so ha o que o SO
// manda: criar o window filho do host, apresentar a Surface ARGB, bombear
// mensagens nativas e abrir o dialogo de arquivo do Windows.
//
// Por que uma thread propria, que e a decisao estrutural deste arquivo: no X11
// descobrimos que o REAPER nunca chama idle() nem show(), e pintar num window sem
// ancestral viewable e descartado. No Win32 a mesma regra vem do outro lado -- quem
// precisa da thread e o Windows: um window so recebe e responde mensagens na thread
// que o criou, e SetFocus/ShowWindow/GDI de uma thread estranha ou falam com a fila
// errada ou derrubam o processo. Entao a thread do host faz instantiate(), espera o
// window nascer e recebe o HWND; daqui em diante tudo o que toca a janela acontece
// na thread da UI, e o que a thread do host quer pedir (redesenhar, mapear) chega
// por PostMessage.
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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <windowsx.h>       // GET_X_LPARAM / GET_Y_LPARAM
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace pfui;
using namespace pforge;   // Surface, forge:: (a paleta)

// Os mesmos numeros do design do painel; o controlador tem os mesmos para o layout
// e para os hints, e um host que der menos recebe o painel no tamanho dele.
constexpr int kWinW = 1000;
constexpr int kWinH = 520;

// Mensagens de controle entre a thread do host e a thread da janela. PostMessage e
// a unica travessia segura entre as duas.
constexpr UINT kMsgWake = WM_APP + 1;    // ha frame novo pendente
constexpr UINT kMsgMap = WM_APP + 2;     // show() do host
constexpr UINT kMsgUnmap = WM_APP + 3;   // hide() do host

const wchar_t* kPanelClass = L"PianoForgeForgePanel";
const wchar_t* kWakeClass = L"PianoForgeForgeWake";

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
        // Ha bomba de eventos propria: idle() do host nao desenha, e todo pedido de
        // redraw vira PostMessage. Deixar isso falso faria o host desenhar na thread
        // dele, que nao e a dona do window.
        hasEventThread_ = true;
        readyEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        running_.store(true, std::memory_order_release);
        pump_ = std::thread([this] { pumpLoop(); });
        // instantiate() tem de devolver o HWND no mesmo retorno: e o que o host da ao
        // Windows como filho do container dele. A espera e o unico ponto em que as
        // duas threads se encontram e ela nao segura trava nenhuma; a pump_ aponta o
        // evento mesmo se a criacao da janela falhar, entao no pior caso o custo e um
        // instantiate que devolve nullptr na hora.
        if (readyEv_) WaitForSingleObject(readyEv_, 5000);
    }

    ~Win32ForgeUi() override {
        running_.store(false, std::memory_order_release);
        if (wake_) PostMessageW(wake_, kMsgWake, 0, 0);
        if (pump_.joinable()) pump_.join();   // a pump_ destroi janela, DIB e DC
        if (readyEv_) CloseHandle(readyEv_);
    }

    HWND widget() const { return win_; }

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

    void requestRedraw() override { postCtrl(kMsgWake); }

    // O window e filho do container do host, que esta em OUTRA thread. SetFocus nao
    // atravessa para o foreground de quem nao e a nossa thread -- a troca de foco e
    // por input queue, entao as duas filas sao coladas por um instante.
    void focusWindow() override {
        if (!win_) return;
        HWND fg = GetForegroundWindow();
        const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        const DWORD myTid = GetCurrentThreadId();
        const bool attach = fgTid != 0 && fgTid != myTid;
        if (attach) AttachThreadInput(fgTid, myTid, TRUE);
        SetFocus(win_);
        if (attach) AttachThreadInput(fgTid, myTid, FALSE);
    }

    // show()/hide() chegam da thread do host; ShowWindow tem de rodar na dona da
    // janela, por isso o pedido e postado e nao chamado. O guarda nao e enfeite:
    // PostMessage com hwnd NULL nao e "nenhum destinatario", e uma mensagem de
    // THREAD entregue a fila de quem chamou -- aqui, a do host, que nao tem laço
    // nenhum para consumi-la.
    void mapWindow() override { postCtrl(kMsgMap); }
    void unmapWindow() override { postCtrl(kMsgUnmap); }

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
    // Traversal host -> thread da janela. wake_ pode estar nulo (janela criada antes
    // do message-only window, ou criacao falhou); PostMessage com hwnd NULL seria uma
    // mensagem de THREAD na fila do proprio chamador.
    void postCtrl(UINT m) {
        if (wake_) PostMessageW(wake_, m, 0, 0);
    }

    // ---- thread da janela -------------------------------------------------
    // Mesma forma do pumpLoop() do X11: um frame, depois as mensagens, depois o
    // dialogo (FORA do lock -- GetOpenFileName pode ficar minutos aberto, e com a
    // trava tomada o port_event da thread principal do host pararia junto, ou seja,
    // o DAW inteiro), depois dormir. Dormir aqui e MsgWait, que acorda tanto com
    // mensagem nova quanto pelo timeout do relogio do painel.
    void pumpLoop() {
        // Os dialogos comuns e o shell namespace exigem coinit nesta thread; cada
        // thread tem o seu, e o do host nao e tocado.
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool coMine = SUCCEEDED(hr);

        createWindow();
        SetEvent(readyEv_);

        MSG msg;
        bool more = true;
        while (running_.load(std::memory_order_acquire) && more) {
            serviceFrame();
            int handled = 0;
            while (running_.load(std::memory_order_acquire) &&
                   PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { more = false; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (++handled >= 256) break;   // rajada de mensagem nao trava o frame
            }
            if (!running_.load(std::memory_order_acquire) || !more) break;
            std::string dialog;
            if (takeDialogRequest(dialog)) {
                runDialogRequest(dialog);
                continue;
            }
            if (needDraw_) continue;
            const bool anim = animating();
            MsgWaitForMultipleObjectsEx(0, nullptr, anim ? 33 : 1000, QS_ALLINPUT,
                                        MWMO_INPUTAVAILABLE);
        }
        destroyWindow();
        if (coMine) CoUninitialize();
    }

    void createWindow() {
        HINSTANCE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Win32ForgeUi::panelProc), &mod);
        background_ = CreateSolidBrush(RGB(gdiByte(forge::iron1, 16), gdiByte(forge::iron1, 8),
                                           gdiByte(forge::iron1, 0)));
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = DWORD(CS_PARENTDC | CS_HREDRAW | CS_VREDRAW);
        wc.lpfnWndProc = &Win32ForgeUi::panelProc;
        wc.hInstance = mod;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = background_;
        wc.lpszClassName = kPanelClass;
        RegisterClassExW(&wc);      // ERROR_CLASS_ALREADY_EXISTS e ok: 2a instancia

        WNDCLASSEXW wk{};
        wk.cbSize = sizeof(wk);
        wk.lpfnWndProc = &Win32ForgeUi::wakeProc;
        wk.hInstance = mod;
        wk.lpszClassName = kWakeClass;
        RegisterClassExW(&wk);

        // Sem ui:parent (host "flutuante") o painel precisa de janela propria: filho
        // de janela nula nao aparece em lugar nenhum.
        const bool embedded = (parent_ != nullptr);
        win_ = CreateWindowExW(
            0, kPanelClass, L"PianoForge - Forja de Pianos",
            embedded ? DWORD(WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN)
                     : DWORD(WS_OVERLAPPEDWINDOW),
            0, 0, kWinW, kWinH, embedded ? parent_ : nullptr, nullptr, mod, this);
        if (!win_) {
            log("CreateWindowExW falhou: nenhuma janela sera criada");
            return;
        }
        wake_ = CreateWindowExW(0, kWakeClass, L"PianoForgeWake", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, mod, this);

        // Adotar o tamanho que o container do host ja tem ANTES de mostrar: pintamos
        // uma vez, e so entao a janela aparece. Mostrar primeiro daria um flick de
        // fundo de classe, e pintar depois do ShowWindow no laço valeria o mesmo.
        RECT rc{};
        if (embedded && GetClientRect(parent_, &rc) && rc.right > 0 && rc.bottom > 0)
            adoptSize(int(rc.right), int(rc.bottom));
        else
            adoptSize(kWinW, kWinH);
        serviceFrame();
        ShowWindow(win_, embedded ? SW_SHOW : SW_SHOWNORMAL);
        needDraw_ = true;
    }

    // Roda na thread que criou tudo: janela, DIB e DC.
    void destroyWindow() {
        if (win_) DestroyWindow(win_);
        win_ = nullptr;
        wake_ = nullptr;            // destruida junto com a thread (HWND_MESSAGE)
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

    static LRESULT CALLBACK wakeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) {
            SetWindowLongPtrW(h, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(
                                  reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
            return DefWindowProcW(h, m, w, l);
        }
        auto* self = reinterpret_cast<Win32ForgeUi*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, m, w, l);
        switch (m) {
            case kMsgWake:
                self->needDraw_ = true;
                return 0;
            case kMsgMap:
                if (self->win_) { ShowWindow(self->win_, SW_SHOW); self->needDraw_ = true; }
                return 0;
            case kMsgUnmap:
                if (self->win_) ShowWindow(self->win_, SW_HIDE);
                return 0;
            default:
                return DefWindowProcW(h, m, w, l);
        }
    }

    // Todo handler que mexe estado toma a trava: o port_event do host roda em outra
    // thread com a MESMA trava (recursiva, porque write_() pode voltar em
    // port_event), e o desenho e sempre aqui.
    LRESULT handle(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
            case WM_SIZE: {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                onWindowGeometry(LOWORD(l), HIWORD(l));
                return 0;
            }
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(h, &ps);
                needDraw_ = true;
                serviceFrame();
                EndPaint(h, &ps);
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
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                closed_ = true;              // idle() devolve 1 e o host desmonta a UI
                DestroyWindow(h);
                win_ = nullptr;              // nao blitar mais num HWND morto
                return 0;
            }
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(h, m, w, l);
        }
    }

    HWND parent_ = nullptr;
    HWND win_ = nullptr;
    HWND wake_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP dib_ = nullptr;
    uint32_t* dibBits_ = nullptr;
    int dibW_ = 0, dibH_ = 0;
    HBRUSH background_ = nullptr;
    wchar_t pendingSurrogate_ = 0;
    HANDLE readyEv_ = nullptr;
    std::thread pump_;
    std::atomic<bool> running_{false};
    bool loggedDraw_ = false;
};

} // namespace pfui

// ---- LV2 UI descriptor -------------------------------------------------

namespace {

pfui::Win32ForgeUi* uiFromHandle(LV2UI_Handle handle) {
    return static_cast<pfui::Win32ForgeUi*>(handle);
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

    auto* ui = new pfui::Win32ForgeUi(parent, bundlePath ? bundlePath : "", writeFunction,
                                      controller);
    if (!ui->widget()) {
        delete ui;
        return nullptr;
    }
    *widget = reinterpret_cast<LV2UI_Widget>(reinterpret_cast<uintptr_t>(ui->widget()));
    // Sem pedir o tamanho, um host que hospeda a UI numa aba nao tem como saber
    // 1000x520 e deixa o container do tamanho dele.
    if (resize && resize->ui_resize)
        resize->ui_resize(resize->handle, pfui::Win32ForgeUi::preferredWidth(),
                          pfui::Win32ForgeUi::preferredHeight());
    return ui;
}

void uiCleanup(LV2UI_Handle handle) { delete uiFromHandle(handle); }

// A assinatura do typedef e (handle, port_index, BUFFER_SIZE, FORMAT, buffer)
// -- third_party/lv2/lv2/include/lv2/ui/ui.h:200. Os tres primeiros sao inteiros,
// entao nomear ao contrario compila igual e faz o guarda `format != 0` ver o tamanho
// do buffer (4): todo port_event do host seria descartado em silencio. Conferir
// ordem no header, nunca na memoria.
void uiPortEvent(LV2UI_Handle handle, uint32_t port, uint32_t bufferSize, uint32_t format,
                 const void* buffer) {
    uiFromHandle(handle)->portEvent(port, bufferSize, format, buffer);
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
