// Fabrica de classes do binario VST3: e o que o host le para descobrir o plugin.
//
// O UID e estavel por contrato: mudar qualquer um dos quatro campos faz o DAW
// tratar isto como OUTRO plugin e a sessao salva perde o piano. Os bytes foram
// escolhidos para serem legiveis a olho num editor de binario do .vst3.
//
// Licenca: o SDK do VST3 e MIT desde a serie 3.8, entao este binario e MIT igual ao
// resto do projeto -- nao ha contrato com a Steinberg nem GPLv3 no caminho. Se a tag
// do SDK descer para antes de 3.8, ai sim isto passa a ser GPLv3 (ver NOTICE).

#include "PianoForgeVst3.hpp"

#include "public.sdk/source/main/pluginfactory.h"

#ifndef PFORGE_VST3_VERSION
#define PFORGE_VST3_VERSION "1.0.0"
#endif

using namespace Steinberg;

static const FUID kPianoForgeProcessorUID(0x50465247, 0x56535433, 0x50463331, 0x30303031);

BEGIN_FACTORY("PianoForge", "https://pianoforge.audio", "mailto:contato@pianoforge.audio",
              PFactoryInfo::kNoFlags)

// Um componente so: o mesmo objeto e IComponent, IAudioProcessor e
// IEditController (SingleComponentEffect), e por isso nao ha segunda classe
// registrada. O host descobre isso por getControllerClassId == kNotImplemented.
DEF_CLASS2(INLINE_UID_FROM_FUID(kPianoForgeProcessorUID), PClassInfo::kManyInstances,
           kVstAudioEffectClass, "PianoForge", 0, "Instrument|Synth", PFORGE_VST3_VERSION,
           kVstVersionString, pianoforge::vst3::PianoForgeProcessor::createInstance)

END_FACTORY
