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

Anche la BATTERIA e' campionata, e prima non lo era. Era sintetizzata — una
cassa era una sinusoide che scendeva mentre si spegneva, un rullante rumore piu'
tono — e suonava esattamente per quello che era: una drum machine, non una
batteria. Sono due strumenti diversi, e chi vuole il secondo non si accontenta
del primo.

I tredici pezzi vengono dalla MuldjordKit, registrata da Lars Muldjord e
pubblicata per DrumGizmo; la versione stereo la assembla il progetto FreePats.
Licenza Creative Commons Attribution 4.0, che e' la ragione per cui e' lei e non
un'altra: come il piano finisce dentro firmware/firmware.bin, e quel file viene
ridistribuito. L'attribuzione richiesta e' testuale — "Drum samples provided by
DrumGizmo.org" — e sta nel README del progetto e nelle note della release.

Per il piano basta la libreria standard. Per la batteria serve anche un
convertitore che sappia leggere il FLAC: su macOS c'e' afconvert di sistema,
altrove ffmpeg. E' la stessa dipendenza che ha gia' tools/make_samples.py.

    python tools/make_instruments.py [--piano-dir CARTELLA] [--drums-dir CARTELLA]

I file si scaricano con:

    tools/fetch_piano.sh
    tools/fetch_drums.sh
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
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
    """Legge AIFF o WAV e restituisce (frames, sampwidth, rate, canali).

    L'AIFF nasce sui Macintosh a processore Motorola e ha i campioni in ordine
    **big-endian**; il modulo `aifc` li restituisce cosi' come stanno, senza
    girarli. Tutto il resto della catena — `audioop`, e poi il C che legge i
    blob — lavora nell'ordine della macchina, che qui e' little-endian: dare in
    pasto i byte non girati vuol dire scambiare la meta' alta con la meta' bassa
    di ogni campione, e cio' che ne esce **e' rumore**, con l'ampiezza sparata a
    fondo scala e nessuna traccia della nota che c'era dentro.

    E' esattamente cio' che era successo al piano: le sette radici erano fruscio
    a volume pieno invece che sette note. Un byte fuori posto, e il campione
    piu' curato del progetto suonava peggio del preset che doveva sostituire.

    Il WAV invece e' gia' little-endian per definizione del formato, e il modulo
    `wave` non ha niente da girare.
    """
    if path.lower().endswith((".aiff", ".aif", ".aifc")):
        with aifc.open(path, "rb") as f:
            raw = f.readframes(f.getnframes())
            width = f.getsampwidth()
            # Solo i campioni non compressi arrivano grezzi: se `aifc` ha dovuto
            # decodificare (ulaw, alaw, sowt) ha gia' consegnato roba nell'ordine
            # della macchina, e girarla di nuovo la romperebbe.
            if f.getcomptype() == b"NONE" and width > 1:
                raw = audioop.byteswap(raw, width)
            return (raw, width, f.getframerate(), f.getnchannels())
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
# Tredici pezzi, uno per tasto. Nomi dei file come li scrive tools/fetch_drums.sh.
#
# Il secondo numero e' il tetto in secondi: e' un tetto e non una durata, perche'
# a tagliare davvero ci pensa trim_decay() sul silenzio vero del campione. Serve
# solo a impedire che un piatto lasciato suonare quattro secondi si porti via
# sessantaquattro kilobyte di coda che nessuno sente.
DRUMS = [
    ("CASSA", "il colpo che tiene il tempo", "01-cassa", 1.0),
    ("RULLANTE", "pelle e cordiera", "02-rullante", 1.0),
    ("BORDO", "secco, quasi solo attacco", "03-bordo", 0.8),
    ("TOM 1", "il piu' acuto dei tre", "04-tom1", 1.2),
    ("TOM 2", "quello di mezzo", "05-tom2", 1.2),
    ("TOM 3", "il piu' grave dei tre", "06-tom3", 1.4),
    ("TOM BASSO", "il timpano, da terra", "07-tombasso", 1.6),
    ("CHARLESTON", "chiuso, corto", "08-charleston", 0.5),
    ("CHARLES.AP", "aperto, resta sospeso", "09-charlestonaperto", 1.4),
    ("RIDE", "il piatto che si cavalca", "10-ride", 2.0),
    ("CAMPANA", "la cupola del ride", "11-campana", 1.8),
    ("PIATTO", "coda lunga che resta", "12-piatto", 3.0),
    ("CINESE", "sporco, si spegne prima", "13-cinese", 2.6),
]


def _to_wav(path):
    """Porta un FLAC a WAV PCM in un file temporaneo.

    Nessun modulo della libreria standard legge il FLAC, e mettere una
    dipendenza Python solo per questo sarebbe sproporzionato: macOS ha afconvert
    di sistema, il resto del mondo ha ffmpeg. E' la stessa scelta — e lo stesso
    codice — di tools/make_samples.py, che il FLAC lo incontra da sempre fra i
    file personali della schermata SUONI.
    """
    tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
    tmp.close()
    if shutil.which("afconvert"):
        cmd = ["afconvert", "-f", "WAVE", "-d", "LEI16@%d" % RATE, "-c", "1", path, tmp.name]
    elif shutil.which("ffmpeg"):
        cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", path,
               "-ac", "1", "-ar", str(RATE), tmp.name]
    else:
        os.unlink(tmp.name)
        sys.exit("per leggere %s serve afconvert (macOS) o ffmpeg."
                 % os.path.basename(path))
    subprocess.run(cmd, check=True)
    return tmp.name


def trim_decay(raw, floor=0.012, tail_ms=40):
    """Taglia la coda dove il colpo e' finito davvero.

    Un colpo di batteria registrato dura quanto dura la stanza: dopo il rullante
    restano due secondi di riverbero sempre piu' vicini al rumore di fondo, e a
    8 bit quel riverbero non e' nemmeno piu' rappresentabile — sono migliaia di
    campioni che valgono tutti 128 o 129 e costano 16 kB al secondo.
    Si cerca all'indietro l'ultimo punto sopra l'uno per cento del picco e si
    lascia un pelo di coda, che poi la dissolvenza chiude.

    All'indietro e non in avanti: un piatto ha dei buchi in mezzo — momenti in
    cui le due lamiere si ritrovano in fase e il livello crolla — e chi cerca dal
    principio il primo punto sotto la soglia taglia li', a meta' del colpo.
    """
    peak = audioop.max(raw, 2)
    if peak == 0:
        return raw
    th = peak * floor
    step = 32 * 2
    last = len(raw)
    for i in range(len(raw) - step, 0, -step):
        if audioop.max(raw[i:i + step], 2) > th:
            last = i + step
            break
    tail = int(RATE * tail_ms / 1000) * 2
    return raw[:min(len(raw), last + tail)]


def load_drum(path, max_seconds):
    wav = _to_wav(path)
    try:
        raw, width, rate, channels = read_any(wav)
    finally:
        os.unlink(wav)
    raw = to_mono_16k(raw, width, rate, channels)
    # Soglia piu' bassa che sul piano: un colpo di batteria e' gia' al massimo
    # nei primi millisecondi, e cercare meta' del picco rischia di entrare dopo
    # l'attacco invece che prima.
    raw = trim_to_attack(raw, frac=0.35, preroll_ms=4)
    raw = raw[:int(RATE * max_seconds) * 2]
    raw = trim_decay(raw)
    raw = normalize(raw)
    # In coda molto meno che sul piano: trenta millisecondi bastano a non fare il
    # gradino, e piu' di cosi' si mangerebbe la fine di un charleston chiuso, che
    # dura in tutto un decimo di secondo.
    raw = fade(raw, ms_in=1, ms_out=30)
    return to_u8(raw)


# ------------------------------------------------------- le due voci in coda
# Nome e didascalia con cui PIANO e BATTERIA compaiono in fondo all'elenco dei
# timbri. Stanno qui e non in settings.cpp perche' il menu e la schermata TIMBRI
# li chiedevano ognuno per conto suo — il primo li aveva scritti a mano, la
# seconda non li aveva affatto e sotto il cursore restava il nome del primo
# preset. Una tabella sola, della stessa forma di PRESETS[], e le due schermate
# smettono di poter dire cose diverse.
SAMPLED = [
    ("PIANO", "campionato, non imitato"),
    ("BATTERIA", "un pezzo per tasto"),
]


# -------------------------------------------------------------------- output
def ident(name):
    """Nome di variabile C a partire dal nome del pezzo.

    I nomi dei pezzi sono fatti per il display, non per il compilatore: "TOM 1"
    ha uno spazio dentro e "CHARLES.AP" un punto. Sostituire solo il punto,
    com'era prima, bastava finche' i pezzi erano otto e nessuno aveva spazi —
    poi "TOM BASSO" e' diventato `DRUM_TOM BASSO` e il file generato non
    compilava piu'. Qui tutto cio' che non e' lettera o cifra diventa un
    trattino basso, una volta sola e in un posto solo.
    """
    return "".join(c if c.isalnum() else "_" for c in name)


def emit_blob(out, name, data):
    out.write("const uint8_t %s[%d] PROGMEM = {\n" % (name, len(data)))
    for i in range(0, len(data), 16):
        out.write("    " + ",".join("%d" % b for b in data[i:i + 16]) + ",\n")
    out.write("};\n\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--piano-dir", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "piano"))
    ap.add_argument("--drums-dir", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "drums"))
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "src", "instruments.cpp"))
    args = ap.parse_args()

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
    for name, hint, stem, secs in DRUMS:
        path = os.path.join(args.drums_dir, stem + ".flac")
        if not os.path.exists(path):
            sys.exit("manca %s\nScaricali con tools/fetch_drums.sh" % path)
        blob = load_drum(path, secs)
        if len(blob) > RATE * MAX_SECONDS:
            sys.exit("%s supera i %g secondi" % (name, MAX_SECONDS))
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
        out.write("// restrizioni d'uso.\n")
        out.write("//\n")
        out.write("// Batteria: MuldjordKit di Lars Muldjord (DrumGizmo), versione FreePats,\n")
        out.write("// Creative Commons Attribution 4.0. L'attribuzione richiesta e':\n")
        out.write("//     Drum samples provided by DrumGizmo.org.\n\n")
        out.write('#include "instruments.h"\n\n')
        out.write("namespace {\n\n")
        for name, _, blob in piano:
            emit_blob(out, "PIANO_%s" % name.replace("#", "s"), blob)
        for name, _, blob in drums:
            emit_blob(out, "DRUM_%s" % ident(name), blob)
        out.write("}  // namespace\n\n")

        out.write("const PianoRoot PIANO_ROOTS[] = {\n")
        for name, midi, blob in piano:
            out.write("    {PIANO_%s, %d, %d},\n" % (name.replace("#", "s"), len(blob), midi))
        out.write("};\n")
        out.write("const uint8_t PIANO_ROOT_COUNT = %d;\n\n" % len(piano))

        out.write("const DrumHit DRUM_KIT[] = {\n")
        for name, hint, blob in drums:
            out.write('    {"%s", "%s", DRUM_%s, %d},\n'
                      % (name, hint, ident(name), len(blob)))
        out.write("};\n")
        out.write("const uint8_t DRUM_COUNT = %d;\n\n" % len(drums))

        out.write("const SampledInstrument SAMPLED_INSTRUMENTS[] = {\n")
        for name, hint in SAMPLED:
            out.write('    {"%s", "%s"},\n' % (name, hint))
        out.write("};\n")

    print("scritto %s (%.1f kB di sorgente)" % (out_path, os.path.getsize(out_path) / 1024))


if __name__ == "__main__":
    main()
