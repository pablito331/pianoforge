cmake_minimum_required(VERSION 3.16)
project(PianoForge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Todo fonte deste repo e UTF-8 com acento em portugues, inclusive dentro de
# literal de string (as mensagens do plugin e da UI sao escritas assim). O MSVC le
# na codepage do runner quando nao recebe /utf-8: os literais saem mojibake na
# janela de log do host e cada arquivo com acento em comentario dispara C4819.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# O sufixo que um host LV2 espera em cada sistema, uma unica vez: vale para o
# plugin, para a UI e para o nome que sai escrito no manifest gerado.
if(WIN32)
    set(PFORGE_LIB_SUFFIX ".dll")
elseif(APPLE)
    set(PFORGE_LIB_SUFFIX ".dylib")
    # 11.0 e o minimo que serve aos dois bracos do CI: em arm64 o clang recusa um
    # deployment target abaixo de 11.0, e nada no backend Cocoa usa API mais nova
    # que 10.12 (buttonMask). Sem isto o binario nasce casado com o macOS do runner.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS minimo suportado")
else()
    set(PFORGE_LIB_SUFFIX ".so")
endif()

# Includes
include_directories(
    src/core
    third_party/lv2/lv2/include
)

# Core DSP & Engine Library
add_library(pforge_core STATIC
    src/core/Engine.cpp
    src/core/Engine.hpp
    src/core/Bundle.cpp
    src/core/Bundle.hpp
    src/core/Parameters.hpp
    src/core/dsp/BiquadFilter.hpp
    src/core/dsp/VelocityCurve.hpp
    src/core/dsp/HammerModel.hpp
    src/core/dsp/SympatheticResonance.hpp
    src/core/dsp/SoundboardEQ.hpp
    src/core/dsp/LidFilter.hpp
    src/core/dsp/Chorus.hpp
    src/core/dsp/Reverb.hpp
    src/core/dsp/MasterStage.hpp
)

# Inflate dos bundles .pforge. Miniz (MIT) entra vendorizado como tsf.h e stb:
# esta maquina tem libz.so mas nao tem zlib.h, e o formato grava entries
# comprimidos quando compensa. So inflate: nada aqui escreve bundle.
add_library(pforge_miniz STATIC third_party/miniz/miniz.c)
target_include_directories(pforge_miniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz)
target_compile_definitions(pforge_miniz PRIVATE MINIZ_NO_ARCHIVE_APIS MINIZ_NO_TIME)
target_link_libraries(pforge_core PUBLIC pforge_miniz)

# ---- sfizz, opcional -----------------------------------------------------
# SF2 e .pforge com SF2 dentro nao dependem de nada de terceiros: tsf.h esta
# vendorizado. SFZ/SF3 passam pelo sfizz, que NAO vendorizamos (e BSD-2, mas sao
# cabecalhos + lib + os submodulos dele, e nenhum runner de CI tem isso instalado).
# O produto entao compila com ou sem: sem, Engine.cpp recusa .sfz/.sf3 com uma
# frase no stderr e o plugin continua um SF2 completo. Aponta a dependencia com
# -DSFIZZ_DIR=<prefixo>, ou construa-a com scripts/get_sfizz.sh.
option(PF_HAVE_SFIZZ "SFZ/SF3 via sfizz (procura cabecalho e biblioteca)" ON)
if(PF_HAVE_SFIZZ)
    find_path(SFIZZ_INCLUDE_DIR NAMES sfizz.hpp
              HINTS ${SFIZZ_DIR} $ENV{SFIZZ_DIR}
              PATH_SUFFIXES include/sfizz include src)
    find_library(SFIZZ_LIBRARY NAMES sfizz libsfizz
                 HINTS ${SFIZZ_DIR} $ENV{SFIZZ_DIR}
                 PATH_SUFFIXES lib lib64)
    if(NOT SFIZZ_INCLUDE_DIR OR NOT SFIZZ_LIBRARY)
        message(STATUS "sfizz nao encontrado: build SEM SFZ/SF3 (SF2 e .pforge seguem normais). "
                       "scripts/get_sfizz.sh constroi a dependencia.")
        set(PF_HAVE_SFIZZ OFF)
    else()
        message(STATUS "sfizz: ${SFIZZ_LIBRARY}")
    endif()
endif()
if(PF_HAVE_SFIZZ)
    target_compile_definitions(pforge_core PUBLIC PF_HAVE_SFIZZ=1)
    target_include_directories(pforge_core PUBLIC ${SFIZZ_INCLUDE_DIR})
    # Um build a partir da arvore do sfizz expoe sfizz.hpp em src/ e os cabecalhos
    # internos em src/external; uma instalacao normal ja vem com tudo num include.
    if(IS_DIRECTORY "${SFIZZ_INCLUDE_DIR}/external")
        target_include_directories(pforge_core PUBLIC "${SFIZZ_INCLUDE_DIR}/external")
    endif()
    target_link_libraries(pforge_core PUBLIC ${SFIZZ_LIBRARY})
    get_filename_component(SFIZZ_LIB_DIR "${SFIZZ_LIBRARY}" DIRECTORY)
endif()

# Fallback loader thread para hosts que nao oferecem lv2:worker
find_package(Threads REQUIRED)

# Standalone Executable
add_executable(pforge_standalone
    src/standalone/main.cpp
    src/core/AudioOut.cpp          # --live: libasound aberta com dlopen, sem headers
)
target_link_libraries(pforge_standalone PRIVATE pforge_core ${CMAKE_DL_LIBS})

# LV2 Plugin (Linux / macOS / Windows)
add_library(pianoforge_lv2 MODULE
    src/lv2/pianoforge_lv2.cpp
)
set_target_properties(pianoforge_lv2 PROPERTIES
    OUTPUT_NAME "pianoforge"
    PREFIX ""
    SUFFIX "${PFORGE_LIB_SUFFIX}"
)
target_link_libraries(pianoforge_lv2 PRIVATE pforge_core Threads::Threads)

# SFZ e a unica dependencia dinamica do plugin, e ela vive fora do repo. Um bundle
# de LV2 e instalado por COPIA de pasta, nao por cmake --install, entao sem isto o
# RUNPATH apontaria para a arvore de quem buildou (era o que acontecia: um caminho
# absoluto de build/, valido so na maquina que buildou) e o plugin abriria mudo em
# qualquer outra. $ORIGIN/@loader_path mais a lib copiada do lado do .so e o que
# torna a pasta transportavel.
if(PF_HAVE_SFIZZ)
    if(APPLE)
        set(PFORGE_ORIGIN_RPATH "@loader_path")
    elseif(UNIX)
        set(PFORGE_ORIGIN_RPATH "$ORIGIN")
    else()
        set(PFORGE_ORIGIN_RPATH "")   # Windows: a DLL vai ao lado do .dll, no PATH do host
    endif()
    set_target_properties(pianoforge_lv2 PROPERTIES
        BUILD_RPATH "${PFORGE_ORIGIN_RPATH}"
        INSTALL_RPATH "${PFORGE_ORIGIN_RPATH}")
    file(GLOB PFORGE_SFIZZ_LIBS
         "${SFIZZ_LIB_DIR}/libsfizz.so*"
         "${SFIZZ_LIB_DIR}/libsfizz*.dylib"
         "${SFIZZ_LIB_DIR}/sfizz*.dll")
endif()

# ---- UI ------------------------------------------------------------------
# Tres camadas, e a divisao e o que permite o mesmo painel nos tres sistemas:
#   pforge_ui_common  rasterizador ARGB + fonte + desenho do painel (nada de SO)
#   pforge_ui_ctrl    o controlador portavel: estado, gesto, portas, registro
#   pianoforge_ui     o arquivo de janela do sistema, e so este
# O pforge_ui_preview linka common + ctrl e renderiza offscreen: e assim que o
# visual e conferido sem host LV2, e por isso o desenho nao pode saber de X11.
add_library(pforge_ui_common STATIC
    src/core/ui/Font.cpp
    src/core/ui/ForgePanel.cpp
    src/core/ui/Image.cpp
)
target_include_directories(pforge_ui_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)

add_library(pforge_ui_ctrl STATIC
    src/ui/ForgeUi.cpp
    src/ui/ForgeUi.hpp
    src/core/Bundle.cpp      # o manifest do .pforge: nome, arte e params
    src/core/SoundbankManager.hpp
)
target_include_directories(pforge_ui_ctrl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
target_link_libraries(pforge_ui_ctrl PUBLIC pforge_ui_common pforge_miniz Threads::Threads)

# O backend de janela escolhido pelo sistema. Cada um exporta o seu
# lv2ui_descriptor; nao ha #ifdef de SO dentro do controlador.
find_package(X11)
set(PFORGE_UI_OK OFF)
if(WIN32)
    add_library(pianoforge_ui MODULE src/ui/backend_win32.cpp)
    # shell32 e o SHBrowseForFolderW do "Adicionar pasta"; sem ele a UI linka e o
    # botao some (nao da erro de compilacao: user32 e comdlg32 quase sempre bastam
    # para o resto do arquivo).
    target_link_libraries(pianoforge_ui PRIVATE pforge_ui_ctrl user32 gdi32 comdlg32
                                                ole32 shlwapi shell32)
    set(PFORGE_UI_OK ON)
elseif(APPLE)
    enable_language(OBJCXX)
    add_library(pianoforge_ui MODULE src/ui/backend_cocoa.mm)
    # -fobjc-arc porque o backend captura um std::shared_ptr dentro de um bloco, e
    # clang so constroi/destroi C++ nao-trivial em bloco com ARC ligado. O arquivo
    # mesmo assim retem/libera a view e a janela a mao (membros
    # __unsafe_unretained), entao continua correto se alguem ligar/desligar o ARC.
    target_compile_options(pianoforge_ui PRIVATE -fobjc-arc)
    target_link_libraries(pianoforge_ui PRIVATE pforge_ui_ctrl
                                                "-framework Cocoa" "-framework CoreGraphics")
    set(PFORGE_UI_OK ON)
elseif(X11_FOUND)
    add_library(pianoforge_ui MODULE src/ui/backend_x11.cpp)
    target_include_directories(pianoforge_ui PRIVATE ${X11_INCLUDE_DIR})
    # Xext e o MIT-SHM (XShmPutImage). Sem ele o backend compila e o blit cai no
    # XPutImage de 2 MB por frame em todo host.
    target_link_libraries(pianoforge_ui PRIVATE pforge_ui_ctrl
                                                ${X11_LIBRARIES} ${X11_Xext_LIB})
    set(PFORGE_UI_OK ON)
else()
    message(STATUS "X11 nao encontrado: UI grafica do plugin desabilitada")
endif()

if(PFORGE_UI_OK)
    set_target_properties(pianoforge_ui PROPERTIES
        OUTPUT_NAME "pianoforge_ui"
        PREFIX ""
        SUFFIX "${PFORGE_LIB_SUFFIX}"
    )
endif()

# ---- manifest.ttl --------------------------------------------------------
# Sufixo e classe de UI sao por sistema, entao o manifest nao pode ser um arquivo
# morto no repo. O tipo da UI e o que o host usa para decidir o handle que manda
# na create(): HWND no Win32, Window no X11, NSView* no Cocoa. Uma UI descrita
# como X11UI num host Windows e aberta com o handle errado, ou ignorada.
set(PFORGE_BINARY "pianoforge${PFORGE_LIB_SUFFIX}")
set(PFORGE_UI_BLOCK "")
set(PFORGE_UI_REF "")
if(PFORGE_UI_OK)
    if(WIN32)
        set(PFORGE_UI_TYPE "ui:WinUI")
    elseif(APPLE)
        set(PFORGE_UI_TYPE "ui:CocoaUI")
    else()
        set(PFORGE_UI_TYPE "ui:X11UI")
    endif()
    set(PFORGE_UI_REF " ;\n    ui:ui <https://pianoforge.audio/plugin#ui>")
    set(PFORGE_UI_BLOCK "
<https://pianoforge.audio/plugin#ui>
    a ${PFORGE_UI_TYPE} ;
    ui:binary <pianoforge_ui${PFORGE_LIB_SUFFIX}> ;
    ui:extensionData ui:idleInterface , ui:showInterface ;
    rdfs:seeAlso <pianoforge.ttl> .")
endif()
# Direto para dentro do bundle: escrito no generate do CMake, um re-configure ja
# atualiza a pasta. Copiar no POST_BUILD deixaria o manifest velho no bundle
# sempre que so o configure mudasse -- a mesma classe de bug do .so da UI.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/lv2/manifest.ttl.in
               ${CMAKE_BINARY_DIR}/pianoforge.lv2/manifest.ttl @ONLY)

# Post-build: Assemble LV2 bundle folder
add_custom_command(TARGET pianoforge_lv2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:pianoforge_lv2>/pianoforge.lv2
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:pianoforge_lv2> $<TARGET_FILE_DIR:pianoforge_lv2>/pianoforge.lv2/
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/src/lv2/pianoforge.ttl $<TARGET_FILE_DIR:pianoforge_lv2>/pianoforge.lv2/
)

# O libsfizz.so do sistema de quem buildou nao acompanha o bundle por default, e
# sem ele o plugin carrega e fica sem SFZ. So entra quando a dependencia foi
# encontrada; na CI (sem sfizz) o bundle sai sem nada e o SF2 segue inteiro.
foreach(_sfizz_lib IN LISTS PFORGE_SFIZZ_LIBS)
    add_custom_command(TARGET pianoforge_lv2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_sfizz_lib} $<TARGET_FILE_DIR:pianoforge_lv2>/pianoforge.lv2/
    )
endforeach()

if(PFORGE_UI_OK)
    add_dependencies(pianoforge_lv2 pianoforge_ui)
    # Copiar no POST_BUILD do proprio plugin nao basta: quando so a UI muda,
    # pianoforge_lv2 nao relinca e o bundle ficava com o .so velho da UI.
    add_custom_command(TARGET pianoforge_ui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/pianoforge.lv2
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:pianoforge_ui> ${CMAKE_BINARY_DIR}/pianoforge.lv2/
        # As quatro DejaVu sao do bundle: a UI resolve o caminho pelo bundle_path
        # que recebe, e um host que instale so o .so cai nos diretorios de fonte do
        # sistema (ForgeUi::loadFonts).
        COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/DejaVuSans.ttf
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/DejaVuSans-Bold.ttf
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/DejaVuSerif.ttf
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/DejaVuSansMono.ttf
            ${CMAKE_BINARY_DIR}/pianoforge.lv2/
    )
endif()

# ---- verificacao ---------------------------------------------------------
# Host LV2 minimo: carrega o plugin via dlopen e prova que nada e carregado
# sem pedido do usuario e que o load roda fora da thread de audio.
add_executable(pforge_host_test tools/host_stub.cpp)
target_link_libraries(pforge_host_test PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
add_dependencies(pforge_host_test pianoforge_lv2)

# Probe de UI: janela X11 real off-screen + captura dos pixels. Sem host LV2
# instalado e o unico jeito de ver se a interface pintou alguma coisa.
# So onde o backend X11 existe: um runner Windows pode ter cabecalhos X11 soltos
# (vcpkg/msys) e o find_package achal-los, e ai o target nasceria para nao linkar
# -- o CI ia quebrar por um motivo que nao e o plugin.
if(X11_FOUND AND NOT WIN32 AND NOT APPLE)
    add_executable(pforge_ui_probe tools/ui_probe.cpp)
    target_include_directories(pforge_ui_probe PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(pforge_ui_probe PRIVATE ${X11_LIBRARIES} ${CMAKE_DL_LIBS} Threads::Threads)
    add_dependencies(pforge_ui_probe pianoforge_ui)
endif()

# Preview offscreen do painel da forja: escreve PPM (magick x.ppm x.png) para eu
# conferir o visual sem precisar de host LV2.
add_executable(pforge_ui_preview tools/ui_preview.cpp src/core/Bundle.cpp)
target_link_libraries(pforge_ui_preview PRIVATE pforge_ui_common pforge_miniz)

# Custo por estagio da cadeia de timbre, por chamada e por amostra. Existe porque
# o teste G dizia "0,7 ms num bloco denso" sem dizer ONDE, e a resposta nao esta
# no trabalho por amostra: esta no custo FIXO de chamar cada estagio uma vez por
# fronteira de fatia.
add_executable(pforge_dsp_stages tools/dsp_stages.cpp)
target_include_directories(pforge_dsp_stages PRIVATE src)
