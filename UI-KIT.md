# PianoForge — kit para escrever uma UI

Tudo que uma interface precisa saber sobre o plugin para não ter de ler o C++ dele:
o contrato com o shell, os 17 parâmetros com faixa e default, o protocolo de escrita e
leitura, os códigos de estado, onde mora a lista de pianos, o formato `.pforge`, e
por último o painel atual — geometria, paleta, interação — como ponto de partida.

Escrito contra o código de hoje. Se um número aqui divergir de
`src/core/Parameters.hpp` ou de `src/core/StatusPorts.hpp`, o código ganha e este
arquivo está errado; a tabela de parâmetros é derivada do primeiro, e o
`pforge_vst3_probe` confere a contagem e a faixa pelo host.

---

## 1. O contrato com o shell

O plugin é um **VST3 único** e o painel compila dentro do mesmo binário: não há UI
carregada à parte, nem descritor de UI a implementar. Uma UI nova herda de
`pfui::ForgeUi` (`src/ui/ForgeUi.hpp`) e nasce de `pfui::createUi(...)`
(`src/ui/UiFactory.hpp`) — essa é a única porta de entrada.

| O que | Por que é obrigatório aqui |
|---|---|
| `createUi(parent, bundlePath, write, controller, &widget)` | o shell entrega o container do host (`IPlugView::attached`) e o caminho do bundle, de onde saem as quatro DejaVu |
| `WriteFunction` + `void* controller` | **é assim que se muda um parâmetro**: `write(controller, evento, sizeof(float), 0, &v)`; o shell traduz para `setParamNormalized` |
| `portEvent(port, size, format, buffer)` | **é assim que se fica sabendo** o que o plugin publicou e o que o host automatizou |
| `widgetOut` | devolve o handle nativo que o host embute (HWND, janela X11, `NSView*`) |
| `destroyUi(ui)` | desmapear, parar a thread da UI e fechar — na thread em que foi criado |

O host chama `attached()`, pode chamar `setFrame()` e depois pode passar minutos
calado. **Não existe `idle()` no VST3**, então tudo que tem de acontecer por tempo —
animar, redesenhar, sentir automação — é responsabilidade da própria UI, na thread
dela (§9).

> **A armadilha número um deste projeto**: a assinatura do callback é
> `(controller, port, buffer_size, format, buffer)` — **`buffer_size` antes de
> `format`**. O tipo é nosso (`pfui::WriteFunction`, em `src/ui/ForgeUi.hpp`), mas
> nasceu idêntico ao do formato que existia antes, e a ordem continua sendo a ordem:
> os três primeiros parâmetros são `uint32_t`, então nomeá-los invertido compila,
> liga, e o `if (format != 0) return;` passa a ver o tamanho do buffer (`4`) e
> rejeita **todo** evento — a UI fica muda sem erro nenhum.
>
> A nota já esteve escrita ao contrário nesta documentação (e no código, e no probe,
> que errava espelhado e por isso dava verde). Se você está portando a UI: confira a
> ordem no header, nunca na memória — nem na deste arquivo.

---

## 2. Mapa dos 17 parâmetros e do estado do painel

O índice é o `ParameterId` do host e, ao mesmo tempo, o número do evento do painel
(`evento = 3 + ParameterId`): **nunca reordenar**, só acrescentar depois de 23. Áudio é
um barramento estéreo declarado no shell e MIDI chega por `IEventList` — nenhum dos
dois entra nesta numeração.

### Os 10 knobs do TIMBRE (são os `ParameterId` 0..9, e o painel desenha nesta ordem de tela)

| # | símbolo | nome (host) | rótulo curto (tela) | min | max | default | o que faz |
|---|---|---|---|---|---|---|---|
| 3 | `volume` | Volume | Volume | 0 | 1 | **0.85** | saída, com rampa suave |
| 4 | `hammer` | Ataque do Martelo | Martelo | 0 | 1 | **0.35** | transiente de feltro (§ DSP abaixo) |
| 5 | `velCurve` | Curva de Dinâmica | Dinâmica | 0 | 1 | 0.5 | curva velocity→amplitude |
| 6 | `sympRes` | Ressonância das Cordas | Ressonância | 0 | 1 | 0.25 | cordas simpáticas |
| 7 | `lid` | Abertura da Tampa | Tampa | 0 | 1 | **1.0** | filtro de tampa |
| 8 | `bassEQ` | Graves (EQ) | Graves | 0 | 1 | 0.5 | shelf, 0.5 = plano |
| 9 | `midEQ` | Médios (EQ) | Médios | 0 | 1 | 0.5 | peaking, 0.5 = plano |
| 10 | `highEQ` | Agudos (EQ) | Agudos | 0 | 1 | 0.5 | shelf, 0.5 = plano |
| 11 | `chorus` | Chorus | Chorus | 0 | 1 | **0.0** | desligado por padrão |
| 12 | `Reverb (Ambiência)` → `reverb` | Reverb (Ambiência) | Reverb | 0 | 1 | 0.10 | sala |

### Os 7 do MOTOR (portas 13..19 — knobs da segunda aba)

| # | símbolo | min | max | default | nota |
|---|---|---|---|---|---|
| 13 | `reverbType` | 0 | 3 | 1 | inteiro com `scalePoint`: Sala Pequena / Salão de Estúdio / Plate / Catedral |
| 14 | `trimDb` | −12 | 12 | 0 | bipolar — desenhar a partir do centro |
| 15 | `width` | 0 | 2 | 1 | |
| 16 | `releaseDamp` | 0 | 1 | 0.35 | |
| 17 | `hammerTone` | 0 | 1 | 0.5 | brilho do martelo |
| 18 | `sympTune` | 0 | 1 | 0.5 | |
| 19 | `engineTune` | −100 | 100 | 0 | cents, bipolar |

Até out/2026 eles eram **texto miúdo no rodapé** e não se editavam. Agora são knobs da
aba **MOTOR** (`TIMBRE | MOTOR` no cabeçalho, grade 4×2 dentro da chapa `06 MOTOR`),
com o mesmo arrasto/roda/duplo-clique dos dez do TIMBRE e escrevendo nos mesmos
eventos 13..19. Os chips do rodapé ficaram como leitura. O `hammer` (evento 4) também mudou de
escala: `Engine::applyNoteOn` multiplica o valor por 0.10, então 100% do knob vale o
que 10% valia antes.

### Banco, preset e estado

| # | símbolo | sentido | faixa | default | o que é |
|---|---|---|---|---|---|
| 20 | `soundbank` | UI → shell | **−1** .. 255, inteiro | **−1** | índice na lista registrada. `−1` = "nenhum piano" |
| 21 | `preset` | UI → shell | 0 .. 1023, inteiro | 0 | `presetindex` do SF2 publicado |
| 22 | `bankState` | **shell → UI** | −4 .. 255 | −2 | o que o plugin está fazendo (§5) |
| 23 | `presetCount` | **shell → UI** | 0 .. 1024 | 0 | presets do banco **publicado** |

No host, `Banco` e `Preset` também existem como parâmetros VST3 (escondidos da lista
genérica, automatizáveis): é por eles que a sessão salva sabe qual piano estava lá.
Os códigos de `bankState` continuam sendo o que a UI lê: `-4` falha, `-3` carregando,
`-2` nenhum piano, `≥0` o índice que está soando.

---

## 3. Escrever e ler

**Escrever** (knob, linha da lista, preset, tudo):

```c
float v = 0.42f;
write(controller, /*evento=*/4, sizeof(float), /*format=*/0, &v);
```

`format` 0 = "o buffer é um `float` cru". É o único formato que o shell conhece, e ele
traduz `setParamNormalized` a partir dele — a UI não fala com o host diretamente.

**Ler** — `portEvent(port, buffer_size, format, buffer)`, aceitando só
`format == 0 && buffer_size == sizeof(float)`:

Os dois guarda-chuivas são obrigatórios, não higiene: sem checar `buffer_size`, um
host que notifique com outro tamanho faz a UI ler 4 bytes de um buffer menor (lixo)
ou metade de um evento maior. E sem `std::isfinite(valor)`, um NaN vindo de
automação quebrada **trava o knob para sempre** — o zipper não escapa de NaN e nem
o duplo-clique no default conserta, porque `NaN + (alvo − NaN)·α` é NaN.

| evento | o que chega | o que a UI faz |
|---|---|---|
| 3..19 | o valor que o host automatizou ou recallou | atualiza o knob, **mas guarda o último valor escrito** para não recuar quando o host reenvia |
| 20 | o índice que o plugin aceitou como pedido | marca a linha selecionada |
| 21 | índice de preset | atualiza a pílula e a linha do rodapé |
| 22 | código de estado | liga "FORJANDO", "pronto", "não abriu" (§5) |
| 23 | contagem de presets | decide se a pílula de preset aparece |

> O shell tem o último valor de cada parâmetro e o painel sente automação por um poll
> de ~6 Hz na thread da UI (`onHostPoll`), não por callback do host. Se nada chegou
> ainda, o painel mostra os defaults de `kParameterDefs` — não um valor inventado.

---

## 4. As regras de produto que uma UI não pode violar

Vêm de "puxa algo e eu queria poder escolher". São o contrato, não sugestão:

1. **Nada é carregado sozinho.** O plugin nasce com `bank_ == nullptr` = silêncio. A
   UI nasce com `current_ = -1`. Uma UI que pré-seleciona o primeiro item da lista no
   `instantiate()` **reintroduz o bug original** — ela escreve na porta 20 e o plugin
   obedece.
2. **Valor restaurado pelo host não é pedido.** O `activate()` grava o valor da porta
   20 como baseline; só uma *mudança* depois disso carrega algo. Consequência aceita
   pelo usuário: **sessão salva abre muda** até escolher de novo. A UI não deve
   "consertar" isso escrevendo o índice de novo na abertura.
3. **Escolher = escrever o índice na porta 20.** Uma vez. Não em loop, não no redraw.
4. **`−1` é uma escolha, não um vazio.** Escrever `−1` publica banco vazio e silencia.
   O botão de "nenhum piano" da UI faz exatamente isso.
5. **Banco inválido não silencia nada.** Se a porta 22 vier `-4`, o piano anterior
   continua soando; a UI diz "não abriu — ainda toca X". Nunca apague o nome do piano
   em uso por causa de uma falha.
6. **Trocar de banco zera o preset.** A UI deve zerar o índice local quando a 22 muda
   de `-3` para `≥0`.
7. **A pílula de preset só aparece com `presetCount > 1`.** SFZ/SF3 têm 1 preset
   sempre. `> 0` é o bug clássico (aparece "preset 1/1" e não faz nada).
8. **Índice de preset ≠ nome de preset.** Uma control port leva um `float`. Não existe
   canal para o nome do preset do SF2; o máximo honesto é "preset i/N — arquivo".
9. **Lista vazia é o estado inicial, não um erro.** O registro não é semeado por nada
   (§6): na primeira execução a biblioteca tem `00` itens e o plugin está mudo. A UI tem
   de desenhar isso — véu no ar, pedido explícito de um soundfont, e `Procurar`/`+ Pasta`
   funcionando sem haver nada selecionado. Pré-carregar uma lista de exemplo, um `.sf2`
   do bundle ou o `soundfonts.conf` do formato antigo é violar a regra 1 pela raiz.

---

## 5. O ciclo de vida de um load, e a porta 22

A UI não tem como saber o que o plugin está fazendo — por isso a 22 existe. Antes de
ela, "FORJANDO" durava uma estimativa de 35 MB/s derivada do tamanho do arquivo, e
mentia.

```
UI escreve 20 = i
   plugin: run() vê o delta -> schedule_work(MSG_LOAD_BANK)  [RT, sem alocação]
   22 = -3  (carregando)          worker: devolve a RAM do banco velho, depois abre o arquivo
   22 = i   (publicado)           work_response publica por troca de ponteiro
   23 = N                          presetCount do banco novo
```

| valor da 22 | significado | o que desenhar |
|---|---|---|
| `-4` | o último pedido falhou | "NÃO ABRIU" + o que continua soando |
| `-3` | worker/carregador em cima do arquivo **ou esperando a RAM do velho voltar** | "FORJANDO <nome>" + barra ⠿ |
| `-2` | publicado: nenhum piano | "ESCOLHA UM PIANO" / "a forja está fria" |
| `≥ 0` | índice do banco **soando** | nome do banco, painel aceso |

**Por que `-3` agora inclui "esperar a RAM voltar"**: um SF2 custa o **dobro do
arquivo** em RAM (o tsf converte int16→float32 ao carregar — 612 MB de arquivo viram
1,2 GB residentes). Alocar o novo com o velho ainda no ar custa **1,7 GB** ao par de
pianos do registro real. O load então *estaciona* o pedido: silencia o banco publicado
(`publishBank(nullptr)`, `allNotesOff()`), descarta o banco velho e só pega o disco quando o
`munmap` terminou. A porta 22 **continua `-3`** durante o estacionamento — nada soa, nada
falhou, e "FORJANDO" é a verdade. Consequência que a UI precisa aceitar: **o som corta
antes de o arquivo novo abrir**, com um degrau de silêncio no meio do `-3`. Isso é
correto, não é bug: o swap de piano mata vozes em andamento de qualquer jeito.

Três regras de posse, que ninguém de fora vê e que já custaram um `corrupted size vs.
prev_size while consolidating` (dois threads deletando a mesma `Bank*`):

* A fila de aposentadoria tem **dono único**: a thread de carga do plugin. Não existe
  worker de host no VST3 (não há `work()` a pedir), então quem libera é sempre ela; o
  `Engine` ainda assim protege com `retireMx` + steal-then-delete, porque um pedido
  pode chegar enquanto o free anterior ainda está na mão.
* **Nada de `free()` de banco dentro de `process()`**, em hipótese alguma: medido **8,471 ms**
  para libertar 49 MB dentro do bloco de áudio, **0,509 ms** com o free na thread de
  carga. Um banco de 1,2 GB seria um xrun de ~100 ms em toda troca.
* O estacionamento tem **teto de 300 ms**. Passou disso, o load segue com o velho ainda
  aposentado na RAM — um host com o transporte parado não pode deixar o pedido preso em
  "FORJANDO" para sempre.

**Detalhe que já custou uma sessão inteira** (e que no VST3 deixou de existir): com
`lv2:worker`, um host podia aceitar `schedule_work()` com sucesso e **nunca rodar
`work()` com o transporte parado** (o REAPER fazia isso), e o pedido ficava preso em
`-3`. Hoje a carga é sempre da thread do plugin, então a UI recebe `-3` seguido de um
índice independentemente do que o host faça. O que continua valendo: **não declare
timeout curto** para o `-3` — um SF2 grande leva segundos, e o estacionamento pode
somar 300 ms.

---

## 6. Onde vivem os pianos

O arquivo, por sistema (`SoundbankManager::configDir()`, a mesma ordem de prioridade nos
três):

| | caminho |
|---|---|
| `XDG_CONFIG_HOME` definido | `$XDG_CONFIG_HOME/pianoforge/soundfonts.list` |
| Linux / macOS | `~/.config/pianoforge/soundfonts.list` |
| Windows | `%APPDATA%\pianoforge\soundfonts.list` (senão `%USERPROFILE%\AppData\Roaming\...`) |

Num host gráfico de Windows não existe `HOME` — o REAPER é launchado pelo explorador e só
traz `USERPROFILE`/`APPDATA`. Sem esse fallback o registro cairia em `/.config/...`
relativo ao diretório de trabalho do host, e o piano que o usuário registrou sumiria na
abertura seguinte. É também por isso que `XDG_CONFIG_HOME` é lido nos três: é o override
com que o harness monta um `~/.config` de mentira.

Uma linha de caminho absoluto por piano, **na
ordem em que foi adicionada**. `#` começa comentário. Nada é varrido: nenhuma pasta é
lida recursivamente, e uma linha que não termine em `.sf2/.sf3/.sfz/.pforge` (uma pasta
inclusa) não entra na lista — nem como item riscado, porque riscado é o arquivo que sumiu
(foi a varredura recursiva de 26 arquivos que fazia o plugin abrir carregando uma bateria).

* **O arquivo não é semeado por nada.** Na primeira execução ele não existe, e o plugin não
  copia para lá `soundfonts.conf` (o formato antigo, de pastas), nem `custom_soundfonts.list`,
  nem `~/.local/share/pianoforge/soundfonts`, nem qualquer piano do bundle — o bundle não
  traz piano nenhum (`.so`, `.ttl` e as TTF). Uma UI tem de desenhar esse estado: lista
  vazia, nenhum banco publicado, e um caminho visível para registrar o primeiro arquivo.
  Quem escreve no registro é só o usuário (`addSoundfont` / `addMany`), e o arquivo nasce
  no primeiro `add`.
* O **índice na porta 20 é a linha do arquivo**, contada a partir de 0, pulando as
  linhas inválidas. Remover uma linha **desloca todos os índices seguintes** — a UI
  deve reescrever a porta 20 depois de um "Remover", ou o banco soando passa a ser
  outro sem ninguém ter pedido.
* Duplicata é ignorada (comparação por `canonical`, não por nome).
* Extensões aceitas: `.sf2`, `.sf3`, `.sfz`, `.pforge`.
* Uma UI que lista precisa de: nome (basename sem extensão), tamanho em bytes e se o
  arquivo ainda existe. `SoundbankManager::list()` devolve os três; o arquivo sumido é
  riscado na tela e não pode ser escolhido.
* Os botões "Procurar" e "+ Pasta" da UI atual chamam `zenity`/`qarma` por `popen`.
  `kdialog` não aceita `--multiple` do mesmo jeito. Qualquer substituto tem de
  **rodar fora do mutex** da UI: `popen` bloqueante com a lock tomada congela o redraw.

---

## 7. O contêiner `.pforge`

Zip "PFG1" com cabeçalho + diretório + manifest lidos no `open()`; o banco é lido por
`tsf_stream` na posição certa do arquivo (zero cópia de 250 MB), com CRC32 por entry.

Manifest (JSON, no início do arquivo):

```json
{
  "produto": "Forja de Teste",
  "presets": [
    { "nome": "Felt Grand", "sf2": "FeltGrand.sf2", "imagem": "grand.png",
      "params": { "hammer": 0.4, "reverb": 0.2 } }
  ]
}
```

O que o painel faz com cada campo:

| campo | tela |
|---|---|
| `produto` | troca o nome do arquivo na lista — a lista lê o nome do piano, não `ForjaTeste.pforge` |
| `presets[].nome` | rótulo da pílula e a linha miúda do rodapé, **só quando `count(presets[]) == presetCount` da porta 23** |
| `presets[].imagem` | arte na placa abaixo da lista. É o **nome do entry**, sem `/` — `packaging/bundle.py` grava por basename e o verificador dele recusa caminho |
| `presets[].sf2` | banco deste preset. Se o bundle tiver presets com `sf2` **diferentes**, o painel mostra só o preset 0 e avisa no stderr: um `.pforge` multi-banco ainda não é suportado |
| `presets[].params` | **deliberadamente não aplicados.** O índice que o plugin troca é o `presetindex` do SF2; `presets[]` do manifest é outro eixo. Escrever 17 parâmetros de dentro da UI sem o host pedir é divergir tela de áudio. Fica atrás de `setState`/`getState` (§8c item 6 do PIANOFORGE.md) |

A regra da contagem existe porque, se o manifest anuncia 2 presets e a porta 23 diz 62,
os dois eixos não são o mesmo — mostrar nome do manifest seria mentir sobre qual som
sai.

---

## 8. O painel atual

Janela **1000×520** (não redimensionável; se o host impuser outro tamanho, o painel se
adapta a ele, nunca maior). É a transcrição de
`ideia para ui/PianoForge-Concept-v2/index.html` — o `.window` de `min(1015px) ×
min(519px)` — e cada constante abaixo tem uma regra CSS correspondente. Onde o mock
divergiu de uma decisão anterior (inclusive a grade 5×2 ditada à mão), **o mock
ganhou**.

```
+------------------+--------------------------------------------------------------+
| FORJA            | * CONTROLES DE TIMBRE            [FORJA DSP] [ENGINE tsf]   |
|   DE PIANOS      +--------------+------------+----------------+----------------+
| BIBLIOTECA   02  |01 MASTER     |03 DINAMICA |04 CORPO        |05 ESPACO       |
| [Q filtrar...  ] |   .----.     |  .----.    |  .----. .----. |   .----.       |
| * Forja Teste    |  ( 0.85 )    | ( 0.35 )   | ( 1.00 )( 0.50)|  ( 0.00 )     |
|   172 MB         |   Volume     |  Martelo   |  Tampa   Graves|   Chorus       |
| o Felt Grand     |--------------+------------+  .----. .----. |   .----.       |
| +------------+   |02 RESONANCIA |            | ( 0.50 )( 0.50)|  ( 0.10 )     |
| |   GRAND    |   |   .----.     |  .----.    |  Medios Agudos |                |
| | (manifest) |   |  ( 0.25 )    | ( 0.50 )   |                |   Reverb       |
| +------------+   |   Resson.    |  Dinamica  |                |                |
| [Procurar ...]   +--------------+------------+----------------+----------------+
| [+Pasta][Remover]                                                              |
| bancos locais...                                                               |
+------------------+-------------------------------------------------------------+
| Forja de Teste (172MB) [CARREGANDO] Forja...  < PRESET 8/62 >   reverbType Sala1|
| ForjaTeste.pforge - 172MB  0 presets - estimativa por tamanho  trimDb +0.0 ...  |
+---------------------------------------------------------------------------------+
```

Colunas do `.control-area`: `.78 / .88 / 1.64 / .92 fr`, com a coluna 1 empilhando
MASTER sobre RESSONÂNCIA (`left-stack`, `grid-template-rows: 1fr 1fr`) e o CORPO em
`body-grid` 2×2. **Os grupos voltaram a ser colunas** — é o que o `.78` de um knob só
resolve sem truque: o disco de um grupo sozinho ocupa a meia-altura que lhe cabe, em
vez de herdar a altura inteira e cair no meio de tudo.

### Geometria (`src/core/ui/ForgePanel.cpp`)

| constante | valor | regra CSS |
|---|---|---|
| `kPadWin` / `kGapWin` | 7 / 7 | `.workarea{padding:7px 7px 5px; gap:7px}` |
| `kSideW` | 238 | `.workarea{grid-template-columns:238px 1fr}` |
| `kStatusH` | 76 | `.window{grid-template-rows:1fr 76px}` |
| `kHeadH` | 38 | `.panel-head{height:38px}` |
| `kSidePadX/Y` | 12 / 13–11 | `.sidebar{padding:13px 12px 11px}` |
| `kBtnH` | 28 | `.ui-button`, `input{height:28px}` |
| `kRowH` / `kRowGap` | 31 / 3 | `.bank-row{min-height:31px}`, `.bank-list{gap:3px}` |
| `colFr(g)` | .78/.88/1.64/.92 | `.control-area{grid-template-columns}` |
| `knobU[k]` | `lado/80` | **é o fator de escala do SVG**: todo número do `viewBox 0 0 80 80` vira pixel multiplicando por `u` |
| `veilCard` | `min(280, w−24) × 156` | `.veil-card{max-width:280px; padding:18px}` — a altura é a do **conteúdo**; com 132 o guarda do botão comia a terceira linha da frase |
| `statusbar` | 3 colunas `1.05 / 1 / 1.7 fr`, `gap 12`, `pad 8×12` | `.statusbar{...}` |

Regras que não podem ser perdidas por quem reimplementa:

* **`knobs[]` é indexado por `ParameterId`**, e `kGroups[]` decide a ordem de tela.
  Preencher `knobs[k]` em ordem de lista faz todo knob herdar o "Volume 0.85".
* Os 10 visíveis têm de ser os ids 0..9; a UI avisa no construtor se isso quebrar.
* O **`veilCard` é geometria pura calculada em `layoutFor()`**, antes de qualquer
  desenho, porque o *hit-test* do botão precisa dele e a UI consulta o `Layout` que
  `layoutFor()` devolveu — não uma cópia que `drawPanel()` mexeu.
* O vão entre grupos nunca é menor que o vão interno de um grupo.

### Paleta (`src/core/ui/ForgeTheme.hpp`, ARGB `0xAARRGGBB`)

Os nomes são os do CSS; cada linha abaixo foi conferida byte a byte contra
`index.html`.

| grupo | valores |
|---|---|
| ferros | `iron0 #0D0F13` (página) · `iron1 #13161C` (janela) · `iron2 #191D24` · `iron3 #20252D` · `iron4 #282E37` |
| superfícies | `card #171B22` / `cardLo #14171D` (sidebar, painel) · `headHi` e `groupHi/groupLo` (gradientes do cabeçalho e dos cards) · `well #101318` (campo de busca) · `btnTop/btnBot #242A33/#1C2128` · `rowHover #212630` · `rowOn #292E37` |
| linhas | `line #303743` · `line2 #3B4551` · `line3..line7` (bordas de card, sidebar, painel, réguas) · `edge #424954` (moldura da janela) · `div #343C47` (divisores do rodapé) |
| texto | `text #ECEEF1` · `muted #9AA3AE` · `dim #68727F` · `rowText #D2D6DC` · `knobName #C3CAD2` · `knobTip #64707D` · `chipTx/chipVal` · `groupTx #8E99A6` · `headTx #929BA7` · `countTx #626C78` |
| brasa | `ember #FF7A18` · `ember2 #FFB05A` (valor modificado) · `ember3 #FFDCAE` (**ponteiro**) · `accent #824419` (fim do filete) · `indexTx #70523C` ("01") |
| estado | ok `#A7D1AD/#1E3027/#34523D` · warn `#FFBD75/#342617/#785126` · err `#FF9A8E/#321D1B/#71403B` |
| knob | `track #3B4552` · face `#343B45 → #242A33 → #171B21` · `faceEdge #414A56` · `pin #57616D` |
| véu | `veil #AA0C0F13` · `veilCard #EF171B22` · botão `#322519` com linha `#78471F` e texto `#F6C18C` |
| toast | fundo `#242D29`, linha `#49614E`, texto `#D8EADB` |

`heat(t)` é a única função de cor "viva": bronze → `ember` em t=0.5 → `hot` em t=1.
**Sem banco publicado o painel é desenhado com `live = false`** e o véu entra por
cima; as duas coisas são o mesmo estado, dito de formas diferentes.

### Desenho do knob (tudo em unidades de `u`, sobre o `viewBox 80`)

* Trilho: `r = 34u`, espessura `3,7u` (`stroke-width:3.7`), de `0,75π` a `2,25π`,
  com `linecap` redondo nas duas pontas.
* Arco ativo: mesma trajetória até `stop`, espessura `4u`, **mais uma camada a
  `±4,6u` com alpha 0,27** — é o `drop-shadow(0 0 3px #ff7a1844)`, que é sombra do
  *caminho*, não halo radial.
* Face: disco `26u` com gradiente diagonal em três paradas (0% / 58% / 100%), contorno
  `1u` em `faceEdge`, e um anel de branco a 7% entre `1,05π` e `1,95π` (`.face-shine`).
* Ponteiro: segmento de `r = 12u` a `r = 20u` (**dentro da face**, nunca sobre o
  trilho), 1,6u em `ember3`, com duas camadas de brilho de 3,2u/2,2u a alpha 0,12/0,20.
  Uma camada só de 5,4u virava barra de 8,6 px encostando nos dígitos do valor: o
  `blur(3px)` do navegador é 3 px numa face de ~48 px, e aqui a face tem ~68 px.
* Valor: **dentro da face**, centrado, baseline em `cy + 16u`, fonte 8u tabular;
  `ember2` se `|value − defValue| > 1e-4`, `muted` se o painel está frio.
* Nome do knob a 9 px embaixo do disco, dica do port a 7 px abaixo do nome, e o
  ponto de brasa de 3 px do `.knob-card.modified:after`.

### Interação

| gesto | efeito |
|---|---|
| arrasto vertical no knob | `dy/180 × range`, a partir do valor no momento do clique |
| `shift` + arrasto | fine, fator 0,15 |
| duplo clique (< 450 ms no mesmo knob) | volta ao default da porta |
| roda **para cima** sobre o knob | `+range/50` |
| roda sobre a lista | rola; o teto é do que **passa no filtro**, não de `banks.size()` |
| roda sobre a pílula | preset ±1 |
| clique na linha do piano | escreve o índice na porta 20 — **funciona com o véu no ar** |
| clique no campo de busca | foco; qualquer clique fora o solta |
| tecla imprimível com foco | acrescenta ao filtro (UTF-8, Latin-1 reencodado) |
| `BackSpace` | apaga um **glifo**, não um byte ("Pianão" tem dois "ão" e `pop_back()` deixaria metade de um UTF-8 no filtro) |
| `Escape` com foco | limpa o filtro; se já estava vazio, solta o foco |
| `Escape` sem foco | fecha |
| `Return` | com foco na busca = escolhe o primeiro **visível**; sem foco = diálogo de arquivo |
| `↑` / `↓` | banco anterior / seguinte |
| `←` / `→` | preset −1 / +1 |

Duas coisas que este quadro corrige na prática, ambas provadas por evento sintético
e não por leitura de código:

* **A roda estava invertida** para knob e preset: `button==5 ? +1 : -1` era usado
  para tudo, e "para cima" baixava o valor. A lista era o único sítio onde o sinal
  estava certo, porque lá o gesto é de *scroll* e não de *valor* — por isso o código
  agora deriva `up` do botão e inverte só na lista.
* **O véu bloqueava a lista.** `hitTest()` tinha o guarda
  `if (veilVisible(s)) return {};` antes do teste de linha, e com a forja fria o
  clique na lista não escrevia nada: sobrava apenas o botão "Escolher primeiro
  banco". No protótipo o `.veil` é `position:absolute; inset:38px 0 0` **dentro do
  `.panel`** — ele cobre os knobs e nunca a sidebar. A ordem correta é: botões →
  busca → lista → guarda do véu → pílula → knobs.

Fagulhas só animam no knob sob o mouse e por ~1,5 s depois de soltar; fora disso o
redraw é estático, para não queimar CPU num painel parado.

---

## 9. Como a janela aparece dentro de um host de verdade

Estas cinco regras custaram uma sessão cada. Uma UI em outro toolkit precisa das
mesmas, mudando só a API:

1. **XEmbed**: `_XEMBED_INFO` na janela filha + `XEMBED_EMBEDDED_NOTIFY` para o
   parent. Sem isso o host mostra um retângulo preto.
2. **Não confie em `idle()`.** O REAPER passa o parent, faz o handshake, lê e escreve
   parâmetros e **nunca chama `idle()` nem `show()`**. Quem bomba os eventos X tem de
   ser uma **segunda conexão X dona da thread da UI**: `XOpenDisplay()` de novo,
   `XSelectInput(hostConn, win, 0L)` (senão você entupa a fila do host) e
   `XSelectInput(myConn, win, eventos)` na sua. Uma conexão por thread dispensa
   `XInitThreads()` e dispensa mutex em cada chamada X.
3. **Pintar num window cujo ancestral não é *viewable* é descartado pelo servidor X**
   — sem backing store, o primeiro `draw()` nunca existiu. Redenhe em `MapNotify`,
   `ReparentNotify` e `VisibilityNotify`.
4. **`XShmAttach` devolve 1 mesmo quando o servidor recusa.** O `BadAccess` chega
   depois, assíncrono, e todo `XShmPutImage` vira `BadShmSeg`: a UI desenha, o X não
   mostra nada. Envolva o primeiro attach com handler de erro + `XSync` e caia em
   `XPutImage` se qualquer erro aparecer. `shmget` com **0666** (0600 dá BadAccess com
   Xorg como root). Nesta máquina o SHM é recusado — o caminho real é `XPutImage`.
5. **`XPutImage` maior que o drawable é `BadMatch` em todo frame.** Se o host
   redimensiona o filho (o REAPER faz), a UI tem de adotar o tamanho recebido e
   refazer Surface + XImage — nunca alargar acima do que o host deu. `bytes_per_line`
   é em **bytes**, `stride` do Surface em **pixels**; errar por 4 é pintar ¼ da janela.

E a sexta, que não é de janela:

6. **Um evento do shell não atravessa um `select()`.** A thread da UI dorme no fd do
   X com timeout de 1 s; um `needDraw_` atômico armado pela thread do host não acorda
   ninguém. Precisa de um **self-pipe** (`pipe2(O_NONBLOCK|O_CLOEXEC)`) no conjunto do
   `select()`, escrito no fim de todo `portEvent()`. `NONBLOCK` não é enfeite: com o
   pipe cheio, o `write()` de um segundo evento pararia a thread **principal** do host.
   Sem o pipe, o painel fica até 1 s cego a automação (medido no probe antigo, que
   falava o formato anterior: 1276 cores iguais antes de mandar evento; 1457 depois).

---

## 10. Reaproveitando o código atual

`drawPanel(Surface&, Font regular, Font bold, Font small, Layout&, PanelState&, time)`
em `src/core/ui/ForgePanel.cpp` **não conhece X11**. Recebe um framebuffer ARGB32 e
duas fontes. É o mesmo código na janela do plugin e no `pforge_ui_preview` offscreen.

Para portar para outro toolkit, três caminhos, do mais barato ao mais trabalhado:

1. **Manter o rasterizador** e trocar só a apresentação. Leva junto `Surface`
   (`fillRect/rrect/arc/glow/vgrad/hgrad/radialGlow/blendImage`), `Font`
   (`stb_truetype` com cache por codepoint) e `ForgeTheme`. Nada de Cairo, FreeType ou
   GL — nenhum dos headers existe na máquina de desenvolvimento, e é por isso que o
   rasterizador próprio existe.
2. **Reaproveitar os dados, redesenhar.** `PanelState`, `Layout`, `KnobState`,
   `BankRow` e `hitTest()` são puros; um toolkit de widgets real só precisa dos
   retângulos que `layoutFor(w,h)` produz e do `PanelState` que a UI monta.
3. **Reescrever do zero** respeitando §1 a §5. O §8 é descrição, não dependência.

`PanelState` é o ponto de contato honesto: tudo que o painel sabe cabe ali
(`banks, current=-1, hoverBank, scroll, filter, searchFocus, knobs[10], hoverKnob,
dragKnob, sparkAge, loading, loadingName, status, toast, toastAge, activeBankName,
hiddenSummary, presetName, plugLoading, plugFailed, published, presetTotal, preset,
haveStatus, art`). É estado de *desenho*: nenhum campo dele toca X11, e `drawPanel()`
não sabe que uma janela existe — é isso que permite ao preview offscreen e à janela do
plugin renderizarem o mesmo pixel.

---

## 11. Como conferir sem DAW

| ferramenta | o que prova |
|---|---|
| `pforge_ui_preview --state empty\|normal\|hover\|loading --bank "Nome\|1,6 GB\|1" --selected N --value hammer=0.9 --status ready\|failed --presets 62 --preset 7 --bundle X.pforge --out x.ppm` | renderiza o painel offscreen, sem X. É assim que o visual é julgado: PPM → `magick x.ppm x.png` → ler o PNG de volta |
| `pforge_vst3_probe <PianoForge.vst3> [--work dir] [--ui]` | abre o bundle como um DAW abriria (fábrica, 17 parâmetros, estado ida-e-volta com lixo recusado, silêncio antes do banco e áudio depois). Com `--ui`, cria um container de verdade — X11 sob Xvfb no Linux, HWND no Windows — chama `attached()`, bombeia a fila e conta as cores do filho: é a prova de que o painel pinta dentro do host |
| `pforge_dsp_stages --sf2 X.sf2 [--notes N] [--voices T] [--strings N] [--sf2b Y.sf2]` \| `--ring [--seconds S]` \| `--pedalload [--voices T] [--seconds S]` | custo de cada estágio da cadeia e, principalmente, **o custo de uma fronteira de fatia**: renderiza 1×512 e 32×16 e imprime a diferença por fronteira. É o número que decide o que pode ser quantizado (`kCcSlice`) e o que não pode (`kMinSlice`). `--voices T` põe o teto de vozes do pool, que é como se mede o preço de `kMaxVoices` (com a piscina lotada: 128→313 µs, 192→465, 256→623, 384→956 µs por bloco de 512, com a RAM do banco inalterada — teto de vozes é decisão de CPU, não de memória). Com `--sf2b` ele ainda imprime o bloco `memoria:` — arquivo vs RAM por banco (`custo do banco`) e o **pico da troca** com os dois vivos. Cada passagem do bench **recoloca a entrada**: sem isso os estágios com realimentação compõem energia até virar `inf`/`NaN`, e o `dissipador` (que existe para o `-O2` não apagar o trabalho) passa a ser o detector — ele nomeia o estágio que entregou amostra não-finita |
| `pforge_dsp_stages --ring --sf2 X.sf2 [--seconds 30]` | **quanto soa cada corda deste banco** com o abafador levantado: toca nota a nota e imprime em que tempo cada uma cruza −40/−60 dB e onde termina. É o dado bruto que transforma "128 vozes basta?" em conta de duracao-de-cauda × densidade-de-notas (aqui: 25–28 s no grave, 4,8 s em A4, 0,4 s no agudo extremo) |
| `pforge_dsp_stages --pedalload --sf2 X.sf2 [--voices T] [--seconds 60]` | a mesma passagem pedalizada **contada pelo backend**, não pela sua sombra: imprime `MUDAS` (teclas que casaram com região e não ganharam voz), `regioes sem slot`, `abafadas antes da hora` e a **idade média** das cordas que o amortecedor derrubou. `MUDAS` sai de `pfMuteNotes`/`pfDroppedRegions`, **não** do delta de `tsf_active_voice_count()` — uma nota que fura a fila (mata uma voz e herda o slot) também não move a contagem, e o delta confunde roubo com silêncio; foi assim que a primeira versão deste modo acusou 26 notas mudas onde nenhuma ficou sem som |
| `pforge_dsp_stages --pedallift --sf2 X.sf2 [--chain] [--trace] [--voices T]` | **em quanto tempo a textura cai quando o pé sobe**: toca o caso, levanta o pedal e procura a primeira passada por −6/−20/−40 dB. `--chain` põe os 8 estágios do `Engine` atrás (é o número que o ouvido julga); `--trace` imprime, bloco a bloco, o **valor exato do `ampenv` de cada voz abafada** contra a tabela `tsf_damped_release_seconds` — é a leitura do modelo, e é o que sobrevive ao batimento das camadas. Medido no agregado com a cadeia: feltro por registro 128/213/555 ms e −74,8 dB aos 2 s; `tsf_voice_end` com o release chato do banco 11/75/139 ms e −60,8 dB |

Truques que fazem a diferença:

* `XDG_CONFIG_HOME=/tmp/pf_x11cfg` aponta o registro para um de mentira, sem encostar
  no `soundfonts.list` real do usuário — é como se injeta um `.pforge` soando numa janela
  de teste. Para provar *scroll*, um registro com 8 linhas (nem que seja caminho
  inexistente: ele desenha riscado e continua ocupando linha).
* **A primeira execução é um estado, e tem de ser conferido como tal**: `XDG_CONFIG_HOME`
  num diretório vazio com só um `soundfonts.conf` legado dentro (pastas + um `.sf2`).
  Esperado — nenhum `soundfonts.list` criado depois de abrir a UI, biblioteca em `00`,
  véu "A forja está fria", rodapé "Nenhum piano", e o plugin mudo. É a prova de que nada
  no produto semeia piano: o bundle não traz `.sf2` nenhum, e os dois arquivos legados do
  formato antigo não são lidos por ninguém.
* `--status loading --presets 62 --preset 7` no `pforge_ui_preview` excita o painel
  **sem host**: é o único jeito de ver "FORJANDO" e a pílula de preset fora de um DAW.
* **RAM se mede de dentro do processo.** `VmRSS`/`VmHWM` lidos em `/proc/self/status`
  pelo próprio binário. Um poll externo a cada 50 ms deu "pico de 6 MB" para um render
  que `ulimit -v 262144` mostra precisar de mais de 262 MB: o load inteiro acontece entre
  dois sleeps da amostragem, e `VmHWM` é por processo, não por amostra. `--sf2 A --sf2b B`
  é o que faz o `pforge_dsp_stages` somar os dois bancos vivos.

### O banco sintético (`tools/SynthBank.hpp`, `--makesf2 N`)

Nenhum soundfont entra no repositório (licença), e sem banco metade das provas deixa de
existir — o render do standalone, o `--pedalload` e o `pforge_vst3_probe` precisam de um
piano para escolher, e o harness antigo recusava com `nenhum banco registrado` em vez de
devolver um `TUDO OK` pela metade. `--makesf2 N` gera N bancos SF2 na hora
(`psynth::build`): senoides com harmônicos inteiros, **ciclos
inteiros dentro do loop** (é o que o `--regions` confere: o arquivo diz `CONTINUOUS`),
platô de sustain e **bandas de velocidade** — camada par é FORTE (vel 80..127, pico
0,115), ímpar é piano (vel 0..79, pico 0,02). A banda tem de ser função **só da camada**:
a amostra é indexada por (tecla, camada) e os presets compartilham as primeiras N camadas,
então banda dependente de preset daria regiões sobrepostas. Com as quatro camadas em
`velRange 0..127` e níveis quase iguais, o teste M do harness antigo devolvia
`menor ganho=-0,269 dB | tecla muda: 6` — o diferencial não tinha o que separar. Com
bandas: `0,415 dB`, nenhuma muda, e aquela suíte fechava `TUDO OK (0 falhas)` com
`--makesf2 2`.

Isto não substitui o piano gravado em nada que envolva martelo, feltro ou ressonância: um
sample de 0,12 s em loop não tem decaimento físico. É o instrumento certo para perguntar
"o arquivo diz loop?" e "o modelo sustenta?", e é com ele que o CI roda as provas abaixo
nos três sistemas sem baixar banco nenhum.

### O release automático (`.github/workflows/release.yml`)

Três builds (`linux-x64`, `windows-x64` com MSVC via `ilammy/msvc-dev-cmd`,
`macos-universal` com `-DCMAKE_OSX_ARCHITECTURES='arm64;x86_64'`), seis provas em cada
(C só no Linux, C2 só no Windows, F informativo), e um job `release` que só roda em tag
`v*` (com `--notes` próprio: o `gh` trata nota gerada e nota escrita como excludentes, e
num repo no primeiro tag não há história a gerar). As provas são as ferramentas desta
seção:

| | |
|---|---|
| **A** | `pforge_dsp_stages --makesf2 1 --pedalload --seconds 20` — **gate duro**: o modo devolve 1 se alguma tecla entrar muda. É o único teste de "nota descartada" que não depende de diferencial de áudio, e por isso o único que pode fechar um build |
| **B** | `--regions` no banco gerado + `pforge_standalone --bank synth0.sf2 --feed "c4:250 e4:250 g4:250 -"`, conferido por `Teto cheio: 0 amostras` e pico ≥ 0,01 (render mudo reprova) |
| **C** | `pforge_vst3_probe PianoForge.vst3 --ui` sob Xvfb — o painel dentro de um X11 de verdade, só no Linux |
| **C2** | o mesmo com `--ui` sobre um HWND, só no Windows (no macOS a sessão do runner não garante window server: é o único backend sem prova de UI em CI) |
| **D** | `pforge_ui_preview --state normal --status ready` e `--page 1` nos três, com `test -s` |
| **E** | `pforge_vst3_probe PianoForge.vst3` — fábrica, 17 params, estado ida-e-volta com lixo recusado, silêncio antes do banco e áudio depois |
| **F** | `pluginval` (informativo, `continue-on-error`: binário de terceiro baixado da rede não reprova um build que A–E aprovaram) |

Sete armadilhas de medida que já custaram tempo:

* **Um caminho errado no `--sf2` não faz barulho — faz o teste passar.**
  `std::filesystem::file_size` de arquivo inexistente devolve `(uintmax_t)-1` com `ec` posto,
  e esse lixo é maior que qualquer banco: o orçamento de memória do teste J virava 1,8e7 MB e
  a asserção "a troca nunca empilha os dois" passava sem provar nada. Agora só entra na conta
  quem abriu, e a linha imprime `SEM orçado: N de M caminhos de banco abriram`. Regra geral:
  toda asserção derivada de um tamanho de arquivo tem de conferir `ec`, porque o valor de
  erro é sempre o maior da sala.
* **"A nota sumiu" e "a nota foi roubada" têm a mesma assinatura externa.**
  Nem o delta de vozes nem o RMS diferencial separam as duas coisas sozinhos — o primeiro
  porque herdar slot não move contagem, o segundo porque sob escassez a textura inteira se
  reorganiza a cada evento. O que resolve é (a) contador dentro do backend, onde o evento é
  decidido, e (b) fundo ~30 dB abaixo da sonda (o pré-preenchimento velado do teste M). E
  o **piso** do método diferencial é uma grandeza a medir, não um chute: com symp/reverb/chorus
  ligados ele sobe, e acima de ~0,5 dB ele já não separa nada — reprovar o próprio teste é
  mais honesto que aprovar no escuro.
* **Não confunda sustentar com congelar.** Interromper o avanço do envelope enquanto CC64
  estiver no ar faz o pedal "segurar" qualquer nota por 20 minutos — a asserção de cauda
  maior passa, o piano para de parar, e o custo de CPU sobe porque a cauda vira piscina
  cheia permanente. A afirmação que importa é dupla: *com* pedal a cauda é ordens de grandeza
  maior (K) **e** soltar o pedal abafa (K, segunda metade) — uma sem a outra é defeito com
  nome de feature.
* **Uma asserção sem dente medido não é asserção.** O primeiro limiar de W1 ("soltar o pedal
  não é corte") era 4× = 12 dB e **passava no código doente**. Só depois de reverter o lift
  para `tsf_voice_end`, reconstruir e remedir é que se viu: velho 11,1 dB, novo 3,7 dB. O
  limiar virou 2,4× (7,6 dB) — o médio geométrico, que reprova o bug que motivou a asserção
  e deixa folga para o conserto. Regra: antes de confiar num `[PASS]`, veja o teste falhar no
  código quebrado. Isso custa um rebuild e é a única coisa que distingue prova de cerimônia.
* **O RMS de um bloco numa tecla sola não mede envelope.** As ~4 camadas do banco batem
  entre si e o bloco oscila ±6 dB sem nenhuma relação com o abafador — o `--trace` mostrou as
  quatro vozes de key 69 seguindo exatamente a curva de 0,900 s enquanto o "tempo de queda"
  medido por pico dava 21 ms. O que funciona: (a) ler o **valor do envelope** da voz, que é
  exato, (b) RMS com boxcar de 8 blocos (85 ms) antes de cruzar limiar, e (c) confiar só no
  caso **agregado** (12 teclas, onde o batimento se médio). Medir a queda com pico de bloco é
  o erro que fez o número antigo da doc estar errado em 3×.
* **`cmd | head -3` reprova um CI bom.** O `shell: bash` do GitHub roda com
  `-e -o pipefail`: quando o `head` fecha o pipe, o `cmd` da esquerda morre com SIGPIPE e
  o step devolve 141. Um passo de log com `| head` é uma bomba de efeito colateral
  assíncrona — corta a saída no script, não no pipe.
* **Off-screen não tem bits.** O container do probe nasce em `-4096,-4096` para não
  aparecer na área de trabalho de quem roda isto numa X de verdade. Com compositor isso é
  inofensivo; no Xvfb do CI, sem backing store, a região fora da tela não existe em lugar
  nenhum e o `XGetImage` da janela filha devolve ora `NULL`, ora um retângulo preto —
  indistinguível de "a UI não pintou", que é exatamente a afirmação que o `--ui` faz.
  O probe agora trata as duas leituras ruins como *leitura inválida* (não como veredito):
  volta a ler com o container em `8,8`. Mover o pai não muda um pixel do que a UI desenha,
  muda o que o servidor pode ler; e se depois disso ainda estiver preto, é porque nada foi
  desenhado. Medido: 2527 cores tanto no `:0` quanto no `:99`.

As coordenadas mudam com o layout, e um clique sintético que não acerta nada **não
falha** — ele só reporta `writes=0`. Foi assim que o clique da lista, morto pelo guarda
do véu (§8, Interação), passou numa rodada inteira de verificação: o `pforge_ui_probe`
antigo usava a coordenada do layout anterior. Ao mudar geometria, recalcular as
coordenadas de quem clica é parte da verificação, não detalhe: busca `126:102`, linha 0
`100:130`, Volume `325:155`, centro do botão do véu `622:301`, seta `▸` da pílula
`416:486`.

Contagem de cores distintas é a métrica que pega "a UI não pintou": uma tela preta dá
1 cor, e um painel que ignora um evento dá exatamente as mesmas cores de antes.

---

## 12. DSP que a UI descreve

Aparece na tela, então vale saber o que o número significa:

* **`hammer` (porta 4)** — dois componentes somados ao sample: o *click* (feltro
  saindo da corda: ruído em bandpass 700 Hz→4,5 kHz conforme a nota, Q 1,4, teto fixo
  em 9,5 kHz, envelope 20 ms no grave → 9 ms no agudo) e o *thump* (a caixa: lowpass
  90→230 Hz, envelope 32→18 ms). Escala com `vel^0.55` no click e `vel^0.80` no thump,
  para o ataque continuar audível em *pianissimo*. O nível de cada caminho é **medido**
  (energia da resposta ao impulso do filtro, por Parseval) numa tabela de 128 notas
  montada no `prepare()` — sem isso, o ganho dependeria da frequência do filtro e cada
  nota sairia com um volume diferente.
* **`velCurve` (5)** — amplitude da corda em função da velocity.
* **`lid` (7), `bassEQ`/`midEQ`/`highEQ` (8..10)** — filtros; 0.5 nos três é plano.
* **`sympRes` (6)** — cordas simpáticas; `chorus` (11); `reverb` (12) + `reverbType` (13).
* **`volume` (3)** e `trimDb` (14) — saída.

Todos os 17 passam por rampa exponencial de 20 ms **avançada por amostra do bloco**
(`advance(frames)`), não por chamada. Com o coeficiente de uma amostra chamado uma vez
por bloco, os 20 ms viram ~5 s num bloco de 256 — foi por isso que EQ, Reverb, Chorus,
Tampa e Volume pareceram "mortos" dentro do REAPER.
