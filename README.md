# PianoForge

**PT** — Plugin LV2 de piano com cadeia de timbre própria (martelo, ressonância
simpática, EQ de tampa, chorus, reverb), painel desenhado do zero em "forja de
pianos", e um formato de empacotamento (`.pforge`) para distribuir um banco com
nome, arte e preset.

**EN** — An LV2 piano plugin with its own timbre chain (hammer noise, sympathetic
resonance, lid EQ, chorus, reverb), a hand-drawn dark UI, and a `.pforge` container
for shipping a bank together with a name, artwork and preset list. Backends:
TinySoundFont for SF2, sfizz for SFZ/SF3 (optional).

---

## As duas regras que mandam no plugin

1. **Nada é carregado sozinho.** O plugin nasce mudo: `instantiate()` não toca disco
   nenhum, e um valor que o host *restaure* de uma sessão salva não dispara load.
   Você escolhe o piano, e a escolha acontece fora da thread de áudio.
2. **A lista só tem o que você registrou — e na primeira execução ela está vazia.**
   Um caminho absoluto por linha, ordem de inserção preservada, em
   `~/.config/pianoforge/soundfonts.list` (Linux e macOS) ou
   `%APPDATA%\pianoforge\soundfonts.list` (Windows) — `XDG_CONFIG_HOME` manda quando
   existe, e é com ele que os testes montam um registro de mentira. Não há varredura
   de pasta e não há soundfont dentro do bundle do plugin.

Consequência aceita, e deliberada: **uma sessão salva abre muda até você escolher de
novo.**

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Isso produz `build/pianoforge.lv2/` (o plugin, a UI e as fontes) e os binários de
verificação em `build/`. Requisitos: CMake ≥ 3.16, compilador C++17.

| Dependência | Para quê | Sem ela |
|---|---|---|
| sfizz (opcional) | backend `.sfz` / `.sf3` | o plugin continua tocando `.sf2` e `.pforge` com SF2 dentro |
| X11 (só Linux) | a janela da UI | o plugin carrega mudo, sem painel |

No Windows e no macOS a janela não depende de nada de terceiro: GDI+User32 e Cocoa vêm
com o sistema, e o painel por baixo deles é o mesmo `drawPanel()` portável.

Para apontar um sfizz instalado fora do caminho padrão: `-DSFIZZ_DIR=/prefixo`.

### Instalando no host

```sh
# Linux
cp -r build/pianoforge.lv2 ~/.lv2/
# macOS
cp -r build/pianoforge.lv2 ~/Library/Audio/Plug-Ins/LV2/
# Windows
# copie pianoforge.lv2 para %LOCALAPPDATA%\Programs\Common\LV2\
```

## Usando

Abra o PianoForge num host LV2 (REAPER, Carla, Jalv, …), clique em
**Procurar soundfont…** e escolha um `.sf2`, `.sf3`, `.sfz` ou `.pforge`. O rodapé lê
`FORJANDO` enquanto o worker carrega e `piano na forja` quando o banco está no ar.
Antes disso, silêncio — é a regra 1.

Os dez knobs são volume, curva de dinâmica, ataque do martelo, tampa, graves/médios/
agudos, ressonância simpática, chorus e reverb. Arraste na vertical, `Shift` para
ajuste fino, duplo clique restaura o default da porta, roda do mouse sobre o knob ou
sobre a pílula de preset.

## O que há neste repositório

```
src/core/          engine, Bank (SF2/SFZ/bundle), DSP, leitor .pforge, fontes da UI
src/lv2/           wrapper LV2: 24 portas, lv2:worker, duas saídas de estado
src/ui/            a janela (X11 / Win32 / Cocoa) sobre o painel portável
src/core/ui/       Surface ARGB, fonte stb_truetype, drawPanel() — sem dependência de SO
src/standalone/    render roteirizado, --live na placa de áudio, teclado MIDI
packaging/         bundle.py (empacotador .pforge) + fixture de teste
tools/             os quatro harnesses de verificação, abaixo
third_party/       miniz, stb, headers LV2 (vendorizados; ver NOTICE)
docs/design/       o protótipo HTML que originou o painel
PIANOFORGE.md      a especificação do produto e as razões das regras
UI-KIT.md          o contrato com o host e as regras da tela, para quem for editar a UI
```

## Verificação (sem DAW)

Nada aqui é "compilou, então funciona": cada regra tem uma medida.

| Ferramenta | O que prova |
|---|---|
| `pforge_host_test` | suíte A–M contra o `.so` instalado: nada auto-carrega, escolher carrega fora da thread de áudio, `-1` silencia, banco inválido não silencia, `delayFrames`, orçamento de bloco em p99.9, custo de troca, RAM, e o pedal de sustain nos três momentos do gesto |
| `pforge_ui_preview` | renderiza o painel real offscreen em PPM — é assim que o visual é julgado sem host |
| `pforge_ui_probe` | janela X11 de verdade com eventos sintéticos (clique, arrasto, teclado, roda, `port_event` injetado) e captura dos pixels |
| `pforge_dsp_stages` | custo por estágio da cadeia, preço de cada fronteira de fatia, anel de sustain por tecla e o abafador por registro |

Os testes que precisam de um soundfont recebem o caminho por `--sf2`, ou geram o seu
por `--makesf2 <N>`: `tools/SynthBank.hpp` escreve um SF2 sintético — senoides em loop,
platô de sustain e bandas de velocidade — no mesmo formato que o `tsf` lê. **O
repositório não distribui banco nenhum** (licença), então é com esse instrumento que o
CI roda as mesmas suítes que você roda em casa; o que só um piano gravado prova
(martelo, feltro, ressonância) continua sendo conferido a ouvido.

## Release automática (Linux, Windows, macOS)

`.github/workflows/release.yml` builda os três em `windows-latest`, `ubuntu-latest` e
`macos-latest` (universal `arm64;x86_64`), roda as cinco provas abaixo em cada um e
publica os artefatos; numa tag `v*`, cria a release com os três pacotes.

| Prova | O que tem de sair |
|---|---|
| **A** `pforge_host_test --makesf2 2` | suíte A–M verde sem nenhum soundfont no runner |
| **B** `pforge_dsp_stages --pedalload` | nenhuma tecla muda com o pedal no ar — retorno ≠ 0 se uma entrar muda |
| **C** `pforge_standalone --bank synth0.sf2` | render não-mudo, zero amostra em teto cheio |
| **D** `pforge_ui_probe --noidle` (Linux, Xvfb) | a janela pinta sem o host chamar `idle()` |
| **E** `pforge_ui_preview` | o painel offscreen continua desenhando nos três |

## Licença

MIT — ver `LICENSE`. Terceiros vendorizados e as fontes estão em `NOTICE`.
