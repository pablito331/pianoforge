#pragma once
// Uma porta so para o painel, dois formatos de plugin.
//
// Ate aqui o unico jeito de criar o painel era o lv2ui_descriptor de cada
// backend, e cada backend tinha a sua propria sequencia de "le as features,
// constroi a classe concreta, devolve o handle nativo". O VST3 precisa do mesmo
// painel por outro caminho (IPlugView entregando um HWND/Window/NSView), entao a
// sequencia virou esta funcao, e o LV2 passou a chamar a mesma: um caminho so
// significa que um bug do painel aparece nos dois formatos, e nao so no que eu
// testei naquele dia.
//
// Cada arquivo de plataforma (backend_win32.cpp, backend_x11.cpp,
// backend_cocoa.mm) implementa estas duas funcoes. O alvo do plugin VST3 compila
// exatamente UM deles, entao nao ha #ifdef de sistema dentro do shell do VST3.

#include "ForgeUi.hpp"

namespace pfui {

// parent: handle nativo do container do host, do tipo que ui:parent carrega --
// HWND no Win32, Window no X11, NSView* no Cocoa. nullptr pede janela propria
// (modo flutuante, que e o que o probe usa).
//
// bundlePath: pasta do bundle, de onde saem as fontes e a arte do .pforge. Pode
// ser vazio; ForgeUi::loadFonts cai nos diretorios do sistema.
//
// widgetOut (opcional): recebe o handle nativo do painel, que e o que o host
// embute. Devolve nullptr se a janela nao pode ser criada -- quem chama NAO
// pode seguir com um objeto sem janela.
ForgeUi* createUi(void* parent, const char* bundlePath, LV2UI_Write_Function write,
                  LV2UI_Controller controller, void** widgetOut);

// Destroi na thread em que foi criado: no Win32 isso nao e formalidade, destruir
// um filho de janela de outra thread trava o processo inteiro.
void destroyUi(ForgeUi* ui);

} // namespace pfui
