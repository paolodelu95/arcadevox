#!/bin/sh
# build.sh — compila il simulatore, rende le scene, converte in PNG.
#
# Un solo comando perche' i tre passi non hanno senso separati: se il PNG non
# esce, non interessa sapere che l'oggetto e' stato prodotto.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$HERE/out"
BUILD="$HERE/build"

mkdir -p "$BUILD" "$OUT"

# -I stub prima di tutto: e' li' che stanno Arduino.h e Arduino_GFX_Library.h
# finti. src/ non entra negli include path — display.cpp e settings.cpp trovano i
# propri header per via del percorso relativo, che e' quello che vogliamo: i
# moduli veri restano veri.
CXXFLAGS="-std=c++17 -O1 -g -Wall -Wno-unused-function -I$HERE/stub -I$HERE/vendor"

clang++ $CXXFLAGS -c "$HERE/sim_main.cpp"        -o "$BUILD/sim_main.o"
clang++ $CXXFLAGS -c "$HERE/sim_fakes.cpp"       -o "$BUILD/sim_fakes.o"
# settings.cpp e' il sorgente vero: etichette, categorie e valueLabel() che
# finiscono sullo schermo sono quelli del firmware, non una loro imitazione.
clang++ $CXXFLAGS -c "$ROOT/src/settings.cpp"    -o "$BUILD/settings.o"
clang   -std=c99 -O1 -w -I"$HERE/vendor" -c "$HERE/vendor/qrcode.c" -o "$BUILD/qrcode.o"

clang++ -o "$BUILD/simdisplay" "$BUILD/sim_main.o" "$BUILD/sim_fakes.o" "$BUILD/settings.o" \
        "$BUILD/qrcode.o" -lm

rm -f "$OUT"/*.ppm "$OUT"/*.png
"$BUILD/simdisplay" "$OUT" > "$OUT/scene.txt"
python3 "$HERE/ppm2png.py" "$OUT"
rm -f "$OUT"/*.ppm

echo "PNG in $OUT"
ls "$OUT" | grep -c '\.png$' | sed 's/^/immagini: /'
