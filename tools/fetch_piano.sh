#!/bin/sh
# fetch_piano.sh — scarica le sette note di piano da cui nasce src/instruments.cpp.
#
# I file grezzi non stanno nel repository: sono cinquanta megabyte di AIFF a
# 44,1 kHz stereo, e cio' che serve davvero — i 240 kB gia' convertiti — e' in
# src/instruments.cpp, che invece e' committato. Questo script esiste perche' la
# catena resti rifacibile da chiunque, senza tenersi in casa il materiale grezzo.
#
# La fonte e' la University of Iowa Electronic Music Studios. Dal 1997 pubblica
# queste registrazioni dichiarando che "may be downloaded and used for any
# projects, without restrictions": e' per questo che sono loro e non altre, dato
# che finiscono in un firmware ridistribuito via OTA.
#
#   sh tools/fetch_piano.sh          # scarica in tools/piano/
#   python tools/make_instruments.py # e rigenera src/instruments.cpp
set -e

DIR="$(dirname "$0")/piano"
BASE="https://theremin.music.uiowa.edu/sound%20files/MIS/Piano_Other/piano"

# Una ogni tre semitoni, da Do3 a Fa#4. Vanno tenute allineate con PIANO_ROOTS
# in make_instruments.py: sono le stesse note, scritte con i bemolle perche' cosi'
# le chiama la Iowa.
NOTES="C3 Eb3 Gb3 A3 C4 Eb4 Gb4"

mkdir -p "$DIR"
for n in $NOTES; do
    f="$DIR/Piano.ff.$n.aiff"
    if [ -s "$f" ]; then
        echo "$n gia' presente"
        continue
    fi
    echo "scarico $n..."
    # --retry: il server dell'universita' ogni tanto lascia cadere una
    # connessione a meta', e riprovare basta.
    curl -sSL --max-time 300 --retry 3 --retry-delay 2 -o "$f" "$BASE/Piano.ff.$n.aiff"
done

echo "fatto: $(du -sh "$DIR" | cut -f1) in $DIR"
