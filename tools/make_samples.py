#!/usr/bin/env python3
"""Genera src/samples.cpp — i tredici suoni della schermata SUONI.

I TUOI FILE VENGONO PRIMA
------------------------
Se in tools/samples/ ci sono dei file audio, sono quelli a finire nel firmware:
si chiamano "01 nome.wav", "02 altro.mp3" e via, e il numero decide su quale
tasto vanno. Vedi tools/samples/README.md. I posti lasciati vuoti li riempiono i
suoni sintetizzati qui sotto, quindi se ne possono sostituire tre e tenere gli
altri dieci.

Quella cartella e' esclusa da git apposta: il repository e' pubblico, e i suoni
che girano in rete sono quasi tutti registrazioni di qualcuno. Sulla propria
scheda e' un conto, in un repository che chiunque puo' clonare e' un altro.

PERCHE' QUELLI DI SERIE SONO SINTETIZZATI
-----------------------------------------
I suoni "meme" che girano in rete sono registrazioni, e quasi tutte hanno un
padrone: la trombetta e' un campione di qualcuno, il boom viene da un film, la
risata da uno spettacolo. Infilarle in un firmware che sta su un repository
pubblico vorrebbe dire ridistribuirle, e non e' una cosa che si possa fare
alla leggera.

Quindi qui non si scarica niente: i suoni si *fanno*, con la sintesi, partendo
da cio' che li rende riconoscibili. Una trombetta da stadio e' tre lame
scordate fra loro che calano di un semitono mentre suonano; un boom e' una
sinusoide che scende sotto i quaranta hertz con un colpo di rumore davanti; una
vocale urlata sono tre risonanze in fila su un treno di impulsi. Sono
quell'idea, non quella registrazione — e sono anche molto piu' piccoli, perche'
un suono descritto da venti righe di formule occupa in flash solo il tempo che
dura.

FORMATO
-------
8 bit senza segno a 16 kHz, mono. Un secondo costa 16 kB: tutti e tredici
stanno in circa 150 kB, su oltre due megabyte liberi nella partizione
dell'applicazione. Otto bit bastano perche' questi suoni sono rumorosi o
distorti per natura — su una vocale urlata la differenza con i sedici non si
sente, e sul boom nemmeno.

Il firmware li rilegge a 44100 con un accumulatore di fase e interpolazione
lineare: alzare la frequenza e' l'unica cosa che il motore deve fare.

Uso:  python3 tools/make_samples.py
"""

import math
import os
import sys

import numpy as np

RATE = 16000
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "src", "samples.cpp")
USER_DIR = os.path.join(HERE, "samples")

# Quattro secondi. Non e' una preferenza: il motore audio tiene la posizione di
# lettura come indice intero piu' frazione a 16 bit, e oltre questo l'indice non
# ci sta piu'. Meglio fermarsi qui e dirlo che consegnare un suono che ricomincia
# da capo all'infinito.
MAX_SECONDS = 4.0


# ---------------------------------------------------------------- mattoni

def t(dur):
    """Asse dei tempi in secondi."""
    return np.arange(int(dur * RATE)) / RATE


def env(n, attack=0.005, decay=0.0, sustain=1.0, release=0.05):
    """Inviluppo ADSR sui campioni, in secondi."""
    a = int(attack * RATE)
    d = int(decay * RATE)
    r = int(release * RATE)
    s = max(0, n - a - d - r)
    parts = [
        np.linspace(0.0, 1.0, a, endpoint=False) if a else np.zeros(0),
        np.linspace(1.0, sustain, d, endpoint=False) if d else np.zeros(0),
        np.full(s, sustain),
        np.linspace(sustain, 0.0, r) if r else np.zeros(0),
    ]
    out = np.concatenate(parts)
    return np.pad(out, (0, max(0, n - len(out))))[:n]


def phase(freq):
    """Fase continua da un vettore di frequenze istantanee."""
    return 2.0 * np.pi * np.cumsum(freq) / RATE


def saw(freq):
    """Dente di sega con l'attenuazione delle armoniche alte: a 16 kHz una sega
    ideale alias in modo udibile, e su una trombetta si sente eccome."""
    ph = phase(freq) / (2.0 * np.pi)
    out = np.zeros(len(ph))
    h = 1
    while True:
        # Si fermano le armoniche che supererebbero Nyquist: sopra, non sono
        # suono, sono lo specchio del suono.
        if np.max(freq) * h > RATE * 0.45:
            break
        out += np.sin(2.0 * np.pi * ph * h) / h
        h += 1
        if h > 40:
            break
    return out * (2.0 / np.pi)


def square(freq, duty=0.5):
    ph = phase(freq) / (2.0 * np.pi)
    return np.where((ph % 1.0) < duty, 1.0, -1.0)


def noise(n, seed=1):
    """Rumore *riproducibile*: lo stesso script deve dare lo stesso file, o ogni
    rigenerazione sporcherebbe il diff di centocinquanta kB di roba uguale."""
    return np.random.default_rng(seed).uniform(-1.0, 1.0, n)


def biquad(x, b0, b1, b2, a1, a2):
    """Un biquadratico, campione per campione. Un IIR non si vettorizza — dipende
    dalle sue stesse uscite — e per centomila campioni il ciclo in Python va
    benissimo."""
    y = np.zeros(len(x))
    x1 = x2 = y1 = y2 = 0.0
    for i, xi in enumerate(x):
        yi = b0 * xi + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        y[i] = yi
        x2, x1 = x1, xi
        y2, y1 = y1, yi
    return y


def lowpass(x, fc, q=0.707):
    w = 2.0 * math.pi * fc / RATE
    alpha = math.sin(w) / (2.0 * q)
    cosw = math.cos(w)
    a0 = 1.0 + alpha
    return biquad(x, (1 - cosw) / 2 / a0, (1 - cosw) / a0, (1 - cosw) / 2 / a0,
                  -2 * cosw / a0, (1 - alpha) / a0)


def bandpass(x, fc, q):
    w = 2.0 * math.pi * fc / RATE
    alpha = math.sin(w) / (2.0 * q)
    cosw = math.cos(w)
    a0 = 1.0 + alpha
    return biquad(x, alpha / a0, 0.0, -alpha / a0, -2 * cosw / a0, (1 - alpha) / a0)


def highpass(x, fc, q=0.707):
    w = 2.0 * math.pi * fc / RATE
    alpha = math.sin(w) / (2.0 * q)
    cosw = math.cos(w)
    a0 = 1.0 + alpha
    return biquad(x, (1 + cosw) / 2 / a0, -(1 + cosw) / a0, (1 + cosw) / 2 / a0,
                  -2 * cosw / a0, (1 - alpha) / a0)


def echo(x, delay_s, feedback, mix, taps=6):
    """Eco a ripetizioni: quello che rende la vocale urlata *quella* vocale."""
    d = int(delay_s * RATE)
    out = np.pad(x, (0, d * taps)).astype(float)
    g = feedback
    for k in range(1, taps + 1):
        out[d * k:d * k + len(x)] += x * g * mix
        g *= feedback
    return out


def glottal(f0, n, shape=6.0):
    """Treno di impulsi glottali: la sorgente di una voce. Non e' un impulso
    secco — sarebbe un click — ma una gobba asimmetrica, che e' cio' che da'
    alla voce il suo timbro prima ancora delle risonanze."""
    ph = (phase(f0) / (2.0 * np.pi)) % 1.0
    return np.exp(-shape * ph) * np.sin(np.pi * ph) * 2.0


def vowel(f0, dur, formants, spread=(12.0, 10.0, 8.0), gains=(1.0, 0.7, 0.35)):
    """Vocale per sintesi sorgente-filtro: impulsi glottali dentro tre risonanze.
    Le tre frequenze sono quelle che decidono *quale* vocale si sente."""
    n = int(dur * RATE)
    f0v = np.resize(np.asarray(f0, dtype=float), n) if np.ndim(f0) else np.full(n, float(f0))
    src = glottal(f0v, n)
    out = np.zeros(n)
    for f, q, g in zip(formants, spread, gains):
        out += bandpass(src, f, q) * g
    return out


def norm(x, peak=0.92):
    m = np.max(np.abs(x))
    return x * (peak / m) if m > 1e-9 else x


def fade(x, ms=8):
    """Sfuma le due code: un campione che comincia o finisce di netto fa un click,
    e su un altoparlante piccolo il click si sente piu' del suono."""
    k = int(ms * RATE / 1000)
    if len(x) < 2 * k:
        return x
    x = x.copy()
    x[:k] *= np.linspace(0.0, 1.0, k)
    x[-k:] *= np.linspace(1.0, 0.0, k)
    return x


# ------------------------------------------------------------- i tuoi file

def _read_wav(path):
    """Legge un WAV PCM e lo riporta a mono, float -1..1."""
    import wave

    with wave.open(path, "rb") as w:
        ch, width, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 1:
        # PCM a 8 bit e' senza segno, con lo zero a 128: e' l'unica larghezza in
        # cui il formato cambia convenzione, e dimenticarsene da' un rumore.
        x = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
    elif width == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    elif width == 4:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0
    else:
        raise ValueError("WAV a %d byte per campione: converti prima" % width)
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return x, sr


def _to_wav(path):
    """Porta qualunque formato a WAV PCM in un file temporaneo. Su macOS c'e'
    afconvert di sistema, altrove ffmpeg: uno dei due basta, e se non c'e'
    nessuno dei due lo si dice invece di fallire con un errore incomprensibile."""
    import shutil
    import subprocess
    import tempfile

    tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
    tmp.close()
    if shutil.which("afconvert"):
        cmd = ["afconvert", "-f", "WAVE", "-d", "LEI16@%d" % RATE, "-c", "1", path, tmp.name]
    elif shutil.which("ffmpeg"):
        cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", path,
               "-ac", "1", "-ar", str(RATE), tmp.name]
    else:
        raise RuntimeError(
            "per leggere %s serve afconvert (macOS) o ffmpeg. "
            "In alternativa esporta il file in WAV." % os.path.basename(path))
    subprocess.run(cmd, check=True)
    return tmp.name


def resample(x, sr):
    """A 16 kHz, per interpolazione lineare. I file dell'utente arrivano a 44 o 48
    kHz e vanno *abbassati*, quindi prima si toglie quello che sopra gli 8 kHz non
    ci starebbe piu': senza, quelle frequenze si ripiegano in banda e il suono si
    riempie di fischi che nell'originale non c'erano."""
    if sr == RATE:
        return x
    if sr > RATE:
        # Passa-basso a poco meno di Nyquist, due volte per farlo piu' ripido.
        nyq = RATE * 0.45
        old, globals()["RATE"] = RATE, sr  # i filtri lavorano sulla frequenza del file
        try:
            x = lowpass(lowpass(x, nyq), nyq)
        finally:
            globals()["RATE"] = old
    n = int(len(x) * RATE / sr)
    return np.interp(np.arange(n) * (sr / RATE), np.arange(len(x)), x)


def trim(x, floor=0.02):
    """Toglie il silenzio in testa e in coda: nei file scaricati ce n'e' quasi
    sempre, ed e' ritardo fra il dito e il suono."""
    loud = np.abs(x) > floor
    if not loud.any():
        return x
    i0, i1 = np.argmax(loud), len(x) - np.argmax(loud[::-1])
    return x[max(0, i0 - int(0.005 * RATE)):min(len(x), i1 + int(0.02 * RATE))]


def load_user():
    """Legge tools/samples/. Ritorna {indice: (nome, descrizione, campioni)}."""
    out = {}
    if not os.path.isdir(USER_DIR):
        return out
    exts = (".wav", ".mp3", ".m4a", ".aif", ".aiff", ".ogg", ".flac")
    for fname in sorted(os.listdir(USER_DIR)):
        stem, ext = os.path.splitext(fname)
        if ext.lower() not in exts:
            continue
        head = stem.split(" ", 1)
        if not head[0].isdigit():
            print("  ! %s: manca il numero davanti, lo salto" % fname)
            continue
        slot = int(head[0]) - 1
        rest = head[1] if len(head) > 1 else "SUONO"
        # "nome-descrizione": il primo trattino separa i due, il resto e' testo.
        bits = rest.replace("_", "-").split("-", 1)
        name = bits[0].strip().upper()[:11]
        hint = (bits[1].strip() if len(bits) > 1 else "").replace("-", " ")[:24]

        path = os.path.join(USER_DIR, fname)
        tmp = None
        try:
            if ext.lower() != ".wav":
                tmp = _to_wav(path)
                x, sr = _read_wav(tmp)
            else:
                try:
                    x, sr = _read_wav(path)
                except Exception:
                    tmp = _to_wav(path)  # WAV compresso o a 24 bit: passa dal convertitore
                    x, sr = _read_wav(tmp)
        finally:
            if tmp and os.path.exists(tmp):
                os.unlink(tmp)

        x = trim(resample(x, sr))
        if len(x) > MAX_SECONDS * RATE:
            print("  ! %s: %.1f s, tagliato a %.0f" % (fname, len(x) / RATE, MAX_SECONDS))
            x = x[:int(MAX_SECONDS * RATE)]
        out[slot] = (name, hint, norm(fade(x)))
        print("  + tasto %2d  %-11s %s" % (slot + 1, name, fname))
    return out


# ----------------------------------------------------------------- i suoni

def s_trombetta():
    """Trombetta da stadio. Tre lame scordate che calano di poco mentre suonano:
    e' quel calare, piu' della nota, a farla riconoscere."""
    d = 1.05
    n = len(t(d))
    fall = np.linspace(1.0, 0.94, n)
    x = np.zeros(n)
    for det in (1.0, 1.007, 0.994):
        x += saw(np.full(n, 466.0) * det * fall)
    x = lowpass(x, 3000.0)
    x += 0.35 * lowpass(saw(np.full(n, 932.0) * fall), 4500.0)
    e = env(n, attack=0.012, decay=0.05, sustain=0.85, release=0.12)
    return norm(x * e)


def s_faaa():
    """La vocale urlata con l'eco. Pitch che parte alto e cala: e' il gesto di
    uno che grida, e senza quel calare suonerebbe come una nota tenuta."""
    d = 0.62
    n = int(d * RATE)
    f0 = np.concatenate([
        np.linspace(210.0, 178.0, int(n * 0.25)),
        np.linspace(178.0, 150.0, n - int(n * 0.25)),
    ])
    # Formanti della /a/ aperta, spinte in alto come in un grido.
    x = vowel(f0, d, formants=(780.0, 1250.0, 2650.0), spread=(9.0, 8.0, 7.0),
              gains=(1.0, 0.85, 0.45))
    x = np.tanh(x * 2.2)  # la voce urlata satura: e' meta' del suo carattere
    x *= env(n, attack=0.02, decay=0.10, sustain=0.80, release=0.18)
    x = echo(x, 0.115, feedback=0.62, mix=0.85, taps=7)
    return norm(fade(x))


def s_boom():
    """Il colpo grave. Sinusoide che scende sotto i quaranta hertz, con davanti un
    colpo di rumore filtrato che da' l'impatto."""
    d = 1.15
    n = int(d * RATE)
    f = 120.0 * np.exp(-np.arange(n) / (RATE * 0.16)) + 32.0
    body = np.sin(phase(f))
    body *= np.exp(-np.arange(n) / (RATE * 0.30))
    hit = lowpass(noise(int(0.05 * RATE), seed=7), 900.0)
    hit *= np.exp(-np.arange(len(hit)) / (RATE * 0.012))
    x = body.copy()
    x[:len(hit)] += hit * 0.8
    x = np.tanh(x * 1.6)
    return norm(fade(x))


def s_trombone():
    """Il trombone triste: quattro note che scendono, l'ultima che si affloscia.
    Il portamento fra una e l'altra e' la battuta."""
    steps = [(311.0, 0.20), (277.0, 0.20), (247.0, 0.22), (208.0, 0.55)]
    freqs = []
    for i, (f, dur) in enumerate(steps):
        nn = int(dur * RATE)
        start = steps[i - 1][0] if i else f
        glide = int(0.05 * RATE)
        seg = np.full(nn, f)
        seg[:glide] = np.linspace(start, f, glide)
        freqs.append(seg)
    f = np.concatenate(freqs)
    # L'ultima nota cala e vibra: e' la parte che fa ridere.
    tail = int(0.35 * RATE)
    f[-tail:] *= np.linspace(1.0, 0.86, tail)
    f *= 1.0 + 0.02 * np.sin(phase(np.full(len(f), 5.5)))
    x = saw(f)
    x = bandpass(x, 520.0, 1.4) * 1.2 + lowpass(x, 1800.0) * 0.6
    # La risonanza a 520 Hz raccoglie sempre piu' armoniche mentre la nota scende,
    # quindi senza questo il trombone *cresce* mentre dovrebbe sgonfiarsi — che e'
    # il contrario esatto della battuta.
    x *= np.exp(-np.arange(len(x)) / (RATE * 0.75))
    x *= env(len(x), attack=0.03, decay=0.08, sustain=0.9, release=0.20)
    return norm(fade(x))


def s_bruh():
    """Vocale chiusa e corta, con il pitch che scivola giu': la sillaba che si usa
    quando non c'e' niente da aggiungere."""
    d = 0.42
    n = int(d * RATE)
    f0 = np.linspace(145.0, 96.0, n)
    x = vowel(f0, d, formants=(520.0, 1120.0, 2400.0), spread=(10.0, 9.0, 8.0),
              gains=(1.0, 0.55, 0.20))
    x = np.tanh(x * 1.8)
    x *= env(n, attack=0.015, decay=0.06, sustain=0.75, release=0.16)
    return norm(fade(x))


def s_risata():
    """Risata: quattro sillabe che calano di tono, ognuna con la sua gobba. Non e'
    una risata vera e non vuole esserlo — e' la sua forma."""
    out = []
    base = 200.0
    for i in range(4):
        d = 0.13
        n = int(d * RATE)
        f0 = np.linspace(base * 1.12, base * 0.92, n)
        v = vowel(f0, d, formants=(700.0, 1150.0, 2500.0), spread=(9.0, 8.0, 7.0),
                  gains=(1.0, 0.7, 0.3))
        v *= env(n, attack=0.012, decay=0.05, sustain=0.55, release=0.06)
        out.append(v)
        out.append(np.zeros(int(0.045 * RATE)))
        base *= 0.90
    x = np.concatenate(out)
    x = np.tanh(x * 1.5)
    return norm(fade(x))


def s_applauso():
    """Applauso: cento battute di mani sparse, ognuna un colpo di rumore filtrato.
    La densita' cresce e poi cala, come succede davvero."""
    d = 1.4
    n = int(d * RATE)
    x = np.zeros(n)
    rng = np.random.default_rng(11)
    for _ in range(150):
        pos = int(abs(rng.normal(0.45, 0.30)) * RATE) % max(1, n - 800)
        clap = noise(600, seed=int(rng.integers(1, 9999)))
        clap *= np.exp(-np.arange(600) / (RATE * 0.006))
        x[pos:pos + 600] += clap * rng.uniform(0.4, 1.0)
    x = bandpass(x, 1800.0, 0.8) * 1.4 + x * 0.3
    x *= env(n, attack=0.05, decay=0.0, sustain=1.0, release=0.45)
    return norm(fade(x))


def s_moneta():
    """La monetina raccolta: due note in salita, onda quadra, niente filtro. E' il
    suono piu' corto e piu' riconoscibile di tutti."""
    n1, n2 = int(0.055 * RATE), int(0.32 * RATE)
    a = square(np.full(n1, 988.0), 0.5)
    b = square(np.full(n2, 1319.0), 0.5)
    x = np.concatenate([a, b])
    x *= env(len(x), attack=0.002, decay=0.0, sustain=1.0, release=0.22)
    return norm(x * 0.8)


def s_laser():
    """Sparo: una quadra che precipita di due ottave in un quarto di secondo."""
    d = 0.28
    n = int(d * RATE)
    f = 1800.0 * np.exp(-np.arange(n) / (RATE * 0.055)) + 140.0
    x = square(f, 0.35)
    x *= env(n, attack=0.002, decay=0.0, sustain=1.0, release=0.12)
    x = lowpass(x, 5200.0)
    return norm(fade(x, 4))


def s_errore():
    """Il pulsante della risposta sbagliata: quadra bassa e sporca, due colpi."""
    beep = square(np.full(int(0.30 * RATE), 155.0), 0.5)
    beep = np.tanh(beep * 2.0)
    beep *= env(len(beep), attack=0.004, decay=0.0, sustain=1.0, release=0.05)
    gap = np.zeros(int(0.07 * RATE))
    x = np.concatenate([beep, gap, beep])
    return norm(fade(x))


def s_tada():
    """Fanfara: due colpi di rullante e l'accordo maggiore che apre. La cosa che
    parte quando qualcosa e' andato bene."""
    parts = []
    for f in (523.25, 659.26):
        n = int(0.11 * RATE)
        v = saw(np.full(n, f)) + 0.5 * saw(np.full(n, f * 2))
        v *= env(n, attack=0.006, decay=0.03, sustain=0.7, release=0.05)
        parts.append(v)
    n = int(0.75 * RATE)
    chord = np.zeros(n)
    for f in (523.25, 659.26, 783.99, 1046.50):
        vib = 1.0 + 0.006 * np.sin(phase(np.full(n, 5.0)))
        chord += saw(np.full(n, f) * vib)
    chord *= env(n, attack=0.01, decay=0.12, sustain=0.75, release=0.45)
    parts.append(chord)
    x = np.concatenate(parts)
    x = lowpass(x, 6000.0)
    return norm(fade(x))


def s_rullo():
    """Rullo e piatto: il «ba-dum-tss» che chiude una battuta scadente."""
    def snare(dur, seed):
        m = int(dur * RATE)
        body = np.sin(phase(np.linspace(220.0, 150.0, m))) * 0.6
        sn = highpass(noise(m, seed=seed), 1400.0)
        y = body + sn * 0.9
        y *= np.exp(-np.arange(m) / (RATE * 0.045))
        return y

    def cymbal(dur, seed):
        m = int(dur * RATE)
        y = highpass(noise(m, seed=seed), 5200.0)
        y *= np.exp(-np.arange(m) / (RATE * 0.22))
        return y

    x = np.concatenate([
        snare(0.13, 3), np.zeros(int(0.02 * RATE)),
        snare(0.13, 4), np.zeros(int(0.02 * RATE)),
        cymbal(0.75, 5),
    ])
    return norm(fade(x))


def s_scratch():
    """Il disco graffiato: la puntina che torna indietro. Rumore rosa modulato in
    altezza avanti e indietro, che e' letteralmente quello che succede."""
    d = 0.55
    n = int(d * RATE)
    base = lowpass(noise(n * 3, seed=21), 2600.0)
    # Lettura avanti-indietro dentro il rumore: la velocita' e' il gesto.
    speed = 1.0 + 1.6 * np.sin(phase(np.full(n, 3.2)))
    pos = np.cumsum(np.abs(speed)) * 2.0
    pos = np.clip(pos, 0, len(base) - 2).astype(int)
    x = base[pos] * (0.4 + 0.6 * np.abs(speed) / 2.6)
    x = bandpass(x, 900.0, 0.9) * 1.5 + x * 0.4
    x *= env(n, attack=0.01, decay=0.0, sustain=1.0, release=0.12)
    return norm(fade(x))


# Tredici suoni, uno per tasto nota, nell'ordine in cui stanno sulla tastiera.
# La descrizione e' quella che il display scrive sotto il nome.
SOUNDS = [
    ("TROMBETTA", "da stadio", s_trombetta),
    ("FAAA", "con l'eco", s_faaa),
    ("BOOM", "il colpo grave", s_boom),
    ("BRUH", "niente da aggiungere", s_bruh),
    ("TROMBONE", "che tristezza", s_trombone),
    ("RISATA", "quattro sillabe", s_risata),
    ("APPLAUSO", "bravo", s_applauso),
    ("RULLO", "ba dum tss", s_rullo),
    ("MONETA", "raccolta", s_moneta),
    ("LASER", "pew", s_laser),
    ("ERRORE", "risposta sbagliata", s_errore),
    ("SCRATCH", "ferma tutto", s_scratch),
    ("TADA", "e' andata bene", s_tada),
]


def main():
    user = load_user()
    if user:
        print("  (%d suoni presi da tools/samples/)\n" % len(user))

    blobs = []
    total = 0
    for slot, (name, hint, fn) in enumerate(SOUNDS):
        if slot in user:
            name, hint, x = user[slot]
        else:
            x = fn()
        # 8 bit senza segno: 128 e' lo zero. Il tondeggiamento e' quello vero,
        # non un taglio: mezzo bit di differenza su un suono cosi' non si sente,
        # ma un troncamento sistematico sposterebbe lo zero e farebbe un ronzio.
        q = np.clip(np.round(x * 127.0) + 128.0, 0, 255).astype(np.uint8)
        blobs.append((name, hint, q))
        total += len(q)
        print("  %-10s %6.2f s  %6d byte" % (name, len(q) / RATE, len(q)))

    lines = []
    lines.append('// samples.cpp — i tredici suoni della schermata SUONI.')
    lines.append('//')
    lines.append('// GENERATO da tools/make_samples.py: non si modifica a mano.')
    lines.append('//')
    lines.append('// Non sono registrazioni: sono sintetizzati, perche' + "'" +
                 ' i suoni "meme" che')
    lines.append('// girano in rete hanno quasi tutti un padrone e questo repository e' + "'" +
                 ' pubblico.')
    lines.append('// Ognuno e' + "'" + ' costruito da cio' + "'" +
                 ' che lo rende riconoscibile — una trombetta')
    lines.append('// sono tre lame scordate che calano, un boom e' + "'" +
                 ' una sinusoide che scende')
    lines.append('// sotto i quaranta hertz — e in flash costa il tempo che dura.')
    lines.append('//')
    lines.append('// 8 bit senza segno a %d Hz, 128 = silenzio. Totale: %d byte.'
                 % (RATE, total))
    lines.append('#include "samples.h"')
    lines.append('')

    for name, hint, q in blobs:
        var = "SMP_" + name.replace("'", "").replace(" ", "_")
        lines.append('static const uint8_t %s[] = {' % var)
        row = []
        for i, b in enumerate(q):
            row.append("%d," % b)
            if len(row) == 24:
                lines.append('    ' + ''.join(row))
                row = []
        if row:
            lines.append('    ' + ''.join(row))
        lines.append('};')
        lines.append('')

    lines.append('const MemeSample MEME_SAMPLES[] = {')
    for name, hint, q in blobs:
        var = "SMP_" + name.replace("'", "").replace(" ", "_")
        lines.append('    {"%s", "%s", %s, sizeof(%s)},' % (name, hint, var, var))
    lines.append('};')
    lines.append('')
    lines.append('const uint8_t MEME_COUNT = sizeof(MEME_SAMPLES) / sizeof(MEME_SAMPLES[0]);')
    lines.append('')

    with open(OUT, "w") as f:
        f.write("\n".join(lines))
    print("\n%d suoni, %d byte in flash -> %s" % (len(blobs), total, OUT))


if __name__ == "__main__":
    sys.exit(main())
