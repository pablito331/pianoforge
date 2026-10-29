// PianoForge - backend Cocoa da UI "Forja de Pianos".
//
// Terceiro par do backend_x11.cpp e do backend_win32.cpp: estado, gesto, portas,
// registro, manifest e desenho estao em src/ui/ForgeUi.{hpp,cpp} e src/core/ui/.
// Aqui so ha NSView, CGBitmapContext, NSOpenPanel e a bomba de eventos do run loop.
//
// A diferenca estrutural desta plataforma: nao abrimos thread. AppKit so aceita
// chamadas na main thread, e um host de audio (REAPER, Ardour, Carla) INSTA a UI
// justamente nela -- entao a nossa "thread de eventos" e o proprio run loop da main
// thread. Isso da o mesmo resultado que a pump_ do X11 e o PeekMessage do Win32: o
// painel se redesenha sem depender de o host chamar idle() (o REAPER no Linux nunca
// chama), e um port_event de outra thread acorda o run loop em vez de desenhar na
// thread errada.
//
// Quatro regras deste arquivo que nao sao gosto:
//   - a view e flipped (isFlipped = YES). A Surface tem origem no TOPO e o hit-test
//     tambem; com a origem do Cocoa no basso cada clique sairia espelhado e o
//     arrasto do knob subiria quando desce.
//   - todo bloco dispatchado para a main thread carrega um shared_ptr de um flag de
//     "esta vivo". O dtor tambem roda na main thread e uma requestRedraw anterior
//     pode ter deixado um bloco na fila: sem o flag (que sobrevive ao objeto porque
//     e compartilhado) isso seria um use-after-free dentro do host.
//   - nada de [super drawRect:]/updateLayer: apresentar a Surface, so
//     layer.contents =. Assim o painel nunca depende de o AppKit decidir
//     re-renderizar a view.
//   - as constantes de tecla sao numeros Unicode locais, nao os NS*Character/
//     NS*FunctionKey do AppKit. Mesmos valores, e o arquivo deixa de depender de
//     qual SDK declara cada enum (historicamente NSText.h) -- e de um typo vir um
//     identificador inexistente que so aparece no macOS da CI.

#include "ForgeUi.hpp"

#include "ForgeTheme.hpp"
#include "UiFactory.hpp"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace pfui;
using namespace pforge;

constexpr int kWinW = 1000;
constexpr int kWinH = 520;

// Valores fixos do Unicode usado pelo AppKit para teclas de controle (NSBackspace-
// Character, NSEnterCharacter, NSUpArrowFunctionKey, ...). O keypad Enter e 0x03,
// nao 0x0D -- um host que entrega o 0x03 como "caractere digitado" encheria a busca
// com um CTRL-C invisivel se trataossemos Enter so pelo 0x0D.
enum : unsigned {
    kKeyBackspace = 0x08,
    kKeyTab = 0x09,
    kKeyNewline = 0x0A,
    kKeyEnter = 0x03,        // keypad enter
    kKeyReturn = 0x0D,
    kKeyEscape = 0x1B,
    kKeyDelete = 0x7F,       // forward delete / backspace no Mac
    kKeyFunctionBase = 0xF700,
    kKeyUp = 0xF700,
    kKeyDown = 0xF701,
    kKeyLeft = 0xF702,
    kKeyRight = 0xF703,
};

// Uma tecla "especial" no Cocoa chega como codepoint na faixa das function keys
// (0xF700..0xF8FF), nao como keycode: e o que charactersIgnoringModifiers devolve.
UiKey keyFromCocoa(unsigned unich) {
    switch (unich) {
        case kKeyBackspace: return UiKey::Backspace;
        case kKeyDelete:    return UiKey::Backspace;
        case kKeyEscape:    return UiKey::Escape;
        case kKeyReturn:
        case kKeyEnter:
        case kKeyNewline:   return UiKey::Return;
        case kKeyUp:        return UiKey::Up;
        case kKeyDown:      return UiKey::Down;
        case kKeyLeft:      return UiKey::Left;
        case kKeyRight:     return UiKey::Right;
        default:            return UiKey::Text;
    }
}

inline bool isCocoaSpecial(unsigned unich) { return unich >= kKeyFunctionBase && unich <= 0xF8FF; }

}  // namespace

namespace pfui {
class CocoaForgeUi;
}  // namespace pfui

// ---- a view -------------------------------------------------------------
// Declarada antes do controlador porque o controlador a cria; a IMPLEMENTACAO vem
// depois, quando CocoaForgeUi ja e um tipo completo e pode ser chamada.
@interface PFCocoaPanelView : NSView {
    // Ponteiro para a C++ do controlador: nao e objeto ObjC, entao ARC nenhum aqui.
    pfui::CocoaForgeUi* _ui;
}
- (void)setForgeUi:(pfui::CocoaForgeUi*)ui;
@end

namespace pfui {

class CocoaForgeUi final : public ForgeUi {
public:
    // `parent` e o NSView que o host deu em ui:parent (ui.ttl, classe ui:CocoaUI:
    // "the LV2_Widget is a pointer to a NSView"). Nulo = modo flutuante, e ai a
    // janela e nossa.
    //
    // Este construtor toca AppKit, entao so e chamado na main thread -- que e onde
    // os hosts de macOS instantiam a UI. Nao ha como garantir isso daqui: um host
    // que instantiasse fora da main thread ja estaria violando AppKit antes de nos
    // sermos criados.
    CocoaForgeUi(NSView* parent, const std::string& bundlePath, LV2UI_Write_Function writeFn,
                 LV2UI_Controller controller)
        : ForgeUi(bundlePath, writeFn, controller), parent_([parent retain]) {
        // Ha bomba de eventos (o run loop): ver ForgeUi::idle()/wakeOrDraw(), que com
        // isto ligado se recusam a desenhar na thread chamadora.
        hasEventThread_ = true;
        alive_ = std::make_shared<std::atomic<bool>>(true);
        createView();
        installTimer();
    }

    ~CocoaForgeUi() override {
        // Nada de dispatch depois daqui: quem ja esta na fila passa a checar o flag.
        if (timer_) {
            CFRunLoopTimerInvalidate(timer_);
            CFRelease(timer_);
            timer_ = nullptr;
        }
        alive_->store(false, std::memory_order_release);
        if (bitmapCtx_) {
            CGContextRelease(bitmapCtx_);
            bitmapCtx_ = nullptr;
        }
        ctxBits_ = nullptr;
        // Local __unsafe_unretained de caso: com -fobjc-arc um local __strong
        // receberia um retain extra do proprio ARC, e o nosso [release] passaria a
        // depender de quando cada compilador decide soltar.
        __unsafe_unretained PFCocoaPanelView* v = view_;
        [v removeFromSuperview];
        [v release];
        view_ = nil;
        if (ownedWindow_) {
            __unsafe_unretained NSWindow* w = ownedWindow_;
            ownedWindow_ = nil;
            [w close];
            [w release];
        }
        [parent_ release];
        parent_ = nil;
    }

    // O que devolvemos ao host em *widget: o NSView, nao um ponteiro para ele.
    void* widget() const { return (__bridge void*)view_; }

    // ---- chamadas da view (ja na main thread; a trava e tomada aqui) ------
    void pressAt(double x, double y, double timeSec) {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        onPress(int(x), int(y), timeSec);
    }
    void dragTo(double x, double y, bool shift, bool dragging) {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        onMotion(int(x), int(y), shift, dragging);
    }
    void releaseAt(double x, double y) {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        onRelease(int(x), int(y));
    }
    void wheelAt(double x, double y, int up) {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        onWheel(int(x), int(y), up);
    }
    void keyText(NSString* s) {
        const char* u8 = s ? [s UTF8String] : nullptr;
        if (!u8 || !*u8) return;
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        onKey(UiKey::Text, std::string(u8));
    }
    void keySpecial(unsigned unich) {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        onKey(keyFromCocoa(unich), std::string());
    }
    void geometryChanged(int w, int h) {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        onWindowGeometry(w, h);
    }
    void visibilityChanged() {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        onVisibilityChanged();
    }

protected:
    // ---- apresentacao ----------------------------------------------------
    // O backing store e um CGBitmapContext que o proprio CoreGraphics aloca. A
    // codificacao escolhida e XRGB little-endian, que e a MESMA palavra de 32 bits
    // da Surface (0xAARRGGBB, com o byte de alpha ignorado): nenhum conversao por
    // pixel no blit, e o painel compoe tudo opaco por natureza.
    bool resizeBacking(int w, int h) override {
        if (bitmapCtx_) {
            CGContextRelease(bitmapCtx_);
            bitmapCtx_ = nullptr;
            ctxBits_ = nullptr;
        }
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        const CGBitmapInfo info =
            (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little;
        bitmapCtx_ = CGBitmapContextCreate(nullptr, size_t(w), size_t(h), 8, size_t(w) * 4,
                                          space, info);
        CGColorSpaceRelease(space);
        if (!bitmapCtx_) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "CGBitmapContextCreate falhou em %dx%d", w, h);
            log(buf);
            return false;
        }
        ctxBits_ = static_cast<uint32_t*>(CGBitmapContextGetData(bitmapCtx_));
        // STRIDE EM PIXELS. O memcpy abaixo usa cols*4 BYTES -- no X11 troquei os
        // dois e a janela pintou um quarto da altura, com "muitas cores" no probe
        // dizendo que estava tudo bem.
        ctxStride_ = int(CGBitmapContextGetBytesPerRow(bitmapCtx_) / 4);
        ctxW_ = w;
        ctxH_ = h;
        return ctxBits_ != nullptr;
    }

    void present(const pforge::Surface& surf) override {
        if (!view_ || !ctxBits_) return;
        if (!loggedDraw_) {
            loggedDraw_ = true;
            std::fprintf(stderr, "[PianoForge UI] primeiro frame Cocoa: janela %dx%d, "
                                 "contexto %dx%d stride=%d\n",
                         winW_, winH_, ctxW_, ctxH_, ctxStride_);
        }
        const int rows = std::min(surf.h, ctxH_);
        const int cols = std::min(surf.w, ctxW_);
        const size_t copyBytes = size_t(cols) * sizeof(uint32_t);
        for (int y = 0; y < rows; ++y)
            std::memcpy(ctxBits_ + size_t(y) * size_t(ctxStride_),
                        surf.px.data() + size_t(y) * size_t(surf.w), copyBytes);
        CGImageRef img = CGBitmapContextCreateImage(bitmapCtx_);
        if (!img) return;
        CALayer* layer = view_.layer;
        if (layer) layer.contents = (__bridge id)img;
        CGImageRelease(img);
    }

    // Acorda a main thread. dispatch_async de um bloco minusculo e o jeito de
    // chamar AppKit de outra thread sem um pipe, e o flag compartilhado cuida de o
    // bloco chegar depois do dtor.
    void requestRedraw() override { scheduleFrame(); }

    // Foco: sem isto a busca digitada nao chega a lugar nenhum, porque o painel e
    // subview de um container que nao e a janela-chave por si so.
    //
    // Os tres blocos de baixo checam o flag de "esta vivo" ANTES de encostar na
    // view: sem ARC um bloco nao retem o ponteiro que captura, e um dispatch feito
    // de outra thread pouco antes do dtor chegaria na main thread com a view ja
    // solta. Com o flag, o bloco e no-op; por isso ele precisa vir primeiro.
    void focusWindow() override {
        __unsafe_unretained PFCocoaPanelView* v = view_;
        auto alive = alive_;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!alive->load(std::memory_order_acquire)) return;
            NSWindow* win = [v window];
            if (win) [win makeFirstResponder:v];
        });
    }

    void mapWindow() override {
        __unsafe_unretained PFCocoaPanelView* v = view_;
        auto alive = alive_;
        needDraw_ = true;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!alive->load(std::memory_order_acquire)) return;
            [v setHidden:NO];
            NSWindow* win = [v window];
            if (win) [win makeFirstResponder:v];
        });
    }
    void unmapWindow() override {
        __unsafe_unretained PFCocoaPanelView* v = view_;
        auto alive = alive_;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!alive->load(std::memory_order_acquire)) return;
            [v setHidden:YES];
        });
    }

    // Os dois dialogos rodam com runModal, AQUI na main thread e SEM a trava: um
    // NSOpenPanel aberto minutos com o lock tomado pararia o port_event da thread
    // do host -- o DAW inteiro, nao so o painel.
    std::vector<std::string> browseFiles() override {
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.allowsMultipleSelection = YES;
        p.canChooseDirectories = NO;
        p.canChooseFiles = YES;
        p.message = @"Escolha soundfonts";
        // allowedFileTypes esta deprecado desde o macOS 11 (UTType), mas continua
        // honrado e nao exige UniformTypeIdentifiers, que so existe la -- o bundle
        // e entregue para 10.13+.
        p.allowedFileTypes = @[ @"sf2", @"sf3", @"sfz", @"pforge" ];
        if ([p runModal] != NSModalResponseOK) return {};
        std::vector<std::string> out;
        for (NSURL* url in p.URLs) {
            const char* u8 = [[url path] UTF8String];
            if (u8) out.emplace_back(u8);
        }
        return out;
    }

    std::string browseFolder() override {
        NSOpenPanel* p = [NSOpenPanel openPanel];
        p.allowsMultipleSelection = NO;
        p.canChooseDirectories = YES;
        p.canChooseFiles = NO;
        p.message = @"Pasta com soundfonts";
        if ([p runModal] != NSModalResponseOK) return {};
        const char* u8 = [[p.URL path] UTF8String];
        std::string out = u8 ? std::string(u8) : std::string();
        while (out.size() > 1 && out.back() == '/') out.pop_back();
        return out;
    }

    void log(const char* msg) override {
        const char* text = msg ? msg : "(vazio)";
        std::fprintf(stderr, "[PianoForge UI] %s\n", text);
        @autoreleasepool {
            // %@ com NSString, nao %s com NSString: no primeiro NSLog imprimiria o
            // ponteiro como bytes e travaria na segunda palavra.
            NSLog(@"[PianoForge UI] %@", [NSString stringWithUTF8String:text]);
        }
    }

private:
    void createView() {
        __unsafe_unretained PFCocoaPanelView* v =
            [[PFCocoaPanelView alloc] initWithFrame:NSMakeRect(0, 0, kWinW, kWinH)];
        [v setForgeUi:this];
        v.wantsLayer = YES;                 // camada propria: o blit e contents=
        // 1 ponto = 1 pixel da Surface. Em tela Retina (2x) o painel sai um pouco
        // macio -- aceitavel: a alternativa seria desenhar o layout em pixels e ter
        // o mouse, que o AppKit entrega em PONTOS, apontando para outro lugar.
        v.layer.contentsScale = 1.0;
        // O [retain] explicito e o que faz o +1 do alloc ser NOSSO nos dois modos:
        // com -fobjc-arc o compilador solta o resultado do alloc no fim do escopo (e
        // a view desapareceria com o painel em tela); sem ARC ele fica onde esta.
        view_ = [v retain];
        if (parent_) {
            // O container e do host; se ele mudar de tamanho queremos ser
            // redimensionados junto, porque e o setFrameSize: de baixo que chama
            // geometryChanged -> adoptSize. Com NSViewNotSizable o painel ficaria
            // num canto e o usuario veria ferro preto no resto.
            v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            [parent_ addSubview:v];
            const NSRect pb = [parent_ bounds];
            if (pb.size.width > 1.0 && pb.size.height > 1.0)
                adoptSize(int(pb.size.width), int(pb.size.height));
            else
                adoptSize(kWinW, kWinH);
        } else {
            __unsafe_unretained NSWindow* w = [[NSWindow alloc]
                initWithContentRect:NSMakeRect(0, 0, kWinW, kWinH)
                          styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                     NSWindowStyleMaskMiniaturizable |
                                     NSWindowStyleMaskResizable)
                            backing:NSBackingStoreBuffered
                              defer:NO];
            w.releasedWhenClosed = NO;      // quem solta a janela somos nos, no dtor
            w.title = @"PianoForge - Forja de Pianos";
            w.contentView = v;
            [w makeKeyAndOrderFront:nil];
            ownedWindow_ = [w retain];
            adoptSize(kWinW, kWinH);
        }
        // Primeiro paint sincrono, a mesma regra do X11: um host que nunca chama
        // idle() nao pode deixar o painel no fundo da camada.
        needDraw_ = true;
        serviceFrame();
    }

    // O timer do run loop e o relogio do painel: fagulhas, brasa de load e toast
    // vivem por tempo, e um host que nunca chama idle() precisa de alguem para
    // redesenhar -- no X11 e a pump_, aqui e isto.
    void installTimer() {
        CFRunLoopTimerContext ctx{};
        ctx.info = this;
        timer_ = CFRunLoopTimerCreate(nullptr, CFAbsoluteTimeGetCurrent() + 0.02, 1.0, 0, 0,
                                      &CocoaForgeUi::onTimer, &ctx);
        if (timer_)
            CFRunLoopAddTimer(CFRunLoopGetMain(), timer_, kCFRunLoopCommonModes);
    }

    static void onTimer(CFRunLoopTimerRef, void* info) {
        auto* self = static_cast<CocoaForgeUi*>(info);
        if (!self || !self->alive_->load(std::memory_order_acquire)) return;
        self->pumpFrame();
        const double next = self->animating() ? 0.033 : 1.0;
        CFRunLoopTimerSetNextFireDate(self->timer_, CFAbsoluteTimeGetCurrent() + next);
    }

    void scheduleFrame() {
        auto alive = alive_;
        CocoaForgeUi* ui = this;   // "self" tem outro significado em ObjC++: evitar
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!alive->load(std::memory_order_acquire)) return;
            ui->pumpFrame();
        });
    }

    // runModal faz um NESTED run loop: o timer continua disparando enquanto o
    // painel de arquivos esta aberto, e serviceFrame() ali dentro so gastaria CPU
    // atras de uma janela modal. O flag e lido/escrito so na main thread.
    void pumpFrame() {
        std::string kind;
        if (takeDialogRequest(kind)) {
            dialogUp_ = true;
            runDialogRequest(kind);
            dialogUp_ = false;
            needDraw_ = true;
            return;
        }
        if (dialogUp_) return;
        serviceFrame();
    }

    // Ponteiros ObjC gerenciados a mao: membros __unsafe_unretained para o ARC nao
    // tentar gerenciar estado dentro de uma classe C++ (que ele nao faz) e para o
    // arquivo compilar tambem sem -fobjc-arc.
    __unsafe_unretained NSView* parent_ = nil;
    __unsafe_unretained PFCocoaPanelView* view_ = nil;
    __unsafe_unretained NSWindow* ownedWindow_ = nil;
    CGContextRef bitmapCtx_ = nullptr;
    uint32_t* ctxBits_ = nullptr;
    int ctxStride_ = 0, ctxW_ = 0, ctxH_ = 0;
    CFRunLoopTimerRef timer_ = nullptr;
    std::shared_ptr<std::atomic<bool>> alive_;
    bool dialogUp_ = false;
    bool loggedDraw_ = false;
};

}  // namespace pfui

// ---- fabrica host-agnostica (UiFactory.hpp) ----------------------------
// Mesmo contrato dos backends Win32 e X11: o LV2 e o VST3 criam o painel aqui.

namespace pfui {

ForgeUi* createUi(void* parent, const char* bundlePath, LV2UI_Write_Function write,
                  LV2UI_Controller controller, void** widgetOut) {
    auto* ui = new CocoaForgeUi((__bridge NSView*)parent, bundlePath ? bundlePath : "", write,
                                controller);
    if (!ui->widget()) {
        delete ui;
        return nullptr;
    }
    if (widgetOut) *widgetOut = ui->widget();
    return ui;
}

void destroyUi(ForgeUi* ui) { delete static_cast<CocoaForgeUi*>(ui); }

}  // namespace pfui

// ---- implementacao da view (aqui CocoaForgeUi ja e tipo completo) --------

@implementation PFCocoaPanelView
- (void)setForgeUi:(pfui::CocoaForgeUi*)ui { _ui = ui; }

// Origem no TOPO, como a Surface e o hit-test.
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*) { return YES; }   // clicar no painel ja da foco

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (_ui) _ui->visibilityChanged();    // o MapNotify do X11: o conteudo volta a valer
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    if (_ui) _ui->geometryChanged(int(newSize.width), int(newSize.height));
}

// O painel NAO desenha nada por aqui: quem apresenta e present() via layer.contents.
// Sem esta recusa o AppKit poderia chamar updateLayer/drawRect num redraw dele.
- (BOOL)wantsUpdateLayer { return NO; }

- (NSPoint)pointInView:(NSEvent*)event {
    return [self convertPoint:event.locationInWindow fromView:nil];
}

- (void)mouseDown:(NSEvent*)event {
    if (!_ui) return;
    [[self window] makeFirstResponder:self];
    const NSPoint p = [self pointInView:event];
    // event.timestamp e relogio monotonico desde o boot -- o que o duplo clique do
    // controlador precisa (CurrentTime do X11 virava todo clique em duplo).
    _ui->pressAt(p.x, p.y, event.timestamp);
}

- (void)mouseDragged:(NSEvent*)event {
    if (!_ui) return;
    const NSPoint p = [self pointInView:event];
    _ui->dragTo(p.x, p.y, (event.modifierFlags & NSEventModifierFlagShift) != 0, true);
}

- (void)mouseUp:(NSEvent*)event {
    if (!_ui) return;
    const NSPoint p = [self pointInView:event];
    _ui->dragTo(p.x, p.y, (event.modifierFlags & NSEventModifierFlagShift) != 0, false);
    _ui->releaseAt(p.x, p.y);
}

- (void)mouseMoved:(NSEvent*)event {
    if (!_ui) return;
    const NSPoint p = [self pointInView:event];
    const bool shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    // buttonMask bit 0 = botao esquerdo no arrasto de hover; sem isto o painel nao
    // saberia distinguir "passar por cima" de "arrastando".
    const bool down = (event.buttonMask & 1u) != 0;
    _ui->dragTo(p.x, p.y, shift, down);
}

// A roda no Cocoa vem em LINHAS (mouse comum) ou em pixels continuos (trackpad).
// O painel conhece DENTES: um por linha, e um por ~10 px de trackpad, senao um
// swipe de dois dedos mexe dez knobs.
- (void)scrollWheel:(NSEvent*)event {
    if (!_ui) return;
    const NSPoint p = [self pointInView:event];
    const double d = event.hasPreciseScrollingDeltas ? double(event.scrollingDeltaY)
                                                     : double(event.deltaY);
    if (d == 0.0) return;
    const int up = d > 0 ? 1 : -1;
    int steps = int(std::abs(d)) / (event.hasPreciseScrollingDeltas ? 10 : 1);
    if (steps < 1) steps = 1;
    if (steps > 8) steps = 8;              // teto: um flick de 800 px nao zera o knob
    for (int i = 0; i < steps; ++i) _ui->wheelAt(p.x, p.y, up);
}

- (void)keyDown:(NSEvent*)event {
    if (!_ui) return;
    NSString* ignoring = event.charactersIgnoringModifiers;
    const unsigned unich = ignoring.length > 0 ? unsigned([ignoring characterAtIndex:0]) : 0;
    if (unich == kKeyReturn || unich == kKeyEnter || unich == kKeyNewline) {
        _ui->keySpecial(kKeyReturn);
        return;
    }
    if (unich == kKeyBackspace || unich == kKeyDelete) {
        _ui->keySpecial(kKeyBackspace);
        return;
    }
    if (unich == kKeyEscape) {
        _ui->keySpecial(kKeyEscape);
        return;
    }
    if (isCocoaSpecial(unich)) {
        _ui->keySpecial(unich);
        return;
    }
    // O texto e o que layout + Shift + acento morto produziram (characters, nao
    // charactersIgnoringModifiers): so assim um "ccedilla" vira cedilha. Tab e os
    // outros controles caem aqui como texto e o filtro do controlador os descarta
    // (cp >= 0x20), entao nao ha porque enumerar cada um.
    _ui->keyText(event.characters);
}
@end

// ---- LV2 UI descriptor -------------------------------------------------

namespace {

pfui::CocoaForgeUi* uiFromHandle(LV2UI_Handle handle) {
    return static_cast<pfui::CocoaForgeUi*>(handle);
}

LV2UI_Handle uiInstantiate(const LV2UI_Descriptor*,
                           const char*,
                           const char* bundlePath,
                           LV2UI_Write_Function writeFunction,
                           LV2UI_Controller controller,
                           LV2UI_Widget* widget,
                           const LV2_Feature* const* features) {
    NSView* parent = nil;
    const LV2UI_Resize* resize = nullptr;
    for (int i = 0; features && features[i]; ++i) {
        if (!std::strcmp(features[i]->URI, LV2_UI__parent) && features[i]->data) {
            // ui:parent "e geralmente do mesmo tipo que o LV2_Widget que a UI
            // devolveria", e para ui:CocoaUI o LV2_Widget e o NSView*.
            parent = (__bridge NSView*)features[i]->data;
        } else if (!std::strcmp(features[i]->URI, LV2_UI__resize) && features[i]->data) {
            resize = static_cast<const LV2UI_Resize*>(features[i]->data);
        }
    }

    @autoreleasepool {
        void* native = nullptr;
        pfui::ForgeUi* ui = pfui::createUi((__bridge void*)parent, bundlePath, writeFunction,
                                          controller, &native);
        if (!ui) return nullptr;
        *widget = (LV2UI_Widget)native;
        if (resize && resize->ui_resize)
            resize->ui_resize(resize->handle, pfui::ForgeUi::preferredWidth(),
                              pfui::ForgeUi::preferredHeight());
        return ui;
    }
}

void uiCleanup(LV2UI_Handle handle) { pfui::destroyUi(uiFromHandle(handle)); }

// A assinatura do typedef e (handle, port_index, BUFFER_SIZE, FORMAT, buffer)
// -- third_party/lv2/lv2/include/lv2/ui/ui.h:200. Trocar buffer_size por format
// compila igual e faz o guarda `format != 0` ver 4 em todo evento: nenhum
// port_event chegaria ao painel. Conferir ordem no header, nunca na memoria.
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

}  // namespace

LV2_SYMBOL_EXPORT
const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index) {
    return (index == 0) ? &kDescriptor : nullptr;
}
