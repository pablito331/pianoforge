#!/bin/sh
# SFZ/SF3 no PianoForge passam pelo sfizz, que NAO e vendorizado neste repo: ele
# traz uma dezena de submodulos proprios (abseil, simde, cephes, fmidi...) e a
# licenca BSD-2 dele nao pede que more aqui. O plugin sem sfizz e um SF2 completo
# e recusa .sfz/.sf3 com uma linha no stderr -- e e exatamente assim que o build
# de saida do CI acontece.
#
# Para ter SFZ na sua maquina:
#
#   scripts/get_sfizz.sh
#   cmake -S . -B build -DSFIZZ_DIR="$(pwd)/.deps/sfizz"
#   cmake --build build
#
# O sufixo da lib e por sistema (libsfizz.so / libsfizz.dylib / sfizz.dll) e o
# CMake procura os tres, entao o mesmo -DSFIZZ_DIR vale nos tres.
set -eu

TAG=v1.2.3
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEST=${PF_SFIZZ_DEST:-$ROOT/.deps/sfizz}
SRC=${PF_SFIZZ_SRC:-$ROOT/.deps/sfizz.src}

for candidate in "$DEST/lib/libsfizz.so" "$DEST/lib/libsfizz.dylib" "$DEST/lib/sfizz.dll" \
                 "$DEST/lib64/libsfizz.so"; do
    if [ -e "$candidate" ]; then
        echo "sfizz ja construido: $candidate"
        echo "configure com -DSFIZZ_DIR=$DEST"
        exit 0
    fi
done

command -v git   >/dev/null || { echo "precisa de git para baixar o sfizz ($TAG)"; exit 1; }
command -v cmake >/dev/null || { echo "precisa de cmake para construir o sfizz"; exit 1; }

if [ ! -e "$SRC/CMakeLists.txt" ]; then
    # --recursive e o ponto: sem os submodulos o sfizz nao configura, e eles nao
    # sao um submodule do PianoForge (sao do sfizz).
    git clone --recursive --depth 1 --branch "$TAG" https://github.com/sfzstuff/sfizz.git "$SRC"
fi

JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
cmake --build "$SRC/build" -j"$JOBS"
cmake --install "$SRC/build" --prefix "$DEST"

echo "sfizz instalado em $DEST"
echo "configure o PianoForge com:  cmake -S . -B build -DSFIZZ_DIR=$DEST"
