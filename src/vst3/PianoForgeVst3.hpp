#pragma once
// Shell VST3 do PianoForge: o mesmo motor do LV2, outro contrato de host.
//
// O que muda em relacao a src/lv2/pianoforge_lv2.cpp, e por que:
//   - nao ha lv2:worker. Todo load de banco cai na NOSSA thread de carga, e o
//     "park" do LV2 (pedido estacionado esperando a RAM do banco velho voltar)
//     nao precisa de vigia: o laco da thread e o unico dono.
//   - IPlugView nao tem idle(). O painel nao recebe chamada periodica nenhuma, e
//     por isso a UI do VST3 e empurrada pelo gancho ForgeUi::onHostPoll: sem ele,
//     automacao e estado do banco so apareceriam no proximo clique.
//   - o host restaura o estado e espera som: setState PEDE o load do banco. No
//     LV2 o valor restaurado e linha de base justamente para nao carregar um
//     piano sozinho; aqui o estado foi salvo pela nossa propria getState, entao
//     restaurar e o comportamento certo.
//   - um componente so (SingleComponentEffect): processador + controlador no
//     mesmo objeto, que e o formato recomendado para plugin combinado, e evita
//     uma tabela de mensagens entre metade de audio e metade de UI.
//
// O painel em si nao e duplicado: ele entra por pfui::createUi (UiFactory.hpp),
// a mesma fabrica que o lv2ui_descriptor usa.

#include "Engine.hpp"

// A ORDEM DAQUI IMPORTA. vstsinglecomponenteffect.h faz
// "#define setState setEditorState" antes de incluir ivsteditcontroller.h, para
// que o setState do IComponent e o do IEditController (que colidem no componente
// combinado) tenham nomes diferentes no mesmo TU. Se ivsteditcontroller.h ja
// estiver no guard de include quando este cabecalho chega, o renomeamento nao
// acontece e o GCC acusa "marked override, but does not override" nas linhas 23
// e 24 do proprio cabecalho do SDK. Por isso: um include so, o do SDK.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace pfui {
class ForgeUi;
}

namespace pianoforge {
namespace vst3 {

// Protocolo de portas do painel (ForgeUi.cpp): 3..19 = ParameterId 0..16,
// 20 = banco, 21 = preset, 22/23 = estado. E o que o painel JA fala no LV2, e
// reusa-lo aqui e o que permite um painel so para os dois formatos.
inline constexpr uint32_t kUiPortParamBase = 3;
inline constexpr uint32_t kUiPortBank      = 20;
inline constexpr uint32_t kUiPortPreset    = 21;

// Parametros que no LV2 sao PORTA e aqui precisam ser PARAMETRO, porque no VST3
// e o parametro que entra no estado, na automacao e no painel generico do host.
// 0..16 ficam reservados para os ParameterId do motor.
inline constexpr Steinberg::int32 kBankParamId   = 100;
inline constexpr Steinberg::int32 kPresetParamId = 101;
// Teto de bancos do registro e de presets por banco. Nao e o limite do formato:
// e o passo do parametro discreto (o valor util vem sempre clampado pelo motor).
inline constexpr Steinberg::int32 kMaxBanks   = 255;
inline constexpr Steinberg::int32 kMaxPresets = 1023;

class PianoForgeProcessor final : public Steinberg::Vst::SingleComponentEffect {
public:
    PianoForgeProcessor();
    ~PianoForgeProcessor() override;

    static Steinberg::FUnknown* createInstance(void* /*context*/) {
        return static_cast<Steinberg::Vst::IAudioProcessor*>(new PianoForgeProcessor());
    }

    // ---- IPluginBase ---------------------------------------------------
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;

    // ---- IComponent ----------------------------------------------------
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;

    // ---- IAudioProcessor -----------------------------------------------
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;
    Steinberg::uint32 PLUGIN_API getTailSamples() override;

    // ---- IEditController -----------------------------------------------
    Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID tag,
                                                     Steinberg::Vst::ParamValue value) override;
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override;

    // ---- ponte com o painel (thread da UI, sempre) ----------------------
    // O que o painel escreve: write_() do LV2 chegando com os mesmos numeros.
    void uiWrite(uint32_t port, uint32_t size, uint32_t format, const void* buffer);
    void attachUi(pfui::ForgeUi* ui);
    void detachUi();
    // Empurra para o painel o que mudou (automacao, estado do banco, presets).
    void pushToUi();

    // Caminho do bundle de onde saem as fontes do painel.
    void setBundlePath(std::string path) { bundlePath_ = std::move(path); }
    const std::string& bundlePath() const { return bundlePath_; }

private:
    void loaderLoop();
    void loadNow(int index, const std::string& path);
    void requestBank(int index) noexcept;
    void requestPath(const std::string& path);
    void applyPlain(Steinberg::Vst::ParamID tag, float plain);
    int indexOfPath(const std::string& path);
    float plainOf(Steinberg::Vst::ParamID tag) const;

    Engine engine_;
    double sampleRate_ = 44100.0;
    size_t blockSize_ = 512;

    // Thread de carga: e a nossa unica via de I/O de banco (nao ha worker do
    // host no VST3). jobIndex_ e atomico porque requestBank pode vir da thread
    // de audio (automacao do parametro de banco); jobPath_ so vem do setState,
    // que e thread principal, e por isso tem mutex.
    std::thread loader_;
    std::atomic<bool> loaderRun_{false};
    std::atomic<bool> loadingNow_{false};
    std::atomic<int> jobIndex_{-2};
    std::mutex jobMx_;
    std::string jobPath_;

    // O que esta SOANDO, nao o que foi pedido: e o que o painel mostra.
    std::atomic<int> published_{-1};
    std::atomic<int> publishedPresets_{0};
    std::atomic<bool> loadInFlight_{false};
    std::atomic<bool> loadFailed_{false};
    int currentPreset_ = 0;

    // Painel. Vive na thread principal, como o window dele.
    pfui::ForgeUi* ui_ = nullptr;
    float uiSent_[Param_Count];
    int uiSentState_ = 0;
    int uiSentPresets_ = -1;
    int uiSentBank_ = -2;
    int uiSentPreset_ = -1;
    std::string bundlePath_;
};

} // namespace vst3
} // namespace pianoforge
