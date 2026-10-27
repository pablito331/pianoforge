#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace pianoforge {

struct SoundbankInfo {
    std::string name;
    std::string path;
    bool exists = false;
    std::uint64_t sizeBytes = 0;
};

// Registro explícito de pianos. A única fonte é ~/.config/pianoforge/soundfonts.list:
// um caminho absoluto por linha, na ordem em que foi adicionado. Nenhuma pasta é
// varrida e nada é semeado no arquivo, então nenhum instrumento aparece na lista sem
// o usuário ter apontado pra ele. Sem o arquivo (ou vazio) a primeira execução é
// silêncio e o painel pede o primeiro soundfont.
class SoundbankManager {
public:
    static SoundbankManager& getInstance() {
        static SoundbankManager instance;
        return instance;
    }

    // Reconstrói a lista a partir do registro. Barato: um stat por linha, sem recursão.
    void scan() {
        std::lock_guard<std::mutex> lock(mutex);
        scanLocked();
    }

    // Registra um soundfont escolhido pelo usuário. Retorna o índice na lista, ou -1.
    int addSoundfont(const std::string& path) {
        std::filesystem::path p(path);
        if (!p.is_absolute()) return -1;

        std::lock_guard<std::mutex> lock(mutex);
        int existing = indexOfPathLocked(path);
        if (existing >= 0) return existing;
        if (!isSoundfont(p)) return -1;

        writeLine(listFile(), p.string());
        banks.push_back(makeInfo(p));
        return static_cast<int>(banks.size()) - 1;
    }

    // Registra vários de uma vez (o seletor de arquivos aceita multi-seleção).
    // Retorna quantos entraram novos.
    int addMany(const std::vector<std::string>& paths) {
        std::lock_guard<std::mutex> lock(mutex);
        int added = 0;
        for (const std::string& p : paths) {
            std::filesystem::path path(p);
            if (indexOfPathLocked(p) < 0 && isSoundfont(path)) {
                writeLine(listFile(), path.string());
                banks.push_back(makeInfo(path));
                ++added;
            }
        }
        return added;
    }

    // Tira do registro (o arquivo no disco não é tocado).
    bool removeAt(int index) {
        std::lock_guard<std::mutex> lock(mutex);
        if (index < 0 || index >= static_cast<int>(banks.size())) return false;
        const std::string doomed = banks[index].path;

        std::vector<std::string> keep;
        for (const std::string& raw : readLines(listFile())) {
            if (raw == doomed) continue;
            keep.push_back(raw);
        }
        rewriteList(keep);
        scanLocked();
        return true;
    }

    // Copia sob lock: a UI (thread do host) e o worker do plugin leem o mesmo
    // singleton em threads diferentes.
    std::vector<SoundbankInfo> list() const {
        std::lock_guard<std::mutex> lock(mutex);
        return banks;
    }

    int getBankCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(banks.size());
    }

    bool getBankAt(int index, SoundbankInfo& out) const {
        std::lock_guard<std::mutex> lock(mutex);
        if (index < 0 || index >= static_cast<int>(banks.size())) return false;
        out = banks[index];
        return true;
    }

    int findByPath(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mutex);
        return indexOfPathLocked(path);
    }

    static std::string listFile() { return configDir() + "/soundfonts.list"; }
    static std::string configDir();

private:
    SoundbankManager() { scan(); }

    void scanLocked() {
        banks.clear();
        for (const std::string& raw : readLines(listFile())) {
            std::filesystem::path p(raw);
            // Linha que e pasta, ou que nao e soundfont, nao e um banco: nao entra na
            // lista nem como item riscado -- riscado e o arquivo que sumiu.
            if (!p.is_absolute() || !isSoundfont(p)) continue;
            appendBank(p);
        }
    }

    int indexOfPathLocked(const std::string& path) const {
        std::error_code ec;
        auto want = std::filesystem::canonical(std::filesystem::path(path), ec);
        if (ec) return -1;
        for (size_t i = 0; i < banks.size(); ++i) {
            std::error_code e2;
            auto have = std::filesystem::canonical(std::filesystem::path(banks[i].path), e2);
            if (!e2 && have == want) return static_cast<int>(i);
        }
        return -1;
    }

    static bool isSoundfont(const std::filesystem::path& p) {
        std::string ext = lower(p.extension().string());
        return ext == ".sf2" || ext == ".sf3" || ext == ".sfz" || ext == ".pforge";
    }

    SoundbankInfo makeInfo(const std::filesystem::path& p) const {
        SoundbankInfo info;
        info.name = p.stem().string();
        info.path = p.string();
        std::error_code ec;
        info.exists = std::filesystem::is_regular_file(p, ec);
        if (info.exists) info.sizeBytes = std::filesystem::file_size(p, ec);
        return info;
    }

    void appendBank(const std::filesystem::path& p) {
        for (const auto& b : banks) {
            if (b.path == p.string()) return;
        }
        banks.push_back(makeInfo(p));
    }

    void rewriteList(const std::vector<std::string>& lines) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(listFile()).parent_path(), ec);
        std::ofstream out(listFile(), std::ios::trunc);
        for (const std::string& l : lines) out << l << "\n";
    }

    static void writeLine(const std::string& file, const std::string& line) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
        std::ofstream out(file, std::ios::app);
        out << line << "\n";
    }

    static std::vector<std::string> readLines(const std::string& file) {
        std::vector<std::string> lines;
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            lines.push_back(line);
        }
        return lines;
    }

    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::vector<SoundbankInfo> banks;
    mutable std::mutex mutex;
};

inline std::string SoundbankManager::configDir() {
    // XDG_CONFIG_HOME vale nos tres sistemas, e nao por pedantismo: e o override com
    // que o harness de teste monta um ~/.config de mentira e nunca encosta no
    // registro real de quem roda.
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/pianoforge";
#if defined(_WIN32)
    // Num host grafico de Windows nao existe HOME: o REAPER e launchado pelo
    // explorador e so traz USERPROFILE/APPDATA. Sem isto o caminho virava
    // "/.config/pianoforge" relativo ao diretorio de trabalho do host, e o piano
    // registrado pelo usuario sumia na proxima abertura (ou era escrito em cima
    // da pasta do projeto). %APPDATA% e onde um usuario Windows procura config.
    if (const char* appdata = std::getenv("APPDATA")) {
        if (*appdata) return std::string(appdata) + "/pianoforge";
    }
    if (const char* profile = std::getenv("USERPROFILE")) {
        if (*profile) return std::string(profile) + "/AppData/Roaming/pianoforge";
    }
    return "./pianoforge";
#else
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.config/pianoforge";
#endif
}

} // namespace pianoforge
