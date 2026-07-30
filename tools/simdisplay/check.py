#!/usr/bin/env python3
"""check.py — controlla che quello che disegna display.cpp stia dentro il vetro tondo.

Il pannello GC9A01 e' un cerchio di 240 px di diametro dentro una cornice
quadrata che non esiste fisicamente: i quattro angoli non sono neri, sono
*assenti*. Un pixel a raggio 125 dal centro non e' brutto, e' invisibile — e
guardando il sorgente non lo si nota, perche' 236 e' un numero perfettamente
plausibile per una x.

Tre domande, in ordine di gravita':

  1. c'e' roba oltre il bordo del vetro? (raggio > 119.5: non la vedra' nessuno)
  2. c'e' roba che finisce addosso alla cornice? (fra 116 e 119.5, ma non e' la
     cornice stessa: a video sembra una sbavatura sull'anello)
  3. la cornice e' ancora tutta li'? (e' la domanda che conta di piu' sulle
     scene "-dopo-uso": una fascia di pulizia alta due pixel di troppo si mangia
     un arco di anello, e quel buco resta finche' non si cambia schermata)

Il terzo controllo ha senso solo dove la cornice viene disegnata. Le schermate
del QR non ne hanno una, e quella di avvio ha due anelli suoi a raggi diversi:
per quelle il controllo si salta, e viene DETTO che si e' saltato. Scrivere "OK"
su una verifica che non e' stata fatta sarebbe il modo piu' veloce di rendere
inutile tutto il resto.
"""

import math
import os
import struct
import sys
import zlib

CX = 120.0
CY = 120.0

# Raggio oltre il quale il vetro finisce. 119.5 e non 120 perche' un pixel e' un
# quadratino: il suo centro geometrico sta a (x+0.5, y+0.5) rispetto all'angolo,
# e qui i raggi si misurano dal centro del pixel.
R_VETRO = 119.5
# Sotto questa soglia si e' comodamente nell'area dei contenuti.
R_ZONA_CORNICE = 116.0


# --------------------------------------------------------------------- PNG

def leggi_png(percorso):
    """Legge un PNG a 8 bit RGB. Gestisce tutti e cinque i filtri per riga."""
    with open(percorso, "rb") as f:
        dati = f.read()
    if dati[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s non e' un PNG" % percorso)

    i = 8
    larghezza = altezza = 0
    idat = bytearray()
    while i < len(dati):
        (lung,) = struct.unpack(">I", dati[i : i + 4])
        tipo = dati[i + 4 : i + 8]
        corpo = dati[i + 8 : i + 8 + lung]
        if tipo == b"IHDR":
            larghezza, altezza, prof, colore = struct.unpack(">IIBB", corpo[:10])
            if prof != 8 or colore != 2:
                raise ValueError("%s: atteso RGB a 8 bit" % percorso)
        elif tipo == b"IDAT":
            idat += corpo
        elif tipo == b"IEND":
            break
        i += 12 + lung

    grezzo = zlib.decompress(bytes(idat))
    passo = larghezza * 3
    fuori = bytearray()
    precedente = bytearray(passo)
    p = 0
    for _ in range(altezza):
        filtro = grezzo[p]
        p += 1
        riga = bytearray(grezzo[p : p + passo])
        p += passo
        if filtro == 1:
            for x in range(3, passo):
                riga[x] = (riga[x] + riga[x - 3]) & 0xFF
        elif filtro == 2:
            for x in range(passo):
                riga[x] = (riga[x] + precedente[x]) & 0xFF
        elif filtro == 3:
            for x in range(passo):
                a = riga[x - 3] if x >= 3 else 0
                riga[x] = (riga[x] + ((a + precedente[x]) >> 1)) & 0xFF
        elif filtro == 4:
            for x in range(passo):
                a = riga[x - 3] if x >= 3 else 0
                b = precedente[x]
                c = precedente[x - 3] if x >= 3 else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                riga[x] = (riga[x] + pr) & 0xFF
        fuori += riga
        precedente = riga
    return larghezza, altezza, bytes(fuori)


def accesi(larghezza, altezza, pixel):
    """Insieme dei pixel non neri, con il loro raggio dal centro."""
    punti = {}
    for y in range(altezza):
        base = y * larghezza * 3
        for x in range(larghezza):
            k = base + x * 3
            if pixel[k] or pixel[k + 1] or pixel[k + 2]:
                punti[(x, y)] = math.hypot(x - CX, y - CY)
    return punti


# ------------------------------------------------------------------ cornice

def _hline(s, x, y, w):
    for i in range(x, x + w):
        s.add((i, y))


def _vline(s, x, y, h):
    for i in range(y, y + h):
        s.add((x, i))


def pixel_cerchio(cx, cy, r):
    """Gli stessi pixel che accende gfx->drawCircle(cx, cy, r).

    E' la trascrizione di writeEllipseHelper() di Arduino_GFX: non e' il
    Bresenham da manuale, e con raggio 118 la differenza sui diagonali e' di
    qualche pixel. Ricalcolarlo "a occhio" con una formula trigonometrica
    farebbe risultare interrotto un anello perfettamente intero.
    """
    s = set()
    rx = ry = int(r)
    rx2, ry2 = rx * rx, ry * ry

    i, xt, yt = -1, 0, ry
    st = (ry2 << 1) + rx2 * (1 - (ry << 1))
    while True:
        while st < 0:
            xt += 1
            st += ry2 * ((xt << 2) + 2)
        _hline(s, cx - xt, cy - yt, xt - i)
        _hline(s, cx + i + 1, cy - yt, xt - i)
        _hline(s, cx + i + 1, cy + yt, xt - i)
        _hline(s, cx - xt, cy + yt, xt - i)
        i = xt
        yt -= 1
        st -= yt * rx2 << 2
        if not (ry2 * xt <= rx2 * yt):
            break

    i, yt, xt = -1, 0, rx
    st = (rx2 << 1) + ry2 * (1 - (rx << 1))
    while True:
        while st < 0:
            yt += 1
            st += rx2 * ((yt << 2) + 2)
        _vline(s, cx - xt, cy - yt, yt - i)
        _vline(s, cx + xt, cy - yt, yt - i)
        _vline(s, cx + xt, cy + i + 1, yt - i)
        _vline(s, cx - xt, cy + i + 1, yt - i)
        i = yt
        xt -= 1
        st -= xt * ry2 << 2
        if not (rx2 * yt <= ry2 * xt):
            break
    return s


# Cornice normale: i due anelli che disegna chrome(), raggio 118 e 117.
CORNICE_CHROME = pixel_cerchio(120, 120, 118) | pixel_cerchio(120, 120, 117)
# La schermata di avvio ha una cornice sua: 118 e 116, con un buco in mezzo.
CORNICE_SPLASH = pixel_cerchio(120, 120, 118) | pixel_cerchio(120, 120, 116)


def profilo(nome):
    """Che cornice si aspetta questo PNG, e se ha senso controllarne l'integrita'.

    Ritorna (insieme dei pixel di cornice, controllo_anello, motivo_salto).
    """
    base = nome.lower()
    if "splash" in base:
        return CORNICE_SPLASH, False, "la schermata di avvio non usa chrome(): ha due anelli suoi (118 e 116)"
    if "network-qr" in base or "network-in-rete" in base or "network-dopo-uso" in base:
        return set(), False, "la schermata del QR non disegna nessuna cornice: fillScreen(BLACK) e via"
    return CORNICE_CHROME, True, None


# ------------------------------------------------------------------ controlli

def settori(cornice):
    """Divide i pixel della cornice in 360 spicchi da un grado, per angolo.

    Prima provavo l'opposto — partire dal grado e cercare un pixel acceso lungo
    quel raggio — e sui quattro diagonali esatti veniva fuori un buco che non
    c'era: a 45 gradi il raggio passa in mezzo allo scalino della circonferenza
    discretizzata e non incontra nessuno dei due pixel che la compongono.
    Partendo invece dai pixel della cornice e chiedendo a ciascuno "in che
    spicchio stai" il problema sparisce, perche' nessun pixel resta orfano.
    """
    s = [[] for _ in range(360)]
    for (x, y) in cornice:
        g = int(math.degrees(math.atan2(y - CY, x - CX))) % 360
        s[g].append((x, y))
    return s


def archi_interrotti(punti, cornice):
    """Per ogni grado: fra i pixel di cornice di quello spicchio, ce n'e' uno acceso?"""
    spicchi = settori(cornice)
    rotti = []
    for grado in range(360):
        gruppo = spicchi[grado]
        if not gruppo:
            continue  # spicchio senza pixel di cornice: non c'e' niente da verificare
        if not any(p in punti for p in gruppo):
            rotti.append(grado)
    if not rotti:
        return []

    # Gradi consecutivi = un solo arco mancante. 0 e 359 sono adiacenti.
    archi = []
    inizio = rotti[0]
    atteso = rotti[0]
    for g in rotti[1:]:
        if g == atteso + 1:
            atteso = g
            continue
        archi.append((inizio, atteso))
        inizio = atteso = g
    archi.append((inizio, atteso))
    if len(archi) > 1 and archi[0][0] == 0 and archi[-1][1] == 359:
        archi[0] = (archi[-1][0] - 360, archi[0][1])
        archi.pop()
    return archi


# Quante righe stampare per categoria prima di riassumere. I pixel sono ordinati
# dal piu' esterno al meno: i primi dodici sono quelli che dicono di quanto si e'
# sbordato, dal tredicesimo in giu' e' la stessa notizia ripetuta. Il conteggio
# completo resta comunque nel riepilogo, quindi non si perde niente.
LIMITE_RIGHE = 12


def controlla(percorso, limite_righe=LIMITE_RIGHE):
    nome = os.path.basename(percorso)
    w, h, px = leggi_png(percorso)
    punti = accesi(w, h, px)
    cornice, fai_anello, motivo = profilo(nome)

    problemi = 0
    print("=== %s" % nome)

    if not punti:
        print("  [VUOTO] nessun pixel acceso: la scena non ha disegnato niente")
        return 1

    # --- 1. oltre il bordo del vetro
    fuori = sorted(((r, p) for p, r in punti.items() if r > R_VETRO), key=lambda t: -t[0])
    for idx, (r, p) in enumerate(fuori):
        problemi += 1
        if idx < limite_righe:
            print("  [FUORI DALLO SCHERMO] (%d,%d) raggio %.2f: pixel acceso oltre il vetro, invisibile" % (p[0], p[1], r))
    if len(fuori) > limite_righe:
        print("  [FUORI DALLO SCHERMO] ... e altri %d pixel oltre il vetro" % (len(fuori) - limite_righe))

    # --- 2. dentro la fascia della cornice ma non e' la cornice
    tocca = sorted(
        ((r, p) for p, r in punti.items() if R_ZONA_CORNICE <= r <= R_VETRO and p not in cornice),
        key=lambda t: -t[0],
    )
    for idx, (r, p) in enumerate(tocca):
        problemi += 1
        if idx < limite_righe:
            print("  [TOCCA LA CORNICE] (%d,%d) raggio %.2f: disegno addosso all'anello" % (p[0], p[1], r))
    if len(tocca) > limite_righe:
        print("  [TOCCA LA CORNICE] ... e altri %d pixel nella fascia 116..119.5" % (len(tocca) - limite_righe))

    # --- 3. integrita' dell'anello
    if not fai_anello:
        print("  [ANELLO NON CONTROLLATO] %s" % motivo)
    else:
        for a, b in archi_interrotti(punti, cornice):
            problemi += 1
            ampiezza = b - a + 1
            print("  [ANELLO INTERROTTO] da %d a %d gradi (%d gradi): qui la cornice non c'e' piu'" % (a, b, ampiezza))

    # --- misure, sempre
    rmax = max(punti.values())
    esterni = sorted(punti.items(), key=lambda kv: -kv[1])[:10]
    print("  raggio massimo acceso: %.2f" % rmax)
    print("  10 pixel piu' esterni: %s" % ", ".join("(%d,%d)@%.2f" % (p[0], p[1], r) for p, r in esterni))
    if problemi == 0:
        print("  nessun problema")
    return problemi


def main():
    cartella = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "out")
    file_png = sorted(n for n in os.listdir(cartella) if n.endswith(".png"))
    if not file_png:
        print("nessun PNG in %s" % cartella)
        return 1

    riepilogo = []
    for nome in file_png:
        riepilogo.append((nome, controlla(os.path.join(cartella, nome))))

    print()
    print("=========================== RIEPILOGO ===========================")
    totale = 0
    for nome, n in riepilogo:
        totale += n
        print("%-40s %s" % (nome, ("%d problemi" % n) if n else "ok"))
    print("-----------------------------------------------------------------")
    print("file: %d   problemi totali: %d" % (len(riepilogo), totale))
    return 0


if __name__ == "__main__":
    sys.exit(main())
