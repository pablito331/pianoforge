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
        [ "$matched" -ge 10 ] && break
    done < <(grep -E -i 'error|\[FALHA\]|falhou|undefined symbol|cannot ' "$log")

    if [ "$matched" -eq 0 ]; then
        printf '::error title=%s::o passo falhou (codigo %s) e nao deixou linha de erro reconhecivel\n' \
            "$name" "$rc"
    fi
fi

exit "$rc"
