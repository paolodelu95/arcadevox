#!/bin/sh
# release.sh — prepara una release OTA: versione, binario, manifest.
#
#   sh tools/release.sh 2.7.0 "cosa cambia in questa versione"
#
# Fa i primi quattro passi di firmware/README.md — alza FW_VERSION, compila,
# copia il binario, riscrive il manifest — e si ferma prima del commit, che resta
# una tua decisione.
#
# ---------------------------------------------------------------------------
# Perche' esiste, visto che i quattro passi erano gia' scritti
#
# Perche' da quando c'e' tools/fetch_memes.sh, uno di quei quattro passi puo'
# pubblicare tredici registrazioni altrui senza che nessuno se ne accorga.
#
# `pio run` compila cio' che trova, e cio' che trova in src/samples.cpp puo'
# essere la versione coi suoni veri. Quel file il gancio pre-commit lo ferma —
# ma firmware/firmware.bin e' un altro file, non porta nessun marcatore, e
# committarlo e' esattamente cio' che una release deve fare. Il divieto va quindi
# messo prima: qui, dove il binario viene prodotto.
#
# Lo stesso non vale per il piano e per la batteria. Quelli stanno dentro apposta:
# University of Iowa senza restrizioni d'uso, e MuldjordKit sotto Creative Commons
# Attribution — la seconda chiede una riga di credito, e questo script se la porta
# dietro nelle note del manifest.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VERSION="$1"
NOTES="$2"

if [ -z "$VERSION" ]; then
    echo "uso: sh tools/release.sh MAGGIORE.MINORE.PATCH [\"note della release\"]" >&2
    echo "     versione attuale: $(grep -o '\"[0-9]*\.[0-9]*\.[0-9]*\"' src/version.h | head -1)" >&2
    exit 1
fi

# Tre numeri, ognuno 0..255: e' il formato che il confronto nel firmware sa
# leggere. Un "2.7" o un "v2.7.0" passerebbero di qui e fallirebbero molto piu'
# tardi, sulla scheda, sotto forma di aggiornamento che non viene mai offerto.
if ! echo "$VERSION" | grep -qE '^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$'; then
    echo "versione '$VERSION': serve maggiore.minore.patch, tutti e tre 0..255" >&2
    exit 1
fi

# ------------------------------------------------------- il cancello
if head -20 src/samples.cpp | grep -q "CAMPIONI-PERSONALI"; then
    cat >&2 <<'STOP'
src/samples.cpp contiene le registrazioni vere della schermata SUONI.

Una release compilata cosi' le pubblica dentro firmware/firmware.bin, che e'
il file che le schede scaricano da internet. Non e' un dettaglio formale: e'
ridistribuzione, solo dentro un binario invece che dentro una cartella.

  git restore src/samples.cpp    # torna ai tredici sintetizzati
  sh tools/release.sh ...        # e la release riparte

I file scaricati restano in tools/samples/: dopo la release, un
`python3 tools/make_samples.py` e un upload te li rimettono sulla scheda.
STOP
    exit 1
fi

PIO="$HOME/.platformio/penv/bin/pio"
[ -x "$PIO" ] || PIO="pio"

echo "== versione $VERSION"
# Solo la riga del #define, non ogni "2.6.0" che passa: nel file ci sono anche
# esempi di manifest con dentro dei numeri di versione finti.
python3 - "$VERSION" <<'PY'
import re, sys
v = sys.argv[1]
p = "src/version.h"
s = open(p, encoding="utf-8").read()
s2 = re.sub(r'(#define FW_VERSION ")[^"]*(")', r'\g<1>%s\g<2>' % v, s, count=1)
if s == s2:
    sys.exit("non ho trovato la riga #define FW_VERSION in " + p)
open(p, "w", encoding="utf-8").write(s2)
PY

echo "== compilo"
"$PIO" run -e esp32-s3-devkitc-1 >/dev/null

echo "== copio il binario"
cp .pio/build/esp32-s3-devkitc-1/firmware.bin firmware/firmware.bin

echo "== riscrivo il manifest"
python3 - "$VERSION" "$NOTES" <<'PY'
import json, sys
version, notes = sys.argv[1], sys.argv[2]
p = "firmware/manifest.json"
m = json.load(open(p, encoding="utf-8"))
m["version"] = version
if notes:
    m["notes"] = notes
# L'attribuzione della batteria viaggia con la release, che e' il posto in cui la
# licenza chiede che stia: chi riceve il binario riceve anche il credito.
credit = "Drum samples provided by DrumGizmo.org."
if credit not in m.get("notes", ""):
    m["notes"] = (m.get("notes", "").rstrip() + " " + credit).strip()
# Piatto e senza a capo: il firmware lo legge con un estrattore di stringhe, non
# con un parser vero.
open(p, "w", encoding="utf-8").write(json.dumps(m, ensure_ascii=False))
print(json.dumps(m, ensure_ascii=False, indent=2))
PY

cat <<EOF

--
Pronto. Restano due comandi, e sono tuoi:

  git add src/version.h firmware/firmware.bin firmware/manifest.json
  git commit -m "ArcadeVox $VERSION"
  git push

Finche' non spingi su main, raw.githubusercontent serve ancora la vecchia
versione e nessuna scheda vede l'aggiornamento.
EOF
