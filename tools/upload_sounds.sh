#!/bin/sh
# upload_sounds.sh — scrive tools/suoni.bin nella partizione dei suoni.
#
#   python3 tools/make_sample_image.py   # costruisce l'immagine
#   sh tools/upload_sounds.sh            # e la manda sulla scheda
#
# Non tocca il firmware, e il firmware non tocca lei: sono due caricamenti
# indipendenti, ed e' tutto il senso della faccenda. `pio run -t upload` scrive
# l'applicazione e lascia i suoni dove sono; questo script scrive i suoni e lascia
# l'applicazione dov'e'.
#
# L'INDIRIZZO NON STA SCRITTO QUI
#
# Lo script legge la **tabella delle partizioni dalla scheda** e ci cerca dentro
# la partizione dati di tipo spiffs: quella e' la casa dei suoni, ovunque sia.
#
# Prima era una costante, 0x670000, e ha smesso di essere giusta nel momento in
# cui la configurazione e' passata da N8 a N16R8: con default_16MB.csv quella
# partizione sta a 0xc90000, e uno script rimasto alla costante vecchia avrebbe
# scritto quattrocento kilobyte in mezzo alla seconda immagine dell'applicazione.
#
# Non e' un numero che si possa sbagliare a cuor leggero — 0x10000 sarebbe
# l'applicazione, e scriverci sopra vuol dire una scheda che non parte piu' — e
# la lezione e' che a saperlo deve essere la scheda, non questo file.
#
# LA MODALITA' DOWNLOAD
#
# Sulla porta USB nativa la scrittura si corrompe (verificato: lo stub non passa,
# e senza stub fallisce il primo blocco). Prima di lanciare questo script, sulla
# DevKitC-1: tieni premuto BOOT, premi e rilascia RST, lascia BOOT.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="$ROOT/tools/suoni.bin"
PARTS=/tmp/avox_parts.bin
WHERE=/tmp/avox_offset.txt

if [ ! -s "$IMAGE" ]; then
    echo "manca $IMAGE — prima: python3 tools/make_sample_image.py" >&2
    exit 1
fi

PY="$HOME/.platformio/penv/bin/python"
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
[ -x "$PY" ] || PY="python3"
if [ ! -f "$ESPTOOL" ]; then
    echo "non trovo esptool.py in $ESPTOOL" >&2
    exit 1
fi

PORT="$1"
if [ -z "$PORT" ]; then
    PORT=$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | head -1)
fi
if [ -z "$PORT" ]; then
    echo "nessuna porta trovata: passala come argomento" >&2
    exit 1
fi

echo "porta   $PORT"
echo "immagine $(wc -c < "$IMAGE" | tr -d ' ') byte"

echo "== chiedo alla scheda dov'e' la partizione dei suoni"
"$PY" "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 460800 \
    read_flash 0x8000 0xc00 "$PARTS" >/dev/null

# Lo script Python scrive l'indirizzo in un file invece di restituirlo dentro una
# sostituzione di comando: un heredoc annidato dentro $(...) e' esattamente il
# genere di cosa che si rompe in silenzio su una shell diversa da quella su cui
# l'hai provato.
"$PY" "$ROOT/tools/find_sound_partition.py" "$PARTS" "$IMAGE" "$WHERE" || exit 1
OFFSET=$(cat "$WHERE")

echo "== scrivo a $OFFSET"
"$PY" "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 460800 \
    --before default_reset --after hard_reset \
    write_flash -z "$OFFSET" "$IMAGE"

echo
echo "Fatto. Sulla seriale, all'avvio, deve comparire:"
echo "  SUONI: 13 suoni dalla partizione dati (NNN kB)."
