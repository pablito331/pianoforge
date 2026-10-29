// Renderiza o painel da forja offscreen e escreve PPM (converta com
// `magick x.ppm x.png`). E o olho dentro da UI sem host LV2 instalado: eu leio
// o PNG de volta e julgo fonte, gradiente, arco de brasa e layout.
//
//   ./build/pforge_ui_preview --state loading --value hammer=0.9 --out /tmp/a.ppm
//   ./build/pforge_ui_preview --status ready --presets 592 --preset 7 --selected 1
//   ./build/pforge_ui_preview --page 1 --out /tmp/motor.ppm     # a chapa MOTOR
// `--status` e a porta 22 do plugin (none|loading|ready|failed): sem ela o
// painel mostra o que a UI chuta sozinha, sem canal de status.
//
// O preview so vale como prova se montar o PanelState do mesmo jeito que
// src/ui/ForgeUi.cpp (o controlador): knobs indexados por ParameterId, texts do
// rodape pelas funcoes compartilhadas (identityLine/stateSubLine) e os mesmos
// chips informativos. O renderer e compartilhado de verdade (drawPanel/hitTest),
// mas o preview NAO linka pforge_ui_ctrl: o que ele monta e uma copia do estado.
// Quando os dois divergem, o que eu estou olhando e o preview, nao a UI.

#include "ForgePanel.hpp"
#include "Bundle.hpp"
#include "Parameters.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using namespace pforge;

void writePPM(const char* path, int w, int h, const std::vector<uint32_t>& px) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "nao deu pra escrever %s\n", path); return; }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<unsigned char> row(size_t(w) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t p = px[size_t(y) * w + x];
            row[size_t(x) * 3 + 0] = (p >> 16) & 0xFF;
            row[size_t(x) * 3 + 1] = (p >> 8) & 0xFF;
            row[size_t(x) * 3 + 2] = p & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

std::string fileNameOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool hasExt(const std::string& path, const char* ext) {
    const size_t n = std::strlen(ext);
    if (path.size() < n) return false;
    const size_t base = path.size() - n;
    for (size_t i = 0; i < n; ++i) {
        const char c = path[base + i];
        const char lo = (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
        if (lo != ext[i]) return false;
    }
    return true;
}

// Mesma decisao de rotulo do X11UI (engineNameOf): o chip diz qual backend vai
// rolar, e .pforge depende do que esta dentro dele.
std::string engineNameOf(const std::string& path) {
    if (path.empty()) return {};
    if (hasExt(path, ".sfz") || hasExt(path, ".sf3")) return "SFIZZ";
    if (hasExt(path, ".pforge")) {
        pianoforge::Bundle b;
        std::string err;
        if (b.open(path, err) && !b.presets().empty())
            return hasExt(b.presets().front().sf2, ".sfz") ||
                       hasExt(b.presets().front().sf2, ".sf3") ? "SFIZZ" : "TINYSONIC";
        return "TINYSONIC";
    }
    return "TINYSONIC";
}

// Os chips dos sete informativos, com o DEFAULT do TTL (o preview nao tem host
// mandando port_event, entao "valor vivo" aqui seria mentira).
std::vector<HiddenChip> defaultChips() {
    std::vector<HiddenChip> out;
    for (int pid = pianoforge::Param_ReverbType; pid < pianoforge::Param_Count; ++pid) {
        const auto& def = pianoforge::kParameterDefs[pid];
        if (def.isVisible) continue;
        char buf[48];
        const float v = def.defaultValue;
        switch (pid) {
            case pianoforge::Param_ReverbType: std::snprintf(buf, sizeof(buf), "Sala %d", int(std::lround(v))); break;
            case pianoforge::Param_TrimDb:     std::snprintf(buf, sizeof(buf), "%+.1f dB", v); break;
            case pianoforge::Param_EngineTune: std::snprintf(buf, sizeof(buf), "%.0f ct", v); break;
            default:                           std::snprintf(buf, sizeof(buf), "%.2f", v); break;
        }
        out.push_back({def.symbol, buf});
    }
    return out;
}

} // namespace

// O default de saida nao pode ser "/tmp/...": o Windows nao tem /tmp, e o CI roda
// este preview nos tres sistemas para conferir que o painel continua desenhando.
static std::string defaultOut() {
    std::error_code ec;
    auto base = std::filesystem::temp_directory_path(ec);
    if (ec) base = std::filesystem::current_path(ec);
    if (ec) return "pforge_ui_preview.ppm";
    return (base / "pforge_ui_preview.ppm").string();
}

int main(int argc, char** argv) {
    int w = 1000, h = 520;
    std::string out = defaultOut();
    std::string state = "normal";
    std::string statusSpec;        // vazio = host nunca mandou port_event de estado
    int presetTotal = 0;
    int presetIndex = 0;
    std::string fontsDir = "assets/ui";
    std::vector<std::string> bankSpecs;
    std::string bundlePath;          // --bundle: .pforge de verdade na lista
    std::string filter;              // --filter: o que foi digitado na busca
    std::string toastSpec;           // --toast: aviso vivo no rodape
    int selected = -1;
    int page = 0;                    // --page: 1 = a chapa MOTOR (os sete escondidos)
    float time = 0.0f;
    std::vector<std::string> overrides;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* fallback) { return i + 1 < argc ? argv[++i] : fallback; };
        if (a == "--w") w = std::atoi(next("1000"));
        else if (a == "--h") h = std::atoi(next("520"));
        else if (a == "--out") out = (i + 1 < argc) ? argv[++i] : defaultOut();
        else if (a == "--state") state = next("normal");
        else if (a == "--status") statusSpec = next("ready");
        else if (a == "--presets") presetTotal = std::atoi(next("0"));
        else if (a == "--preset") presetIndex = std::atoi(next("0"));
        else if (a == "--fonts") fontsDir = next("assets/ui");
        else if (a == "--bank") bankSpecs.push_back(next(""));
        else if (a == "--bundle") bundlePath = next("");
        else if (a == "--filter") filter = next("");
        else if (a == "--toast") toastSpec = next("");
        else if (a == "--selected") selected = std::atoi(next("-1"));
        else if (a == "--page") page = std::atoi(next("0"));
        else if (a == "--time") time = std::atof(next("0"));
        else if (a == "--value") overrides.push_back(next(""));
        else { std::fprintf(stderr, "opcao desconhecida: %s\n", a.c_str()); return 2; }
    }

    Fonts fonts;
    if (!fonts.load(fontsDir + "/DejaVuSans.ttf", fontsDir + "/DejaVuSans-Bold.ttf",
                    fontsDir + "/DejaVuSerif.ttf", fontsDir + "/DejaVuSansMono.ttf")) {
        std::fprintf(stderr, "fontes nao carregaram de %s\n", fontsDir.c_str());
        return 2;
    }

    PanelState st;
    st.page = page;
    for (int pid = 0; pid < pianoforge::Param_Count; ++pid) {
        const auto& def = pianoforge::kParameterDefs[pid];
        if (pid >= kKnobCount) continue;
        KnobState& knob = st.knobs[pid];          // INDEXADO POR ParameterId
        knob.port = 3 + pid;
        knob.label = def.shortName;
        knob.fullLabel = def.name;
        knob.tip = def.tip;
        knob.symbol = def.symbol;
        knob.minValue = def.minValue;
        knob.maxValue = def.maxValue;
        knob.defValue = def.defaultValue;
        knob.value = def.defaultValue;
        knob.bipolar = def.minValue < 0.0f;
    }
    for (const std::string& o : overrides) {
        const size_t eq = o.find('=');
        if (eq == std::string::npos) continue;
        const std::string sym = o.substr(0, eq);
        const float v = std::strtof(o.substr(eq + 1).c_str(), nullptr);
        for (int k = 0; k < kKnobCount; ++k) {
            if (st.knobs[k].symbol == sym) st.knobs[k].value = v;
        }
    }
    st.hidden = defaultChips();
    st.filter = filter;

    if (bankSpecs.empty()) {
        bankSpecs = {"Forge Grand|246 MB|1", "Studio Upright|1,6 GB|1", "Felt Grand|832 MB|1",
                    "Stage Piano|1,2 GB|1", "Old Missing Piano|410 MB|0"};
    }
    for (const std::string& spec : bankSpecs) {
        BankRow b;
        size_t p1 = spec.find('|');
        b.name = spec.substr(0, p1);
        if (p1 != std::string::npos) {
            size_t p2 = spec.find('|', p1 + 1);
            const std::string sz = spec.substr(p1 + 1, p2 == std::string::npos
                                                          ? std::string::npos : p2 - p1 - 1);
            double mult = sz.find("GB") != std::string::npos ? 1024.0 * 1024.0 * 1024.0
                      : (sz.find("MB") != std::string::npos ? 1024.0 * 1024.0 : 1024.0);
            std::string num;
            for (char c : sz) { if (c == ',') num += '.'; else if (!(c == ' ')) num += c; }
            b.sizeBytes = uint64_t(std::strtod(num.c_str(), nullptr) * mult);
            if (p2 != std::string::npos) b.exists = spec.substr(p2 + 1) != "0";
        }
        st.banks.push_back(b);
    }
    for (BankRow& b : st.banks) {
        std::string slug;
        for (char c : b.name) slug += (c == ' ' ? '_' : c);
        b.path = "/registrados/" + slug + ".sf2";     // a linha do rodape mostra o arquivo
    }

    // --bundle entra na lista com o nome do manifest e a arte do blob, e assume a
    // selecao: e o mesmo caminho do X11UI (bundleDisplayName + loadBundleArt),
    // replicado aqui pra que o offscreen mostre o que o painel mostra.
    std::vector<std::string> bundleNames;
    if (!bundlePath.empty()) {
        pianoforge::Bundle bd;
        std::string err;
        if (!bd.open(bundlePath, err)) {
            std::fprintf(stderr, "bundle: %s\n", err.c_str());
            return 1;
        }
        for (const pianoforge::Bundle::Preset& p : bd.presets()) bundleNames.push_back(p.name);
        BankRow row;
        row.name = bd.productName();
        if (row.name.empty() && !bd.presets().empty()) row.name = bd.presets().front().name;
        if (row.name.empty()) row.name = fileNameOf(bundlePath);
        row.path = bundlePath;
        row.exists = true;
        std::error_code ec;
        row.sizeBytes = uint64_t(std::filesystem::file_size(bundlePath, ec));
        selected = int(st.banks.size());
        st.banks.push_back(row);
    }

    if (state == "hover") st.hoverKnob = 1;
    if (state == "dragging") { st.dragKnob = 1; st.knobs[1].value = 0.62f; }
    if (state == "loading") {
        st.loading = true;
        st.hoverBank = -2;
        st.loadingName = "Forge Grand (246 MB)";
    }
    if (state == "empty") { st.banks.clear(); st.current = -1; }
    if (state == "normal" && selected < 0) st.hoverBank = 0;

    // `selected` e o CLICADO (state_.current); o que SOA e o publicado. Sao dois
    // estados e a tela precisa mostrar os dois -- foi assim que o "ainda toca o
    // anterior" de uma recusa pode ser conferido sem host.
    st.current = selected;

    // A porta 22 do plugin tem a palavra final sobre o rodape: e isso que esta
    // sendo conferido aqui, nao o chute de tempo que a UI usa sem status.
    if (!statusSpec.empty()) {
        st.haveStatus = true;
        st.plugLoading = statusSpec == "loading";
        st.plugFailed = statusSpec == "failed";
        // "failed" modela o caso real: VOCE clicou em `selected`, mas o que esta
        // soando ainda e o banco anterior -- a recusa nao troca o piano.
        st.published = statusSpec == "ready" ? std::max(0, selected)
                     : statusSpec == "failed" ? std::max(0, selected - 1)
                     : -1;                                     // "none"
        st.loading = st.plugLoading;
        if ((st.plugLoading || st.plugFailed) && selected >= 0 &&
            selected < int(st.banks.size())) {
            st.loadingName = st.banks[selected].name + " (" +
                             formatSize(st.banks[selected].sizeBytes) + ")";
        }
    }
    st.presetTotal = presetTotal;
    st.preset = presetTotal > 0 ? std::clamp(presetIndex, 0, presetTotal - 1) : 0;

    // Arte do manifest: o preset mostrado e o publicado, nao o ultimo clicado
    // (mesma regra de syncSoundingName + loadBundleArt na UI).
    const int pub = st.haveStatus ? st.published : st.current;
    std::string presetFile;
    if (pub >= 0 && pub < int(st.banks.size())) {
        st.activeBankName = st.banks[pub].name;
        st.engineChip = engineNameOf(st.banks[pub].path);
        presetFile = fileNameOf(st.banks[pub].path);
        if (!bundlePath.empty() && st.banks[pub].path == bundlePath) {
            pianoforge::Bundle bd;
            std::string err;
            if (bd.open(bundlePath, err) && !bd.presets().empty()) {
                const auto& pres = bd.presets();
                const pianoforge::Bundle::Preset& pick =
                    pres[size_t(std::clamp(st.preset, 0, int(pres.size()) - 1))];
                std::vector<uint8_t> blob;
                if (!pick.image.empty() && bd.readBlob(pick.image, blob, err))
                    decodeImage(blob, st.art);
            }
        }
    }
    st.instrumentFile = identityLine(st, presetFile, bundleNames);
    st.stateSub = stateSubLine(st);
    if (!toastSpec.empty()) { st.toast = toastSpec; st.toastAge = 0.6f; }

    Surface surf;
    surf.resize(w, h);
    // A mesma conta da UI (artPlateH -> artPlateHeight): a placa entra no layout e
    // encolhe a lista; duas contas diferentes = clique acertando outra linha.
    const Layout l = layoutFor(w, h, artPlateHeight(st.art.w, st.art.h));
    drawPanel(surf, fonts, l, st, time);
    writePPM(out.c_str(), w, h, surf.px);
    std::printf("%s  %dx%d  state=%s  status=%s  banks=%zu  current=%d  published=%d  "
                "presets=%d@%d  filtro=%s  arte=%d\n",
                out.c_str(), w, h, state.c_str(), statusSpec.empty() ? "-" : statusSpec.c_str(),
                st.banks.size(), st.current, st.published, st.presetTotal, st.preset,
                st.filter.empty() ? "-" : st.filter.c_str(), st.art.empty() ? 0 : st.art.w);
    return 0;
}
