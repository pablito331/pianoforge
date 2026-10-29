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

# ---- runtime do MinGW -----------------------------------------------------
# O plugin do Windows viaja como DLL solta dentro de um bundle que e copiado por
# pasta, e a maquina de quem usa nao tem o MinGW. Sem runtime estatico, pianoforge.dll
# importa libstdc++-6.dll, libgcc_s_seh-1.dll e libwinpthread-1.dll: o host chama
# LoadLibrary, a dependencia nao esta la e o erro aparece longe do build ("modulo nao
# encontrado"), sem nenhuma pista do que falta. O -static cobre os tres. O MSVC ja
# linka o CRT assim (as api-ms-win-crt-* sao do proprio Windows).
if(MINGW)
    add_link_options(-static)
endif()

# 11.0 e o minimo que serve aos dois bracos do CI: em arm64 o clang recusa um
# deployment target abaixo de 11.0, e nada no backend Cocoa usa API mais nova
# que 10.12 (buttonMask). Sem isto o binario nasce casado com o macOS do runner.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS minimo suportado")
endif()

# Includes
include_directories(
    src/core
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

# Threads: a thread de carga do banco e o runtime dos testes e do standalone.
find_package(Threads REQUIRED)

# Standalone Executable
add_executable(pforge_standalone
    src/standalone/main.cpp
    src/core/AudioOut.cpp          # --live: libasound aberta com dlopen, sem headers
)
target_link_libraries(pforge_standalone PRIVATE pforge_core ${CMAKE_DL_LIBS})

# ---- sfizz fora do repo --------------------------------------------------
# SFZ e a unica dependencia dinamica do plugin, e ela vive fora do repo. O bundle
# do VST3 e instalado por COPIA de pasta, nao por cmake --install, entao sem isto o
# RUNPATH apontaria para a arvore de quem buildou (era o que acontecia: um caminho
# absoluto de build/, valido so na maquina que buildou) e o plugin abriria mudo em
# qualquer outra. $ORIGIN/@loader_path mais a lib copiada do lado do binario e o
# que torna a pasta transportavel.
if(PF_HAVE_SFIZZ)
    if(APPLE)
        set(PFORGE_ORIGIN_RPATH "@loader_path")
    elseif(UNIX)
        set(PFORGE_ORIGIN_RPATH "$ORIGIN")
    else()
        set(PFORGE_ORIGIN_RPATH "")   # Windows: a DLL vai ao lado do .vst3, no PATH do host
    endif()
    file(GLOB PFORGE_SFIZZ_LIBS
         "${SFIZZ_LIB_DIR}/libsfizz.so*"
         "${SFIZZ_LIB_DIR}/libsfizz*.dylib"
         "${SFIZZ_LIB_DIR}/sfizz*.dll")
endif()

# ---- UI ------------------------------------------------------------------
# Duas camadas portaveis, e a divisao e o que permite o mesmo painel nos tres
# sistemas:
#   pforge_ui_common  rasterizador ARGB + fonte + desenho do painel (nada de SO)
#   pforge_ui_ctrl    o controlador portavel: estado, gesto, portas, registro
# A terceira peca e o arquivo de janela do sistema, que o alvo do VST3 compila
# dentro do proprio binario -- nao ha mais modulo de UI separado.
# O pforge_ui_preview linka common + ctrl e renderiza offscreen: e assim que o
# visual e conferido sem abrir host, e por isso o desenho nao pode saber de X11.
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

# O backend de janela escolhido pelo sistema. Ele nao vira modulo separado: nao ha
# mais um arquivo de UI que o host carrega sozinho -- o alvo do VST3 compila o
# backend dentro do proprio binario, e o controlador nao tem #ifdef de SO.
find_package(X11)
set(PFORGE_UI_OK OFF)
if(WIN32)
    set(PFORGE_UI_OK ON)
elseif(APPLE)
    enable_language(OBJCXX)
    set(PFORGE_UI_OK ON)
elseif(X11_FOUND)
    set(PFORGE_UI_OK ON)
else()
    message(STATUS "X11 nao encontrado: UI grafica do plugin desabilitada")
endif()

# As quatro DejaVu viajam no bundle: a UI resolve a fonte pelo bundle_path que
# recebe, e o caminho relativo "assets/ui" nao existe dentro de um host instalado
# (o cwd do DAW e outro). Um host que copie so o binario cai nos diretorios de
# fonte do sistema, em ForgeUi::loadFonts.
set(PFORGE_UI_FONTS
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/DejaVuSans.ttf
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/DejaVuSans-Bold.ttf
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/DejaVuSerif.ttf
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/DejaVuSansMono.ttf
)

# ---- VST3 ----------------------------------------------------------------
# O formato unico de plugin deste projeto, nos tres sistemas. O motor e o painel
# nao sao duplicados: o backend de janela do sistema entra neste modulo, e a
# fabrica do painel (pfui::createUi) e a unica porta de entrada.
#
# O SDK nao e vendorizado (sao tres repositorios e ~40 MB de fonte):
# scripts/get_vst3sdk.ps1|sh busca a tag fixa em third_party/vst3sdk. Sem ele nao
# ha plugin -- e por isso o teste e EXISTS no .tag e nao uma option ligada por
# padrao: quem clonou o repo nao deve ver um erro vermelho antes de rodar o script.
set(PFORGE_VST3_SDK ${CMAKE_CURRENT_SOURCE_DIR}/third_party/vst3sdk)
option(PF_VST3 "Shell VST3 (precisa de third_party/vst3sdk)" ON)

if(PF_VST3 AND EXISTS ${PFORGE_VST3_SDK}/.tag AND PFORGE_UI_OK)
    # ${arch}-<so> e o nome que o host espera dentro de Contents/; no macOS o
    # diretorio e MacOS, sem arquitetura, porque o binario pode ser universal.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(_pf_cpu "aarch64")
    else()
        set(_pf_cpu "x86_64")
    endif()
    if(WIN32)
        # Windows nao tem VST3 de 32 bits no ecossistema atual (o SDK 3.8 tambem
        # nao): o diretorio e o mesmo, e um build de 32 bits e um engano de config.
        set(PFORGE_VST3_ARCH_DIR "x86_64-win")
    elseif(APPLE)
        set(PFORGE_VST3_ARCH_DIR "MacOS")
    else()
        set(PFORGE_VST3_ARCH_DIR "${_pf_cpu}-linux")
    endif()

    # Fonte do SDK, item por item. Nao e "tudo que termina em .cpp": vstgui (que
    # so existe em public.sdk/samples), vstpresetfile e vstrepresentation puxam
    # dependencias que este plugin nao usa, e o que nao entra aqui nao linka.
    set(PFORGE_VST3_SDK_SOURCES
        ${PFORGE_VST3_SDK}/pluginterfaces/base/conststringtable.cpp
        ${PFORGE_VST3_SDK}/pluginterfaces/base/coreiids.cpp
        ${PFORGE_VST3_SDK}/pluginterfaces/base/funknown.cpp
        ${PFORGE_VST3_SDK}/pluginterfaces/base/ustring.cpp
        ${PFORGE_VST3_SDK}/base/source/baseiids.cpp
        ${PFORGE_VST3_SDK}/base/source/fbuffer.cpp
        ${PFORGE_VST3_SDK}/base/source/fdebug.cpp
        ${PFORGE_VST3_SDK}/base/source/fdynlib.cpp
        ${PFORGE_VST3_SDK}/base/source/fobject.cpp
        ${PFORGE_VST3_SDK}/base/source/fstreamer.cpp
        ${PFORGE_VST3_SDK}/base/source/fstring.cpp
        ${PFORGE_VST3_SDK}/base/source/timer.cpp
        ${PFORGE_VST3_SDK}/base/source/updatehandler.cpp
        # FLock (usado por fobject/updatehandler) mora em base/thread, que e um
        # repositorio separado no SDK e nao entra por "base/source".
        ${PFORGE_VST3_SDK}/base/thread/source/fcondition.cpp
        ${PFORGE_VST3_SDK}/base/thread/source/flock.cpp
        ${PFORGE_VST3_SDK}/public.sdk/source/common/commoniids.cpp
        ${PFORGE_VST3_SDK}/public.sdk/source/common/commonstringconvert.cpp
        ${PFORGE_VST3_SDK}/public.sdk/source/common/memorystream.cpp
        ${PFORGE_VST3_SDK}/public.sdk/source/common/pluginview.cpp
        ${PFORGE_VST3_SDK}/public.sdk/source/main/moduleinit.cpp
        ${PFORGE_VST3_SDK}/public.sdk/source/main/pluginfactory.cpp
        ${PFORGE_VST3_SDK}/public.sdk/source/vst/vstaudioeffect.cpp
        ${PFORGE_VST3_SDK}/public.sdk/source/vst/vstbus.cpp
        ${PFORGE_VST3_SDK}/public.sdk/source/vst/vstcomponent.cpp
        ${PFORGE_VST3_SDK}/public.sdk/source/vst/vstcomponentbase.cpp
        # vsteditcontroller.cpp NAO entra na lista, de proposito:
        # vstsinglecomponenteffect.cpp ja o inclui (com os #define que separam
        # IComponent::setState de IEditController::setEditorState). Compilar os
        # dois da "multiple definition" de meia duzia de simbolos do EditController.
        ${PFORGE_VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp
        ${PFORGE_VST3_SDK}/public.sdk/source/vst/vstparameters.cpp
        ${PFORGE_VST3_SDK}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
    )

    # A entrada de modulo e uma por sistema: DllMain no Windows, bundleEntry no
    # Linux, macmain no macOS. Todas preenchem o caminho do bundle, que e de onde
    # o shell acha o Contents/Resources.
    if(WIN32)
        list(APPEND PFORGE_VST3_SDK_SOURCES ${PFORGE_VST3_SDK}/public.sdk/source/main/dllmain.cpp)
        set(PFORGE_VST3_SUFFIX ".vst3")
    elseif(APPLE)
        list(APPEND PFORGE_VST3_SDK_SOURCES ${PFORGE_VST3_SDK}/public.sdk/source/main/macmain.cpp)
        set(PFORGE_VST3_SUFFIX "")     # Contents/MacOS/PianoForge, sem extensao
    else()
        list(APPEND PFORGE_VST3_SDK_SOURCES ${PFORGE_VST3_SDK}/public.sdk/source/main/linuxmain.cpp)
        set(PFORGE_VST3_SUFFIX ".vst3")
    endif()

    if(WIN32)
        set(PFORGE_UI_BACKEND_SRC src/ui/backend_win32.cpp)
    elseif(APPLE)
        set(PFORGE_UI_BACKEND_SRC src/ui/backend_cocoa.mm)
    else()
        set(PFORGE_UI_BACKEND_SRC src/ui/backend_x11.cpp)
    endif()

    add_library(pianoforge_vst3 MODULE
        src/vst3/Entry.cpp
        src/vst3/PianoForgeVst3.cpp
        ${PFORGE_VST3_SDK_SOURCES}
        ${PFORGE_UI_BACKEND_SRC}
    )
    set_target_properties(pianoforge_vst3 PROPERTIES
        OUTPUT_NAME "PianoForge"
        PREFIX ""
        SUFFIX "${PFORGE_VST3_SUFFIX}"
    )
    target_include_directories(pianoforge_vst3 PRIVATE
        ${PFORGE_VST3_SDK}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    # A versao do binario sai do project(): o host mostra
    # este numero no gerenciador de plugins, e um literal morto aqui divergiria do
    # zip do release.
    target_compile_definitions(pianoforge_vst3 PRIVATE PFORGE_VST3_VERSION="${PROJECT_VERSION}")
    target_link_libraries(pianoforge_vst3 PRIVATE pforge_core pforge_ui_ctrl Threads::Threads)

    if(WIN32)
        # As mesmas do backend: shell32 e o SHBrowseForFolderW do "Adicionar pasta".
        target_link_libraries(pianoforge_vst3 PRIVATE user32 gdi32 comdlg32 ole32 shlwapi shell32)
    elseif(APPLE)
        target_compile_options(pianoforge_vst3 PRIVATE -fobjc-arc)
        target_link_libraries(pianoforge_vst3 PRIVATE "-framework Cocoa" "-framework CoreGraphics")
    else()
        target_include_directories(pianoforge_vst3 PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(pianoforge_vst3 PRIVATE ${X11_LIBRARIES} ${X11_Xext_LIB}
                                                        ${CMAKE_DL_LIBS})
    endif()

    # RPATH da sfizz: um caminho de build/ absoluto faz o plugin abrir mudo em
    # qualquer outra maquina.
    if(PF_HAVE_SFIZZ)
        set_target_properties(pianoforge_vst3 PROPERTIES
            BUILD_RPATH "${PFORGE_ORIGIN_RPATH}"
            INSTALL_RPATH "${PFORGE_ORIGIN_RPATH}")
    endif()

    # Bundle: PianoForge.vst3/Contents/<arch>/PianoForge.vst3 + Resources. A
    # montagem e no build/ e nao em dist/: o release empacota a pasta, e um host
    # apontado para build/vst3 ja encontra o plugin montado.
    set(PFORGE_VST3_BUNDLE ${CMAKE_BINARY_DIR}/vst3/PianoForge.vst3)
    set(PFORGE_VST3_BIN_DIR ${PFORGE_VST3_BUNDLE}/Contents/${PFORGE_VST3_ARCH_DIR})

    if(APPLE)
        # O macOS nao carrega um .vst3 sem Info.plist: e o plist que diz o
        # CFBundleExecutable a procurar dentro de Contents/MacOS.
        file(WRITE ${PFORGE_VST3_BUNDLE}/Contents/Info.plist
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
"<plist version=\"1.0\">\n"
"<dict>\n"
"  <key>CFBundleExecutable</key><string>PianoForge</string>\n"
"  <key>CFBundleIdentifier</key><string>audio.pianoforge.vst3</string>\n"
"  <key>CFBundleName</key><string>PianoForge</string>\n"
"  <key>CFBundleDisplayName</key><string>PianoForge</string>\n"
"  <key>CFBundlePackageType</key><string>BNDL</string>\n"
"  <key>CFBundleSignature</key><string>PFRG</string>\n"
"  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>\n"
"  <key>CFBundleShortVersionString</key><string>${PROJECT_VERSION}</string>\n"
"  <key>CFBundleVersion</key><string>${PROJECT_VERSION}</string>\n"
"  <key>NSHighResolutionCapable</key><true/>\n"
"  <key>NSPrincipalClass</key><string>NSApplication</string>\n"
"</dict>\n"
"</plist>\n")
    endif()

    add_custom_command(TARGET pianoforge_vst3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PFORGE_VST3_BIN_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:pianoforge_vst3> ${PFORGE_VST3_BIN_DIR}/
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PFORGE_VST3_BUNDLE}/Contents/Resources
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PFORGE_UI_FONTS}
            ${PFORGE_VST3_BUNDLE}/Contents/Resources/
    )

    if(PF_HAVE_SFIZZ)
        foreach(_sfizz_lib IN LISTS PFORGE_SFIZZ_LIBS)
            add_custom_command(TARGET pianoforge_vst3 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_sfizz_lib}
                    ${PFORGE_VST3_BIN_DIR}/
            )
        endforeach()
    endif()

    # Host VST3 minimo: carrega o bundle pelo mesmo caminho de um DAW
    # (GetPluginFactory -> createInstance -> initialize) e prova audio, estado e
    # painel. Sem ele o "build passou" so diz que compilou.
    add_executable(pforge_vst3_probe tools/vst3_probe.cpp
        # Os IID (IComponent::iid e companhia) sao simbolos de verdade, nao
        # constantes de cabecalho: sem este .cpp o probe linka e morre no
        # queryInterface em tempo de execucao -- ou nem linka.
        ${PFORGE_VST3_SDK}/public.sdk/source/vst/vstinitiids.cpp
    )
    target_include_directories(pforge_vst3_probe PRIVATE
        ${PFORGE_VST3_SDK}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    )
    target_link_libraries(pforge_vst3_probe PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
    if(WIN32)
        target_link_libraries(pforge_vst3_probe PRIVATE user32 gdi32)
    endif()
    add_dependencies(pforge_vst3_probe pianoforge_vst3)
elseif(PF_VST3 AND NOT EXISTS ${PFORGE_VST3_SDK}/.tag)
    message(STATUS "VST3 SDK ausente: nao ha plugin neste build (o SDK nao e vendorizado). "
                   "scripts/get_vst3sdk.ps1 (Windows) ou scripts/get_vst3sdk.sh (Unix) busca.")
endif()

# ---- verificacao ---------------------------------------------------------
# Preview offscreen do painel da forja: escreve PPM (magick x.ppm x.png) para eu
# conferir o visual sem abrir host nenhum.
add_executable(pforge_ui_preview tools/ui_preview.cpp src/core/Bundle.cpp)
target_link_libraries(pforge_ui_preview PRIVATE pforge_ui_common pforge_miniz)

# Custo por estagio da cadeia de timbre, por chamada e por amostra. Existe porque
# o teste G dizia "0,7 ms num bloco denso" sem dizer ONDE, e a resposta nao esta
# no trabalho por amostra: esta no custo FIXO de chamar cada estagio uma vez por
# fronteira de fatia.
add_executable(pforge_dsp_stages tools/dsp_stages.cpp)
target_include_directories(pforge_dsp_stages PRIVATE src)
