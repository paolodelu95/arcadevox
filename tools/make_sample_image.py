#!/usr/bin/env python3
"""make_sample_image.py — impacchetta tools/samples/ nell'immagine della partizione.

I tredici suoni della schermata SUONI possono venire da due posti. I sintetizzati
stanno dentro il firmware, compilati; i tuoi stanno in una **partizione di dati
separata**, che si carica per conto suo e che il firmware non tocca mai.

    python3 tools/make_sample_image.py     # legge tools/samples/ -> tools/suoni.bin
    sh tools/upload_sounds.sh              # e lo scrive nella partizione

PERCHE' NON DENTRO IL FIRMWARE

Perche' quei file sono registrazioni con un padrone e firmware/firmware.bin
viene pubblicato. Finche' i suoni erano compilati dentro, "non metterli su
GitHub" dipendeva dal ricordarsi di non committare il binario — cioe' da una
promessa. Ora dipende dalla forma delle cose: nell'eseguibile non ci sono, e
nessuna svista puo' metterceli.

L'altra meta' del guadagno arriva dopo: un aggiornamento OTA riscrive solo la
partizione dell'applicazione, quindi aggiornare il firmware **non cancella i tuoi
suoni**. Prima ogni aggiornamento te li riportava a quelli sintetizzati.

IL FORMATO

Tutto little-endian, come il processore che lo legge.

    intestazione, 32 byte
        magic      8   "AVSND1\\0"
        count      4   quanti suoni
        totalSize  4   quanto occupa l'immagine intera
        riserva   16   zeri, per un domani

    tabella, 16 byte per voce
        nameOff    4   scostamento dall'inizio dell'immagine
        hintOff    4
        dataOff    4
        len        4

    poi le stringhe (con lo zero finale) e i campioni, 8 bit senza segno.

Sono scostamenti e non puntatori perche' l'indirizzo a cui la flash viene mappata
si scopre solo quando la scheda e' accesa. Il firmware li verifica tutti prima di
usarli: un'immagine troncata a meta' caricamento deve far tornare i suoni
sintetizzati, non riavviare la scheda.

I file di partenza e le regole per i nomi sono quelli di sempre, vedi
tools/samples/README.md. La conversione — 16 kHz, mono, 8 bit, normalizzata,
silenzio tagliato — la fa lo stesso codice di tools/make_samples.py, importato da
qui: due catene diverse per lo stesso suono sarebbero due modi di farlo suonare
diverso.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import numpy as np

import make_samples as MS

MAGIC = b"AVSND1\0\0"
HEADER_SIZE = 32
ENTRY_SIZE = 16
OUT = os.path.join(HERE, "suoni.bin")

# Quanto ci sta. La partizione `spiffs` di default_8MB.csv e' 0x180000, cioe'
# 1,5 MB: a 16 kHz e 8 bit sono novantotto secondi di suono, molto piu' dei
# quattro secondi per tasto che il motore audio sa leggere.
PARTITION_SIZE = 0x180000


def main():
    user = MS.load_user()
    if not user:
        print("tools/samples/ e' vuota: non c'e' niente da impacchettare.")
        print("Scaricali con tools/fetch_memes.sh, o mettici i tuoi file.")
        return 1

    # Si parte dai tredici sintetizzati e si sostituisce solo cio' che l'utente ha
    # messo: chi vuole cambiare tre suoni su tredici non deve procurarsi gli altri
    # dieci. E' la stessa regola di make_samples.py, e deve restare la stessa.
    items = []
    for slot, (name, hint, fn) in enumerate(MS.SOUNDS):
        if slot in user:
            name, hint, x = user[slot]
            source = "tuo"
        else:
            x = fn()
            source = "sintetizzato"
        q = np.clip(np.round(x * 127.0) + 128.0, 0, 255).astype(np.uint8)
        items.append((name, hint, q.tobytes(), source))

    count = len(items)
    blob = bytearray()
    entries = []
    cursor = HEADER_SIZE + ENTRY_SIZE * count

    def put(raw):
        nonlocal cursor
        off = cursor
        blob.extend(raw)
        cursor += len(raw)
        return off

    for name, hint, data, _ in items:
        name_off = put(name.encode("utf-8") + b"\0")
        hint_off = put(hint.encode("utf-8") + b"\0")
        # I campioni allineati a quattro byte. Non e' obbligatorio — si leggono a
        # byte — ma tenerli allineati costa qualche zero e lascia la porta aperta
        # a leggerli a parole intere il giorno che servisse.
        pad = (-cursor) % 4
        if pad:
            put(b"\0" * pad)
        data_off = put(data)
        entries.append((name_off, hint_off, data_off, len(data)))

    total = HEADER_SIZE + ENTRY_SIZE * count + len(blob)
    image = bytearray()
    image += struct.pack("<8sII16s", MAGIC, count, total, b"\0" * 16)
    for e in entries:
        image += struct.pack("<IIII", *e)
    image += blob

    assert len(image) == total, (len(image), total)

    if total > PARTITION_SIZE:
        print("l'immagine e' %.1f kB e la partizione ne tiene %.1f: togli qualcosa"
              % (total / 1024, PARTITION_SIZE / 1024), file=sys.stderr)
        return 1

    with open(OUT, "wb") as f:
        f.write(image)

    print()
    for (name, hint, data, source), e in zip(items, entries):
        print("  %-11s %6.2f s  %7d byte  (%s)" % (name, len(data) / MS.RATE, len(data), source))
    print("\n%d suoni, %.1f kB su %.1f disponibili -> %s"
          % (count, total / 1024, PARTITION_SIZE / 1024, OUT))
    print("Adesso: sh tools/upload_sounds.sh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
