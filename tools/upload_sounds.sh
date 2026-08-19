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
# L'INDIRIZZO
#
# 0x670000, la partizione `spiffs` di default_8MB.csv — 1,5 MB che quella tabella
# prevede da sempre e che nessuno usava. Sta scritto qui e in sample_store.h, e
# sono gli unici due posti: se un domani la tabella cambia, cambiano tutti e due.
#
# Non e' un numero che si possa sbagliare a cuor leggero: 0x10000 sarebbe
# l'applicazione, e scriverci sopra i suoni vuol dire una scheda che non parte
# piu' finche' non la si riprogramma. Per questo lo script legge la tabella vera
# dalla scheda prima di scrivere, invece di fidarsi della costante.
#
# LA MODALITA' DOWNLOAD
#
# Sulla porta USB nativa la scrittura si corrompe (verificato: lo stub non passa,
# e senza stub fallisce il primo blocco). Prima di lanciare questo script, sulla
# DevKitC-1: tieni premuto BOOT, premi e rilascia RST, lascia BOOT.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="$ROOT/tools/suoni.bin"
OFFSET="0x670000"

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

# La verifica che rende innocuo l'indirizzo scritto a mano qui sopra: si legge la
# tabella delle partizioni dalla scheda e si controlla che a 0x670000 ci sia
# davvero una partizione di dati, non l'applicazione.
echo "== controllo la tabella delle partizioni sulla scheda"
"$PY" "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 460800 \
    read_flash 0x8000 0xc00 /tmp/avox_parts.bin >/dev/null

"$PY" - "$OFFSET" <<'PY'
import struct, sys
want = int(sys.argv[1], 16)
data = open("/tmp/avox_parts.bin", "rb").read()
found = None
for i in range(0, len(data), 32):
    e = data[i:i + 32]
    if e[:2] != b"\xaa\x50":
        break
    ptype, subtype, offset, size = struct.unpack("<BBII", e[2:12])
    label = e[12:28].rstrip(b"\x00").decode("ascii", "replace")
    if offset == want:
        found = (label, ptype, size)
print("   partizioni lette dalla scheda")
if not found:
    sys.exit("   a 0x%06x non c'e' nessuna partizione: NON scrivo." % want)
label, ptype, size = found
if ptype != 1:
    sys.exit("   a 0x%06x c'e' '%s', che e' di tipo app: NON scrivo." % (want, label))
print("   0x%06x = '%s', dati, %.1f kB — via libera" % (want, label, size / 1024))
PY

echo "== scrivo"
"$PY" "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 460800 \
    --before default_reset --after hard_reset \
    write_flash -z "$OFFSET" "$IMAGE"

echo
echo "Fatto. Sulla seriale, all'avvio, deve comparire:"
echo "  SUONI: 13 suoni dalla partizione dati (NNN kB)."
