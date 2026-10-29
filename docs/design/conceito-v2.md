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

## Integração com o plugin

Este documento é o conceito visual, e só isso. Quando foi escrito, a ponte entre o HTML e
o plugin era planejada sobre as portas do LV2; o plugin é hoje **VST3 único**, e o painel
compila dentro do próprio binário — a referência vigente para a ponte é o `UI-KIT.md` na
raiz, não esta pasta. Nada aqui precisa ser portado: o protótipo serve para julgar
geometria, paleta e gesto.

O HTML não toca áudio nem lê o conteúdo de SF2/SFZ/PFORGE, não altera o estado da DAW e
não salva parâmetros no plugin. Os bancos e tamanhos iniciais são dados ilustrativos. O
protótipo serve como conceito visual e referência funcional para o layout do renderer C++.
