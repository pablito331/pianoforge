#include "Engine.hpp"
#include "WavOut.hpp"
#include "AudioOut.hpp"
#include "Bundle.hpp"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <algorithm>

#include <csignal>
#include <atomic>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
// O modo ao vivo e POSIX puro: termios para o raw mode, poll para nao perder o tempo
// do driver, read para a tecla. Um port de ReadConsole no Windows nao ha como
// conferir nesta maquina, e o que o release precisa do standalone la e o render para
// arquivo. RawTerminal, printLiveStatus e runLive estao no mesmo #if, e o --live do
// Windows responde em vez de tocar mudo.
#if !defined(_WIN32)
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_quit{false};
void onSigInt(int) { g_quit.store(true); }

struct LiveCfg {
    std::string device = "default";   // "none" = sem placa; captura no --out
    uint32_t latencyUs = 40000;
    int octave = 4;                   // oitava do 'q'
    int velocity = 100;
    std::string feed;                 // vazio = teclado do terminal
    std::string out;                  // captura WAV (obrigatoria com device=none)
    uint32_t tailMs = 1500;
};

#if !defined(_WIN32)

const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F",
                                    "F#", "G", "G#", "A", "A#", "B"};

std::string noteName(int midi) {
    if (midi < 0 || midi > 127) return std::to_string(midi);
    return std::string(kNoteNames[midi % 12]) + std::to_string(midi / 12 - 1);
}

// Teclado do computador em duas fileiras: z..m uma oitava abaixo de q..p.
// Apertar de novo solta a nota - o terminal nao entrega "chave solta".
int semitoneOfKey(char c) {
    switch (c) {
        case 'z': return -12; case 's': return -11; case 'x': return -10;
        case 'd': return -9;  case 'c': return -8;  case 'v': return -7;
        case 'g': return -6;  case 'b': return -5;  case 'h': return -4;
        case 'n': return -3;  case 'j': return -2;  case 'm': return -1;
        case 'q': return 0;   case '2': return 1;   case 'w': return 2;
        case '3': return 3;   case 'e': return 4;   case 'r': return 5;
        case '5': return 6;   case 't': return 7;   case '6': return 8;
        case 'y': return 9;   case '7': return 10;  case 'u': return 11;
        case 'i': return 12;  case '9': return 13;  case 'o': return 14;
        case '0': return 15;  case 'p': return 16;
        default:  return -999;
    }
}

// "c4:250 e4:250 g4:400" - nota e quantos ms fica apertada; "-" e silencio.
struct FeedEvent {
    size_t frame = 0;
    bool on = true;
    int note = 0;
};

bool parseNoteToken(const std::string& token, int& midiOut) {
    if (token.empty()) return false;
    if (std::all_of(token.begin(), token.end(), ::isdigit)) {
        midiOut = std::stoi(token);
        return midiOut >= 0 && midiOut <= 127;
    }
    std::string t = token;
    std::transform(t.begin(), t.end(), t.begin(), ::tolower);
    int step = 0;
    size_t i = 0;
    switch (t[0]) {
        case 'c': step = 0; break; case 'd': step = 2; break; case 'e': step = 4; break;
        case 'f': step = 5; break; case 'g': step = 7; break; case 'a': step = 9; break;
        case 'b': step = 11; break; default: return false;
    }
    i = 1;
    while (i < t.size() && (t[i] == '#' || t[i] == 'b' || t[i] == '-')) {
        step += (t[i] == '#' ? 1 : -1);
        ++i;
    }
    if (i >= t.size()) return false;
    const int octave = std::stoi(t.substr(i));
    midiOut = 12 * (octave + 1) + step;
    return midiOut >= 0 && midiOut <= 127;
}

std::vector<FeedEvent> parseFeed(const std::string& feed, float sampleRate) {
    std::vector<FeedEvent> events;
    size_t frame = 0;
    std::istringstream in(feed);
    std::string token;
    while (in >> token) {
        std::string head = token;
        uint32_t holdMs = 250;
        const size_t colon = token.find(':');
        if (colon != std::string::npos) {
            head = token.substr(0, colon);
            holdMs = static_cast<uint32_t>(std::stoul(token.substr(colon + 1)));
        }
        const size_t frames = static_cast<size_t>(holdMs * sampleRate / 1000.0);
        if (head == "-" || head.empty()) {
            frame += frames;
            continue;
        }
        int midi = 0;
        if (!parseNoteToken(head, midi)) {
            std::cerr << "[AVISO] evento nao entendido no --feed: " << token << "\n";
            continue;
        }
        events.push_back(FeedEvent{frame, true, midi});
        events.push_back(FeedEvent{frame + frames, false, midi});
        frame += frames;
    }
    std::sort(events.begin(), events.end(),
              [](const FeedEvent& a, const FeedEvent& b) { return a.frame < b.frame; });
    return events;
}

class RawTerminal {
public:
    explicit RawTerminal(bool enable) {
        if (!enable) return;
        if (tcgetattr(STDIN_FILENO, &saved_) != 0) return;
        termios raw = saved_;
        cfmakeraw(&raw);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        active_ = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
    ~RawTerminal() {
        if (active_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
    }
    RawTerminal(const RawTerminal&) = delete;
    RawTerminal& operator=(const RawTerminal&) = delete;

private:
    termios saved_{};
    bool active_ = false;
};

void printLiveStatus(size_t frame, size_t totalFrames, float sampleRate, int octave,
                     bool pedal, const std::set<int>& sounding, float peak,
                     uint64_t underruns, bool animated) {
    std::string notes;
    for (int midi : sounding) notes += (notes.empty() ? "" : " ") + noteName(midi);
    std::string bar(20, '-');
    if (totalFrames) {
        const int filled = static_cast<int>(std::min<size_t>(
            20, frame * 20 / totalFrames));
        bar.assign(filled, '#');
        bar.append(20 - filled, '-');
    }
    // Sem tty nao ha o que "animar": cada estado vira uma linha, que e o que
    // deixa rastro em log de verificacao.
    std::cout << (animated ? "\r  " : "\n  ") << std::fixed << std::setprecision(1)
              << double(frame) / sampleRate << "s [" << bar << "]  oitava " << octave
              << "  pedal " << (pedal ? "ON " : "OFF") << "  notas: "
              << (notes.empty() ? std::string("-") : notes) << "  pico "
              << std::setprecision(1)
              << (peak > 1e-6f ? 20.0f * std::log10(peak) : -120.0f) << " dBFS";
    if (underruns) std::cout << "  underrun " << underruns;
    std::cout << (animated ? "      " : "") << std::flush;
}

int runLive(pianoforge::Engine& engine, float sampleRate, size_t blockSize,
            const LiveCfg& cfg) {
    const bool capture = cfg.device == "none";
    if (capture && cfg.out.empty()) {
        std::cerr << "[ERRO] --device none precisa de --out ARQUIVO.wav para capturar\n";
        return 2;
    }

    pforge::AudioOut out;
    std::string error;
    if (!capture && !out.open(cfg.device, static_cast<uint32_t>(sampleRate),
                              cfg.latencyUs, error)) {
        std::cerr << "[ERRO] " << error << "\n"
                  << "       tente --device plughw:0,0 (direto na placa) ou --device none\n";
        return 1;
    }

    std::vector<float> capL, capR;
    if (capture) {
        // Nada de alocar no laco: o total e conhecido antes de comecar.
        std::vector<FeedEvent> probe = parseFeed(cfg.feed, sampleRate);
        size_t last = probe.empty() ? 0 : probe.back().frame;
        const size_t total = last + static_cast<size_t>(cfg.tailMs * sampleRate / 1000.0);
        capL.assign(total, 0.0f);
        capR.assign(total, 0.0f);
    }

    const std::vector<FeedEvent> feed = parseFeed(cfg.feed, sampleRate);
    const size_t feedEnd = feed.empty() ? 0 : feed.back().frame;
    const size_t totalFrames = feedEnd + static_cast<size_t>(cfg.tailMs * sampleRate / 1000.0);
    const bool scripted = !feed.empty();

    std::map<char, int> held;
    std::set<int> sounding;          // o que esta soando, para a linha de status
    bool pedal = false;
    int octave = cfg.octave;
    float peak = 0.0f;
    const bool tty = isatty(STDOUT_FILENO) != 0;
    bool statusDirty = false;

    std::signal(SIGINT, onSigInt);
    RawTerminal raw(!scripted);

    std::vector<float> blockL(blockSize, 0.0f), blockR(blockSize, 0.0f);
    size_t frame = 0;
    size_t feedCursor = 0;

    if (!scripted) {
        std::cout << "Tocando ao vivo em " << out.device() << "  (q..p e z..m = duas oitavas;"
                  << " aperta de novo = solta)\n"
                  << "espaco = pedal   - / + = oitava   backspace = calar tudo   esc = sair\n\n";
    }

    while (!g_quit.load()) {
        if (scripted && frame >= totalFrames) break;

        // ---- entrada -----------------------------------------------------
        if (scripted) {
            while (feedCursor < feed.size() && feed[feedCursor].frame < frame + blockSize) {
                const FeedEvent& ev = feed[feedCursor];
                const int delay = static_cast<int>(std::min<size_t>(
                    ev.frame >= frame ? ev.frame - frame : 0, blockSize - 1));
                if (ev.on) {
                    engine.handleNoteOn(delay, ev.note, cfg.velocity);
                    sounding.insert(ev.note);
                } else {
                    engine.handleNoteOff(delay, ev.note, 64);
                    sounding.erase(ev.note);
                }
                ++feedCursor;
            }
        } else {
            // timeout 0: quem cadencia o laco e o snd_pcm_writei bloqueando quando o
            // buffer do driver enche (5,8 ms de audio por bloco de 256). Dormir 20 ms
            // aqui derrubaria a entrega para ~12.800 amostras/s e o driver entraria em
            // underrun continuo - o piano tocando em camera lenta.
            pollfd pfd{STDIN_FILENO, POLLIN, 0};
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                char buf[64];
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n <= 0) break;                    // EOF: o terminal fechou
                for (ssize_t i = 0; i < n; ++i) {
                    const char c = buf[i];
                    if (c == 0x03 || c == 0x1b) { g_quit.store(true); break; }
                    const int semi = semitoneOfKey(c);
                    if (semi != -999) {
                        const int midi = std::clamp(12 * (octave + 1) + semi, 0, 127);
                        auto it = held.find(c);
                        if (it != held.end()) {
                            engine.handleNoteOff(0, it->second, 64);
                            sounding.erase(it->second);
                            held.erase(it);
                        } else {
                            engine.handleNoteOn(0, midi, cfg.velocity);
                            sounding.insert(midi);
                            held[c] = midi;
                        }
                        continue;
                    }
                    if (c == ' ') {
                        pedal = !pedal;
                        engine.handleCC(0, 64, pedal ? 127 : 0);
                    } else if (c == '=' || c == '+') {
                        octave = std::min(8, octave + 1);
                    } else if (c == '-' || c == '_') {
                        octave = std::max(0, octave - 1);
                    } else if (c == 0x7f || c == 0x08) {   // backspace: panic
                        for (const auto& kv : held) engine.handleNoteOff(0, kv.second, 64);
                        held.clear();
                        sounding.clear();
                    }
                }
                statusDirty = true;      // a linha de status acompanha a tecla, nao o relogio
            }
        }

        // ---- render + saida ---------------------------------------------
        std::fill(blockL.begin(), blockL.end(), 0.0f);
        std::fill(blockR.begin(), blockR.end(), 0.0f);
        engine.render(blockL.data(), blockR.data(), blockSize);

        for (size_t i = 0; i < blockSize; ++i) {
            peak = std::max(peak, std::max(std::abs(blockL[i]), std::abs(blockR[i])));
        }

        if (capture) {
            for (size_t i = 0; i < blockSize && frame + i < capL.size(); ++i) {
                capL[frame + i] = blockL[i];
                capR[frame + i] = blockR[i];
            }
        } else {
            // Mesma conversao que grava o WAV: o que sai na placa e o que se escuta no arquivo.
            const std::vector<int16_t> pcm = pforge::toInterleavedS16(blockL, blockR);
            if (!out.write(pcm.data(), blockSize, error)) {
                std::cerr << "\n[ERRO] saida de audio: " << error << "\n";
                return 1;
            }
        }

        // No modo feed o resultado e o WAV (ou o dispositivo) e nao uma linha de
        // status: imprimir aqui só polui a saida de verificacao.
        if (!scripted && (statusDirty || frame % (blockSize * 20) == 0)) {
            statusDirty = false;
            printLiveStatus(frame, 0, sampleRate, octave, pedal, sounding, peak,
                            out.underruns(), tty);
        }
        frame += blockSize;
    }

    // Solta o que ficou pendurado antes de derrubar o terminal.
    for (const auto& kv : held) engine.handleNoteOff(0, kv.second, 64);
    held.clear();
    sounding.clear();
    if (pedal) engine.handleCC(0, 64, 0);

    std::cout << "\n";
    if (capture) {
        if (!pforge::writeWavFile(cfg.out, capL, capR, static_cast<uint32_t>(sampleRate))) {
            std::cerr << "[ERRO] nao escreveu " << cfg.out << "\n";
            return 1;
        }
        std::cout << "captura ao vivo: " << cfg.out << "  " << capL.size() / sampleRate
                  << " s  pico " << peak << "\n";
    } else {
        std::cout << "sai da forja (" << frame / sampleRate << " s tocados, "
                  << out.underruns() << " underrun(s))\n";
    }
    return 0;
}

#else   // _WIN32

// Mesma assinatura, mesma saida de codigo, nenhum teclado: o main() nao tem um
// #if no proprio chamada, e quem rodar --live num .exe le o motivo em vez de um
// piano mudo girando a 100% da CPU sem nunca receber uma tecla.
int runLive(pianoforge::Engine&, float, size_t, const LiveCfg&) {
    std::cerr << "[ERRO] --live precisa de termios/poll (Linux e macOS). No Windows o "
                 "standalone renderiza para arquivo: -o saida.wav\n";
    return 2;
}

#endif  // _WIN32

} // namespace

int main(int argc, char* argv[]) {
    std::string bankPath;      // sem --bank nao ha render: nenhum piano e assumido
    int presetIndex = 0;       // --preset: so faz sentido em SF2 com varios presets
    std::string wavOut = "piano_natural_render.wav";
    std::string manifestPath;  // --manifest: so imprime o que o leitor entendeu
    LiveCfg live;              // opcoes do modo ao vivo; so valem com --live
    bool liveMode = false;

    // Lidos de kParameterDefs, nao escritos a mao: o default do painel, do TTL e do
    // standalone ja divergiram antes, e "soa diferente no standalone" e uma caeca
    // que ninguem precisa ter.
    float hammer = pianoforge::kParameterDefs[pianoforge::Param_Hammer].defaultValue;
    float resonance = pianoforge::kParameterDefs[pianoforge::Param_SympRes].defaultValue;
    float lid = pianoforge::kParameterDefs[pianoforge::Param_Lid].defaultValue;
    float reverb = pianoforge::kParameterDefs[pianoforge::Param_Reverb].defaultValue;
    float chorus = pianoforge::kParameterDefs[pianoforge::Param_Chorus].defaultValue;
    float volume = pianoforge::kParameterDefs[pianoforge::Param_Volume].defaultValue;
    // --param SIMBOLO=VALOR, repetivel: cobre os 17 e e assim que se mede se cada
    // knob do painel faz diferenca audivel sem precisar de ouvido (A/B por RMS).
    std::vector<std::string> paramSpecs;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--bank" || arg == "--sf2" || arg == "--sfz") && i + 1 < argc) bankPath = argv[++i];
        else if (arg == "--manifest" && i + 1 < argc) manifestPath = argv[++i];
        else if (arg == "--preset" && i + 1 < argc) presetIndex = std::stoi(argv[++i]);
        else if (arg == "--out" && i + 1 < argc) wavOut = argv[++i];
        else if (arg == "--hammer" && i + 1 < argc) hammer = std::stof(argv[++i]);
        else if (arg == "--resonance" && i + 1 < argc) resonance = std::stof(argv[++i]);
        else if (arg == "--lid" && i + 1 < argc) lid = std::stof(argv[++i]);
        else if (arg == "--reverb" && i + 1 < argc) reverb = std::stof(argv[++i]);
        else if (arg == "--chorus" && i + 1 < argc) chorus = std::stof(argv[++i]);
        else if (arg == "--volume" && i + 1 < argc) volume = std::stof(argv[++i]);
        else if (arg == "--param" && i + 1 < argc) paramSpecs.push_back(argv[++i]);
        else if (arg == "--live") liveMode = true;
        else if (arg == "--device" && i + 1 < argc) live.device = argv[++i];
        else if (arg == "--latency" && i + 1 < argc)
            live.latencyUs = static_cast<uint32_t>(std::stoul(argv[++i])) * 1000u;
        else if (arg == "--octave" && i + 1 < argc) live.octave = std::stoi(argv[++i]);
        else if (arg == "--velocity" && i + 1 < argc) live.velocity = std::stoi(argv[++i]);
        else if (arg == "--feed" && i + 1 < argc) live.feed = argv[++i];
        else if (arg == "--tail" && i + 1 < argc) live.tailMs = static_cast<uint32_t>(std::stoul(argv[++i]));
    }

    // --manifest: da de ver o que o leitor de manifest entendeu sem renderizar nada.
    // E o mesmo codigo que a UI usa pra mostrar nome, arte e params de um .pforge;
    // sem saida propria ele so seria provado de forma indireta.
    if (!manifestPath.empty()) {
        pianoforge::Bundle b;
        std::string err;
        if (!b.open(manifestPath, err)) {
            std::cerr << "[ERRO] " << err << "\n";
            return 1;
        }
        std::cout << "arquivo:  " << manifestPath << "\n";
        std::cout << "produto:  " << (b.productName().empty() ? "(sem nome)" : b.productName())
                  << "\n";
        std::cout << "presets:  " << b.presets().size() << "\n";
        for (size_t i = 0; i < b.presets().size(); ++i) {
            const pianoforge::Bundle::Preset& p = b.presets()[i];
            std::cout << "  [" << i << "] nome=\"" << p.name << "\"  sf2=\"" << p.sf2
                      << "\"  imagem=\"" << p.image << "\"  params=" << p.params.size() << "\n";
            for (const auto& kv : p.params)
                std::cout << "         " << kv.first << " = " << kv.second << "\n";
            if (!p.image.empty()) {
                std::vector<uint8_t> blob;
                std::string ierr;
                std::cout << (b.readBlob(p.image, blob, ierr)
                                  ? "         blob lido: " + std::to_string(blob.size()) + " bytes\n"
                                  : "         " + ierr + "\n");
            }
        }
        return 0;
    }

    if (bankPath.empty()) {
        std::cerr << "uso: " << (argc > 0 ? argv[0] : "pforge_standalone")
                  << " --bank CAMINHO.sf2|CAMINHO.sfz|CAMINHO.pforge [--preset N]\n"
                  << "     [--out render.wav] [--hammer V] [--resonance V] [--lid V]\n"
                  << "     [--reverb V] [--chorus V] [--volume V]\n\n"
                  << "  --live              toca no teclado do computador em vez de renderizar\n"
                  << "  --manifest X.pforge imprime o manifest lido e sai (sem audio)\n"
                  << "  --device NOME       ALSA: default | plughw:0,0 | none (grava no --out)\n"
                  << "  --latency MS        alvo de latencia da placa (default 40 ms)\n"
                  << "  --octave N          oitava do 'q' (default 4)\n"
                  << "  --velocity V        forca da tecla 1..127 (default 100)\n"
                  << "  --feed \"c4:250 -\"   sequencia no lugar do teclado (verificacao)\n"
                  << "  --tail MS           silencio depois do ultimo evento (default 1500)\n\n"
                  << "Sem --bank nao renderiza nada: o plugin nao puxa piano por conta\n"
                  << "propria e o standalone segue a mesma regra.\n";
        return 2;
    }

    std::cout << "==================================================\n"
              << " PianoForge Audio Engine - Standalone & Fidelity \n"
              << "==================================================\n";

    pianoforge::Engine engine;
    const float sampleRate = 44100.0f;
    const size_t blockSize = 256;
    engine.prepare(sampleRate, blockSize);

    engine.getParameters().setValue(pianoforge::Param_Volume, volume);
    engine.getParameters().setValue(pianoforge::Param_Hammer, hammer);
    engine.getParameters().setValue(pianoforge::Param_SympRes, resonance);
    engine.getParameters().setValue(pianoforge::Param_Lid, lid);
    engine.getParameters().setValue(pianoforge::Param_Chorus, chorus);
    engine.getParameters().setValue(pianoforge::Param_Reverb, reverb);

    for (const std::string& spec : paramSpecs) {
        const size_t eq = spec.find('=');
        int id = -1;
        if (eq != std::string::npos) {
            const std::string want = spec.substr(0, eq);
            for (int p = 0; p < pianoforge::Param_Count; ++p)
                if (want == pianoforge::kParameterDefs[p].symbol) { id = p; break; }
        }
        if (id < 0) {
            std::cerr << "[ERRO] --param \"" << spec << "\" nao entende. Simbolos: ";
            for (int p = 0; p < pianoforge::Param_Count; ++p)
                std::cerr << (p ? " " : "") << pianoforge::kParameterDefs[p].symbol;
            std::cerr << "\n";
            return 2;
        }
        engine.getParameters().setValue(id, std::stof(spec.substr(eq + 1)));
    }
    // Sem isto o valor pedido so chega depois da rampa de 20 ms, e um render curto
    // mediria o default em vez do que foi pedido.
    engine.getParameters().prepare(sampleRate);

    std::cout << "Carregando banco: " << bankPath << " ...\n";
    if (engine.loadSoundBank(bankPath)) {
        std::cout << "[OK] Banco carregado com sucesso!\n";
        int pcount = engine.getPresetCount();
        std::cout << "     Presets disponíveis (" << pcount << "):\n";
        for (int p = 0; p < std::min(5, pcount); ++p) {
            std::cout << "       [" << p << "] " << engine.getPresetName(p) << "\n";
        }
    } else {
        std::cerr << "[ERRO] Nao foi possivel carregar o banco " << bankPath << "\n";
        return 1;
    }

    // preset 0 em SFZ nao e uma troca: o backend recusa por nao ter biblioteca,
    // so reclama se o usuario pediu um indice explicitamente.
    if (presetIndex > 0) {
        if (!engine.setPresetIndex(presetIndex)) {
            std::cerr << "[ERRO] preset " << presetIndex << " recusado: o banco tem "
                      << engine.getPresetCount() << " preset(s)"
                      << " (SFZ nao tem biblioteca de presets, e .pforge depende do backend)\n";
            return 1;
        }
    }
    std::cout << "     Preset em uso: [" << presetIndex << "] "
              << engine.getPresetName(presetIndex) << "\n";

    std::cout << "\nParametros de Modelagem Fisica:\n"
              << "  Volume:    " << volume << "\n"
              << "  Hammer:    " << hammer << " (ataque autentico sutil)\n"
              << "  Resonance: " << resonance << " (ressonancia de cordas)\n"
              << "  Lid:       " << lid << " (tampa aberta, transparencia 100%)\n"
              << "  Chorus:    " << chorus << " (desligado, piano acustico puro)\n"
              << "  Reverb:    " << reverb << " (ambience de concerto natural)\n";

    if (liveMode) {
        if (live.device == "none" && live.out.empty()) live.out = wavOut;
        return runLive(engine, sampleRate, blockSize, live);
    }

    // Progressão harmônica de teste (C Maior com sustain aberto e dinâmicas)
    float totalDurationSec = 4.5f;
    size_t totalFrames = static_cast<size_t>(totalDurationSec * sampleRate);

    std::vector<float> audioL(totalFrames, 0.0f);
    std::vector<float> audioR(totalFrames, 0.0f);

    std::vector<float> blockL(blockSize, 0.0f);
    std::vector<float> blockR(blockSize, 0.0f);

    std::cout << "\nRenderizando " << totalDurationSec << "s de audio em 44.1kHz...\n";

    size_t currentFrame = 0;
    while (currentFrame < totalFrames) {
        size_t framesToProcess = std::min(blockSize, totalFrames - currentFrame);

        // Frame 0: Note On Baixo C2 (36) + C3 (48) com pedal de sustain
        if (currentFrame <= 0 && currentFrame + framesToProcess > 0) {
            engine.handleCC(0, 64, 127); // Sustain pedal down
            engine.handleNoteOn(0, 36, 90);
            engine.handleNoteOn(0, 48, 85);
        }
        // Frame ~0.4s: G3 (55)
        size_t f1 = static_cast<size_t>(0.4f * sampleRate);
        if (currentFrame <= f1 && currentFrame + framesToProcess > f1) {
            int delay = static_cast<int>(f1 - currentFrame);
            engine.handleNoteOn(delay, 55, 95);
        }
        // Frame ~0.8s: C4 (60)
        size_t f2 = static_cast<size_t>(0.8f * sampleRate);
        if (currentFrame <= f2 && currentFrame + framesToProcess > f2) {
            int delay = static_cast<int>(f2 - currentFrame);
            engine.handleNoteOn(delay, 60, 100);
        }
        // Frame ~1.2s: E4 (64), G4 (67)
        size_t f3 = static_cast<size_t>(1.2f * sampleRate);
        if (currentFrame <= f3 && currentFrame + framesToProcess > f3) {
            int delay = static_cast<int>(f3 - currentFrame);
            engine.handleNoteOn(delay, 64, 105);
            engine.handleNoteOn(delay, 67, 108);
        }
        // Frame ~2.8s: Release keys
        size_t fRel = static_cast<size_t>(2.8f * sampleRate);
        if (currentFrame <= fRel && currentFrame + framesToProcess > fRel) {
            engine.handleNoteOff(0, 36, 64);
            engine.handleNoteOff(0, 48, 64);
            engine.handleNoteOff(0, 55, 64);
            engine.handleNoteOff(0, 60, 64);
            engine.handleNoteOff(0, 64, 64);
            engine.handleNoteOff(0, 67, 64);
        }
        // Frame ~3.8s: Release pedal
        size_t fPed = static_cast<size_t>(3.8f * sampleRate);
        if (currentFrame <= fPed && currentFrame + framesToProcess > fPed) {
            engine.handleCC(0, 64, 0); // Sustain pedal up
        }

        std::fill(blockL.begin(), blockL.end(), 0.0f);
        std::fill(blockR.begin(), blockR.end(), 0.0f);

        engine.render(blockL.data(), blockR.data(), framesToProcess);

        for (size_t i = 0; i < framesToProcess; ++i) {
            audioL[currentFrame + i] = blockL[i];
            audioR[currentFrame + i] = blockR[i];
        }

        currentFrame += framesToProcess;
    }

    float maxPeak = 0.0f;
    // Contar amostra em teto cheio e o que separa "alto" de "achatado": um pico de
    // 0,999 numa crista passageira nao distorce, 20 amostras seguidas em FS sim.
    size_t fullScale = 0, longestRun = 0, run = 0;
    for (size_t i = 0; i < totalFrames; ++i) {
        maxPeak = std::fmax(maxPeak, std::fabs(audioL[i]));
        maxPeak = std::fmax(maxPeak, std::fabs(audioR[i]));
        const bool fs = std::fabs(audioL[i]) >= 1.0f || std::fabs(audioR[i]) >= 1.0f;
        run = fs ? run + 1 : 0;
        if (fs) ++fullScale;
        longestRun = run > longestRun ? run : longestRun;
    }
    float peakDbfs = (maxPeak > 1e-6f) ? 20.0f * std::log10(maxPeak) : -120.0f;

    std::cout << "Salvando saida em: " << wavOut << "\n";
    if (pforge::writeWavFile(wavOut, audioL, audioR, static_cast<uint32_t>(sampleRate))) {
        std::cout << "[SUCESSO] Audio de alta fidelidade renderizado com exito!\n"
                  << "  Pico maximo: " << maxPeak << " (" << peakDbfs << " dBFS)\n"
                  << "  Teto cheio:  " << fullScale << " amostras, pior rajada "
                  << longestRun << "\n"
                  << "  Arquivo:     " << wavOut << "\n";
        if (fullScale)
            std::cout << "  [AVISO] o master esta achatando em 0 dBFS -- baixe o volume"
                         " do painel ou o trim.\n";
    }

    return 0;
}
