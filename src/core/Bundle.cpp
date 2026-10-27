#include "Bundle.hpp"

#include "miniz.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace pianoforge {

namespace {

constexpr size_t kHeaderSize = 12;
constexpr size_t kRecordFixed = 30;      // type + off + size + usize + crc
constexpr uint16_t kVersion = 1;
constexpr size_t kMaxName = 512;
constexpr size_t kMaxEntries = 4096;

bool rdU16(const uint8_t* p, size_t n, size_t& i, uint16_t& out) {
    if (i + 2 > n) return false;
    out = uint16_t(p[i]) | uint16_t(p[i + 1]) << 8;
    i += 2;
    return true;
}
bool rdU32(const uint8_t* p, size_t n, size_t& i, uint32_t& out) {
    if (i + 4 > n) return false;
    out = uint32_t(p[i]) | uint32_t(p[i + 1]) << 8 | uint32_t(p[i + 2]) << 16 | uint32_t(p[i + 3]) << 24;
    i += 4;
    return true;
}
bool rdU64(const uint8_t* p, size_t n, size_t& i, uint64_t& out) {
    if (i + 8 > n) return false;
    out = 0;
    for (int b = 0; b < 8; ++b) out |= uint64_t(p[i + b]) << (8 * b);
    i += 8;
    return true;
}

bool looksLikeBank(const std::string& name) {
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".sf2" || ext == ".sf3" || ext == ".sfz";
}

// O manifest e JSON, mas o unico campo que interessa ao plugin e o caminho do
// banco dentro de presets[]. Fazer um parser de JSON para ler duas chaves sairia
// mais caro que a feature: aqui se procura a chave "sf2" e se pega a string
// entre as aspas que vem depois dos dois pontos.
std::string firstStringForKey(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    size_t open = json.find('"', pos + 1);
    if (open == std::string::npos) return {};
    size_t close = json.find('"', open + 1);
    if (close == std::string::npos) return {};
    return json.substr(open + 1, close - open - 1);
}

// ---------------------------------------------------------------------------
// O manifest e JSON. Antes so interessava uma chave ("sf2"); a lista de presets
// inteira passou a interessar quando o painel foi mostrar nome, arte e params.
// Um parser de JSON completo sairia mais caro que a feature, entao o que segue
// reconhece string (com escape), numero e objeto/array por profundidade -- o
// formato do §2 e nada alem dele. Malformado devolve o que conseguiu, nunca lanca.
// ---------------------------------------------------------------------------

void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

// s[i] == '"'. Devolve o conteudo decodificado e avancas i para depois do fechamento.
bool readString(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') { ++i; return true; }
        if (c == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
            if (n == 'n') out.push_back('\n');
            else if (n == 't') out.push_back('\t');
            else if (n == 'r') out.push_back('\r');
            else if (n == 'u' && i + 5 < s.size()) {
                const long cp = std::strtol(s.substr(i + 2, 4).c_str(), nullptr, 16);
                if (cp < 0x80) out.push_back(char(cp));
                else if (cp < 0x800) {
                    out.push_back(char(0xC0 | (cp >> 6)));
                    out.push_back(char(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(char(0xE0 | (cp >> 12)));
                    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(char(0x80 | (cp & 0x3F)));
                }
                i += 6;
                continue;
            }
            else out.push_back(n);
            i += 2;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return false;
}

// Pula um valor JSON inteiro comecando em i (string, numero, objeto ou array por
// profundidade) e o deixa no primeiro byte depois do valor.
bool skipValue(const std::string& s, size_t& i) {
    skipWs(s, i);
    if (i >= s.size()) return false;
    const char c = s[i];
    if (c == '"') { std::string tmp; return readString(s, i, tmp); }
    if (c == '{' || c == '[') {
        int depth = 0;
        bool inStr = false;
        for (; i < s.size(); ++i) {
            const char d = s[i];
            if (inStr) {
                if (d == '\\') ++i;
                else if (d == '"') inStr = false;
                continue;
            }
            if (d == '"') inStr = true;
            else if (d == '{' || d == '[') ++depth;
            else if (d == '}' || d == ']') {
                if (--depth == 0) { ++i; return true; }
            }
        }
        return false;                      // nunca fechou
    }
    if (c == ',' || c == ':') return false;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') ++i;
    return true;
}

// Varre os membros do objeto cujo '{' esta em `begin`. `value` vem decodificado
// para string e cru para numero/objeto/array; `valueBegin` e o indice do valor em
// `s`, que e o que permite descer num objeto aninhado (o "params").
template <class Fn>
bool forEachMember(const std::string& s, size_t begin, const Fn& fn) {
    if (begin >= s.size() || s[begin] != '{') return false;
    size_t after = begin;
    if (!skipValue(s, after)) return false;
    const size_t end = after - 1;          // o '}'
    size_t i = begin + 1;
    while (i < end) {
        skipWs(s, i);
        if (i >= end) break;
        if (s[i] == ',') { ++i; continue; }
        if (s[i] != '"') break;
        std::string key;
        if (!readString(s, i, key)) return false;
        skipWs(s, i);
        if (i >= end || s[i] != ':') return false;
        ++i;
        skipWs(s, i);
        const bool isString = (i < end && s[i] == '"');
        const size_t valueBegin = i;
        std::string value;
        if (isString) { if (!readString(s, i, value)) return false; }
        else {
            if (!skipValue(s, i)) return false;
            value = s.substr(valueBegin, i - valueBegin);
        }
        fn(key, value, isString, valueBegin);
    }
    return true;
}

std::vector<Bundle::Preset> parsePresets(const std::string& json) {
    std::vector<Bundle::Preset> out;
    size_t pos = json.find("\"presets\"");
    if (pos == std::string::npos) return out;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return out;
    ++pos;
    for (;;) {
        skipWs(json, pos);
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] == ',') { ++pos; continue; }
        if (json[pos] != '{') break;
        const size_t obj = pos;
        size_t after = pos;
        if (!skipValue(json, after)) break;

        Bundle::Preset p;
        forEachMember(json, obj,
                      [&](const std::string& key, const std::string& value,
                          bool isString, size_t valueBegin) {
            if (key == "nome" || key == "name") p.name = value;
            else if (key == "imagem" || key == "image") p.image = value;
            else if (key == "sf2") p.sf2 = value;
            else if (key == "params" && !isString && valueBegin < json.size() &&
                     json[valueBegin] == '{') {
                forEachMember(json, valueBegin,
                              [&](const std::string& pk, const std::string& pv,
                                  bool pIsString, size_t) {
                                  if (pIsString) return;
                                  char* stop = nullptr;
                                  const float v = std::strtof(pv.c_str(), &stop);
                                  if (stop != pv.c_str()) p.params.emplace_back(pk, v);
                              });
            }
        });
        out.push_back(std::move(p));
        pos = after;
    }
    return out;
}

bool safeRelativeName(const std::string& name, std::string& err) {
    if (name.empty() || name.front() == '/' || name.front() == '\\') {
        err = "entry com caminho absoluto ou vazio: '" + name + "'";
        return false;
    }
    size_t pos = 0;
    while ((pos = name.find("..", pos)) != std::string::npos) {
        const bool boundary = pos == 0 || name[pos - 1] == '/' || name[pos - 1] == '\\';
        const bool atEnd = pos + 2 == name.size() || name[pos + 2] == '/' || name[pos + 2] == '\\';
        if (boundary && atEnd) { err = "entry com \"..\" no caminho: " + name; return false; }
        pos += 2;
    }
    return true;
}

} // namespace

bool Bundle::open(const std::string& path, std::string& err) {
    path_.clear();
    entries_.clear();
    manifest_.clear();
    presets_.clear();

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) { err = "nao e um arquivo: " + path; return false; }

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "nao abriu: " + path; return false; }

    uint8_t head[kHeaderSize];
    if (std::fread(head, 1, sizeof(head), f) != sizeof(head)) {
        std::fclose(f);
        err = "arquivo curto demais para ser um .pforge";
        return false;
    }
    if (std::memcmp(head, "PFG1", 4) != 0) {
        std::fclose(f);
        err = "magic invalido (nao e .pforge)";
        return false;
    }
    size_t hi = 4;
    uint16_t version = 0, flags = 0;
    uint32_t ndir = 0;
    if (!rdU16(head, sizeof(head), hi, version) ||
        !rdU16(head, sizeof(head), hi, flags) ||
        !rdU32(head, sizeof(head), hi, ndir)) {
        std::fclose(f);
        err = "cabecalho truncated";
        return false;
    }
    (void)flags;
    if (version != kVersion) {
        std::fclose(f);
        err = "versao de bundle nao suportada: " + std::to_string(version);
        return false;
    }
    if (ndir == 0 || ndir > kMaxEntries) {
        std::fclose(f);
        err = "diretorio com " + std::to_string(ndir) + " entradas";
        return false;
    }

    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); err = "seek falhou"; return false; }
    const uint64_t fileLen = uint64_t(std::ftell(f));
    if (std::fseek(f, long(kHeaderSize), SEEK_SET) != 0) { std::fclose(f); err = "seek falhou"; return false; }

    for (uint32_t e = 0; e < ndir; ++e) {
        uint8_t two[2];
        if (std::fread(two, 1, 2, f) != 2) { std::fclose(f); err = "diretorio truncado"; return false; }
        const uint16_t nameLen = uint16_t(two[0]) | uint16_t(two[1]) << 8;
        if (nameLen == 0 || nameLen > kMaxName) {
            std::fclose(f);
            err = "nome de entry ilegio na entrada " + std::to_string(e);
            return false;
        }
        std::vector<uint8_t> nm(nameLen);
        if (std::fread(nm.data(), 1, nameLen, f) != nameLen) {
            std::fclose(f);
            err = "nome truncado na entrada " + std::to_string(e);
            return false;
        }
        uint8_t rec[kRecordFixed];
        if (std::fread(rec, 1, kRecordFixed, f) != kRecordFixed) {
            std::fclose(f);
            err = "diretorio truncado na entrada " + std::to_string(e);
            return false;
        }

        Entry ent;
        ent.name.assign(reinterpret_cast<const char*>(nm.data()), nameLen);
        size_t i = 0;
        uint16_t type = 0;
        uint64_t off = 0, size = 0, usize = 0;
        uint32_t crc = 0;
        if (!rdU16(rec, sizeof(rec), i, type) ||
            !rdU64(rec, sizeof(rec), i, off) ||
            !rdU64(rec, sizeof(rec), i, size) ||
            !rdU64(rec, sizeof(rec), i, usize) ||
            !rdU32(rec, sizeof(rec), i, crc)) {
            std::fclose(f);
            err = "registro ilegivel em " + ent.name;
            return false;
        }
        ent.type = type;
        ent.off = off;
        ent.size = size;
        ent.usize = usize;
        ent.crc = crc;

        // Um entry que aponta para fora do arquivo é corrupção ou maldade: o
        // resto do bundle não vale nada depois disso.
        if (type > kManifest) {
            std::fclose(f);
            err = "tipo desconhecido (" + std::to_string(type) + ") em " + ent.name;
            return false;
        }
        if (size == 0 || off < kHeaderSize || size > fileLen || off + size > fileLen) {
            std::fclose(f);
            err = "payload fora do arquivo em " + ent.name;
            return false;
        }
        if (type == kZlib && usize < size) {
            std::fclose(f);
            err = "tamanho descompactado menor que o gravado em " + ent.name;
            return false;
        }
        if (!safeRelativeName(ent.name, err)) {
            std::fclose(f);
            return false;
        }
        entries_.push_back(std::move(ent));
    }
    std::fclose(f);
    path_ = path;   // readEntry le por path_, e o manifest logo abaixo ja usa

    // O manifest entra inteiro (é texto pequeno); sem ele o bundle ainda funciona,
    // só fica sem nome de produto.
    const Entry* m = nullptr;
    for (const Entry& e : entries_) if (e.type == kManifest) { m = &e; break; }
    if (m) {
        std::vector<uint8_t> bytes;
        std::string merr;
        if (readEntry(*m, bytes, merr)) {
            manifest_.assign(bytes.begin(), bytes.end());
        } else {
            std::cerr << "[PianoForge] manifest ilegivel em " << path << ": " << merr << "\n";
        }
    }
    presets_ = parsePresets(manifest_);

    return true;
}

const Bundle::Entry* Bundle::bankEntry() const noexcept {
    if (entries_.empty()) return nullptr;
    const std::string want = firstStringForKey(manifest_, "sf2");
    if (!want.empty()) {
        for (const Entry& e : entries_) {
            if (e.name == want ||
                (e.name.size() >= want.size() &&
                 e.name.compare(e.name.size() - want.size(), want.size(), want) == 0)) {
                return &e;
            }
        }
    }
    for (const Entry& e : entries_) if (looksLikeBank(e.name)) return &e;
    return nullptr;
}

std::string Bundle::productName() const {
    std::string name = firstStringForKey(manifest_, "produto");
    if (name.empty() || name == "PianoForge") {
        const std::string preset = firstStringForKey(manifest_, "nome");
        if (!preset.empty()) return preset;
    }
    return name;
}

bool Bundle::readBlob(const std::string& name, std::vector<uint8_t>& out, std::string& err) const {
    for (const Entry& e : entries_) {
        if (e.name == name ||
            (e.name.size() > name.size() + 1 &&
             e.name.compare(e.name.size() - name.size() - 1, name.size() + 1, "/" + name) == 0))
            return readEntry(e, out, err);
    }
    err = "bundle nao tem o arquivo '" + name + "'";
    return false;
}

bool Bundle::readEntry(const Entry& e, std::vector<uint8_t>& out, std::string& err) const {
    std::FILE* f = std::fopen(path_.c_str(), "rb");
    if (!f) { err = "nao abriu " + path_; return false; }
    std::vector<uint8_t> stored(size_t(e.size));
    const bool readOk = std::fseek(f, long(e.off), SEEK_SET) == 0 &&
                        std::fread(stored.data(), 1, stored.size(), f) == stored.size();
    std::fclose(f);
    if (!readOk) { err = "leitura do payload de " + e.name + " falhou"; return false; }

    const uint32_t got = uint32_t(mz_crc32(0, stored.data(), size_t(stored.size())));
    if (got != e.crc) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "CRC32 nao bate em %s: esperado %08x, veio %08x",
                      e.name.c_str(), e.crc, got);
        err = buf;
        return false;
    }

    if (e.type != kZlib) {
        out = std::move(stored);
        return true;
    }
    out.assign(size_t(e.usize), 0);
    const size_t did = tinfl_decompress_mem_to_mem(
        out.data(), out.size(), stored.data(), stored.size(), TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (did == size_t(-1)) { err = "inflate falhou em " + e.name; return false; }
    if (did != out.size()) { err = "tamanho descompactado nao bate em " + e.name; return false; }
    return true;
}

bool Bundle::verifyEntry(const Entry& e, std::string& err) const {
    if (e.type != kRaw) { err = e.name + " nao esta gravado cru"; return false; }
    std::FILE* f = std::fopen(path_.c_str(), "rb");
    if (!f) { err = "nao abriu " + path_; return false; }
    std::vector<uint8_t> window(64 * 1024);
    uint32_t crc = 0;
    uint64_t left = e.size;
    bool ok = std::fseek(f, long(e.off), SEEK_SET) == 0;
    while (ok && left > 0) {
        const size_t want = size_t(std::min<uint64_t>(left, window.size()));
        const size_t got = std::fread(window.data(), 1, want, f);
        if (got != want) { ok = false; break; }
        crc = uint32_t(mz_crc32(crc, window.data(), got));
        left -= got;
    }
    std::fclose(f);
    if (!ok) { err = "leitura de " + e.name + " parou no meio"; return false; }
    if (crc != e.crc) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "CRC32 nao bate em %s: esperado %08x, veio %08x",
                      e.name.c_str(), e.crc, crc);
        err = buf;
        return false;
    }
    return true;
}

std::FILE* Bundle::openEntry(const Entry& e) const {
    std::FILE* f = std::fopen(path_.c_str(), "rb");
    if (!f) return nullptr;
    if (std::fseek(f, long(e.off), SEEK_SET) != 0) { std::fclose(f); return nullptr; }
    return f;
}

bool Bundle::unpackAll(const std::string& dir, std::string& err) const {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    if (ec) { err = "nao criou " + dir + ": " + ec.message(); return false; }
    const auto root = std::filesystem::weakly_canonical(std::filesystem::path(dir), ec);
    for (const Entry& e : entries_) {
        std::vector<uint8_t> bytes;
        if (!readEntry(e, bytes, err)) return false;
        auto dest = std::filesystem::path(dir) / e.name;
        auto canon = std::filesystem::weakly_canonical(dest, ec);
        if (ec || !canon.string().rfind(root.string(), 0)) {
            err = "destino fora do diretorio do bundle: " + e.name;
            return false;
        }
        std::filesystem::create_directories(canon.parent_path(), ec);
        std::FILE* f = std::fopen(canon.string().c_str(), "wb");
        if (!f) { err = "nao escreveu " + canon.string(); return false; }
        const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        std::fclose(f);
        if (!wrote) { err = "escrita incompleta em " + canon.string(); return false; }
    }
    return true;
}

} // namespace pianoforge
