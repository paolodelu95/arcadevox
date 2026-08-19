#!/bin/sh
# fetch_drums.sh — scarica i tredici pezzi di batteria da cui nasce la BATTERIA
# di src/instruments.cpp.
#
# Prima erano sintetizzati: una cassa era una sinusoide che scendeva, un rullante
# rumore piu' tono. Suonavano come una drum machine, ed era una scelta difendibile
# — ma una drum machine non e' una batteria, e a chi vuole una batteria vera non
# si risponde con una formula.
#
#   sh tools/fetch_drums.sh          # scarica in tools/drums/
#   sh tools/fetch_piano.sh          # (l'altra meta' dello strumento)
#   python3 tools/make_instruments.py
#
# ---------------------------------------------------------------------------
# La fonte, e perche' proprio questa
#
# MuldjordKit, registrata da Lars Muldjord nel 2009 e pubblicata per DrumGizmo;
# la versione stereo la assembla il progetto FreePats. Licenza **Creative Commons
# Attribution 4.0**, cioe' si puo' ridistribuire — ed e' l'unica cosa che conta
# qui, perche' questi campioni finiscono dentro firmware/firmware.bin, che il
# progetto pubblica e manda alle schede via OTA. E' lo stesso ragionamento che ha
# portato al piano della University of Iowa, e la ragione per cui i suoni "meme"
# di tools/fetch_memes.sh non possono seguire questa strada.
#
# L'attribuzione richiesta e' una riga precisa, e sta nel README del progetto e
# nelle note della release:
#
#     Drum samples provided by DrumGizmo.org.
#
# ---------------------------------------------------------------------------
# Perche' un file per pezzo e non l'archivio
#
# La release completa e' un .7z da 233 MB e macOS non ha di serie niente che lo
# apra. I singoli FLAC stanno pero' anche nel repository git del kit, uno per
# file: tredici richieste per una decina di megabyte, nessun archivio da
# scompattare e nessun programma in piu' da installare.
#
# Il numero davanti al nome e' la **forza del colpo**: dentro ogni cartella i
# file vanno dal piu' piano al piu' forte (verificato: la cassa va da 9333 di
# picco sul primo a 22864 sull'ultimo). Qui si prende sempre l'ultimo, perche' un
# colpo forte non e' solo piu' alto — e' anche piu' brillante, e il volume lo
# rifa' comunque la normalizzazione.
set -e

DIR="$(dirname "$0")/drums"
BASE="https://raw.githubusercontent.com/freepats/muldjordkit/HEAD/samples"

mkdir -p "$DIR"

ok=0
# `nome locale|percorso nel kit`. L'ordine e' quello dei tasti, da sinistra a
# destra: cassa e rullante sotto le dita che stanno ferme, i piatti in fondo.
while IFS='|' read -r name path; do
    case "$name" in ''|'#'*) continue ;; esac
    dest="$DIR/$name.flac"
    if [ -s "$dest" ]; then
        echo "  = $name"
        ok=$((ok + 1))
        continue
    fi
    if curl -sSL --fail --max-time 120 --retry 2 --retry-delay 1 -o "$dest" "$BASE/$path"; then
        echo "  + $name"
        ok=$((ok + 1))
    else
        rm -f "$dest"
        echo "  ! $name — non scaricato"
    fi
done <<'HITS'
01-cassa|KdrumR/26-KdrumR.flac
02-rullante|Snare1/56-Snare.flac
03-bordo|SnareRest1/13-SnareRest.flac
04-tom1|Tom1/11-Tom1.flac
05-tom2|Tom2/13-Tom2.flac
06-tom3|Tom3/15-Tom3.flac
07-tombasso|Tom4/20-Tom4.flac
08-charleston|HihatClosed/29-HihatClosed.flac
09-charlestonaperto|HihatOpen/30-HihatOpen.flac
10-ride|RideR/10-RideR.flac
11-campana|RideRBell/8-RideRBell.flac
12-piatto|CrashR/12-CrashR.flac
13-cinese|China/12-China.flac
HITS

# La licenza viaggia insieme ai file: chi trova questa cartella fra sei mesi deve
# poter sapere da dove viene la roba che ci sta dentro senza tornare qui.
curl -sSL --fail --max-time 60 -o "$DIR/LICENSE.txt" \
    "https://raw.githubusercontent.com/freepats/muldjordkit/HEAD/README.txt" || true

echo "--"
echo "$ok pezzi in $DIR ($(du -sh "$DIR" | cut -f1))"
echo "Adesso: python3 tools/make_instruments.py"
