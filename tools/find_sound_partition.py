#!/usr/bin/env python3
"""find_sound_partition.py — dove stanno i suoni, secondo la scheda.

Legge una tabella delle partizioni grezza — quella che tools/upload_sounds.sh si
fa mandare dalla scheda con `esptool read_flash 0x8000` — e ci cerca dentro la
partizione dati dei suoni. Scrive il suo indirizzo nel file indicato.

    python3 tools/find_sound_partition.py PARTIZIONI.bin IMMAGINE.bin DOVE.txt

Esiste come file a se' per una ragione noiosa e concreta: infilarlo dentro una
sostituzione di comando con un heredoc, nello script chiamante, e' il genere di
costruzione che funziona sulla shell su cui l'hai provata e si rompe sulla
prossima. Un file separato non ha delimitatori da far collidere con nessuno.

E la ragione per cui *qualcuno* deve fare questo lavoro e' piu' seria: prima
l'indirizzo era una costante nello script, 0x670000, e ha smesso di essere giusto
nel momento in cui la configurazione e' passata da N8 a N16R8. Con
default_16MB.csv quella partizione sta a 0xc90000 — e scrivere quattrocento
kilobyte all'indirizzo vecchio vuol dire scriverli in mezzo alla seconda immagine
dell'applicazione.
"""

import os
import struct
import sys

# Una voce della tabella e' lunga 32 byte e comincia con questi due.
ENTRY_MAGIC = b"\xaa\x50"
ENTRY_SIZE = 32

# Tipo 1 = dati, sottotipo 0x82 = spiffs. E' la stessa coppia che cerca
# esp_partition_find_first() in src/sample_store.cpp. Cercare qui per etichetta e
# li' per sottotipo vorrebbe dire due criteri che un giorno pescano due
# partizioni diverse, e nessuno se ne accorgerebbe finche' i suoni non spariscono.
TYPE_DATA = 1
SUBTYPE_SPIFFS = 0x82


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    parts_path, image_path, out_path = sys.argv[1:4]

    image = os.path.getsize(image_path)
    data = open(parts_path, "rb").read()

    for i in range(0, len(data) - ENTRY_SIZE + 1, ENTRY_SIZE):
        entry = data[i:i + ENTRY_SIZE]
        # La tabella finisce dove finisce la firma: il resto del settore e' 0xFF.
        if entry[:2] != ENTRY_MAGIC:
            break
        ptype, subtype, offset, size = struct.unpack("<BBII", entry[2:12])
        if ptype == TYPE_DATA and subtype == SUBTYPE_SPIFFS:
            label = entry[12:28].rstrip(b"\x00").decode("ascii", "replace")
            sys.stderr.write("   '%s' a 0x%06x, %.1f kB\n" % (label, offset, size / 1024))
            if image > size:
                sys.exit("   l'immagine e' %.1f kB e non ci sta: NON scrivo."
                         % (image / 1024))
            with open(out_path, "w") as f:
                f.write("0x%06x" % offset)
            return 0

    sys.exit("   nessuna partizione dati spiffs nella tabella: NON scrivo.")


if __name__ == "__main__":
    sys.exit(main())
