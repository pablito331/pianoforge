# Forja de Pianos — conceito v2

## Abrir

Abra `index.html` em um navegador moderno. O arquivo é autocontido; não precisa instalar dependências.

O protótipo abre no estado vazio, sem carregar um banco automaticamente. Clique numa linha da biblioteca ou use **Procurar soundfont…** para selecionar arquivos locais. O carregamento mostrado é uma simulação visual.

Para abrir diretamente a prévia do estado ativo que acompanha o conceito, use `index.html?demo=ready`.

## Interações demonstrativas

- Arraste knobs verticalmente; mantenha Shift pressionado para ajuste fino.
- Use a roda do mouse ou as setas do teclado para ajustar o valor; duplo clique restaura o padrão.
- Filtre bancos pela busca, adicione arquivos/pastas locais e remova itens da lista.
- Selecione banco para ver estados de carregamento e pronto; use as setas de preset quando houver mais de um.

## Integração LV2

A pasta `lv2-integration/` contém a camada C++17 de comunicação com as portas LV2 (bridge, callbacks e header público). Ela foi validada sintaticamente com tipos LV2 temporários, mas ainda precisa ser ligada ao renderer, à janela X11/XEmbed, ao CMake e às classes reais do repositório.

O HTML não toca áudio nem lê o conteúdo de SF2/SFZ/PFORGE, não altera o estado da DAW e não salva parâmetros no plugin. Os bancos e tamanhos iniciais são dados ilustrativos. O protótipo serve como conceito visual e referência funcional para portar o layout ao renderer C++/LV2.
