#pragma once
// A porta do painel: o shell VST3 entrega o container do host e recebe de volta o
// objeto que desenha dentro dele.
//
// Antes desta funcao cada formato lia as suas proprias "features" e construia a
// classe concreta por conta propria (o lv2ui_descriptor no LV2). Hoje ha um shell
// so, e a sequencia ficou num lugar so: um bug do painel aparece no plugin, nao
// numa copia de cola.
//
// Cada arquivo de plataforma (backend_win32.cpp, backend_x11.cpp,
// backend_cocoa.mm) implementa estas duas funcoes. O alvo do VST3 compila
// exatamente UM deles, entao nao ha #ifdef de sistema dentro do shell do VST3.

#include "ForgeUi.hpp"

namespace pfui {

// parent: handle nativo do container do host -- HWND no Win32, Window no X11,
// NSView* no Cocoa. nullptr pede janela propria (modo flutuante, que e o que o
// probe usa).
//
// bundlePath: pasta do bundle, de onde saem as fontes e a arte do .pforge. Pode
// ser vazio; ForgeUi::loadFonts cai nos diretorios do sistema.
//
// write/controller: o par que o painel usa para escrever num parametro do host.
//
// widgetOut (opcional): recebe o handle nativo do painel, que e o que o host
// embute. Devolve nullptr se a janela nao pode ser criada -- quem chama NAO
// pode seguir com um objeto sem janela.
//
// O tamanho NAO e pedido daqui: quem manda no layout de um VST3 e o IPlugView
// (getSize/canResize), e o backend acompanha o container por conta propria -- no
// Win32 quem cresce depois vira um WM_SIZE no pai, e e o relogio lento que confere
// o cliente dele (ver Win32ForgeUi::seguirContainer()).
ForgeUi* createUi(void* parent, const char* bundlePath, WriteFunction write,
                  void* controller, void** widgetOut);

// Destroi na thread em que foi criado: no Win32 isso nao e formalidade, destruir
// um filho de janela de outra thread trava o processo inteiro.
void destroyUi(ForgeUi* ui);

} // namespace pfui
