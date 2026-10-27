// Probe de UI: sobe o pianoforge_ui.so dentro de uma janela X11 de verdade
// (off-screen, override-redirect, sem tocar no desktop do usuario), bomba
// idle() como um host faria e captura os pixels da janela da UI num PPM.
//
// E assim que se separa "a UI nao renderizou" em:
//   - lv2ui_descriptor/instantiate devolver NULL (nada de janela);
//   - janela criada mas nunca mapeada / sem Expose (draw() nao roda);
//   - draw() rodando mas pintando uniforme (fonte XLFD ausente, GC errado);
//   - XErroreX (BadWindow no reparent do host / falta de XEmbed).
//
// Saida: /tmp/pforge_ui_probe.ppm (converter com `convert` pra PNG) + laudonico.

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include "lv2/ui/ui.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>

namespace {

int g_xerrors = 0;

int xerrHandler(Display*, XErrorEvent* ev) {
    char buf[256] = {};
    XGetErrorText(ev->display, ev->error_code, buf, sizeof(buf));
    std::fprintf(stderr, "[XERROR] request=%d code=%d %s\n", ev->request_code,
                 ev->error_code, buf);
    ++g_xerrors;
    return 0;
}

void writePPM(const char* path, int w, int h, const std::vector<uint8_t>& rgb) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "sem /tmp para escrita\n"); return; }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
}

std::string atomName(Display* dpy, Atom a) {
    char* s = XGetAtomName(dpy, a);
    std::string out = s ? s : "";
    if (s) XFree(s);
    return out;
}

// Evento sintatico de botao/movimento. A UI le xbutton/xmotion direto, entao
// XSendEvent chega bem para provar o caminho clique -> write_() -> porta.
Time fakeTime() {
    static Time t = 1000;
    t += 60;               // CurrentTime faria todo clique parecer duplo clique
    return t;
}
void sendButton(Display* dpy, Window w, int type, int x, int y, int button, unsigned state) {
    XEvent ev{};
    ev.xbutton.type = type;
    ev.xbutton.display = dpy;
    ev.xbutton.window = w;
    ev.xbutton.root = RootWindow(dpy, DefaultScreen(dpy));
    ev.xbutton.subwindow = None;
    ev.xbutton.time = fakeTime();
    ev.xbutton.x = x;
    ev.xbutton.y = y;
    ev.xbutton.button = button;
    ev.xbutton.state = state;
    ev.xbutton.same_screen = True;
    XSendEvent(dpy, w, False, ButtonPressMask | ButtonReleaseMask, &ev);
    XFlush(dpy);
}

void sendMotion(Display* dpy, Window w, int x, int y, unsigned state) {
    XEvent ev{};
    ev.xmotion.type = MotionNotify;
    ev.xmotion.display = dpy;
    ev.xmotion.window = w;
    ev.xmotion.root = RootWindow(dpy, DefaultScreen(dpy));
    ev.xmotion.subwindow = None;
    ev.xmotion.time = fakeTime();
    ev.xmotion.x = x;
    ev.xmotion.y = y;
    ev.xmotion.state = state;
    ev.xmotion.same_screen = True;
    XSendEvent(dpy, w, False, PointerMotionMask, &ev);
    XFlush(dpy);
}

// Tecla sintetica. O keycode vem de XKeysymToKeycode na MESMA display, que e a
// tabela que o XLookupString da UI consulta: mandar um keysym "no campo keysym"
// nao funcionaria, a UI ignora esse campo e traduz o keycode.
void sendKey(Display* dpy, Window w, int type, KeySym ks) {
    const KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (!kc) { std::fprintf(stderr, "sem keycode para keysym 0x%lx\n",
                            static_cast<unsigned long>(ks)); return; }
    XEvent ev{};
    ev.xkey.type = type;
    ev.xkey.display = dpy;
    ev.xkey.window = w;
    ev.xkey.root = RootWindow(dpy, DefaultScreen(dpy));
    ev.xkey.subwindow = None;
    ev.xkey.time = fakeTime();
    ev.xkey.x = 0;
    ev.xkey.y = 0;
    ev.xkey.x_root = 0;
    ev.xkey.y_root = 0;
    ev.xkey.keycode = kc;
    ev.xkey.state = 0;
    ev.xkey.same_screen = True;
    XSendEvent(dpy, w, False, KeyPressMask | KeyReleaseMask, &ev);
    XFlush(dpy);
}

// Roda do mouse: X11 entrega como botao 4 (para cima) e 5 (para baixo), nao como
// um evento proprio. E por aqui que a lista rolada e o preset da pila sao testados.
void sendWheel(Display* dpy, Window w, int x, int y, int button) {
    sendButton(dpy, w, ButtonPress, x, y, button, 0);
    sendButton(dpy, w, ButtonRelease, x, y, button, 0);
}

struct Pt { int x = 0, y = 0; };bool parsePt(const std::string& s, Pt& out) {
    const size_t c = s.find(':');
    if (c == std::string::npos) return false;
    out.x = std::atoi(s.substr(0, c).c_str());
    out.y = std::atoi(s.substr(c + 1).c_str());
    return true;
}

// Um container TrueColor de 32 bits, como o que toolkit/WMs compostos entregam.
// E o caso que o XCreateImage com DefaultVisual/DefaultDepth nao sabe desenhar:
// XPutImage vira BadMatch, o handler do host engole e a janela fica preta.
Visual* findVisual(Display* dpy, int scr, int depth) {
    XVisualInfo tmpl{};
    tmpl.screen = scr;
    tmpl.depth = depth;
    tmpl.c_class = TrueColor;
    int n = 0;
    XVisualInfo* found = XGetVisualInfo(dpy, VisualScreenMask | VisualDepthMask |
                                        VisualClassMask, &tmpl, &n);
    if (!found || n <= 0) { if (found) XFree(found); return nullptr; }
    Visual* v = found[0].visual;
    if (depth == 24 && n > 1) {
        // Um TrueColor de 24 bits DIFERENTE do default: emula o host que entrega
        // um container com visual proprio, que e o que quebra o XImage padrão.
        for (int i = 1; i < n; ++i) {
            if (found[i].visual != DefaultVisual(dpy, scr)) { v = found[i].visual; break; }
        }
    }
    XFree(found);
    return v;
}

// Cria o container do "host" com o visual pedido e confirma que ele existe: o X
// recusa filho de depth diferente do pai com BadMatch, e uma janela morta aqui
// faria o resto do teste mentir.
Window createParent(Display* dpy, int scr, Visual* v, int depth, const char* what) {
    if (!v) return 0;
    XSetWindowAttributes pa{};
    pa.background_pixel = BlackPixel(dpy, scr);
    pa.colormap = XCreateColormap(dpy, RootWindow(dpy, scr), v, AllocNone);
    Window w = XCreateWindow(dpy, RootWindow(dpy, scr), -4096, -4096, 1100, 620, 0,
                             depth, InputOutput, v, CWBackPixel | CWColormap, &pa);
    const int before = g_xerrors;
    XSync(dpy, False);
    XWindowAttributes a{};
    if (g_xerrors != before || !XGetWindowAttributes(dpy, w, &a)) {
        std::printf("%s: o servidor recusou o container desse visual "
                    "(filho precisa da depth do pai)\n", what);
        return 0;
    }
    std::printf("%s: container depth=%d visual=0x%lx\n", what, a.depth,
                static_cast<unsigned long>(XVisualIDFromVisual(a.visual)));
    return w;
}

// Le os pixels da janela da UI sem depender do XGetImage (que da BadMatch em
// janela nao-viewable): XGetImage primeiro, fallback por composicao do pai.
//
// Falhar devolve vetor VAZIO, nao um preto de w*h*3: o chamador distingue "nao
// li nada" de "li e estava tudo preto", que e exatamente a diferenca entre o
// probe estar quebrado e a UI nao ter pintado. (Numa rodada de CI com Xvfb o
// vetor zerado virou "0 cores distintas -> PRETO", e o veredito mentia.)
std::vector<uint8_t> grabWindow(Display* dpy, Window win, int w, int h, int& distinctOut) {
    std::vector<uint8_t> rgb(size_t(w) * h * 3, 0);
    distinctOut = 0;
    XImage* img = XGetImage(dpy, win, 0, 0, w, h, AllPlanes, ZPixmap);
    if (!img) return {};
    std::set<uint32_t> distinct;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned long p = XGetPixel(img, x, y);
            const unsigned long rm = img->red_mask, gm = img->green_mask, bm = img->blue_mask;
            auto shift = [&](unsigned long mask) -> int {
                int s = 0;
                while (!(mask & 1) && mask) { mask >>= 1; ++s; }
                int bits = 0;
                while (mask & 1) { mask >>= 1; ++bits; }
                int v = int((p >> s) & ((1u << bits) - 1u));
                return bits >= 8 ? (v >> (bits - 8)) : (v << (8 - bits));
            };
            size_t i = (size_t(y) * w + x) * 3;
            rgb[i + 0] = uint8_t(shift(rm));
            rgb[i + 1] = uint8_t(shift(gm));
            rgb[i + 2] = uint8_t(shift(bm));
            if (distinct.size() < 70000) distinct.insert(p);
        }
    }
    XDestroyImage(img);
    distinctOut = int(distinct.size());
    return rgb;
}

} // namespace

int main(int argc, char** argv) {
    std::string soPath = "build/pianoforge.lv2/pianoforge_ui.so";
    const char* outPath = "/tmp/pforge_ui_probe.ppm";
    int cycles = 40;
    double settle = 0.0;      // segundos de idle() depois de interagir
    bool callShow = false;
    bool shiftDrag = false;   // --shift: arrastos com ShiftMask (o modo fino do knob)
    bool depth32 = false;
    bool noIdle = false;
    bool unmappedParent = false;
    Pt resizeTo = {0, 0};
    std::vector<Pt> clicks;
    std::vector<Pt> dragA, dragB;
    std::vector<std::pair<uint32_t, float>> portEvents;   // saidas 22/23 do plugin
    std::vector<std::string> typed;      // texto a digitar, caractere por caractere
    std::vector<KeySym> keys;            // teclas nomeadas (BackSpace, Return, Escape)
    std::vector<std::array<int, 3>> wheels;   // x:y:delta, delta<0 = para baixo
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--so" && i + 1 < argc) soPath = argv[++i];
        else if (a == "--port" && i + 1 < argc) {
            // "22:-3" = o host repassando a porta de saida 22 com o plugin forjando.
            const std::string spec = argv[++i];
            const size_t c = spec.find(':');
            if (c != std::string::npos)
                portEvents.emplace_back(uint32_t(std::atoi(spec.substr(0, c).c_str())),
                                        std::strtof(spec.substr(c + 1).c_str(), nullptr));
        }
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--cycles" && i + 1 < argc) cycles = std::stoi(argv[++i]);
        else if (a == "--settle" && i + 1 < argc) settle = std::atof(argv[++i]);
        else if (a == "--show") callShow = true;
        else if (a == "--shift") shiftDrag = true;
        else if (a == "--depth32") depth32 = true;
        else if (a == "--noidle") noIdle = true;
        else if (a == "--unmapped-parent") unmappedParent = true;
        else if (a == "--resize" && i + 1 < argc) parsePt(argv[++i], resizeTo);
        else if (a == "--click" && i + 1 < argc) {
            Pt p;
            if (parsePt(argv[++i], p)) clicks.push_back(p);
        } else if (a == "--drag" && i + 2 < argc) {
            Pt p, q;
            if (parsePt(argv[++i], p) && parsePt(argv[++i], q)) {
                dragA.push_back(p);
                dragB.push_back(q);
            }
        } else if (a == "--type" && i + 1 < argc) {
            typed.emplace_back(argv[++i]);
        } else if (a == "--key" && i + 1 < argc) {
            keys.push_back(XStringToKeysym(argv[++i]));
        } else if (a == "--wheel" && i + 1 < argc) {
            // "100:66:-3" = tres dentes para baixo sobre o ponto (100,66)
            const std::string spec = argv[++i];
            std::array<int, 3> v{};
            size_t pos = 0;
            for (int f = 0; f < 3; ++f) {
                const size_t c = spec.find(':', pos);
                const std::string part = (c == std::string::npos) ? spec.substr(pos)
                                                                  : spec.substr(pos, c - pos);
                v[size_t(f)] = std::atoi(part.c_str());
                if (c == std::string::npos) { pos = c; break; }
                pos = c + 1;
            }
            wheels.push_back(v);
        }
    }

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { std::fprintf(stderr, "sem DISPLAY\n"); return 2; }
    XSetErrorHandler(xerrHandler);

    // ---- parent: uma janela top-level override-redirect longe de tudo -------
    // Override-redirect nao passa pelo WM, e o deslocamento negativo garante que
    // nada aparece no desktop enquanto o usuario dorme.
    int scr = DefaultScreen(dpy);
    Window parent = 0;
    if (depth32) {
        parent = createParent(dpy, scr, findVisual(dpy, scr, 32), 32, "depth32");
        if (!parent) parent = createParent(dpy, scr, findVisual(dpy, scr, 24), 24,
                                           "visual-alt (24 bits, nao o default)");
    }
    if (!parent) {
        parent = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), -4096, -4096,
                                     1100, 620, 0, BlackPixel(dpy, scr),
                                     BlackPixel(dpy, scr));
    }
    XSetWindowAttributes attrs{};
    attrs.override_redirect = True;
    attrs.event_mask = SubstructureNotifyMask | StructureNotifyMask | ExposureMask;
    XChangeWindowAttributes(dpy, parent, CWOverrideRedirect | CWSaveUnder | CWEventMask, &attrs);
    attrs.save_under = True;
    XChangeWindowAttributes(dpy, parent, CWOverrideRedirect | CWSaveUnder | CWEventMask, &attrs);
    if (!unmappedParent) XMapRaised(dpy, parent);
    else std::printf("parent DEIXADO nao-mapeado (host mapeia o container depois)\n");
    XSync(dpy, False);
    std::printf("parent=0x%lx\n", static_cast<unsigned long>(parent));

    // ---- carrega a UI do plugin -------------------------------------------
    void* lib = dlopen(soPath.c_str(), RTLD_NOW);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto getDesc = reinterpret_cast<const LV2UI_Descriptor* (*)(uint32_t)>(
        dlsym(lib, "lv2ui_descriptor"));
    if (!getDesc) { std::fprintf(stderr, "sem lv2ui_descriptor\n"); return 2; }
    const LV2UI_Descriptor* desc = getDesc(0);
    if (!desc) { std::fprintf(stderr, "descriptor index 0 ausente\n"); return 2; }
    std::printf("uri=%s\n", desc->URI);

    static std::vector<std::pair<uint32_t, float>> written;
    auto writeFn = [](LV2UI_Controller, uint32_t port, uint32_t, uint32_t,
                      const void* buffer) {
        written.emplace_back(port, buffer ? *static_cast<const float*>(buffer) : 0.0f);
    };

    LV2_Feature parentFeat = { const_cast<char*>(LV2_UI__parent),
                               reinterpret_cast<void*>(static_cast<uintptr_t>(parent)) };
    // Host que oferece resize: a UI deve pedir o tamanho dela, senao o container
    // fica do tamanho que o host escolheu e o painel sai cortado.
    auto onResize = [](LV2UI_Feature_Handle, int w, int h) -> int {
        std::printf("LV2UI_Resize: a UI pediu %dx%d\n", w, h);
        return 0;
    };
    LV2UI_Resize resizeFeat = { nullptr, onResize };
    LV2_Feature resizeF = { const_cast<char*>(LV2_UI__resize), &resizeFeat };
    const LV2_Feature* features[3] = { &parentFeat, &resizeF, nullptr };

    LV2UI_Widget widget = nullptr;
    LV2UI_Handle ui = desc->instantiate(desc, "https://pianoforge.audio/plugin",
                                        "https://pianoforge.audio/plugin#ui",
                                        writeFn, nullptr, &widget, features);
    if (!ui || !widget) {
        std::fprintf(stderr, "instantiate FALHOU (ui=%p widget=%p)\n", (void*)ui, (void*)widget);
        return 1;
    }
    Window win = static_cast<Window>(reinterpret_cast<uintptr_t>(widget));
    std::printf("widget=0x%lx (janela da UI, filha de 0x%lx)\n",
                static_cast<unsigned long>(win), static_cast<unsigned long>(parent));

    // O que o host vera antes de qualquer coisa: estado da janela.
    XWindowAttributes wa{};
    Bool got = XGetWindowAttributes(dpy, win, &wa);
    std::printf("attrs: ret=%d map_state=%s %dx%d depth=%d visual=0x%lx\n", got ? 1 : 0,
                wa.map_state == IsViewable ? "IsViewable"
                    : (wa.map_state == IsUnmapped ? "IsUnmapped" : "IsUnobscured"),
                wa.width, wa.height, wa.depth,
                static_cast<unsigned long>(XVisualIDFromVisual(wa.visual)));

    // Props que um host XEmbed exige: sem _XEMBED_INFO o Carla/Qtractor pode
    // criar o container e nao mostrar nada.
    Atom xembed = XInternAtom(dpy, "_XEMBED", False);
    Atom xembedInfo = XInternAtom(dpy, "_XEMBED_INFO", False);
    for (Atom probe : {xembed, xembedInfo}) {
        Atom type = None; int fmt = 0; unsigned long nitems = 0, after = 0;
        unsigned char* data = nullptr;
        Status st = XGetWindowProperty(dpy, win, probe, 0, 4, False, AnyPropertyType,
                                       &type, &fmt, &nitems, &after, &data);
        std::printf("prop %-12s: %s\n", atomName(dpy, probe).c_str(),
                    (st == Success && type != None) ? "presente" : "AUSENTE");
        if (data) XFree(data);
    }

    const LV2UI_Show_Interface* showIface = nullptr;
    if (desc->extension_data) {
        showIface = static_cast<const LV2UI_Show_Interface*>(
            desc->extension_data(LV2_UI__showInterface));
        std::printf("showInterface: %s   idleInterface: %s\n",
                    showIface ? "sim" : "NAO",
                    desc->extension_data(LV2_UI__idleInterface) ? "sim" : "NAO");
    }
    if (callShow && showIface) { showIface->show(ui); XSync(dpy, False); }
    if (unmappedParent) {
        XMapRaised(dpy, parent);
        XSync(dpy, False);
        std::printf("parent mapado apos o instantiate (e quando o MapNotify da UI chega)\n");
    }

    // ---- captura ----------------------------------------------------------
    auto snapshot = [&](const char* label) -> int {
        XWindowAttributes a{};
        if (!XGetWindowAttributes(dpy, win, &a)) { std::printf("%s: sem attrs\n", label); return -1; }
        int distinct = 0;
        std::vector<uint8_t> rgb = grabWindow(dpy, win, a.width, a.height, distinct);
        // O container nasce em -4096,-4096 para nao aparecer na area de trabalho de
        // quem roda isto numa X de verdade. Numa X com compositor isso e inofensivo
        // (a janela redirected existe fora da tela); num servidor sem backing store
        // como o Xvfb do CI, a regiao off-screen NAO tem bits em lugar nenhum, e a
        // leitura da filha devolve ora NULL, ora um retangulo preto de w*h*3 -- os
        // dois indistinguiveis de "a UI nao pintou". Entao: se a primeira leitura nao
        // mostrou nada, traga o container para dentro da tela e leia de novo. Mover o
        // pai nao muda um pixel do que a UI desenha, muda o que o servidor pode ler; e
        // se depois disso ainda estiver preto, e porque nada foi desenhado mesmo.
        if (rgb.empty() || distinct <= 2) {
            XMoveWindow(dpy, parent, 8, 8);
            XSync(dpy, False);
            // Expose da filha recem-visita chega pela thread da UI, que desenha ao
            // recebe-la; dar o tempo de uma passada antes de julgar a segunda leitura.
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            rgb = grabWindow(dpy, win, a.width, a.height, distinct);
            if (!rgb.empty()) std::printf("%s: leitura com o container na tela (off-screen nao tem bits)\n", label);
        }
        if (rgb.empty()) { std::printf("%s: XGetImage falhou\n", label); return -1; }
        const std::string path = std::string(outPath) +
                                 (std::strcmp(label, "fim") == 0 ? "" : (std::string("_") + label));
        writePPM(path.c_str(), a.width, a.height, rgb);
        std::printf("%s: %dx%d depth=%d %d cores distintas -> %s\n", label, a.width, a.height,
                    a.depth, distinct, path.c_str());
        return distinct;
    };

    // ---- bomba de idle(), como um host que roda a UI a ~30 Hz --------------
    const LV2UI_Idle_Interface* idleIface = desc->extension_data
        ? static_cast<const LV2UI_Idle_Interface*>(desc->extension_data(LV2_UI__idleInterface))
        : nullptr;
    // O painel se auto-pilota (thread propria de eventos e blit), entao "sem idle"
    // nao e mais immediato: damos o tempo de uma passada da thread antes de julgar.
    if (noIdle) std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const int beforeIdle = snapshot(noIdle ? "sem-idle" : "antes-do-idle");
    if (noIdle) {
        std::printf("VEREDITO (sem jamais chamar idle): %s\n",
                    beforeIdle > 4 ? "a thread da UI pintou sozinha (host sem idle OK)"
                                   : "PRETO -- nem a thread da UI pintou");
        desc->cleanup(ui);
        XCloseDisplay(dpy);
        return beforeIdle > 4 ? 0 : 1;
    }
    for (int i = 0; i < cycles; ++i) {
        if (idleIface) {
            if (idleIface->idle(ui)) { std::printf("idle pediu fechamento no ciclo %d\n", i); break; }
        } else {
            while (XPending(dpy)) { XEvent ev; XNextEvent(dpy, &ev); }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    XSync(dpy, False);
    const size_t writesBefore = written.size();

    // ---- interacao: clique na lista de pianos, arraste num knob ------------
    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i) {
            if (idleIface) idleIface->idle(ui);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };
    // ---- o host repassa as saidas de estado do plugin ----------------------
    // E o caminho real: run() escreve nas portas 22/23, o host manda port_event
    // com format 0. Sem isso a UI so tem o chute de tempo por tamanho de arquivo.
    //
    // Ordem dos argumentos = o typedef do header, NAO a assinatura que a nossa UI
    // declarou: (ui, port_index, buffer_size, format, buffer). O probe de antes
    // mandava (port, 0, sizeof) -- o espelho exato do bug do outro lado, e os dois
    // juntos deixavam o teste verde enquanto um host de verdade era ignorado. Se a
    // UI inverter de novo, este probe tem de FALHAR; e assim que ele vale como prova.
    for (const auto& pv : portEvents) {
        float v = pv.second;
        if (desc->port_event) desc->port_event(ui, pv.first, sizeof(v), 0, &v);
        std::printf("host mandou porta %u = %.3f\n", pv.first, v);
    }
    if (!portEvents.empty()) {
        pump(3);
        snapshot("status");
    }

    for (const Pt& p : clicks) {
        sendButton(dpy, win, ButtonPress, p.x, p.y, 1, 0);
        pump(2);
        sendButton(dpy, win, ButtonRelease, p.x, p.y, 1, Button1Mask);
        pump(6);
    }
    for (size_t d = 0; d < dragA.size() && d < dragB.size(); ++d) {
        // Button1Mask sempre; ShiftMask extra com --shift, que e exatamente o bit
        // que o onMotion da UI lê para escolher 0,15x em vez de 1x.
        const unsigned hold = shiftDrag ? (Button1Mask | ShiftMask) : Button1Mask;
        sendButton(dpy, win, ButtonPress, dragA[d].x, dragA[d].y, 1,
                   shiftDrag ? ShiftMask : 0);
        pump(2);
        for (int s = 1; s <= 6; ++s) {
            sendMotion(dpy, win,
                       dragA[d].x + (dragB[d].x - dragA[d].x) * s / 6,
                       dragA[d].y + (dragB[d].y - dragA[d].y) * s / 6, hold);
            pump(2);
        }
        sendButton(dpy, win, ButtonRelease, dragB[d].x, dragB[d].y, 1, hold);
        pump(6);
    }

    for (const std::string& text : typed) {
        for (unsigned char c : text) {
            const KeySym ks = XStringToKeysym(std::string(1, char(c)).c_str());
            if (!ks) { std::fprintf(stderr, "tecla '%c' sem keysym\n", c); continue; }
            sendKey(dpy, win, KeyPress, ks);
            pump(1);
            sendKey(dpy, win, KeyRelease, ks);
            pump(1);
        }
        std::printf("digitado \"%s\"\n", text.c_str());
        snapshot("digitado");
    }
    for (KeySym ks : keys) {
        sendKey(dpy, win, KeyPress, ks);
        pump(2);
        sendKey(dpy, win, KeyRelease, ks);
        pump(4);
        std::printf("tecla %s\n", XKeysymToString(ks));
    }
    if (!keys.empty()) snapshot("teclas");
    for (const auto& wd : wheels) {
        const int button = wd[2] < 0 ? 5 : 4;
        for (int n = 0; n < std::abs(wd[2]); ++n) {
            sendWheel(dpy, win, wd[0], wd[1], button);
            pump(2);
        }
        std::printf("roda %d:%d delta=%d\n", wd[0], wd[1], wd[2]);
    }
    if (!wheels.empty()) snapshot("roda");

    // Depois de interagir, deixa o idle() rodar: e o que separa "forjando" de
    // "pronto" no rodape do painel. pump(1) dorme ~20 ms.
    for (int i = 0; i < int(settle / 0.02); ++i) pump(1);

    // Um host que ignora os size hints e redimensiona o nosso window: a UI tem de
    // refazer a Surface/XImage, nao por 1000x520 num buraco menor.
    if (resizeTo.x > 0 && resizeTo.y > 0) {
        XResizeWindow(dpy, win, resizeTo.x, resizeTo.y);
        XSync(dpy, False);
        pump(8);
        std::printf("host redimensionou a UI para %dx%d\n", resizeTo.x, resizeTo.y);
        const int r = snapshot("redimensionado");
        std::printf("  -> %s\n", r > 4 ? "ainda pinta conteudo" : "PRETO depois do resize");
    }

    got = XGetWindowAttributes(dpy, win, &wa);
    std::printf("depois do idle: ret=%d map_state=%s %dx%d xerrors=%d writes=%d\n", got ? 1 : 0,
                wa.map_state == IsViewable ? "IsViewable"
                    : (wa.map_state == IsUnmapped ? "IsUnmapped" : "IsUnobscured"),
                wa.width, wa.height, g_xerrors, int(written.size()));
    if (written.size() > writesBefore) {
        std::printf("escritas do teste:");
        for (size_t i = writesBefore; i < written.size(); ++i)
            std::printf(" [%u]=%.3f", written[i].first, written[i].second);
        std::printf("\n");
    } else if (!clicks.empty() || !dragA.empty()) {
        std::printf("ESCRITAS DO TESTE: nenhuma -- o clique nao chegou a write_()\n");
    }

    // ---- captura os pixels da janela da UI ---------------------------------
    if (wa.width <= 0 || wa.height <= 0) { std::fprintf(stderr, "janela sem tamanho\n"); return 1; }
    const int distinct = snapshot("fim");
    if (distinct < 0) { std::fprintf(stderr, "XGetImage FALHOU (BadMatch = janela nao vista)\n"); return 1; }
    std::printf(distinct > 4
                    ? "VEREDITO: a UI PINTOU conteudo\n"
                    : "VEREDITO: saida pratica-uniforme = draw() nao rodou ou nao desenha nada\n");

    desc->cleanup(ui);
    XCloseDisplay(dpy);
    return 0;
}
