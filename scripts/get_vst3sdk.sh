#!/usr/bin/env bash
# Baixa o VST3 SDK para third_party/vst3sdk. Nao e vendorizado no repo: sao ~40 MB de
# cabecalho e helper que o historico nao precisa carregar. Licenca nao e motivo --
# desde a serie 3.8 o SDK e MIT, igual a este repo (ver NOTICE).
#
# Uso:  scripts/get_vst3sdk.sh            (tag padrao, a mesma do CI)
#       VST3SDK_TAG=v3.8.0_build_66 scripts/get_vst3sdk.sh
set -euo pipefail

TAG="${VST3SDK_TAG:-v3.8.1_build_84}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/third_party/vst3sdk"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# So tres repos: pluginterfaces (as interfaces), base (FUID/FString/ConstStringTable)
# e public.sdk (o SingleComponentEffect e o pluginfactory). vstgui nao entra: o
# painel do PianoForge e proprio.
#
# Cada repo e o CONTEUDO de uma pasta do SDK, nao a pasta: vst3_pluginterfaces traz
# base/gui/vst na raiz. test/ e samples/ ficam de fora (nao compilam aqui).
fetch() {
    local repo="$1" dir="$2"; shift 2
    echo "vst3: $repo $TAG"
    curl -fsSL "https://codeload.github.com/steinbergmedia/$repo/tar.gz/refs/tags/$TAG" \
        | tar -xz -C "$TMP"
    local src
    src="$(find "$TMP" -maxdepth 1 -type d -name "$repo-*" | head -1)"
    [ -n "$src" ] || { echo "vst3: $repo nao extraiu" >&2; exit 1; }
    mkdir -p "$DEST/$dir"
    for item in "$@"; do
        [ -e "$src/$item" ] || { echo "vst3: $item nao encontrado em $repo" >&2; exit 1; }
        rm -rf "${DEST:?}/$dir/$item"
        cp -R "$src/$item" "$DEST/$dir/"
    done
}

mkdir -p "$DEST"
fetch vst3_pluginterfaces pluginterfaces base gui vst LICENSE.txt
fetch vst3_base           base           source thread LICENSE.txt
fetch vst3_public_sdk     public.sdk     source LICENSE.txt
echo "$TAG" > "$DEST/.tag"
echo "vst3: ok em $DEST ($TAG)"
