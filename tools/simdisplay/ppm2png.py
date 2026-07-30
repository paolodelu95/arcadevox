#!/usr/bin/env python3
"""ppm2png.py — converte i PPM del simulatore in PNG, solo con la libreria standard.

Il C++ scrive PPM perche' e' tre righe di codice e non sbaglia; il PNG serve
perche' e' l'unico formato che si guarda ovunque senza installare niente. Il
passaggio in mezzo lo fa Python con zlib e struct, che ci sono sempre: nessuna
dipendenza da Pillow, nessun ambiente virtuale da ricordarsi.
"""

import os
import struct
import sys
import zlib


def leggi_ppm(percorso):
    """P6 a 8 bit. Non e' un lettore PPM generale: legge quello che scriviamo noi."""
    with open(percorso, "rb") as f:
        dati = f.read()

    # Intestazione: tre campi separati da spazi bianchi, poi un singolo separatore
    # e subito i byte dei pixel.
    campi = []
    i = 0
    while len(campi) < 4:
        while i < len(dati) and dati[i : i + 1].isspace():
            i += 1
        if dati[i : i + 1] == b"#":  # commento, fino a fine riga
            while i < len(dati) and dati[i : i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(dati) and not dati[j : j + 1].isspace():
            j += 1
        campi.append(dati[i:j])
        i = j
    i += 1  # il singolo byte di separazione dopo il maxval

    if campi[0] != b"P6":
        raise ValueError("%s non e' un PPM binario" % percorso)
    larghezza = int(campi[1])
    altezza = int(campi[2])
    pixel = dati[i : i + larghezza * altezza * 3]
    if len(pixel) != larghezza * altezza * 3:
        raise ValueError("%s troncato" % percorso)
    return larghezza, altezza, pixel


def scrivi_png(percorso, larghezza, altezza, pixel):
    def blocco(tipo, corpo):
        return (
            struct.pack(">I", len(corpo))
            + tipo
            + corpo
            + struct.pack(">I", zlib.crc32(tipo + corpo) & 0xFFFFFFFF)
        )

    # Ogni riga preceduta dal byte di filtro 0 (nessun filtro): le schermate sono
    # grandi campi di nero, zlib le comprime benissimo lo stesso.
    grezzo = bytearray()
    passo = larghezza * 3
    for y in range(altezza):
        grezzo.append(0)
        grezzo += pixel[y * passo : (y + 1) * passo]

    ihdr = struct.pack(">IIBBBBB", larghezza, altezza, 8, 2, 0, 0, 0)
    with open(percorso, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(blocco(b"IHDR", ihdr))
        f.write(blocco(b"IDAT", zlib.compress(bytes(grezzo), 9)))
        f.write(blocco(b"IEND", b""))


def main():
    cartella = sys.argv[1] if len(sys.argv) > 1 else "."
    convertiti = 0
    for nome in sorted(os.listdir(cartella)):
        if not nome.endswith(".ppm"):
            continue
        sorgente = os.path.join(cartella, nome)
        destinazione = os.path.join(cartella, nome[:-4] + ".png")
        w, h, px = leggi_ppm(sorgente)
        scrivi_png(destinazione, w, h, px)
        convertiti += 1
    print("PPM convertiti in PNG: %d" % convertiti)
    return 0 if convertiti else 1


if __name__ == "__main__":
    sys.exit(main())
