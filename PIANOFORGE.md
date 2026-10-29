# PianoForge — Como funciona (guia de arquitetura e base pra UI)

> Plugin LV2 de piano modelado: toca um **SF2/SFZ que você registrou** com um
> backend de amostras (tinysonic ou sfizz) e uma cadeia de DSP própria — martelo,
> ressonância simpática, EQ, tampa, chorus, reverb. Nada é carregado sem você
> escolher. Este doc é a referência de arquitetura e da interface "Forja".
>
> **Vai construir a UI em outro editor (Carla, plugui, Ozugin, web)?** Leia
> `UI-KIT.md` primeiro: ele é extraído deste doc mas só com o que uma UI precisa —
> contrato com o host, as 24 portas com faixa/default, o protocolo de escrita e de
> `port_event`, as regras de produto que uma tela não pode violar, geometria, paleta
> e o caminho de reuso de `drawPanel()` sem tocar em X11. Este arquivo aqui continua
> sendo a razão das regras; `UI-KIT.md` é a especificação.

---

## 1. Visão geral do pipeline

O plugin já lê o bundle `.pforge` (`src/core/Bundle.{hpp,cpp}`): registrar um
`.pforge` no `soundfonts.list` é escolher o piano que está dentro dele.

```
~/.config/pianoforge/soundfonts.list     (registro explícito: 1 linha = 1 piano)
        │  indice na porta soundbank (20), -1 = nenhum piano
        ▼
┌───────────────────────────────┐
│  Bank  (imutável após pronto) │  construído e aquecido NO WORKER
│  ├─ SF2 → tinysonic (tsf)     │  tsf_load_* + max_voices + preset 0
│  ├─ SFZ → libsfizz            │  loadSfzFile = control-thread
│  └─ .pforge → um dos dois     │  Bundle: diretório + CRC32 + inflate
└──────────────┬────────────────┘
               │ publish: std::atomic<Bank*> exchange   (run() só lê rt_)
               ▼
MIDI (atom in) ──▶┌──────────────────────┐
params (3..19) ──▶│  vozes + velCurve    │  velocity layers, loops, pitch
                  └──────────┬───────────┘
                             │ áudio cru L/R
                             ▼
                  ┌──────────────────────┐
                  │  Cadeia DSP Forge    │  hammer → ressonância →
                  │                      │  EQ → lid → chorus → reverb
                  └──────────┬───────────┘
                             ▼
                       áudio out L/R (0,1)
```

O banco velho nunca é fechado na thread de áudio: `tsf_close` de um SF2 grande é
um `munmap` de centenas de MB (medido: 8,5 ms para libertar um banco de 49 MB
dentro de `run()`). Entra na fila de retire e é liberado no próximo turno do
worker, depois de confirmar que o `run()` passou por um bloco sem vê-lo — e o
banco **novo** só é lido do disco depois que essa RAM voltou (ver
"Quanta RAM um banco pede").

O tempo dos eventos é respeitado: `handleNoteOn/NoteOff/CC/PitchBend` colocam o
evento numa fila de array fixo (nada de alocação no RT) e `Engine::render()`
ordena por `delayFrames`, aplica cada lote no instante certo e renderiza o bloco
em fatias. O `tsf` não tem scheduler próprio, então o fatiamento é o que faz uma
nota no meio do bloco soar no meio do bloco; no sfizz o atraso já era interno e
agora chega como 0, sobrando o fatiamento para os dois backends igualmente. O
smoothing dos parâmetros continua por bloco, não por fatia — fatiar não acelera
rampa. Frontias próximas são fundidas (`kMinSlice = 16` amostras): cada frontia
custa uma passada pela cadeia, e um host mandando automação amostra a amostra
produziria 512 fronteiras num bloco de 512; fundidas, o pior caso vira
`numFrames/16` com no máximo 0,3 ms de adiantamento. Medido: bloco com 64 notas +
100 CC cai de 1,34 ms para 0,78 ms de `run()` (o resto é o custo real de renderizar
64 vozes).

O `.pforge` = magic PFG1 + diretório + CRC32 por entry, com `manifest.json`
(produto, `presets[]`, `credits[]`). O leitor (`Bundle`) só abre cabeçalho,
diretório e manifest; o banco cru (entry sem compressão) é lido duas vezes — uma
para fechar o CRC em janelas de 64 KB, outra para entrar no `tsf` por *stream*, na
posição certa do arquivo — o que custa disco sequencial mas não duplica os 250 MB
em RAM. Entry comprimido (zlib) é o único caso que materializa
o payload antes de carregar. SFZ dentro de bundle vai para
`~/.cache/pianoforge/bundles/<nome>/` porque o sfizz só abre por caminho. Nada
disso roda na thread de áudio: é o `work()` do worker. Um entry cujo CRC não bate
é erro legível no stderr e **nada é publicado** — o piano tocando continua tocando
(teste H do `pforge_host_test`).

Ainda projeto: a imagem do piano no painel esquerdo, alimentada pelo `imagem` do
manifest, e os `params` do preset como ponto de partida do painel.

Componentes e onde estão hoje:

| Peça | Status | Onde |
|---|---|---|
| Formato `.pforge` (pack/unpack/verify + CRC) | ✅ pronto e testado | `packaging/bundle.py` |
| leitor `.pforge` no plugin (directório, CRC, inflate, stream para o `tsf`) | ✅ | `src/core/Bundle.{hpp,cpp}` |
| engine sfizz (libsfizz) | opcional — sem ela o build recusa `.sfz`/`.sf3` com uma linha no stderr | `-DPF_HAVE_SFIZZ=ON` (default) + `-DSFIZZ_DIR`, ou `scripts/get_sfizz.sh` |
| headers LV2 | ✅ vendorizados | `third_party/lv2/lv2/include/` |
| wrapper LV2 (24 ports, `lv2:worker`, 2 saídas de estado) | ✅ | `src/lv2/pianoforge_lv2.cpp` |
| backend SF2 (tinysonic `tsf`) + SFZ (sfizz) | ✅ atrás de `struct Bank` | `src/core/Engine.{hpp,cpp}`, `src/core/tsf.h` |
| DSP chain (hammer, ressonância, EQ, tampa, chorus, reverb) | ✅ | `src/core/dsp/` |
| **UI "Forja de Pianos"** | ✅ | `src/core/ui/` (rasterizador + painel), `src/ui/ForgeUi.{hpp,cpp}` (controlador), `src/ui/backend_{x11,win32,cocoa}` (a janela do SO) |
| standalone: render roteirizado + **`--live` na placa de áudio** | ✅ | `src/standalone/main.cpp`, saída em `src/core/AudioOut.{hpp,cpp}` |
| WAV / conversão float→s16 compartilhada | ✅ | `src/core/WavOut.hpp` |

### As duas regras que mandam no resto

1. **Nada é carregado até você escolher.** `instantiate()` não toca disco nenhum;
   `run()` com `soundbank = -1` é silêncio absoluto (nem o martelo, nem a
   ressonância simpática disparam). Um valor que o host *restaure* de uma sessão
   salva é gravado como baseline em `activate()` e **não** dispara load — você
   escolhe de novo. Consequência aceita: sessão salva abre muda.
2. **A lista de pianos só tem o que você registrou — e na primeira execução ela está
   vazia.** `~/.config/pianoforge/soundfonts.list` (Linux/macOS) ou
   `%APPDATA%\pianoforge\soundfonts.list` (Windows), um caminho absoluto por linha, ordem
   de inserção preservada, `#` comenta. `XDG_CONFIG_HOME` tem prioridade nos três
   sistemas, e é por ele que os harnesses montam um registro de mentira sem encostar no
   do usuário — num host gráfico de Windows não existe `HOME` (o REAPER é launchado pelo
   explorador e só traz `USERPROFILE`/`APPDATA`), e sem essa fallback o registro iria
   para o diretório de trabalho do host. Varredura recursiva de pasta não existe mais — foi
   o que fez o plugin abrir carregando `241-Drums.SF2` (o primeiro nome em ordem
   alfabética de 26 arquivos). E o registro **não é semeado**: o plugin não copia para lá
   nada de `soundfonts.conf` (o formato antigo, que listava pastas), nem de
   `custom_soundfonts.list`, nem de `~/.local/share/pianoforge/soundfonts`, nem de dentro
   do próprio bundle — não há piano nenhum no bundle (conferido: só `.so`, `.ttl` e as
   quatro TTF). Arquivo ausente = lista vazia = o painel de véu pedindo o primeiro
   soundfont. Linha que não é `.sf2/.sf3/.sfz/.pforge` (pasta inclusa) não entra na lista
   nem como item riscado: riscado é o arquivo que sumiu. Botões `+ Pasta` importam
   `*.sf2/*.sfz` de **um nível**, uma vez, sem monitorar. `.pforge` é banco como qualquer
   outro.

Trocar de piano não roda I/O na thread de áudio: o `Bank` é construído e
aquecido no worker, publicado por `std::atomic` exchange, e o banco velho é
destruído na thread do worker (`tsf_close` de um banco de 1,2 GB é `munmap` de
dezenas de ms — o dobro do arquivo, ver "Quanta RAM um banco pede").
E como "ter `lv2:worker`" não significa "rodar `work()`" — o REAPER aceita o pedido
e só bombeia o worker com o transporte tocando — a thread de reserva do plugin está
sempre viva e assume o load depois de 2 s sem sinal de vida do host.

### Quanta RAM um banco pede (medido, não estimado)

O número estava escrito como "~1,3 GB residente" desde o plano, e era chute. Medido
com o processo lendo o **próprio** `/proc/self/status` (`tools/dsp_stages.cpp`, e o
pico ao final em `tools/host_stub.cpp`):

| banco | arquivo | RAM depois do `tsf_load` |
|---|---|---|
| piano grande, 62 presets | 246 MB | **496 MB** |
| piano de camadas longas | 612 MB | **1221 MB** |
| banco de 49 MB | 49 MB | 98,2 MB |
| um preset só | 16,3 MB | 38,4 MB |

É ~2× o arquivo, e o motivo é estrutural: o chunk `smpl` de um SF2 é `int16` e o tsf
o aloja como `float32` — `src/core/tsf.h:997` é um único `TSF_MALLOC(count * sizeof(float))`
com a conversão feita no próprio buffer, do fim para o começo; o `rawBuffer` existe só
no caminho vorbis. Reduzir isso é trocar de sampler, não otimizar. Fica sendo o piso
honesto do plugin: um piano de 600 MB pede 1,2 GB de RAM, e é por isso que "consome
recursos" e "trava" eram a mesma reclamação.

**A medição quase mentiu duas vezes.** Um poll externo de `/proc/<pid>/status` a cada
50 ms devolveu *4,5 MB de pico* para um render que existe (`pico=0,922983`): o load
inteiro cabe entre duas amostragens, e `VmHWM` é por processo, não por amostra. O que
desmascarou foi `ulimit -v` — com 262 MB o `pforge_standalone` recusa o banco, com
512 MB ele renderiza. Regra: medir de dentro, ou lascar o limite e ver onde quebra.
Nunca espiar de fora.

**O pico da troca era o dobro do necessário.** `loadBankNow()` media o banco novo com
o velho publicado, ou seja, os dois no ar: no par registrado nesta máquina,
496 + 1221 = **1,7 GB**. Agora ele primeiro `publishBank(nullptr)` (silêncio),
estaciona o pedido e só pega o disco depois que a RAM do velho voltou — o que o `run()`
confirma ao passar por um bloco e o `work()` coleta em `MSG_FREE_OLD`; sem worker do
host, quem trata o aviso é a thread de reserva. Consequências, todas medidas:

- pico do swap **104 MB em vez de 141 MB** no par conferido — os dois números são
  caminhos diferentes do mesmo par de bancos: `pforge_host_test` com dois `--sf2`
  (com o estacionamento) contra `pforge_dsp_stages --sf2 A --sf2b B` (que é exatamente
  "alocar com o outro vivo").
- a troca de piano fica muda por mais ~1 bloco. Preço irrelevante: ela já ia trocar
  de piano, e as vozes em andamento morrem com o banco velho de qualquer forma.
- host que não bomba `work()` não prende nada: o estacionamento tem teto de 300 ms
  (3 ticks da thread de reserva) e ao estourar o load segue com o velho na fila
  (`loadBankNow(index, /*esperarLiberar=*/false)`. Pedido travado em "forjando" pesa
  mais que pico de RAM.

**Um `free()` de 8,5 ms dentro do `run()`.** No caminho sem worker, `pingFreeOld()`
libertava o banco aposentado na própria chamada de áudio. O teste J (novo no
`pforge_host_test`) mede o antes e o depois com o mesmo par de bancos: `maxBlock`
**8,471 ms** no jeito antigo — confirmado mutando a função de volta e rodando o teste,
que reprova — contra **0,509 ms** com o aviso indo para a thread de reserva. Com um
banco de 1,2 GB isso era um xrun a cada troca (não medido: a máquina não tem RAM para
o teste). Duas guardas saíram junto: a fila de retire agora é protegida por um mutex
`retireMx` com a lista *roubada* antes do `delete`, porque dois caminhos não-RT
(`MSG_FREE_OLD` do host e a thread de reserva) podiam libertar a mesma `Bank` — foi
isso que apareceu como `corrupted size vs. prev_size` antes de o dono do retire ficar
único por modo de host.

Duas coisas que a medição **rejeitou**, e que por isso não estão no código:

- *pular o backend em bloco silencioso*. Com zero vozes o `tsf_render_float` escreve
  zeros exatos (conferido: `max|out| = 0,0`, inclusive nos 0,71 s de cauda de uma nota)
  e custa **0,91 µs** por bloco de 512 — 3,70 µs fatiado em 32. O salto foi
  implementado, provado bit-idêntico (`cmp` igual no render do standalone) e
  **revertido**: economiza 0,6 µs num orçamento de 10,6 ms.
- *cortar vozes para economizar CPU*. Com a piscina lotada o `tsf_render_float` custa
  **313 µs** por bloco de 512 em 128 vozes e **623 µs** em 256 (~6% de um núcleo a 44,1 kHz).
  O que o teto **não** muda é RAM: a estrutura de voz são ~200 B e o banco é o mesmo arquivo
  convertido em qualquer teto (medido: 491 MB de RSS de banco nos três). Teto de vozes é
  decisão de CPU, não de memória. `kMaxVoices` é escolha de polifonia em acorde com pedal, e
  o número hoje é **256** — medido, não ouvido. O que a escolha do número
  **não** podia era custar nota muda, e custava: com o pedal no ar, acima do teto o
  `tsf` descartava a tecla em silêncio. Não é mais assim em nenhum teto (seção 4, "O
  pedal de sustain"), e o que o teto decide agora é há quantos segundos uma corda
  pedalizada estava no ar quando o abafador caiu: 3,05 s em 128, 7,62 s em 256.

---

## 2. Formato do bundle `.pforge`

```
offset 0   magic   4B  "PFG1"
offset 4   version u16 (=1) | flags u16 | ndir u32
offset 12  diretório (ndir entries):
    name_len u16 | name utf-8 | type u16 (0=blob, 1=zlib, 2=manifest)
    offset u64 | size u64 | usize u64 | crc32 u32
           payloads (blocos consecutivos)
```

Regras do leitor (`src/core/Bundle.cpp`): nada do bundle é lido na thread de áudio.
O CRC32 de cada entry é conferido antes de entregar ao sampler, e entry fora de
ordem, `size` estourando o arquivo, tipo desconhecido, nome absoluto ou com `..`
⇒ rejeita o bundle inteiro com mensagem no stderr — e como nada é publicado, o
piano que está tocando continua tocando (teste H do `pforge_host_test`).

SF2 dentro do bundle **não** é copiado: depois de fechar o CRC, o `tsf` lê o entry
por `tsf_stream` na posição certa do arquivo. Só SFZ (que o sfizz insiste em abrir
por caminho) é desempacotado para `~/.cache/pianoforge/bundles/<nome>/`.

### manifest.json (formato que a UI lê)

```json
{
  "produto": "PianoForge",
  "versao": "1.0",
  "formato": "pforge/1",
  "presets": [
    {
      "nome": "Concert Grand",
      "imagem": "concert.png",
      "sf2": "bank/grand.sf2",
      "params": {
        "volume": 0.8, "hammer": 0.35, "sympRes": 0.5, "velCurve": 0.5,
        "lid": 0.7, "bassEQ": 0.5, "midEQ": 0.5, "highEQ": 0.5,
        "chorus": 0.1, "reverb": 0.25,
        "reverbType": 1, "trimDb": 0, "width": 1.0, "releaseDamp": 0.35,
        "hammerTone": 0.5, "sympTune": 0.5, "engineTune": 0
      }
    }
  ],
  "credits": [
    { "autor": "...", "licenca": "CC0", "fonte": "url" }
  ]
}
```

- **preset = estado completo**: todos os params (visíveis + ocultos) + sf2 + imagem.
  Trocar de preset reaplica *tudo*, não só os knobs. **Ainda não é o que roda**: o
  leitor parseia os quatro campos e o `--manifest` do standalone imprime os
  `params` um por um (prova de que foram lidos), mas nada os aplica. O motivo é de
  eixo, não de pressa — ver "o que o painel faz com o manifest" abaixo.
- **`imagem` é nome de entry, não caminho.** O empacotador (`packaging/bundle.py`) grava
  cada arquivo pelo *basename* e o próprio `verify` dele recusa referência com `/`.
  `"imagem": "grand.png"` vale; `"img/grand.png"` dá "bundle nao tem o arquivo".
- **`credits` é obrigatório** quando o banco tem licença CC-BY: a atribuição viaja
  dentro do bundle e jamais é removida. Bancos de conteúdo comercial de terceiros
  (ex.: soundfonts de outros plugins) **não** entram em bundle distribuível — só
  estudo local.

### O que o painel faz com o manifest (`src/ui/ForgeUi.cpp`)

Leitura na **thread da UI**, nunca no `run()`: `Bundle::open` pega cabeçalho,
diretório e o manifest (texto pequeno) e a arte são alguns KB descomprimidos.

| campo | o que vira na tela |
|---|---|
| `produto` | o nome na lista de pianos e no rodapé — não o nome do arquivo |
| `presets[].nome` | a linha miúda do rodapé, **só** quando `count(presets[]) == presetCount` que o plugin publicou |
| `presets[].imagem` | a placa de arte embaixo da lista, decodificada por `stb_image` (PNG/JPEG) e composta com `Surface::blendImage` |
| `presets[].params` | lidos e impressos pelo `--manifest`; **nenhum** efeito na tela |

Por que o nome do preset é condicionado à contagem: o índice que o plugin troca é o
`presetindex` **dentro do SF2**, e o `presets[]` do manifest é outro eixo (cada item
pode apontar um `sf2` diferente). Casar os dois sem a conta bater seria mentir no
rodapé — um bundle de piano único com um SF2 de 62 presets mostraria o nome errado
em 61 dos 62. Quando as contas não batem, o rodapé volta a dizer o arquivo.

Por que os `params` não são aplicados: eles chegariam ao plugin por portas de
`float` que o host grava e automatiza. Escrever neles de dentro da UI sem o host
pedir é exatamente a classe de bug que este projeto caçou (valor na tela ≠ valor no
plugin). O caminho honesto é `lv2:state` ou um preset do próprio host; fica no §8c.


---

## 3. Modelo de parâmetros (o coração da UI)

Conceito: **o preset define o estado inteiro; a UI mostra só um subconjunto.**
Todo param é uma LV2 control port (logo: automatable e salvo pelo DAW), mas cada um
tem uma **categoria** no TTL do plugin:

- **`knob`** — aparece na UI como knob editável
- **`hidden`** — não aparece; valor vem do preset e muda só via automação/troca de preset

### Índice das 24 ports (é isto que o host grava; nunca reordenar)

| índice | direção | o que é |
|---|---|---|
| 0, 1 | out | áudio L / R |
| 2 | in | MIDI (`atom:AtomPort`, `midi: MidiEvent`) |
| 3 … 19 | in | os 17 params abaixo — `porta = 3 + ParameterId` |
| 20 | in | `soundbank` — inteiro `-1..255`, default `-1` = nenhum piano; `-1` tem scalePoint "Nenhum piano" |
| 21 | in | `preset` — índice dentro do banco ativo, `0..1023` (o `maximum` do TTL acompanha a porta 23: banco com 592 presets não cabe em 127) |
| 22 | **out** | `bankState` — `-4` falha ao carregar, `-3` carregando, `-2` nenhum piano, `≥0` **índice do banco que está de fato soando** |
| 23 | **out** | `presetCount` — quantos presets o banco publicado tem |

As duas saídas nasceram de um problema concreto: a UI não tinha como saber o que o
worker estava fazendo, e "FORJANDO" durava um chute de tempo (`~35 MB/s` medidos).
Hoje `run()` escreve 22/23 no começo de todo bloco (sem alocação, `StatusPorts.hpp`),
o host repassa como `port_event` de formato 0, e o painel mostra o estado real —
inclusive quando o arquivo **não** abriu, que é quando um "carregando" eterno
mentiria.

> Armadilha paga **duas vezes**: o typedef é
> `(handle, index, **buffer_size**, format, buffer)` — `third_party/lv2/lv2/include/lv2/ui/ui.h:200`.
> Nomear os argumentos ao contrário compila igualzinho — e o guarda
> `if (format != 0) return;` passa a ver o *tamanho* do buffer (`4`), descartando
> **todo** `port_event`. A primeira vez (checkpoint da noite III) foi corrigida ao
> contrário *nos dois lados*: a UI declarou `(port, format, bufferSize, buffer)` e o
> `pforge_ui_probe` mandava `(port, 0, sizeof)` — espelhos exatos, teste verde, e um
> host de verdade seguiria sendo ignorado. Só um header conferido (ou um segundo
> implementante, no caso a camada da pasta `ideia para ui`) mostra o erro. Regra
> agora escrita em `UI-KIT.md` §1: **conferir a ordem no header, nunca na memória —
> nem na nossa documentação.**

### Ports visíveis (10 knobs) — `ParameterId` 0…9

Valores reais de `src/core/Parameters.hpp`, que é o que o TTL replica:

| # | symbol | nome (host) | no painel | min | max | default | grupo na UI |
|---|---|---|---|---|---|---|---|
| 0 | `volume` | Volume | Volume | 0 | 1 | 0.85 | Master |
| 1 | `hammer` | Ataque do Martelo | Martelo | 0 | 1 | **0.35** | Dinâmica |
| 2 | `velCurve` | Curva de Dinâmica | Dinâmica | 0 | 1 | 0.5 | Dinâmica |
| 3 | `sympRes` | Ressonância das Cordas | Ressonância | 0 | 1 | 0.25 | Resonância |
| 4 | `lid` | Abertura da Tampa | Tampa | 0 | 1 | 1.0 | Corpo |
| 5 | `bassEQ` | Graves (EQ) | Graves | 0 | 1 | 0.5 | Corpo |
| 6 | `midEQ` | Médios (EQ) | Médios | 0 | 1 | 0.5 | Corpo |
| 7 | `highEQ` | Agudos (EQ) | Agudos | 0 | 1 | 0.5 | Corpo |
| 8 | `chorus` | Chorus | Chorus | 0 | 1 | 0.0 | Espaço |
| 9 | `reverb` | Reverb (Ambiência) | Reverb | 0 | 1 | 0.10 | Espaço |

`hammer` era 0.05 e **subiu para 0.35** com a reescrita do modelo (§4): o transiente
novo é ~3× mais fraco em RMS por unidade de ganho, então 0.05 ficou inaudível; 0.35 é
o feltro aparecendo sem virar ruído, e o pico medido nesse valor é 0,063 (em 1.0, 0,184).

> **Escala do martelo (out/2026):** o controle somava demais — em 0.35 o feltro
disputava com a amostra. `Engine::applyNoteOn` multiplica o valor da porta por
`kHammerEscala = 0.10`, então **100% do knob agora vale o que 10% valia**: os picos
acima viram 0,0063 (em 0.35) e 0,0184 (em 1.0). A escala mora no encontro do controle
com o modelo, e não dentro do `HammerModel`: as tabelas `cal_` são o transiente medido,
o knob é gosto — e gosto muda de número sem recalibrar tabela nenhuma.

O `name` é o que o **host** mostra (ali há espaço); a coluna "no painel" é
`ParameterDef::shortName`, o rótulo debaixo do knob. Os dois existem porque
"Ataque do Martelo" e "Ressonância das Cordas" não cabem em ~90 px: truncados em
"Ataque do Mart…", cinco rótulos cortados é literalmente o que se lê como "os botões
tão desorganizados". O hover e o display de baixo usam o nome longo.

### Ports ocultos (`hidden`) — `ParameterId` 10…16

| # | symbol | min | max | default | o que faz |
|---|---|---|---|---|---|
| 10 | `reverbType` | 0 | 3 | 1 | sala pequena / grande / plato / catedral |
| 11 | `trimDb` | −12 | +12 | 0 | gain fino de saída (headroom por preset) |
| 12 | `width` | 0 | 2 | 1.0 | largura estéreo (M/S) |
| 13 | `releaseDamp` | 0 | 1 | 0.35 | amortecimento extra no note-off |
| 14 | `hammerTone` | 0 | 1 | 0.5 | cor/espectro do ruído do martelo |
| 15 | `sympTune` | 0 | 1 | 0.5 | afinação fina dos ressonadores |
| 16 | `engineTune` | −100 | 100 | 0 | transposição global do engine (cents) |

Até out/2026 a UI **não** renderizava knobs para `hidden`: eles eram só chips de
leitura no rodapé, e a manipulação direta ficava "pra um painel avançado opcional".
Esse painel é a **aba MOTOR** do cabeçalho do painel: os sete viram discos numa grade
4×2 dentro de uma chapa só (`06 MOTOR`), com o mesmo arrasto, a mesma roda e o mesmo
duplo-clique = default dos dez do TIMBRE, escrevendo nas portas 13..19. Os chips
continuam no rodapé como leitura (mostram o valor que a automação do host mandou) e o
título do bloco passou a dizer onde se edita. A página MOTOR não depende de banco
publicado: com a forja fria ela continua acesa e editável, ao contrário do véu do
TIMBRE.

> Convenção de ID: symbols estáveis e em ordem fixa no TTL. O host grava automação
> pelo índice — **nunca reordenar ports depois do primeiro save**; pra crescer,
> adicione sempre no fim.

---

## 4. DSP chain (ordem de processamento por bloco)

1. **velocityCurve** — mapeia a velocity MIDI *antes* de mandar pro sfizz
   (curva contínua: 0 = linear, 0.5 = como o hardware, 1 = muito expansiva)
2. **backend de amostra** → estéreo cru (layers, loops, resampling): tinysonic nos
   SF2, sfizz nos SFZ. Sem banco publicado, nenhum evento produz som.
3. **Hammer** — o transiente do golpe, duas componentes (`HammerModel.hpp`):
   - **click de feltro**: ruído em banda em `700 + nota*3800` Hz escalado por
     `2^(hammerTone-0,5)*2`, **Q 1.4**, envelope de **20 → 9 ms** do grave ao agudo.
   - **thump da caixa**: ruído lowpass em `90 + nota*140` Hz, envelope de
     **32 → 18 ms**. A duração *é* o ajuste: com 75 ms o thump integrava tanta energia
     que 70–83% do martelo saía abaixo de 200 Hz — som de armário batendo. Reduzir só
     o ganho não consertava (ficava em 43–71%); encurtar o envelope levou a 15–53% cru
     e a ~1% com ponderação A.
   - Os dois cliques passam por um **lowpass fixo de 9,5 kHz compartilhado** por toda a
     voz, aplicado uma vez por canal na soma (`kTailHz`). O bandpass de 2ª ordem sozinho
     vazava agudo: um terço da energia entre 8 e 22 kHz, que é assobio de filtro e não
     feltro. Filtrar a soma equivale a filtrar cada clique e somar (o filtro é linear) e
     custa 2 biquads por bloco em vez de 32.
   - **Calibração por nota, sem Monte Carlo no RT**: o ganho divide pela energia do IR
     do filtro (`irEnergy`, 256 amostras da resposta ao impulso), vezes o desvio padrão
     do ruído uniforme (`1/√3`), numa tabela `cal_[128]` construída no `prepare()` para
     o `hammerTone` vigente. Isso é o que torna o nível por tecla coerente: a janela
     aleatória de 256 amostras é mais curta que um período de um lowpass de 90 Hz e
     media o RMS errado. A tabela é re-builada em passo **inteiro** (`calToneStep =
     round(tone*50)`), porque comparar float de passo 0,02 por igualdade nunca casa e o
     primeiro `trigger()` de cada nota reconstruiria ~1 ms de tabelas dentro do bloco.
   - Pan `(nota-0,5)*0.6` só no click (a caixa é central).
4. **Ressonância simpática** — banco de ressonadores sintonizados nas teclas
   sustentadas, excitados pelo próprio sinal (`sympRes`, `sympTune`)
5. **EQ 3 bandas** — shelf baixo / peaking médio / shelf agudo (`bassEQ/midEQ/highEQ`)
6. **Lid** — lowpass + tilt contínuo: 0 = tampado, 1 = aberto (`lid`)
7. **Chorus** — 2 voices com LFO (`chorus` = mix)
8. **Reverb** — FDN pequeno; `reverbType` troca o preset da sala, `reverb` = mix
9. **width (M/S) → trimDb → volume → soft-clip**

Todo param com smoothing (port-zipper) **avançado por frames**, não por chamada:
`alpha = 1 − exp(−frames · invTauPerSample)`, com a constante de tempo guardada como
`1/(0,02 s · sr)`. O bug antigo aplicava o coeficiente de **uma amostra** uma vez por
**bloco**: em bloco de 256 os 20 ms viravam ~5 s, e era isso que fazia EQ, Reverb,
Chorus, Tampa e Volume parecerem mortos no REAPER — o valor chegava na porta, o áudio
levava segundos para alcançá-lo, e o único knob que parecia vivo era o de pouco tempo
de cauda. `updateBlock(frames)` roda **uma vez por bloco**, não por fatia de tempo:
fatiar o bloco (§1, `delayFrames`) acelerando a rampa por fatia mudaria o smoothing
conforme a densidade de MIDI.

Nota: em v1 o hammer dispara por evento MIDI encaminhado; ressonância simpática em
v1.1 pode passar a ser dirigida por teclas ativas (mesma fonte de eventos).

### O teto do master, e por que ele não pode mais encostar em 0 dBFS

Medido com o volume do painel em 1,0 (`--param volume=0,001` expõe o pico **antes** do
master, porque a curva é linear ali): a cadeia até o `MasterStage` chega a **1,626** de
pico com um banco de 612 MB tocando o material do standalone. O clipper cubico
antigo era `x − (4/27)x³` com `return 1.0f` acima de |x| = 1,5. Duas consequências, e a
segunda é o defeito:

* em **0,85** (o default) o pico já está a 1,38 do joelho: 3,5 dB de compressão de
  transiente, e o plugin sai a 0,991 — ou seja, o default já não tem folga;
* em **1,0** (ou com `trimDb` subido) o sinal passa de 1,5 e vira **1,0 puro**: topo
  chato, que é clipping digital com outro nome. Uma gravação feita no REAPER mostrou
  exatamente isso — **78 amostras em 0 dBFS, 6 rajadas de 3 a 20 amostras seguidas por
  volta de 49–50 s**.

O clipper agora é **assintótico**: o joelho desce para 1,494 para que a cúbica pare em
**0,996 com inclinação zero**, e acima disso uma cauda racional `1 − 0,004/(1+4u²)` sobe
em direção a 1 sem nunca encostar. Abaixo do joelho a curva é a mesma a 0,1%, então nada
muda em volume de todo dia; o que muda é a garantia — `|saída| < 1` por construção, e
nenhum host vai mais ver amostra em teto cheio:

```
pforge_standalone --bank <banco.sf2> --param volume=1.0
  Pico maximo: 0.996259 (-0.032554 dBFS)   Teto cheio: 0 amostras, pior rajada 0
  ... --param volume=1.0 --param trimDb=12
  Pico maximo: 0.99996  (-0.000346879 dBFS) Teto cheio: 0 amostras, pior rajada 0
```

O standalone agora **conta** amostra em FS e rajada mais longa, e avisa na saída — pico
alto passageiro não distorce, 20 amostras grudadas em 1,0 sim, e sem esse número a
diferença só aparece ouvindo (ou no WAV de outra pessoa).

O que **não** foi mexido: o ganho. Baixar o default de 0,85 ou introduzir atenuação
fixa interna (1,63 → folga de ~4 dB) deixaria o piano visivelmente mais baixo em toda
sessão salva, e isso é decisão de ouvido, não de código. Enquanto o volume estiver em
1,0 o plugin comprime transiente — é o preço de encher a tela; a partir de ~0,92 ele
passava a *achatar*, e é esse regime que sumiu.

### O preço de uma fronteira de fatia (medido, não estimado)


`tools/dsp_stages.cpp` (`pforge_dsp_stages`) renderiza o mesmo bloco de 512 de duas
maneiras — uma chamada e 32 fatias de 16 — e imprime `(sliced − umaChamada)/32` por
estágio. Os números desta máquina, com o piano cheio de vozes:

| coisa | custo |
|---|---|
| `tsf_render_float` em 1×512 com 128 vozes | 330 µs |
| idem em 32×16 | 543 µs |
| **fixo por fronteira, no tsf** | **≈ 6,6 µs** |
| os 8 estágios da cadeia pós-backend, 1×512 | 67,6 µs |
| idem em 32×16 | 69,3 µs |
| **fixo por fronteira, na cadeia** | **≈ 0,05 µs** |

A metodologia mudou nesta rodada e os números de cima são da nova: cada passagem do
`bench` **recoloca a entrada** antes de rodar. Antes, só a primeira forma recebia
`ref`, e as repetições processavam a saída da repetição anterior — como symp, chorus e
reverb têm realimentação, a energia compunha até o `dissipador` (o somador que impede o
`-O2` de apagar o trabalho) chegar a 1e26 e, com 40 cordas simpáticas, virar **NaN**. O
tool agora acusa isso pelo nome do estágio (`[FALHA] o estágio 'X' entregou amostra
não-finita`) e com a entrada fixa por passagem o dissipador voltou a ser um número
finito (94,4 com 8 vozes; 203,0 com 128) — o que prova que o NaN era do **harness**, não
da cadeia. Custo por amostra desses estágios não depende do sinal, então a medida em µs
não mudou de natureza; mudou de confiável para confiável.

Ou seja: fatiar o bloco por `delayFrames` (§1) é caro **só no backend de amostra**,
e o custo é o `tsf` re-caminhando a lista de vozes por chamada. Isso decidiu duas
coisas em `Engine::render`:

- **Notas não são quantizadas** (`kMinSlice = 16`): o ataque é a informação que o
  usuário ouve, e 16 amostras de adiantamento máximo é o piso.
- **CC e bend são** (`kCcSlice = 64`, 1,3 ms): o próprio Engine já quantiza o valor
  deles uma vez por bloco em `updateBlock()`, então um host automatizando amostra a
  amostra pagaria 32 fronteiras num bloco de 512 por um controle que ninguém separa
  a 1,3 ms. O merge de CC **não pode parar na nota seguinte** — testado: parar na
  nota deu *mais* fronteiras que o merge uniforme (64 notas + 100 CC em 1,115 ms).
  O teto do merge é `max(pos + kCcSlice, pos + kMinSlice)`, e o piso em `kMinSlice`
  é o que devolveu o bloco denso a 0,807 ms.

Prova no `pforge_host_test`, teste G, agora com 8 repetições e estatística min..max
(um bloco cronometrado sozinho é uma moeda: a mesma build deu 0,706 e 1,036 ms para
o mesmo trabalho): `64 notas = 0,705..1,000 ms`, `+100 CC = 0,807..1,277 ms`, e a
decomposição `100 CC espalhados = 0,123..0,622 ms` mostra que o que o merge comprou
era mesmo fronteira, não voz.

O mesmo tool agora imprime um bloco `memoria:` lido do próprio `/proc/self/status`
(estágios preparados ≈ 0,36 MB, cada banco carregado, o `+%d vozes alocadas` e, com
`--sf2 A --sf2b B`, o pico dos dois vivos) — é a origem da tabela de §1, e é por isso
que a linha de baixo da tabela existe: **0 vozes** no tsf custa `1x512 = 0,91 µs`,
não 330 µs. Foi esse número que matou a ideia de pular o backend em bloco silencioso.

### O pedal de sustain (medido — é o "corta o som quando seguro o pedal")

O sintoma era real e era **descarte de nota**, não abafamento. Mas o primeiro conserto
(furtar a voz segurada de *menor envelope*) era um subterfúgio, e foi substituído. Por
quê: nestes bancos o nível do envelope fica **congelado em 1,0** durante o sustain — é o
*sample* que carrega o decaimento, medido em `--ring` abaixo. Ordenar por nível queria
dizer ordenar por ruído, e `tsf_voice_kill` cortava uma corda que ainda estava alta. O
que segue é o modelo, não o remendo.

**O mecanismo do tsf, lido no código.** Com teto (`tsf_set_max_voices`, que nós
ligamos de propósito — sem teto o tsf faz `TSF_REALLOC` dentro do `run()`), quando não
há slot livre ele procura **a voz mais avançada em `RELEASE`** e a mata; se não houver
*nenhuma* em release, o caminho é `continue` e **a nota nova é descartada em silêncio**
(`tsf.h:1582-1599`). O pedal desliga exatamente esse critério: com CC64≥64 o
`tsf_channel_note_off` não libera nada, só marca `heldSustain`, e a voz nunca entra em
release enquanto o pedal estiver no ar. O pool enche e tudo que vem depois do teto não
toca.

**Quanto é "cheio", medido (`pforge_dsp_stages --ring`, uma tecla por vez, 60 s por
regime, teto 64 para nada aqui ser roubo de voz):**

```
tecla  freq     | segurada: -40 -60 | pedal: -40 -60 | abafada: -40 -60
  21   27,5 Hz  |  25,7  25,8       |  25,7  27,8    |   0,4   0,6
  45  110,0 Hz  |  19,8  25,7       |  19,8  25,7    |   0,4   0,6
  69  440,0 Hz  |   4,8  10,0       |   4,8  10,0    |   0,5   0,6
  93 1760,0 Hz  |   0,9   2,9       |   0,9   3,0     |   0,6   1,5
 105 3520,0 Hz  |   0,4   2,1       |   0,4   2,4     |   0,3   1,2
```

As colunas `segurada` e `pedal` são **iguais**: soltar a tecla com o pedal no ar não
encurta a cauda em nada — a nota fica no ar até o decaimento natural do *sample* terminar.
`abafada` (tecla solta sem pedal) é 0,4-0,6 s, que é o release do preset. Duas
consequências que viraram código:

- O ajuste físico (`t60 = 10·2^((69−tecla)/16)`, clamp [1 s, 45 s]) **não** entra no
  note-off: os primeiros testes truncavam a cauda gravada (A4 passava de 4,8 s para
  3,7 s). Ele entra em `tsf_voice_render`, na **primeira vez que a voz entra no loop**, e
  só para região em loop (`loopStart < loopEnd`) — plateu infinito de loop de gravação é
  um mentiroso, e o decaimento entra onde o plato começa.
- Com ~3 regiões por tecla e caudas de 25 s no grave, 60 s de passagem pedalizada pedem
  **~1086 vozes**. Nenhum teto chega lá, então a escolha honesta não é "nunca abafar", e
  sim *o que* é abafado e *como*.

**Os três caminhos novos no `tsf.h` (todos marcados `PIANOFORGE`):**

1. `tsf_progressive_damp(f, 3/4·teto, 8)` no topo de `tsf_note_on`, **antes** de alocar:
   derruba os abafadores das cordas **seguradas há mais tempo** (idade =
   `renderSamples − sustainStart`, um relógio de amostras renderizadas que o tsf não
   tinha), contando só as que ainda não estão em release, e no máximo 8 por nota. O teto
   de 8 por nota é o que faz disto um amortecedor *progressivo*: a primeira versão
   abafava `(vivas − alvo)` de uma vez (64 cordas no mesmo bloco), a textura virava
   degrau e a medida diferencial ficava caótica.
2. Nota ainda no dedo (`heldSustain == 0`) **nunca** é furtada; o "cluster de 32 teclas
   com o pedal fora" continua como no upstream.
3. Se ainda assim faltar slot, o steal de emergência pega a mais **antiga**, nunca a que
   está entrando, e o corte cai no mesmo instante em que o ataque novo tapa o descontinuo.

**Soltar o pedal não é um corte — o feltro tem registro (o "corta totalmente o áudio
quando tiro o pé").** Os dois caminhos que abafam corda seguradora (`tsf_progressive_damp`
e o lift em `tsf_channel_set_sustain`, mais as vozes `heldSustain` em
`tsf_channel_note_off_all`) chamavam `tsf_voice_end`, que re-entra em RELEASE com
`parameters.release` — o **release chato do preset**. Medido no banco de 62 presets:
0,550 s nas
teclas 21/45/69, sem nenhuma dependência de nota (e 16,0 s numa região da tecla 93, que é
o erro oposto: uma corda aguda que o feltro não segura). O preset foi gravado com o
abafador **já em cima da corda**, então aquele número descreve a tecla saindo 0,1 s depois
do martelo — não a textura inteira caindo do nível em que o pedal a deixou. Numa
exponencial até −80 dB em T, o −6 dB que o ouvido chama de "começou a cair" chega em
0,0753·T: com T = 0,55 s, 41 ms. Todas as 88 cordas juntas, com a mesma inclinação, do
agudo que já ia morrer ao grave que tinha 5 s de corda livre pela frente. É um degrau.

Entrou `tsf_damped_release_seconds(key) = 0,90·2^((69−key)/16)`, clamp [0,45 s, 2,0 s],
aplicado por `tsf_voice_apply_damper` — que **re-aima** o envelope de amplitude e o de
filtro a partir do nível *atual* (`tsf_voice_envelope_release_at`) e abandona o loop
sustenido. Idempotente, sem descontinuidade de nível: o que muda é a inclinação, não a
amostra. A meia-vida por 16 teclas é a mesma lei medida no `--ring` para a corda **livre**
(é a física da corda, não do abafador); o que se ancora aqui é o tempo do feltro em A4. O
resultado é o que uma cauda de pedal faz num piano acústico — o agudo sai na frente e o
grave é o último:

```
pforge_dsp_stages --pedallift --chain --voices 64     (estrato de 12 teclas, 55 Hz a 3,7 kHz)
                 -6 dB   -20 dB   -40 dB   aos 2 s
  feltro por registro | 128ms    213ms    555ms   -74,8 dB
  tsf_voice_end (antes) |  11ms     75ms    139ms   -60,8 dB
```

Medir isso exigiu trocar a métrica da ferramenta, e é armadilha documentada: **o RMS de um
bloco numa tecla sola oscila ±6 dB** porque as ~4 camadas do banco batem entre si (nada a
ver com o abafador — o `--trace` mostra as 4 vozes de key 69 seguindo exatamente a curva de
0,900 s). `--pedallift` agora (a) imprime o valor de `tsf_damped_release_seconds` por voz,
que é a leitura exata do modelo, (b) cruza limiar em RMS com boxcar de 8 blocos (85 ms), e
(c) avisa na cara do leitor que **só a coluna do caso agregado vale**. O primeiro
`--pedallift` lia −6 dB em 21 ms numa corda de 0,90 s porque media pico de bloco, e o
segundo, com RMS puro, ainda lia o batimento.

**A prova de que não há mais nota muda** está nos contadores que o próprio backend agora
mantém (`pfDroppedRegions`, `pfMuteNotes`, `pfDampedHeld`, `pfDampedAgeSamples`) — porque
o delta de `tsf_active_voice_count()` **não** mede isso: uma nota que fura a fila mata
uma voz e herda o slot, então a contagem não se move e a nota soou. A primeira versão
desta medida acusou 26 notas mudas numa passagem onde nenhuma ficou sem som.

```
pforge_dsp_stages --sf2 <banco.sf2> --pedalload --seconds 60 --voices T
  teto 128: notas=357 MUDAS=0 regioes sem slot=0 | abafadas=1406, idade media 3,05 s | 313 us/bloco
  teto 192: notas=357 MUDAS=0 regioes sem slot=0 | abafadas=1338, idade media 5,20 s | 465 us/bloco
  teto 256: notas=357 MUDAS=0 regioes sem slot=0 | abafadas=1252, idade media 7,62 s | 623 us/bloco
  teto 384: notas=357 MUDAS=0 regioes sem slot=0 | abafadas=1022, idade media 13,86 s| 956 us/bloco
```

Ou seja: **o teto não decide se uma nota é perdida — nenhuma é, em nenhum teto.** Ele
decide há quanto tempo uma corda pedalizada estava no ar quando o abafador caiu. O
`Engine` usa **256**: a essa idade o médio e o agudo já passaram dos −40 dB (A4 chega lá
em 4,8 s) e só o grave ainda seria ouvido por cima da textura. RAM de voz é estruturinha
(~200 B); o banco não muda com o teto, e a RSS também não.

**O que o `pforge_host_test` prova, e com que método:**

- **L** (40 teclas, pedal no ar, pool folgadíssimo) é a medida diferencial clássica: a
  mesma sequência inteira, e depois uma vez por tecla omitida; a janela de cada tecla
  começa um bloco depois do ataque e termina antes do próximo. Todas as 40 somam
  ≥ 0,36 dB; o piso do método (duas renderizações idênticas) é 0,050 dB porque o
  `HammerModel` sorteia o ruído por disparo (`HammerModel.hpp:202`), e o limiar é 3× o
  piso medido, não um número fixo.
- **M** é o caso saturado, e **não pode** ser a medida diferencial de L: com o pool
  estourado, omitir uma tecla muda *qual* corda o amortecedor derruba nas teclas
  seguintes, e as duas renderizações deixam de ser a mesma textura com uma nota a menos.
  Medido: o delta da tecla 189 saiu **negativo** (−0,67 dB — tirar a nota deixou a janela
  mais alta). M então enche o pool com **250 teclas pedalizadas a velocidade 14** e só
  depois toca 12 teclas a 110 em chaves virgens; o fundo fica ~30 dB abaixo das notas
  testadas, a troca de vítima do abafador passa a não mover o nível, e a medida
  diferencial volta a valer.

```
M: fundo com o pool cheio=0,015296 | piso=0,017 dB -> limiar=0,051 dB (250 veladas + 12 fortes)
M: ganho de cada tecla forte sobre o fundo estourado (dB):
   t1=11,4 t2=3,5 t3=2,7 t4=1,9 t5=2,5 t6=1,4 t7=1,4 t8=2,2 t9=2,6 t10=2,1 t11=1,9 t12=1,1
M: tecla muda: nenhuma
M: janelas depois do pe (razao pedal-segurado / pedal-solto):
     W1 0,04-0,34 s: segurado=0.064299 solto=0.041802  (1.5x, 3.7 dB)
     W2 0,34-1,36 s: segurado=0.050364 solto=0.010593  (4.8x, 13.5 dB)
     W3 3,8-15 s   : segurado=0.020848 solto=0.000000  (silencio exato)
```

M também é o teste do **outro lado da moeda** — o que impede "sustain correto" de virar
"o piano nunca para" — e ele teve de ser reescrito em três janelas quando o feltro por
registro entrou. A asserção antiga media a cauda numa única janela de 0,35–2,55 s e pedia
≥20×; com o abafador certo aquela janela **cai dentro da queda**, e a razão caiu para
15,7×: o teste reprovou um conserto bom, porque media o tempo errado como se fosse
volume. Agora: **W1** prova que soltar o pé *não é corte* (a textura ainda está lá nos
primeiros 0,34 s), **W2** é a queda em si (informativa), **W3** prova que abafa (≥20× e,
medido, silêncio exato aos 4 s).

**Toda asserção nova tem de ter o dente medido.** O primeiro limiar de W1 era 4× (12 dB) e
**passava no código velho** — não provava nada. Revertido o lift para `tsf_voice_end` e
remedido: o velho dava 11,1 dB, o novo dá 3,7 dB; o limiar virou 2,4× (7,6 dB), o médio
geométrico dos dois, que reprova o bug que motivou a asserção e deixa ~4 dB de folga para
o conserto. Uma asserção só vale depois de você ter visto ela falhar no código doente.

**Orçamento de CPU com a piscina lotada** (`--notes N --voices T`, bloco de 512):

| teto | vozes ativas | 1×512 | 32×16 (pior caso de fatiamento) | fixo/fronteira |
|---|---|---|---|---|
| 128 (32 teclas) | 128 | 330 µs | 535 µs | 6,4 µs |
| 256 (64 teclas) | 256 | 650 µs | 1105 µs | 14,2 µs |
| 384 (96 teclas) | 384 | 987 µs | 1666 µs | 21,2 µs |

O teste **G** parou de assentar "1 ms" absoluto e passou a assentar **1/5 do próprio
período do bloco** (512 amostras = 11,6 ms em 44,1 kHz → 2,32 ms), porque o custo do
pior caso escala com o número de vozes e o teto subiu de 128 para 256 de propósito: um
número mágico em milissegundos reprovaria um conserto correto. Medido no teto 256, com
64 ataques num bloco só mais 100 CC de automação: `1,00..1,35 ms` e `1,10..1,79 ms` —
dentro do orçamento nos mínimos determinísticos, e os 80% restantes ficam para o resto
do projeto do host. O caso típico (carga de banco, troca, notas reais) continua assegurado
em p99.9 < 0,2 ms nos testes B/D/I.

---

## 5. Interface — a Forja de Pianos (o que está construído)

X11UI externa (`ui:X11UI`), 1000×520, desenhada num framebuffer ARGB32 próprio
(`src/core/ui/Surface.hpp`) com `stb_truetype` — sem Cairo, sem FreeType, sem GL:
o mesmo código de desenho serve ao X11UI e ao preview offscreen, e é assim que o
visual é conferido sem host LV2 instalado. Apresentação por `XShmPutImage`, com
fallback **verificado** para `XPutImage`: `XShmAttach` devolve 1 mesmo quando o
servidor recusa o segmento, então só se confia no SHM depois de um `XSync` com
handler de erro próprio (sem isso a UI pintava e o host não mostrava nada). Nesta
máquina o servidor responde `BadAccess` ao anexo, e o caminho real é o
`XPutImage` — o painel inteiro são 2 MB por frame, redraw só quando algo muda.

```
┌────────────────┬──────────────────────────────────────────────────────────┐
│ ⚒ FORJA        │ ● CONTROLES DE TIMBRE            [FORJA DSP][ENGINE tsf] │
│   DE PIANOS    │ ┌─────────┬─────────┬───────────────┬──────────────────┐ │
│ BIBLIOTECA  02 │ │01 MASTER│03 DINÂM.│ 04 CORPO      │ 05 ESPAÇO        │ │
│ [🔍 filtrar… ] │ │ ┌─────┐ │ ┌─────┐ │ ┌──── ┌────┐ │ ┌─────┐          │ │
│ ● Forja Teste  │ │ │ 0.85│ │ │ 0.35│ │ │1.00│ │0.50│ │ │ 0.00│          │ │
│   172 MB       │ │ └─────┘ │ └─────┘ │ └────┘ └────┘ │ └─────          │ │
│ ○ Felt Grand   │ │ Volume  │ Martelo │ Tampa  Graves │  Chorus          │ │
│ ┌────────────┐ │ ├─────────┤         │ ┌────┐ ┌────┐ │ ┌─────┐          │ │
│ │   GRAND    │ │ │02 RESON.│         │ │0.50│ │0.50│ │ │ 0.10│          │ │
│ │ (arte do   │ │ │ ┌─────┐ │         │ └────┘ └────┘ │ └─────          │ │
│ │ manifest)  │ │ │ │ 0.25│ │         │ Médios Agudos │  Reverb          │ │
│ └────────────┘ │ │ │ └─────┘ │                       │                  │ │
│ [Procurar …]   │ │ └ Resson. │                       │                  │ │
│ [+Pasta][Remover]────────────────────────────────────────────────────── │ │
│ bancos locais… │                                                        │ │
├──────────────────────────────────────────────────────────────────────────┤
│ ▎Forja de Teste (172 MB) [CARREGANDO] Forja… ◂PRESET 8/62▸  reverbType …  │
│ ▎ForjaTeste.pforge · 172 MB  0 presets · estimativa por tamanho  (chips)  │
└───────────────────────────────────────────────────────────────────────────┘
```

É a transcrição de `ideia para ui/PianoForge-Concept-v2/index.html`: cada número
do desenho é uma regra CSS do protótipo (`kSideW 238` = `grid-template-columns:
238px`, `kStatusH 76` = `grid-template-rows: … 76px`, `kHeadH 38` =
`.panel-head{height:38px}`, `kRowH 31` = `.bank-row{min-height:31px}`, e o knob
inteiro são os números do `viewBox 0 0 80 80` escalados por `u = lado/80`). Onde o
mock e uma decisão anterior divergiam, **o mock ganhou** — inclusive a grade, que
tinha sido ditada à mão em 5×2 e voltou a ser por categoria.

Regras de UX implementadas:

1. **1 knob = 1 port `knob`, em duas páginas.** Os 10 do TIMBRE escrevem nas portas
   3..12 e os 7 do MOTOR nas 13..19: `knobs[]` é indexado por `ParameterId`, então
   arrasto, roda e duplo-clique = default são o MESMO código nas duas páginas. Os
   chips monoespaçados da terceira coluna do rodapé (`.hidden-chip`) ficaram como
   leitura dos sete — o protótipo pedia texto, e texto ele continua sendo; o que
   saiu foi o "não se edita". A troca de página são duas abas no cabeçalho, `TIMBRE`
   e `MOTOR` (62×18, largura fixa: o layout não tem fonte para medir texto), com o
   filete de brasa embaixo da ativa — o mesmo sinal dos grupos "acesos". As abas vêm
   **antes** do véu no hit-test: sem elas, um host que abre sem banco deixaria o
   motor trancado atrás do `Escolher primeiro banco`.
2. **Quatro colunas de cards, `.78 / .88 / 1.64 / .92 fr`** — a coluna 1 empilha
   MASTER sobre RESSONÂNCIA (os dois grupos de um knob só, e por isso ela é mais
   estreita), a 3 é o CORPO em `.body-grid` 2×2. A ordem mora em `kGroups[]` e em
   `colFr()`, e foi ela que enterrou a grade 5×2 do checkpoint anterior: com o
   Volume sozinho no topo da coluna 1, o disco deixou de cair "exatamente no meio"
   entre duas linhas de todo mundo, que era a queixa original.
3. **O índice do grupo é desenhado** (`01 MASTER`, `.group-index` 7 px em bronze)
   e a barra de brasa à esquerda do card (`.group:before`, 2 px) só existe em MASTER
   e RESSONÂNCIA — é como o olho sabe que aqueles dois são a coluna de um knob.
4. **Knob com valor dentro da face**: o número (`600 8px`, tabular) mora 16 px
   abaixo do centro *dentro* do disco, o nome embaixo do disco (9 px/650) e a dica
   do port abaixo do nome (7 px, `.knob-tip`) — assim o disco não precisa de
   legenda de grupo para se explicar, e um knob modificado ganha o ponto de brasa
   (`.knob-card.modified:after`) e o valor em `--ember2`.
5. **Rótulo curto debaixo do disco, nome do host no hover** (§3, `shortName` /
   `fullLabel`): cinco rótulos truncados com "..." é literalmente o que se lê como
   "botões desorganizados".
6. Drag vertical (180 px = fundo→topo), **shift+drag** fino (0,15×), **duplo
   clique** = default do port, roda do mouse = 1/50 do curso. **Roda para cima
   sobe** o valor do knob e o índice do preset — em host de áudio é o hábito, e
   estava invertido; a lista de bancos é o único lugar onde "para cima" volta ao
   topo, porque lá o gesto é de scroll, não de valor.
7. **Brasa que acompanha o valor**: o arco interpola bronze inerte → laranja →
   palha incandescente. O estado "nenhum piano publicado" não é um texto no display
   nem um painel esvaziado: é o **véu** (`.veil`, `inset:38px 0 0` dentro do painel)
   com o card "A forja está fria", a explicação e o botão `Escolher primeiro banco`.
   O véu fica no ar durante o CARREGANDO também — só um banco *publicado* o tira —
   e ele cobre os knobs, **nunca a lista** (regra 9).
8. Fagulhas só animam no knob sob o mouse e por ~1,5 s depois de soltar; fora
   disso `idle()` não redesenha, pra não queimar CPU em host free-wheeling.
9. **A lista é o controle de escolha, e ela vive sob o véu.** `hitTest()` resolve
   busca, linhas e botões *antes* do guarda do véu; com o guarda primeiro, clicar
   numa linha com a forja fria não fazia nada e sobrava só o botão do card para
   começar — foi o que o probe pegou (`--click 100:130` com `writes=0`). A lista
   mostra nome + tamanho formatado, arquivo sumido riscado, e aceita **filtro por
   substring** (nome *ou caminho*, por isso dá para filtrar por pasta).
   `Procurar soundfont…` e `+ Pasta` abrem zenity/qarma (se houver) e escrevem no
   `.list`; sem diálogo o rodapé manda editar o arquivo. O retorno de um pedido é
   um **toast** (`.toast`, verde-escuro), não o rodapé: o rodapé é o estado do
   plugin, o toast é o pedido do usuário — e ele flutua **acima** da statusbar
   (`l.statusbar.y − h − 6`), não a 24 px do pé da janela como diz o CSS: lá o
   protótipo é `position:fixed` no *viewport* da página, e numa janela embedada o
   mesmo número cobria o `dampTune` do rodapé. Texto de estado coberto por texto de
   feedback não é flutuante, é colisão.
10. **"FORJANDO" é o plugin dizendo**, não a UI estimando: a porta 22 vale `-3`
   enquanto o worker rói o arquivo, vira o índice publicado quando toca, `-2` no
   silêncio e `-4` na recusa. O rodapé distingue os três:
   `FORJANDO / Felt Grand (1,6 GB)`, `NAO ABRIU / <pedido> - ainda toca <publicado>`,
   `<banco> / <arquivo> · piano na forja`. A estimativa por tamanho (~35 MB/s
   medidos nesta máquina) continua no código só como **fallback** pra host que
   nunca repassa `port_event`, com teto de segurança: se o status nunca chega, o
   "forjando" se apaga sozinho e o rodapé lê `sem resposta do plugin`.
11. **Preset (porta 21) tem lugar no rodapé** — a pílula `◂ PRESET i/N ▸` só aparece
   quando a porta 23 informa `N > 1` e há banco publicado (SFZ é sempre 1; SF2
   grande passa de 500). Clique, roda do mouse sobre a pílula e ← → do teclado
   mexem; trocar de banco publicado volta ao preset 0, que o plugin também volta.
   A linha miuda sob o nome lê `preset i/N - <arquivo>` (com `N == 1` fica só o
   arquivo: não há o que escolher), e o canto direito guarda o estado do **plugin**
   — escrever o preset ali apagaria o "piano na forja", que é a informação que a
   porta 22 trouxe. Nome de preset não viaja: as saídas são control ports de
   `float`, então dizer qual preset está soando exige índice + arquivo, não rótulo.
12. **O `.pforge` manda no que o painel diz sobre si mesmo** (regra 8 de §2): o nome
   da linha e do rodapé vem de `manifest.produto`, a arte de `presets[].imagem` sai
   pela placa embaixo da lista, e `presets[].nome` entra na linha miúda quando a
   contagem casa com a porta 23. Arte é por preset: o `key` é `path@preset`, e um
   preset sem `imagem` limpa a placa — inclusive quando quem mudou o preset foi o
   host (`port_event` na 21 chama `syncSoundingName()`, não só a linha do rodapé).
   Tudo isso na thread da UI: `Bundle::open` lê cabeçalho + diretório + manifest e
   o blob da arte são alguns KB, nada passa perto do `run()`.

### O rasterizador, e o preenchimento translúcido que perdia a cor

`Surface::blendPix` (float, um pixel) sempre esteve certo. `fillRow` — o caminho
rápido de linha, inteiro e empacotado — não: ele somava `src * 256` *dentro* do
campo de 8 bits e mascarava no fim. Como o termo da fonte é múltiplo de 256, a
máscara `0xFF << shift` cortava exatamente esse termo, e o resultado era
`dst·(1−α)` puro: **a cor da fonte se perdia e só a luminância do destino
escurecia**. Num fundo ferro `#14161a` isso é quase invisível; no véu
`#0c0f13aa` sobre o painel, o retângulo que deveria ser (16,19,24) saía
**(10,26,14)** — verde. Todo preenchimento grande e translúcido do painel estava
corrompido, e o que denunciou foi medir o pixel, não olhar a tela.

Correção: alpha em 8.8 fixo com `ia = 256 − al`, canal por canal
`(src·al + dst·ia) >> 8`. Como `al + ia = 256`, o resultado nunca passa de 255 e
não precisa de clamp por canal.

A outra regra que só existe porque doeu: **a altura do card do véu é a do conteúdo**
(`ch = 156`), não um número redondo. Com 132 px o guarda que impede o texto de
entrar no botão rejeitava a terceira linha e a frase perdia a palavra que dá
sentido ao estado vazio ("…nenhum banco será carregado **automaticamente**").

### O que faz a janela aparecer dentro de um host de verdade

Sete regras, cada uma paga com uma tela preta real:

1. **O primeiro frame não espera `idle()`.** `instantiate()` mapeia e desenha, e
   `show()` desenha também. Um host que só bomba `idle()` quando quer — ou que não
   conhece `idleInterface` — viajava o fundo da janela (`ironDeep`, indistinguível
   de preto). Prova: `pforge_ui_probe --noidle` → 1276 cores depois de 400 ms sem
   nenhuma chamada de idle (o settle existe porque, desde a regra 6, quem pinta é a
   thread da própria UI, que precisa de alguns ms para existir).
2. **A janela filha herda o visual do container do host** (`XCreateSimpleWindow`
   passa `CopyFromParent`), então o `XImage` é montado com o
   `XGetWindowAttributes` do *nosso* window, não com `DefaultVisual/DefaultDepth`.
3. **O tamanho é do host, não o nosso.** `LV2UI_Resize` (feature `ui:resize`) pede
   1000×520 no instantiate; se o host impuser outro, `ConfigureNotify` chama
   `adoptSize()`, que refaz Surface + XImage. Putar 1000×520 num drawable menor é
   `BadMatch` em todo frame — e `BadMatch` engolido pelo handler do host é tela
   preta. Nunca se alarga abaixo do que o host deu pelo mesmo motivo.
4. **Redesenhar em `MapNotify`/`ReparentNotify`/`VisibilityNotify`**, além de
   `Expose`: é nesses eventos que o host empacota e resolve o tamanho; pintar só
   uma vez no instantiate deixa o conteúdo perdido quando o container é mapeado
   depois (`--unmapped-parent` mostra o antes = 1 cor, depois do MapNotify = cheio).
5. **Cada `XPutImage` é validado uma vez** sob handler de erro próprio
   (`verifyPutPath`): se o servidor recusar, cai de SHM para `XPutImage` ou refaz o
   `XImage` com o visual real do window, com uma linha no stderr. Em visual de 32
   bits o byte alfa é copiado como `0xFF` (senão as bordas antialiasadas ficam
   translúcidas e o fundo do host aparece através delas).

6. **A UI se auto-pilota.** Um host de verdade (REAPER 7.83) passa `ui:parent`,
   mapeia o socket, lê e escreve parâmetros — e **nunca chama `idle()` nem
   `show()`**. Como desenhar numa janela cujo ancestral ainda não é *viewable* é
   descartado pelo servidor X (sem backing store), o paint do `instantiate()` se
   perde e só um redraw *depois* do `MapNotify` salva — o que exige bombear eventos
   que o host não bombeia. A UI então abre **uma segunda conexão X**
   (`gfx_ = XOpenDisplay(nullptr)`) só sua, tira a seleção de eventos da conexão do
   host (`XSelectInput(dpy_, win_, 0L)`, senão entope a fila dele) e roda
   `pumpLoop()` numa thread: `select()` no fd da conexão, tick de animação, redraw
   sob `mtx_`. Uma conexão por thread dispensa `XInitThreads()` e dispensa mutex em
   cada X call. O `std::recursive_mutex` existe por outro motivo: `write_()` pode
   reentrar em `port_event` na thread do host. Diálogo (`zenity`) roda **fora** do
   lock — `popen` bloqueante com a lock tomada congela o redraw.

7. **Um `port_event` do host acorda a thread da UI.** Com a regra 6, quem desenha é
   a `pumpLoop()`, e ela dorme num `select()` com timeout de **1 s** quando nada
   anima na tela. O `needDraw_` armado pela thread do host não atravessa um fd: sem
   um `pipe2(O_NONBLOCK|O_CLOEXEC)` no conjunto do select, o painel fica até um
   segundo atrasado em toda mudança vinda de fora — invisível no REAPER (o mouse do
   usuário acorda a thread) e gritante no probe, que injeta status e captura em
   60 ms. Foi assim que `--port 22:0` voltou a pintar 1457 cores depois de parecer
   morto. O write é não-bloqueante porque um segundo `port_event` com o pipe cheio
   pararia a thread *principal* do host.

Prova de que a janela vive num DAW real: com o REAPER aberto no FX chain, a captura
da janela do host deu o painel completo (1276 cores distintas), um clique real na
linha do piano escreveu a porta 20 (`Banco de Piano=0.000`) e um arrasto real no
knob levou 0.900 → 1.000.

### Como o visual é conferido sem DAW

- `pforge_ui_preview` renderiza o painel offscreen em PPM (→ PNG) com estados
  parametrizáveis: `--state empty|normal|hover|loading|dragging`, `--selected N`,
  `--bank "Nome|1,6 GB|1"`, `--value hammer=0.9`, `--time F`, e o estado vindo do
  plugin: `--status none|loading|ready|failed --presets N --preset I` (sem
  `--status` o painel é desenhado no modo "host nunca mandou port_event", que é o
  caminho de fallback).
- `--bundle CAMINHO.pforge` (no preview) abre o bundle de verdade e monta a linha
  da lista com o nome do manifest + a arte do preset pedido: é como a placa de arte
  e o nome por preset são julgados sem X. Do outro lado, o `.pforge` tocado pelo
  *plugin* no X11 é registrado num `soundfonts.list` de mentira —
  `XDG_CONFIG_HOME=/tmp/pf_x11cfg` com `/tmp/pf_x11cfg/pianoforge/soundfonts.list`
  apontando pro bundle, e o `SoundbankManager` lê de lá sem tocar no registro real.
  Prova: `--port 22:0 --port 23:2 --port 21:0` → 2047 cores (a arte entra na contagem)
  e `--port 21:1` → 1488 (o preset sem `imagem` limpa a placa).
- **A primeira execução é conferida, não assumida.** `XDG_CONFIG_HOME=/tmp/pf_first/.config`
  criado com **só** um `soundfonts.conf` legado dentro (uma pasta + um `.sf2` absoluto) e um
  `custom_soundfonts.list` legado: abrir a UI não deve criar `soundfonts.list`, não deve
  listar nada e não deve carregar nada. Medido: 2397 cores distintas no quadro pintado sem
  `idle()`, biblioteca `00`, véu "A forja está fria", rodapé "Nenhum piano", e o diretório
  de config continua com os dois arquivos legados e nada novo. Uma segunda variante põe no
  próprio `soundfonts.list` uma pasta e um `.sf2`: sai `01` com o piano e a pasta ignorada
  (2426 cores) — linha que não é soundfont não entra nem riscada.
- `pforge_ui_probe` sobe o `pianoforge_ui.so` numa janela X11 de verdade
  (override-redirect fora da tela), faz o handshake XEmbed, injeta
  `--click x:y` / `--drag a:b c:d` e captura os pixels e cada escrita de porta.
  Provou `clique na lista → [20]=0.000` e `arrasto no volume → [3]=0.889 … 1.000`.
  É também o passo **D** do CI, e ali ele roda sob `Xvfb :99`: fora da tela funciona
  num servidor com compositor e **não** funciona num sem backing store — ver a
  armadilha "Off-screen não tem bits" no `UI-KIT.md` §11, que é o motivo de o probe
  reler com o container em `8,8` quando a primeira leitura não mostra nada. Medido
  nas duas: 2527 cores.
  `--port ÍNDICE:VALOR` chama `port_event` do caminho do host pra cá — é assim que
  as saídas 22/23 entram na UI sem rodar o plugin, e foi assim que se descobriu que
  a assinatura trocada de `uiPortEvent` descartava todos os eventos. Combinado com
  `--click 416:486` (a seta `▸` no layout 1000×520) prova o preset:
  `--port 22:0 --port 23:62 --port 21:7 --click 416:486 → [21]=8.000`.
  Modos que emulam o que um host faz com a janela: `--noidle` (nunca chamar
  `idle()`), `--unmapped-parent` (mapear o container depois do instantiate),
  `--resize LxA` (host redimensiona o filho), `--depth32` (container com visual
  próprio — cai em `visual-alt` quando o servidor recusa depth 32 fora do root).
- **Teclado e roda só existem no probe desde a rodada do conceito v2**, e os dois
  acharam defeito. `--type "texto"` injeta caractere por caractere com
  `XKeysymToKeycode` na *mesma* display (a UI traduz keycode por `XLookupString`;
  botar um keysym no campo errado do evento não chega a nada), `--key Nome` manda
  teclas nomeadas, `--wheel x:y:delta` repete o botão 4/5 `|delta|` vezes e
  `--shift` põe `ShiftMask` no arrasto. O que isso provou, com as coordenadas do
  layout atual (busca `126:102`, linha 0 `100:130`, Volume `325:155`, pílula
  `416:486`):

  | gesto | escrita esperada | resultado |
  |---|---|---|
  | `--click 126:102 --type gran` | nenhuma | filtro casa, lista reduzida |
  | `… --type grand piano` | nenhuma | **0** linhas — a busca é substring, não fuzzy: "grand piano" não cabe em "Felt Grand" |
  | `--key BackSpace` ×5 | nenhuma | volta às duas linhas (apaga glifo, não byte) |
  | `--key Escape` ×2 | nenhuma | limpa o filtro e depois solta o foco |
  | `--type zzz --key Return` | nenhuma | toast `nada corresponde a busca` |
  | `--type gran --key Return` | `[20]=0.000` | escolhe o primeiro **visível** |
  | `--click 622:301` (botão do véu) | `[20]=0.000` | mesmo caminho, pelo card |
  | `--click 100:130` / `100:160` | `[20]=0.000` / `[20]=1.000` | a lista responde **sob o véu** |
  | `--wheel 325:155:3` | `[3]=0.870 0.890 0.910` | roda para cima **sobe** |
  | `--wheel 416:486:2` | `[21]=8.000 9.000` | idem para o preset |
  | `--wheel 100:140:-4` (8 bancos) | nenhuma | rola a lista e para no fim |
  | `--drag 325:155 325:105` | `0.894 … 1.000` | curso cheio em 50 px |
  | `--shift --drag` idem | `0.857 … 0.892` | 0,15×, como o código diz |
  | `--click 325:155` ×2 (<450 ms) | `[3]=0.850` | duplo clique = default da porta |

  A linha de `100:130` é a que importa: antes dela `hitTest()` devolvia vazio para
  a lista inteira enquanto o véu estava no ar, e o único jeito de escolher um piano
  num painel frio era o botão do card. `--click` com `writes=0` foi a prova.
- `pforge_host_test` roda o plugin com as portas 22/23 conectadas e o **teste I**
  mede o que a UI vai ver: o worker não é drenado entre blocos (host de verdade
  continua chamando `run()` enquanto o disco trabalha), e as asserções exigem
  `-3` durante o load, o índice publicado depois, `-2` no `-1` e `-4` no banco
  inválido — tudo isso com **p99.9 do bloco < 1 ms** (9 967 blocos medidos durante o
  load deram p99.9 = 0,119 ms, pior bloco 0,375 ms). A asserção já foi o pior bloco
  da série com teto de 2 ms e reprovou uma execução que estava 0,155 ms em tudo o
  mais: um bloco isolado atrasado é page cache e outro processo no disco, não é o
  plugin.
- Dois `--sf2` na linha de comando do `pforge_host_test` ligam o **teste J**, o único
  que roda o plugin **sem** `LV2_Worker_Schedule` nas features do `instantiate()`: a
  thread de reserva faz o load, o swap e a liberação. As asserções são o som dos dois
  bancos, `maxBlock < 1 ms` (a liberação do banco velho não pode cair no `run()`) e
  a RSS caindo de verdade ao trocar para um banco menor. O `pforge_host_test` fecha
  imprimindo `memoria: pico=… kB`, lido de dentro do processo. Medido no par de 49 MB
  + 16 MB: `pico=105 MB` com o estacionamento contra `141 MB` quando os dois são
  alocados juntos — os 141 MB são o que `pforge_dsp_stages --sf2 A --sf2b B` calcula
  de propósito, já que o `--sf2b` existe só para medir o pior caso.
- **K**, **L** e **M** são o pedal de sustain, um em cada metade do gesto e no caso
  saturado: K mede a *cauda* (com CC64≥64 a nota continua depois do note-off, 513× o RMS
  da mesma nota sem pedal; soltar o pedal abafa, e a tecla tocada em seguida continua
  viva — medida com o dedo embaixo, porque comparar caudas com caudas não prova nada);
  L mede a *entrada* com 40 teclas e o pool folgado; M mede a entrada com o pool
  estourado, e é o teste da seção 4. Os três juntos são o motivo de K não bastar: um
  pedal que segura tudo o que foi tocado e ao mesmo tempo descarta as notas novas parece
  funcionar — só corta.
- O `--live` do standalone é conferido sem ninguém tocando:
  `--device none --out x.wav --feed "c4:300 e4:300 g4:600"` grava a execução e o
  perfil de RMS por 100 ms mostra as notas entrando nas horas certas;
  `--device default --volume 0` (inaudível) roda o ALSA de verdade e reporta
  `0 underrun(s)` com os 3,1 s de áudio entregos em 3,1 s de parede — é o
  `snd_pcm_writei` bloqueante que cadencia o laço. A tabela de teclas é testada por
  pipe: `printf 'cq wc \x1b'` tem de terminar em `notas: C4 D4  pedal OFF` (o `c`
  do meio solta o E3, o espaço pisa e solta o pedal), `\x7f` limpa tudo.
- As três juntas substituem um host que não existe nesta máquina — e ainda assim
  nenhuma delas é o REAPER. O que cada uma não prova está na seção seguinte.

### Conferido no REAPER 7.83 (o único host LV2 real da máquina)

Instalado em `~/.lv2` (md5 igual ao `build/`), aberto por linha de comando com um
projeto `.rpp` e um script ReaScript posicionais:
`reaper -nosplash -newinst -cfgfile <cfg> <projeto.rpp> <script.lua>`.

O que o host realmente faz, medido e não suposto:

- **Não chama `idle()` nem `show()`.** É o que estava por trás da "tela preta" e o
  que obrigou a regra 6. Com a thread própria, o painel aparece completo dentro da
  janela de FX do REAPER.
- **Aceita `lv2:worker` e `schedule_work()` responde sucesso — e nunca roda
  `work()` com o transporte parado.** Sem a thread de reserva do plugin, o painel
  fica em `-3` para sempre. Foi exatamente o que aconteceu na primeira vez: a
  escolha do piano não virava som nenhum no REAPER, e no `pforge_host_test` não,
  porque o stub drena o worker ele mesmo. O watchdog de 2 s assume o load e o
  `workerAlive` impede as duas vias de carregarem o mesmo banco.
- **Os índices que o host mostra não são os índices das portas**: `Volume` é o
  parâmetro 0, mas `Banco de Piano` é 17 e `Estado do Banco` 19 (REAPER esconde as
  portas de MIDI/atom e as saídas atrás de uma lista própria, 24 visíveis). Todo o
  resto do teste se faz por nome, nunca por número.
- **Recall funciona e continua mudo**: `.rpp` salvo com `banco=-1` reabre com
  `banco=-1.0 estado=-2.0` e nenhum piano carregado.
- **Áudio de verdade saindo do plugin dentro do host**, medido com
  `Track_GetPeakInfo` (não com gravação nem com render): tocando o mesmo projeto de
  20 s com notas MIDI, depois de escrever 0 na porta do banco e esperar o `estado`
  publicar, o pico foi **0,630665 (−4,00 dBFS) na faixa e no master**. O controle —
  transporte rodando, mesmas notas, porta intocada — deu **0,00000000** na faixa e
  no master. É a prova das duas regras do §1 dentro de um DAW.

Armadilhas pagas no caminho (nenhuma delas é do plugin):

- `Main_OnCommand(40017,0)` abre diálogo **modal** e congela a thread do script
  diferido — foi isso que fez "o transporte nunca roda" em sessões antigas.
- `Main_OnCommand(40044,0)` (play) disparado no mesmo frame de `Main_openProject` é
  **ignorado**; tem de vir alguns frames depois (`defer`).
- `os.clock()` é tempo de CPU, não de parede — não serve para cadenciar nada.
- `GetProjectStateValue("RENDER_TARGETS","")` lança erro e mata o script diferido.
- O REAPER abre o **ALSA direto** (`ALSA: using mmap interleaved`), então os
  monitores de saída do PipeWire não veem nada: gravar o cabo não prova nem desprova
  áudio nesta máquina. Restam os medidores do próprio host.
- O diálogo de render (41827) nunca apareceu disparado de script diferido, e 42098
  não renderizou. Existe `-renderproject arquivo.rpp` na linha de comando, mas um
  projeto reaberto renderiza silêncio **por projeto de produto** (baseline não
  carrega), então ele não serve como prova de áudio.
- O `reaper` desta build expõe 742 funções Lua e **não tem** `CountActions` /
  `GetActionText` / `CountMainActions`; quem chama uma delas recebe erro silencioso e
  o script morre no primeiro `io.open`. `pairs(reaper)` é como se descobre isso.

---

## 6. Estado, automação e save

- **Automação**: os 17 params + `soundbank` + `preset` são control ports comuns
  (portas 3…21) → o DAW automatiza e salva sozinho. Conferido no REAPER: escrever o
  param pelo host (`TrackFX_SetParam`) chega como delta, o `run()` vê a mudança e o
  load acontece — e os valores voltam iguais ao reabrir o `.rpp`. A UI guarda o último valor
  escrito pra não recuar quando o host reenvia; host que não reenvia nada mostra
  os defaults do TTL até o primeiro `port_event` — limitação LV2 conhecida.
- **Saída de estado** (`bankState`/`presetCount`, portas 22/23): escritas no topo de
  todo `run()`, sem alocação, lendo só atômicos (`writeStatusPorts()`). É o canal que
  a UI usa pra saber o que o worker fez; `pforge_host_test --so …` confere os quatro
  códigos sem host gráfico. Um host que não repassa `port_event` de saída não quebra
  nada: a UI cai na estimativa de tempo e o rodapé avisa `sem resposta do plugin`.
- **Worker**: implementado e obrigatório pro load. `run()` só compara a porta
  `soundbank` com o último valor e agenda `LoadRequest`; o `Bank` é construído e
  aquecido no worker, publicado por `std::atomic` exchange, e o banco velho é
  liberado na thread do worker (nunca na de áudio). Host sem `lv2:worker` cai na
  thread de load própria do plugin. `pforge_host_test` mede isso: 6,9 s de load
  com `maxBlock` de 0,14 ms.
  A thread de reserva **existe sempre**, mesmo com worker no host, porque um host
  pode aceitar a feature e simplesmente não bombear `work()`: o REAPER faz isso com
  o transporte parado, e o resultado sem a reserva é um `-3` eterno. Ela assume o
  pedido depois de 2 s sem sinal de vida (`workerAlive`, setado dentro de `work()`)
  e se segura pelo `loadingNow` CAS pra nunca carregar em cima do worker.
- **LV2 state extension**: ainda **não** implementada. A decisão tomada foi
  explícita: valor restaurado pelo host é baseline, não pedido — sessão salva
  abre muda até você escolher de novo. Persistir o banco por caminho (`state`) é
  o follow-up natural, em cima do `Bank` atual.

---

## 7. Licenças e limites (resumo)

O PianoForge é **MIT** (`LICENSE`). As dependências vendorizadas em `third_party/`
e o que elas permitem:

| Item | Licença | Uso no PianoForge |
|---|---|---|
| sfizz | BSD-2-Clause | ok (backend SFZ/SF3; não é vendorizado — é instalado ou passado em `-DSFIZZ_DIR`) |
| tinysonic (`src/core/tsf.h`) | MIT / domínio público | ok — backend SF2 |
| miniz (`third_party/miniz`) | MIT | ok (vendor) — inflate dos entries `.pforge` comprimidos |
| LV2 headers (`third_party/lv2`) | ISC | ok — só headers, para as URIs e as interfaces |
| stb (`third_party/stb`) | MIT / domínio público | ok — rasterização de fonte (`stb_truetype`) e decodificação da arte do `.pforge` (`stb_image`) |
| DejaVu Sans / Bold / Serif / Mono | Bitstream Vera (redistribuição permitida com o nome preservado) | ok — vendorizadas em `assets/ui/`, copiadas pro bundle |

**Soundfonts não são do plugin.** O PianoForge distribui código, fontes e headers;
nenhum `.sf2/.sf3/.sfz/.pforge` sai com o plugin, e a lista do usuário nasce vazia
por decisão de produto (§1, regra 2). Um bundle `.pforge` distribuível só leva
conteúdo próprio, CC0 ou CC-BY com os créditos no `manifest.credits[]`.

---

## 8. Roadmap

### a. Standalone

Além do plugin LV2, o PianoForge precisa de uma versão standalone (executável CLI que roda sem host audio).

**Estado atual (2026-10-26):**

- `build/pforge_standalone` renderiza MIDI→WAV de verdade com a cadeia DSP inteira,
  e **exige `--bank`**: sem banco escolhido ele imprime o uso e sai com código 2,
  seguindo a mesma regra do plugin (nada é assumido no seu lugar).
- Depende de `LD_LIBRARY_PATH` apontando para `build/sfizz/library/lib` (link
  dinâmico para a `libsfizz.so` local) só no caminho SFZ; SF2 vai por tinysonic.
- Dois modos: o render roteirizado (sequência de acorde fixa → WAV) e `--live`, que
  toca na placa de áudio com o teclado do computador como entrada de MIDI.

**O que falta para o standalone cobrir o projeto todo:**

1. ~~Ler bundle `.pforge`~~ — feito: `src/core/Bundle.cpp`, registrado como banco
   normal do `soundfonts.list`, com o manifest no painel (nome do produto na lista,
   arte e nome de preset — §2). Faltam só os `params` do preset, que têm o problema
   de eixo descrito lá.
2. ~~`--preset`~~ — feito: `pforge_standalone --bank X.sf2 --preset N` chama
   `Engine::setPresetIndex` e recusa índice fora do banco com o `presetCount` na
   mensagem (SFZ não tem biblioteca de presets, e índice 0 em SFZ não é troca — por
   isso só reclama quando `N > 0`). Prova com um banco de 62 presets:
   os WAVs de `--preset 0`, `3` e `61` são byte-a-byte diferentes; `--preset 62`
   erra antes de renderizar.
3. ~~Opção de saída para a placa de áudio em vez de WAV~~ — feito com `--live`:
   `src/core/AudioOut.{hpp,cpp}` abre o ALSA por **`dlopen("libasound.so.2")`** (a
   máquina não tem `<alsa/*.h>`, só `<sound/asound.h>` do kernel) e usa
   `snd_pcm_open` + `snd_pcm_set_params` + `snd_pcm_writei` (a máquina não tem
   `<alsa/*.h>`; os inteiros vêm de `<sound/asound.h>`, do kernel — e é aí que mora
   o detalhe: `S16_LE` é **2** e `RW_INTERLEAVED` é **3**, não 0). Com access 0 o
   `writei` devolve `-EINVAL` em todo bloco.
   `--device default` cai no
   plugin `libasound_module_pcm_pipewire.so` (existe nesta máquina), então o mixer do
   desktop continua funcionando; `--device plughw:0,0` vai direto na interface USB. O `writei`
   bloqueante **é** o relógio do laço: é ele que cadencia os blocos de 256 amostras.
   Entrada pelo teclado do computador em modo raw (`cfmakeraw` + `poll` de timeout 0):
   `z..m` e `q..p` são duas oitavas, apertar de novo solta a nota (o terminal não
   entrega "chave solta"), `espaço` = pedal, `-`/`=` = oitava, `backspace` = calar
   tudo, `esc`/`Ctrl-C` = sair. `--device none --out arquivo.wav` grava a mesma
   execução, e `--feed "c4:300 e4:300"` troca o teclado por uma sequência com tempo —
   é assim que o modo ao vivo é verificado sem ninguém tocando.
   O que nada disso prova é a latência percebida: `--latency` pede 40 ms ao driver e
   `snd_pcm_writei` segura o laço, mas nenhum ouvido conferiu o round-trip tecla →
   alto-falante. Fica para quando alguém tocar.

### b. Como o `.pforge` é testado sem distribution de samples

O repo não carrega soundfont nenhum, então o teste de bundle usa fixture:

- `packaging/fixture/` tem um `bank.sf2` mínimo gerado pelo próprio empacotador, um
  `manifest.json` com dois presets e um `piano.png`. `packaging/bundle.py pack` monta
  `test.pforge` e o `verify` confere o CRC de cada entry; `bad.pforge` é o mesmo
  arquivo com **um byte invertido no payload**, usado pelo teste H (banco inválido não
  silencia o que está tocando).
- Um `.pforge` entra em qualquer teste que aceita `.sf2` — `pforge_host_test --sf2
  arquivo.pforge` roda a suíte inteira pelo caminho do bundle (diretório, CRC, inflate
  e o `tsf_stream` lendo na posição certa).
- **`pack` grava entry por basename e o `verify` recusa `imagem` com `/`** — a
  restrição é do empacotador, não do leitor: `src/core/Bundle.cpp` aceita nome exato ou
  sufixo. `"imagem": ""` é rejeitado pelo verify como referência a blob inexistente;
  omita a chave quando não há arte.

### c. Próximos passos sugeridos

1. ~~`delayFrames` ignorado~~ — **resolvido**: `Engine` agora enfileira os eventos
   do bloco (array fixo, sem alocação no RT) e `render()` fatia o bloco nos
   instantes em que algo vence, aplicando cada evento na sua amostra. Medido por
   `pforge_host_test` teste F: notas com delay 0/200/400 num bloco de 512 têm
   onset nas amostras 4/201/400. Consequência: `process()` virou `render()` e o
   `pitchValue` de bend passou a ser assinado (-8192..8191) na porta do LV2.
2. ~~Port de status (ou `lv2:state` na UI) pra trocar o chute de "FORJANDO" por
   feedback real do worker~~ — **resolvido** sem `state`: as portas de saída 22
   (`bankState`) e 23 (`presetCount`) são escritas no topo de `run()` e a UI lê por
   `port_event`. Medido pelo `pforge_host_test` teste I (worker não drenado entre
   blocos, como num host real): `-3` durante, índice publicado depois, `-2` no
   `-1`, `-4` no banco inválido.
3. ~~Preset no painel~~ — **resolvido**: pílula `◂ PRESET i/N ▸` no rodapé, escrita
   na porta 21 por clique, roda do mouse e ← →. Aparece só quando a porta 23 informa
   mais de um preset. Prova no X11 real: `pforge_ui_probe --port 22:0 --port 23:62
   --port 21:7 --click 583:479` → `[21]=8.000` e o rodapé passa a ler `preset 9/62`
   (o host devolve o valor escrito como `port_event`, então o número na tela é o que
   o plugin recebeu, não o que a UI pediu).
4. ~~Bundle `.pforge` no plugin~~ — **resolvido** (`src/core/Bundle.{hpp,cpp}`,
   seção 2), e o painel já mostra o manifest (regra 12 da seção 5). O que falta do
   bundle é aplicar `presets[].params` — que exige `lv2:state`, item 6.
5. ~~Testar a UI num host LV2 de verdade~~ — **resolvido no REAPER 7.83**
   (`/opt/REAPER`, bundle em `~/.lv2` com md5 igual ao build). O painel pinta
   dentro da janela de FX, clique e arrasto reais escrevem as portas 20 e 3, o
   `estado` publicado é lido pelo host (`-2 → -3 → 0`), recall volta `banco=-1` e
   o pico medido no master é −4,00 dBFS tocando e 0,00000000 sem escolher piano.
   Duas coisas que só um host de verdade ensinaram, e que já estão pagas: ele não
   chama `idle()`/`show()` (regra 6 da seção do embed) e aceita `lv2:worker` sem nunca
   rodar `work()` com o transporte parado (thread de reserva com watchdog de 2 s).
   Detalhe em "Conferido no REAPER 7.83".
6. **Aplicar `manifest.presets[].params`** — hoje o painel lê o bundle e mostra
   nome/arte, mas nenhum param do manifest vira valor de knob. Não é descuido: os
   17 valores têm de chegar ao plugin por um caminho que o host grave junto, senão
   a tela e o áudio divergem (a mesma classe de bug da regra 7 da seção do embed).
   O caminho é `lv2:state` na UI + um "carregar preset do bundle" que escreva as
   portas visíveis e ocultas de uma vez, com o host vendo cada escrita.
7. **Fazer o SF2 caber em menos RAM** — o piso de §1 (2× o arquivo, medido no
   `tsf_load`) só cai com um sampler que leia `int16` na hora ou faça streaming do
   `smpl` com cache. Isso não é otimização, é outra decisão de produto: muda o som
   (interpolação, ruído de arredondamento, filtros) e joga fora a invariant de
   *render bit-idêntico* que hoje é a prova de cada mexida no DSP. Fica para quando
   1,2 GB doer mais do que a igualdade de render importa.
8. **`kMaxVoices = 256`** — a polifonia parou de ser um problema de *corte*: nenhum teto
   descarta nota em silêncio hoje, e isso é contado pelo próprio backend
   (`--pedalload`: `MUDAS=0 regioes sem slot=0` em 128/192/256/384; teste M na seção 4).
   O que o número decide é a idade média de uma corda pedalizada quando o abafador cai —
   3,05 s em 128, 5,20 em 192, 7,62 em 256, 13,86 em 384 — contra 313/465/623/956 µs por
   bloco de 512 com a piscina lotada, com a RAM do banco inalterada (491 MB em todos).
   256 foi escolhido porque nessa idade o médio e o agudo já passaram dos −40 dB; o
   subterfúgio anterior (roubo por nível de envelope) não existe mais, e a tabela é
   reimprimível com `--voices` e `--pedalload`.

## 9. Checklist do que existe

1. ✅ Bundle `.pforge` (empacotador/verificador `packaging/bundle.py`), sfizz, toolchain
2. ✅ Wrapper LV2: 24 ports (22 entradas de controle/áudio/MIDI + 2 saídas de
     estado), atom MIDI, `lv2:worker`, sem auto-load
3. ✅ Backends de amostra com troca segura fora da thread de áudio
4. ✅ DSP chain v1 (itens da seção 4)
5. ✅ UI "Forja de Pianos" (X11UI, framebuffer próprio, 10 knobs + seletor de banco
     + seletor de preset)
6. ✅ Harness offline: `pforge_host_test` (silêncio/load/retire/baseline/status) e
     `pforge_ui_preview` / `pforge_ui_probe` (visual e interação sem DAW)
7. ✅ plugin lê e toca um `.pforge` (CRC, inflate, stream), recusando bundle
     corrompido sem silenciar o piano em uso
8. ✅ feedback real do worker na UI (`bankState`) — "FORJANDO"/"NAO ABRIU"/"piano na
     forja" vêm do plugin, não de estimativa
9. ✅ standalone ao vivo: `--live` toca na placa (ALSA por `dlopen`, `default` via
     PipeWire) com o teclado do computador como MIDI; `--device none --out` grava a
     mesma execução e `--feed` a reproduz sem ninguém tocando
10. ✅ verificação num host LV2 real (REAPER 7.83): painel, portas, recall, worker
     não-bombeado e áudio no master
11. ✅ painel lê o manifest do `.pforge`: nome do produto na lista e no rodapé,
     arte de `presets[].imagem` na placa e nome de preset na linha miúda quando a
     contagem casa com a porta 23
12. ⬜ aplicar `presets[].params` (exige `lv2:state` — item 6 do §8c)
13. ✅ orçamento medido, não estimado: custo por estágio e por fronteira (§4), RAM por
     banco e pico da troca (§1). O swap nunca segura dois bancos no ar, o caminho sem
     `lv2:worker` não liberta banco dentro de `run()` (8,471 ms → 0,509 ms medidos), e
     as duas otimizações que a medição rejeitou estão escritas em §1 em vez de no código
14. ✅ master com teto assintótico: nenhuma saída do plugin atinge 0 dBFS (era o que
     achatava as 78 amostras na gravação do usuário; §4 "O teto do master"). O ganho em
     si não mudou — a folga de volume é decisão de ouvido
15. ✅ release automático para os três sistemas (`.github/workflows/release.yml`):
     Linux/Windows (MSVC)/macOS (universal), com as cinco provas A–E da seção 11 do
     `UI-KIT.md` rodando em cada um contra bancos **sintéticos** (`--makesf2`), porque
     o repositório não distribui soundfont. O que isto ainda não provou: MSVC e Cocoa
     nunca foram compilados fora do runner — a validação é a primeira execução do CI.
