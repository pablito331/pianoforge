#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pianoforge {

// Leitor do container .pforge (magic "PFG1"), o formato que packaging/bundle.py
// empacota. Nada aqui e seguro para a thread de audio: so roda no worker do
// host (Bank::load) e na montagem da lista da UI.
//
// Layout: magic(4) version u16 flags u16 ndir u32, entao ndir registros de
// nome variavel, entao os payloads. Cada registro traz o CRC32 do payload
// gravado, e nada e entregue a um sampler sem essa conta bater: bundle
// corrompido tem que virar erro legivel, nao ruido na saida.
class Bundle {
public:
    enum : uint16_t { kRaw = 0, kZlib = 1, kManifest = 2 };

    struct Entry {
        std::string name;      // caminho relativo dentro do bundle
        uint16_t type = kRaw;
        uint64_t off = 0;      // deslocamento do payload no arquivo
        uint64_t size = 0;     // bytes gravados
        uint64_t usize = 0;    // bytes depois de descompactar
        uint32_t crc = 0;
    };

    // manifest.presets[]. O preset de um bundle e um estado completo: nome, arte,
    // banco e os 17 parametros. Nao e o indice que a porta `preset` do plugin toca
    // (esse e o presetindex dentro do SF2), e o que limita o que a UI pode fazer
    // com essa lista.
    struct Preset {
        std::string name;     // "nome"
        std::string image;    // "imagem": caminho do blob dentro do bundle
        std::string sf2;      // "sf2": banco deste preset
        std::vector<std::pair<std::string, float>> params;   // "params": {...}
    };

    // S le cabecalho, diretório e o manifest. Um bundle de gigabyte nao entra
    // inteiro na memoria aqui.
    bool open(const std::string& path, std::string& err);

    const std::vector<Entry>& entries() const noexcept { return entries_; }
    const std::string& manifest() const noexcept { return manifest_; }
    bool isOpen() const noexcept { return !entries_.empty(); }

    // O banco do bundle: o arquivo dito no manifest (presets[].sf2), senao o
    // primeiro entry que parece banco. Devolve nullptr se nao ha.
    const Entry* bankEntry() const noexcept;

    // "produto" do manifest, para a lista da UI mostrar o nome do piano e nao
    // o nome do arquivo. Vazio quando nao ha.
    std::string productName() const;

    // manifest.presets[], na ordem gravada. Lido no open() -- o manifest ja esta
    // na memoria, e a UI precisa dele antes de qualquer banco ser publicado.
    const std::vector<Preset>& presets() const noexcept { return presets_; }

    // O blob de um preset (a arte, descompactada). Falso se o bundle nao tem
    // aquele entry ou se o CRC nao bate.
    bool readBlob(const std::string& name, std::vector<uint8_t>& out, std::string& err) const;

    // Payload ja descompactado. Para entry grande (o SF2 cru) prefira
    // verifyEntry + openEntry, que nao duplicam o banco em RAM.
    bool readEntry(const Entry& e, std::vector<uint8_t>& out, std::string& err) const;

    // Confere o CRC32 do entry lendo-o em janelas de 64 KB. Duas passadas
    // sequenciais num SF2 de 250 MB custam menos que 250 MB de RAM reserva,
    // e e a unica maneira de garantir "bundle corrompido" antes de entregar
    // ao sampler. Nunca na thread de audio.
    bool verifyEntry(const Entry& e, std::string& err) const;

    // FILE posicionado no primeiro byte do payload, com `size` bytes a ler.
    // O chamador fecha. So faz sentido depois de verifyEntry (type kRaw).
    FILE* openEntry(const Entry& e) const;

    // Extrai tudo para um diretorio (o caso do SFZ dentro do bundle, que o
    // sfizz so sabe abrir por caminho). Nomes com ".." ou absolutos sao recusados.
    bool unpackAll(const std::string& dir, std::string& err) const;

    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    std::vector<Entry> entries_;
    std::string manifest_;
    std::vector<Preset> presets_;
};

} // namespace pianoforge
