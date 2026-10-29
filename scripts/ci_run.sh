#!/usr/bin/env bash
# Roda um comando do CI guardando a saida e, se ele reprovar, publica as linhas de
# erro como anotacao (::error).
#
# Por que existe: o log cru de um job so pode ser baixado com sessao no GitHub -- a
# API responde 403 ("Must have admin rights") para quem nao esta logado, mesmo em
# repo publico, e a pagina do job mostra "Sign in to view logs". Sem isto a unica
# pista publica de uma reprovacao e "Process completed with exit code 1", que nao
# diz QUAL assercao ou QUAL arquivo. A anotacao diz: ela e publica e aparece na aba
# Checks, entao o texto do erro viaja junto com o vermelho.
#
# uso: ci_run.sh <nome> <comando> [args...]
#
# A saida completa vai para <nome>.ci.log e e impressa de uma vez no fim, nao em
# fluxo: um gate que passa e rapido, e o que se perde e so o "ao vivo".
set +e

name=${1:?falta o nome do passo}
shift
log="${name}.ci.log"

"$@" > "$log" 2>&1
rc=$?
cat "$log"

if [ "$rc" -ne 0 ]; then
    # Uma anotacao por linha: o GitHub corta mensagem longa, e 200 linhas de
    # compilador dentro de uma anotacao so viram uma parede ilegivel. Dez e o
    # teto que o proprio GitHub aplica por passo -- pedir mais e pedir em silencio.
    matched=0
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        # O comando de workflow tem escape proprio: sem isto um % ou um CR dentro
        # da mensagem quebra a anotacao (e a quebra de linha, o resto do passo).
        line=${line//'%'/'%25'}
        line=${line//$'\r'/'%0D'}
        printf '::error title=%s::%s\n' "$name" "$line"
        matched=$((matched + 1))
        [ "$matched" -ge 6 ] && break
    done < <(grep -E -i 'error|\[FALHA\]|falhou|undefined symbol|cannot ' "$log")

    # Os numeros medidos: uma falha de tempo ("p99.9 estourou") sem a linha que mediu o
    # p99.9 nao diz se foram 4,1 ms ou 40 ms, e e essa diferenca que separa ruido do
    # runner de regressao. O rastro do log inteiro, numa unica anotacao: as linhas de
    # resumo nao tem posicao fixa -- a primeira versao pegava so as cinco ultimas
    # ("G:", "I:", "J:"...) e num passo com dois testes reprovados a medida do que
    # reprovou primeiro saia fora; no Windows o H caiu e o G, que tinha passado, ficou.
    # 40 linhas e a vizinhanca dos vereditos, inclusive quando eles sao dois.
    if [ "$matched" -lt 10 ]; then
        cauda=""
        while IFS= read -r line; do
            line=${line%$'\r'}
            line=${line:0:400}      # uma linha de compilador das longas nao inunda tudo
            line=${line//'%'/'%25'}
            [ -n "$cauda" ] && cauda="$cauda%0A"
            cauda="$cauda$line"
        done < <(tail -40 "$log")
        printf '::error title=%s (fim do log)::%s\n' "$name" "$cauda"
        matched=$((matched + 1))
    fi

    if [ "$matched" -eq 0 ]; then
        printf '::error title=%s::o passo falhou (codigo %s) e nao deixou linha de erro reconhecivel\n' \
            "$name" "$rc"
    fi
fi

exit "$rc"
