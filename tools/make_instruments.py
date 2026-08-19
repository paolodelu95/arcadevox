#!/usr/bin/env python3
"""make_instruments.py — genera src/instruments.cpp: il piano campionato e la batteria.

Due strumenti, e due strade diverse per una ragione precisa.

Il PIANO e' campionato davvero, perche' un pianoforte non si sintetizza in modo
convincente: quello che l'orecchio riconosce e' un transiente di martelletto e
uno spettro inarmonico che nessuna formula corta riproduce. I campioni vengono
dalla University of Iowa Electronic Music Studios, che li pubblica dal 1997 con
una licenza esplicita — "may be downloaded and used for any projects, without
restrictions" — e questo li rende gli unici adatti a un repository pubblico:
finiscono dentro firmware/firmware.bin, che viene ridistribuito via OTA.

La BATTERIA invece e' sintetizzata, e non e' un ripiego. I suoni di batteria
elettronica *nascono* sintetizzati: una cassa e' una sinusoide che scende
d'intonazione mentre si spegne, un rullante e' rumore piu' un tono, un charleston
e' rumore troncato. Sintetizzarli qui costa qualche riga di matematica invece di
centinaia di kilobyte, non porta dentro la licenza di nessuno, e suona come deve
suonare una drum machine. Campionare una batteria acustica sarebbe un'altra cosa,
e vorrebbe una fonte altrettanto libera del piano.

Serve solo la libreria standard: niente numpy, niente ffmpeg. Il resampling e la
conversione li fa `audioop`, che c'e' in ogni Python 3.

    python tools/make_instruments.py [--piano-dir CARTELLA]

I file del piano si scaricano con:

    tools/fetch_piano.sh
"""

import argparse
import math
import os
import random
import struct
import sys
import warnings

warnings.filterwarnings("ignore")  # aifc e audioop sono deprecati ma presenti

import aifc
import audioop
import wave

# Frequenza dei blob. La stessa di samples.cpp: il motore rilegge tutto con lo
# stesso accumulatore di fase, e tenerne due sarebbe solo un modo di sbagliare.
RATE = 16000

# Quanto si tiene di ogni nota di piano. Le registrazioni durano tre quarti di
# minuto perche' arrivano fino all'estinzione completa, ma di un pianoforte si
# riconosce il primo secondo: l'attacco e l'inizio della discesa. Il resto e'
# coda che costa 16 kB al secondo e che sul sequencer verrebbe comunque coperta
# dalla nota dopo.
PIANO_SECONDS = 2.2

# Tetto del motore audio: l'indice di lettura e' a 16 bit interi, oltre i quattro
# secondi ricomincerebbe da capo. Non e' una preferenza, e' un limite.
MAX_SECONDS = 4.0

# --------------------------------------------------------------------- piano
# Sette radici, una ogni tre semitoni. Con lo spostamento massimo di un semitono
# e mezzo in su o in giu' coprono senza buchi le ventuno note fra Do3 e Sol#4, e
# fuori da li' si trasporta di ottave intere — un rapporto esatto di 2, che sul
# piano si sente pochissimo.
PIANO_ROOTS = [
    ("C3", 48),
    ("Eb3", 51),
    ("Gb3", 54),
    ("A3", 57),
    ("C4", 60),
    ("Eb4", 63),
    ("Gb4", 66),
]


def read_any(path):
    """Legge AIFF o WAV e restituisce (frames, sampwidth, rate, canali)."""
    if path.lower().endswith((".aiff", ".aif", ".aifc")):
        with aifc.open(path, "rb") as f:
            return (f.readframes(f.getnframes()), f.getsampwidth(),
                    f.getframerate(), f.getnchannels())
    with wave.open(path, "rb") as f:
        return (f.readframes(f.getnframes()), f.getsampwidth(),
                f.getframerate(), f.getnchannels())


def to_mono_16k(raw, width, rate, channels):
    """Mono, 16 bit, 16 kHz. Nell'ordine che perde meno per strada."""
    if width != 2:
        raw = audioop.lin2lin(raw, width, 2)
        width = 2
    if channels == 2:
        raw = audioop.tomono(raw, 2, 0.5, 0.5)
    if rate != RATE:
        raw, _ = audioop.ratecv(raw, 2, 1, rate, RATE, None)
    return raw


def trim_to_attack(raw, frac=0.5, preroll_ms=6):
    """Porta l'inizio del blob sull'attacco vero della nota.

    Non basta togliere il silenzio: le registrazioni della Iowa cominciano con
    mezzo secondo di rumore di sala e di pedale che arriva quasi al trenta per
    cento del picco. Una soglia bassa lo scambia per l'inizio della nota, e su
    2,2 secondi di campione se ne perde un quarto in fruscio — con l'attacco del
    martelletto, che e' cio' che fa riconoscere un pianoforte, spostato in mezzo
    al blob invece che in testa.

    Un attacco percussivo arriva vicino al picco in pochi millisecondi, quindi si
    cerca il primo punto oltre meta' del massimo e si torna indietro di qualche
    millisecondo per non tagliare il transiente. Cercare la soglia alta e' anche
    piu' robusto del cercare il silenzio: non dipende da quanto e' silenzioso il
    silenzio.
    """
    peak = audioop.max(raw, 2)
    if peak == 0:
        return raw
    th = peak * frac
    step = 16 * 2
    for i in range(0, len(raw) - step, step):
        if audioop.max(raw[i:i + step], 2) > th:
            back = int(RATE * preroll_ms / 1000) * 2
            return raw[max(0, i - back):]
    return raw


def normalize(raw, target=0.89):
    """Porta il picco a un soffio dal fondo scala.

    Non fino in cima: a 8 bit l'arrotondamento puo' spingere un campione oltre il
    limite, e in un blob senza segno un solo valore che gira dall'altra parte fa
    uno schiocco perfettamente udibile.
    """
    peak = audioop.max(raw, 2)
    if peak == 0:
        return raw
    return audioop.mul(raw, 2, (32767.0 * target) / peak)


def fade(raw, ms_in=4, ms_out=120):
    """Sfuma i due estremi.

    In coda serve piu' tempo che in testa: un troncamento netto a meta' della
    vibrazione e' un gradino, e un gradino e' un click. In testa bastano pochi
    millisecondi, giusto per non partire da un valore diverso da zero.
    """
    out = bytearray(raw)
    n = len(out) // 2

    n_in = min(int(RATE * ms_in / 1000), n)
    for i in range(n_in):
        j = i * 2
        v = struct.unpack_from("<h", out, j)[0]
        struct.pack_into("<h", out, j, int(v * i / n_in))

    n_out = min(int(RATE * ms_out / 1000), n)
    for i in range(n_out):
        j = (n - n_out + i) * 2
        v = struct.unpack_from("<h", out, j)[0]
        struct.pack_into("<h", out, j, int(v * (n_out - i) / n_out))
    return bytes(out)


def to_u8(raw):
    """16 bit con segno -> 8 bit senza segno, con 128 come silenzio."""
    return audioop.bias(audioop.lin2lin(raw, 2, 1), 1, 128)


def load_piano_note(path, seconds=PIANO_SECONDS):
    raw, width, rate, channels = read_any(path)
    raw = to_mono_16k(raw, width, rate, channels)
    raw = trim_to_attack(raw)
    raw = raw[:int(RATE * seconds) * 2]
    raw = normalize(raw)
    raw = fade(raw)
    return to_u8(raw)


# ------------------------------------------------------------------ batteria
# Tutto quello che segue e' sintesi sottrattiva elementare, la stessa con cui
# sono fatte le drum machine da cinquant'anni: un oscillatore che scende, del
# rumore, e inviluppi esponenziali.

def env(i, n, curve=5.0):
    """Decadimento esponenziale da 1 a 0 su n campioni."""
    return math.exp(-curve * i / n)


def render(fn, seconds):
    """Chiama fn(i, n) per ogni campione e impacchetta in 8 bit senza segno."""
    n = int(RATE * seconds)
    out = bytearray(n)
    for i in range(n):
        v = fn(i, n)
        v = max(-1.0, min(1.0, v))
        out[i] = int(128 + v * 120)
    return bytes(out)


def drum_kick(i, n):
    # L'intonazione scende da 115 a 45 Hz nei primi 40 ms: e' quella discesa a
    # far sentire "colpo" invece di "nota bassa".
    t = i / RATE
    f = 45 + 70 * math.exp(-t * 28)
    body = math.sin(2 * math.pi * f * t) * env(i, n, 6)
    click = (random.random() * 2 - 1) * env(i, n, 240) * 0.35
    return body * 0.95 + click


def drum_snare(i, n):
    t = i / RATE
    tone = (math.sin(2 * math.pi * 185 * t) + math.sin(2 * math.pi * 278 * t)) * 0.5
    noise = random.random() * 2 - 1
    return (tone * env(i, n, 12) * 0.5 + noise * env(i, n, 9) * 0.7)


def drum_hat_closed(i, n):
    return (random.random() * 2 - 1) * env(i, n, 70) * 0.55


def drum_hat_open(i, n):
    return (random.random() * 2 - 1) * env(i, n, 6) * 0.5


def drum_clap(i, n):
    # Tre raffiche ravvicinate piu' una coda: un battito di mani non e' un colpo
    # solo, ed e' il ritardo fra le raffiche a farlo sembrare tale.
    t = i / RATE
    g = 0.0
    for d in (0.0, 0.011, 0.022):
        if t >= d:
            g = max(g, math.exp(-(t - d) * 180))
    g = max(g, math.exp(-t * 16) * 0.45)
    return (random.random() * 2 - 1) * g * 0.8


def drum_tom(i, n):
    t = i / RATE
    f = 110 + 90 * math.exp(-t * 20)
    return math.sin(2 * math.pi * f * t) * env(i, n, 7) * 0.9


def drum_rim(i, n):
    t = i / RATE
    return (math.sin(2 * math.pi * 1700 * t) * 0.6 +
            (random.random() * 2 - 1) * 0.4) * env(i, n, 260)


def drum_crash(i, n):
    # Rumore che si spegne piano. Due strati con decadimenti diversi: quello
    # veloce da' il colpo, quello lento la coda che resta appesa.
    fast = (random.random() * 2 - 1) * env(i, n, 22)
    slow = (random.random() * 2 - 1) * env(i, n, 3.2)
    return fast * 0.45 + slow * 0.5


DRUMS = [
    ("CASSA", "il colpo che tiene il tempo", drum_kick, 0.34),
    ("RULLANTE", "rumore e tono insieme", drum_snare, 0.26),
    ("CHARLESTON", "chiuso, corto", drum_hat_closed, 0.09),
    ("CHARLES.AP", "aperto, resta sospeso", drum_hat_open, 0.42),
    ("BATTIMANI", "tre raffiche, non una", drum_clap, 0.30),
    ("TOM", "scende d'intonazione", drum_tom, 0.36),
    ("BORDO", "secco, quasi solo attacco", drum_rim, 0.07),
    ("PIATTO", "coda lunga che resta", drum_crash, 1.30),
]


# -------------------------------------------------------------------- output
def emit_blob(out, name, data):
    out.write("const uint8_t %s[%d] PROGMEM = {\n" % (name, len(data)))
    for i in range(0, len(data), 16):
        out.write("    " + ",".join("%d" % b for b in data[i:i + 16]) + ",\n")
    out.write("};\n\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--piano-dir", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "piano"))
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "src", "instruments.cpp"))
    args = ap.parse_args()

    random.seed(20260819)  # rigenerare due volte deve dare lo stesso file

    piano = []
    for name, midi in PIANO_ROOTS:
        path = os.path.join(args.piano_dir, "Piano.ff.%s.aiff" % name)
        if not os.path.exists(path):
            sys.exit("manca %s\nScaricali con tools/fetch_piano.sh" % path)
        blob = load_piano_note(path)
        if len(blob) > RATE * MAX_SECONDS:
            sys.exit("%s supera i %g secondi" % (name, MAX_SECONDS))
        piano.append((name, midi, blob))
        print("piano %-4s  %6.2f kB  %.2f s" % (name, len(blob) / 1024, len(blob) / RATE))

    drums = []
    for name, hint, fn, secs in DRUMS:
        blob = render(fn, secs)
        drums.append((name, hint, blob))
        print("batt. %-11s %6.2f kB  %.2f s" % (name, len(blob) / 1024, len(blob) / RATE))

    total = sum(len(b) for _, _, b in piano) + sum(len(b) for _, _, b in drums)
    print("--\ntotale %.1f kB di flash" % (total / 1024))

    out_path = os.path.abspath(args.out)
    with open(out_path, "w", encoding="utf-8") as out:
        out.write("// instruments.cpp — GENERATO da tools/make_instruments.py.\n")
        out.write("// Non modificare a mano: si cambia lo script e si rigenera.\n")
        out.write("//\n")
        out.write("// Piano: University of Iowa Electronic Music Studios, pubblicati senza\n")
        out.write("// restrizioni d'uso. Batteria: sintetizzata dalle formule nello script.\n\n")
        out.write('#include "instruments.h"\n\n')
        out.write("namespace {\n\n")
        for name, _, blob in piano:
            emit_blob(out, "PIANO_%s" % name.replace("#", "s"), blob)
        for name, _, blob in drums:
            emit_blob(out, "DRUM_%s" % name.replace(".", "_"), blob)
        out.write("}  // namespace\n\n")

        out.write("const PianoRoot PIANO_ROOTS[] = {\n")
        for name, midi, blob in piano:
            out.write("    {PIANO_%s, %d, %d},\n" % (name.replace("#", "s"), len(blob), midi))
        out.write("};\n")
        out.write("const uint8_t PIANO_ROOT_COUNT = %d;\n\n" % len(piano))

        out.write("const DrumHit DRUM_KIT[] = {\n")
        for name, hint, blob in drums:
            out.write('    {"%s", "%s", DRUM_%s, %d},\n'
                      % (name, hint, name.replace(".", "_"), len(blob)))
        out.write("};\n")
        out.write("const uint8_t DRUM_COUNT = %d;\n" % len(drums))

    print("scritto %s (%.1f kB di sorgente)" % (out_path, os.path.getsize(out_path) / 1024))


if __name__ == "__main__":
    main()
