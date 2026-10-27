#pragma once
// dlopen portavel para os harnesses de verificacao.
//
// Em Linux e macOS e o <dlfcn.h> de sempre. No Windows o mingw-w64 nao tem
// <dlfcn.h> nem dl*, e sem isto o pforge_host_test -- a UNICA prova de que o load
// de um banco nunca roda dentro de run(), e de que nada e carregado sem pedido --
// simplesmente nao compilaria la, e o CI publicaria o .dll sem uma medida de audio
// nenhuma. Vinte linhas de shim valem mais que uma suíte pulada.
//
// So o que os harnesses usam: abrir, achar um simbolo, fechar, e o ultimo erro.

#if defined(_WIN32)

#include <windows.h>

#include <string>

// Os harnesses passam RTLD_NOW/RTLD_LOCAL; no Win32 o loader nao tem essa ideia de
// visibilidade, entao os dois viram no-op e a flags relevante e a do LoadLibraryEx.
#define RTLD_NOW 0u
#define RTLD_LAZY 0u
#define RTLD_LOCAL 0u
#define RTLD_GLOBAL 0u

inline void* dlopen(const char* path, unsigned) {
    if (!path) return nullptr;
    // LOAD_WITH_ALTERED_SEARCH_PATH: o .dll do plugin pode pedir o libsfizz.dll que
    // mora AO LADO dele no bundle, e sem esta flag o loader procura no PATH do host e
    // nao no do .dll -- o mesmo problema que o RPATH/$ORIGIN resolve nos outros dois.
    return static_cast<void*>(
        LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

inline void* dlsym(void* handle, const char* name) {
    if (!handle || !name) return nullptr;
    return static_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
}

inline int dlclose(void* handle) {
    if (!handle) return 0;
    return FreeLibrary(static_cast<HMODULE>(handle)) ? 0 : 1;
}

inline const char* dlerror() {
    static std::string last;
    const DWORD code = GetLastError();
    if (!code) { last.clear(); return nullptr; }
    char buf[256];
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, buf, DWORD(sizeof(buf) - 1), nullptr);
    buf[n] = '\0';
    last = "GetLastError " + std::to_string(unsigned(code)) + ": " + buf;
    SetLastError(0);
    return last.c_str();
}

#else

#include <dlfcn.h>

#endif
