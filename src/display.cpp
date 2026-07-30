// display.cpp — GC9A01 240x240 IPS su SPI hardware (Arduino_GFX).
//
// Le schermate cicliche sono 6 (pulsante GPIO 18). Quando l'ADSR EDIT MODE e'
// attivo si passa automaticamente a una schermata dedicata, che bypassa il
// ciclo; all'uscita si torna esattamente alla schermata di prima.
//
// Per evitare flicker si ridisegna la parte statica solo al cambio schermata, e i
// valori dinamici solo quando cambiano davvero. VU e SCOPE fanno eccezione: il
// loro contenuto cambia ad ogni fotogramma, quindi cancellano e ridisegnano solo
// i pixel che occupavano prima invece di ripulire aree intere.

#include "display.h"

#include <Arduino_GFX_Library.h>
#include <math.h>
#include <qrcode.h>
#include <string.h>

#include "audio_engine.h"
#include "logo.h"
#include "net_portal.h"
#include "pinout.h"
#include "sequencer.h"
#include "version.h"

namespace {

constexpr int CX = 120;  // centro del display tondo
constexpr int CY = 120;

Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;

uint8_t screen = 0;
bool inAdsrScreen = false;
bool inSeqOverride = false;  // STEP EDIT o preconteggio: la SEQ scavalca il ciclo
bool forceFull = true;

SynthView prev;
bool prevValid = false;

// ------------------------------------------------------------------- helper
void textAt(const char *s, int x, int y, uint8_t size, uint16_t color) {
    gfx->setTextSize(size);
    gfx->setTextColor(color);
    gfx->setCursor(x, y);
    gfx->print(s);
}

void textCentered(const char *s, int y, uint8_t size, uint16_t color) {
    int w = 6 * size * (int)strlen(s);
    textAt(s, CX - w / 2, y, size, color);
}

// Cancella una fascia orizzontale (usata prima di riscrivere un valore dinamico).
void clearBand(int y, int h) { gfx->fillRect(8, y, 224, h, BLACK); }

void chrome(const char *title, uint16_t titleColor) {
    gfx->fillScreen(BLACK);
    gfx->drawCircle(CX, CY, 118, DARKGREY);
    gfx->drawCircle(CX, CY, 117, DARKGREY);
    textCentered(title, 30, 2, titleColor);
    gfx->drawFastHLine(60, 52, 120, titleColor);
}

// Valore -1..+1 della forma d'onda alla fase t (0..1). Stessa forma del motore audio.
float iconValue(uint8_t wave, float t) {
    switch (wave) {
        case WAVE_SQUARE:
            return (t < 0.5f) ? 1.0f : -1.0f;
        case WAVE_SAW:
            return 2.0f * t - 1.0f;
        case WAVE_TRIANGLE:
            return (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
        case WAVE_SINE:
        default:
            return sinf(2.0f * (float)M_PI * t);
    }
}

// Icona disegnata con primitive grafiche (nessuna bitmap): 2 cicli, ampiezza 26 px.
void drawWaveIcon(uint8_t wave, int cy, uint16_t color) {
    constexpr int HALF_W = 60;
    constexpr float AMP = 26.0f;
    constexpr int PERIOD = 60;  // px per ciclo -> 2 cicli in 120 px

    int prevX = CX - HALF_W;
    int prevY = cy - (int)(AMP * iconValue(wave, 0.0f));
    for (int dx = 1; dx <= 2 * HALF_W; ++dx) {
        float t = (float)(dx % PERIOD) / (float)PERIOD;
        int x = CX - HALF_W + dx;
        int y = cy - (int)(AMP * iconValue(wave, t));
        gfx->drawLine(prevX, prevY, x, y, color);
        // seconda passata a 1px di offset: tratto piu' spesso, si legge meglio
        gfx->drawLine(prevX, prevY + 1, x, y + 1, color);
        prevX = x;
        prevY = y;
    }
    gfx->drawFastHLine(CX - HALF_W, cy, 2 * HALF_W, DARKGREY);
}

// Barra orizzontale con cornice, riempimento 0..1.
void drawBar(int x, int y, int w, int h, float frac, uint16_t color) {
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    gfx->drawRect(x, y, w, h, DARKGREY);
    int fill = (int)((w - 4) * frac);
    gfx->fillRect(x + 2, y + 2, fill, h - 4, color);
    gfx->fillRect(x + 2 + fill, y + 2, (w - 4) - fill, h - 4, BLACK);
}

// ----------------------------------------------------------------- schermate

void drawWaveScreen(const SynthView &v, bool full) {
    if (full) chrome("WAVE", CYAN);
    if (full || v.waveform != prev.waveform) {
        gfx->fillRect(50, 80, 140, 64, BLACK);
        drawWaveIcon(v.waveform, 112, CYAN);
        clearBand(158, 24);
        textCentered(WAVEFORM_NAMES[v.waveform], 158, 3, WHITE);
    }
    // La schermata della voce e' il posto giusto per dire quante ne suonano.
    if (full || v.poly != prev.poly || v.voices != prev.voices) {
        clearBand(192, 16);
        char buf[20];
        if (v.poly) {
            snprintf(buf, sizeof(buf), "POLI  %d/%d voci", (int)v.voices, MAX_VOICES);
        } else {
            snprintf(buf, sizeof(buf), "MONO");
        }
        textCentered(buf, 192, 1, v.poly ? GREENYELLOW : DARKGREY);
    }
}

void drawOctaveScreen(const SynthView &v, bool full) {
    if (full) chrome("OCTAVE", GREENYELLOW);
    if (full || v.octave != prev.octave) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%+d", (int)v.octave);
        gfx->fillRect(20, 90, 200, 70, BLACK);
        textCentered(buf, 96, 8, (v.octave == 0) ? WHITE : GREENYELLOW);
        clearBand(180, 16);
        textCentered("x2^oct", 180, 2, DARKGREY);
    }
}

void drawLevelsScreen(const SynthView &v, bool full) {
    if (full) {
        chrome("LEVELS", ORANGE);
        textAt("CUTOFF", 45, 78, 2, WHITE);
        textAt("VOLUME", 45, 148, 2, WHITE);
    }
    // cutoff: mappatura log 80..8000 Hz per una barra percettivamente lineare
    float cf = logf(v.cutoffHz / 80.0f) / logf(100.0f);
    if (full || fabsf(v.cutoffHz - prev.cutoffHz) > 5.0f) {
        drawBar(45, 98, 150, 24, cf, ORANGE);
        char buf[16];
        snprintf(buf, sizeof(buf), "%5d Hz", (int)v.cutoffHz);
        gfx->fillRect(45, 126, 150, 14, BLACK);
        textAt(buf, 45, 126, 1, DARKGREY);
    }
    if (full || fabsf(v.volume - prev.volume) > 0.01f) {
        drawBar(45, 168, 150, 24, v.volume, GREEN);
        char buf[16];
        snprintf(buf, sizeof(buf), "%3d %%", (int)(v.volume * 100.0f));
        gfx->fillRect(45, 196, 150, 14, BLACK);
        textAt(buf, 45, 196, 1, DARKGREY);
    }
}

// Iniziali usate dentro le celle: una lettera sola deve bastare, e in notazione
// italiana SOL e SI collidono. La riga di dettaglio sotto la griglia scrive
// comunque il nome per esteso, come sul pannello.
const char NOTE_LETTERS[NOTE_COUNT] = {'C', 'D', 'E', 'F', 'G', 'A', 'B'};
const char *const NOTE_NAMES_IT[NOTE_COUNT] = {"DO", "RE", "MI", "FA", "SOL", "LA", "SI"};

// Le note non sono piu' una potenza di due: l'indice va controllato, non mascherato.
inline bool validNote(int8_t n) { return n >= 0 && n < NOTE_COUNT; }

// Colore della cella in base all'ottava con cui lo step e' stato scritto
// (indice = oct + 2): il registro si legge a colpo d'occhio.
const uint16_t OCT_COLORS[5] = {PURPLE, BLUE, CYAN, GREENYELLOW, ORANGE};

uint16_t octColor(int8_t oct) {
    int i = oct + 2;
    if (i < 0) i = 0;
    if (i > 4) i = 4;
    return OCT_COLORS[i];
}

// Geometria della griglia: 2 righe da 8 celle, centrate e dentro il cerchio.
constexpr int GRID_X0 = 30;
constexpr int GRID_Y0 = 86;
constexpr int GRID_CELL = 20;
constexpr int GRID_GAP = 3;

void gridCellPos(int i, int &x, int &y) {
    x = GRID_X0 + (i % 8) * (GRID_CELL + GRID_GAP);
    y = GRID_Y0 + (i / 8) * (GRID_CELL + GRID_GAP);
}

// Una cella: contenuto dello step, piu' gli indicatori di testina e cursore.
void drawStepCell(const SynthView &v, int i) {
    int x, y;
    gridCellPos(i, x, y);

    const Sequencer::Step &s = Sequencer::stepAt(i);
    const bool playhead = (v.seqMode != Sequencer::SEQ_IDLE) && (i == v.seqStep);
    const bool cursor = v.seqEditing && (i == v.seqCursor);

    if (s.note == SEQ_TIE) {
        // Legato: nessuna nota nuova, solo il proseguimento della precedente.
        gfx->fillRect(x, y, GRID_CELL, GRID_CELL, NAVY);
        gfx->fillRect(x + 3, y + GRID_CELL / 2 - 1, GRID_CELL - 6, 3, LIGHTGREY);
    } else if (!validNote(s.note)) {
        gfx->fillRect(x, y, GRID_CELL, GRID_CELL, NAVY);  // pausa
    } else {
        gfx->fillRect(x, y, GRID_CELL, GRID_CELL, octColor(s.oct));
        gfx->setTextSize(1);
        gfx->setTextColor(BLACK);
        gfx->setCursor(x + 7, y + 6);
        gfx->print(NOTE_LETTERS[s.note]);
    }

    // La testina passa sopra al contenuto senza cancellarlo: cornice spessa.
    if (playhead) {
        uint16_t c = (v.seqMode == Sequencer::SEQ_RECORDING) ? RED : GREEN;
        gfx->drawRect(x, y, GRID_CELL, GRID_CELL, c);
        gfx->drawRect(x + 1, y + 1, GRID_CELL - 2, GRID_CELL - 2, c);
    }
    if (cursor) {
        gfx->drawRect(x, y, GRID_CELL, GRID_CELL, WHITE);
    }
}

// Preconteggio: il pattern gira gia', ma qui conta solo sapere quando si parte.
void drawCountIn(const SynthView &v, bool full) {
    if (full) chrome("COUNT IN", RED);
    if (full || v.countIn != prev.countIn) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", (int)v.countIn);
        gfx->fillRect(60, 80, 120, 90, BLACK);
        textCentered(buf, 84, 10, RED);
        clearBand(184, 16);
        textCentered("SUONA AL VIA", 184, 1, DARKGREY);
    }
}

void drawSeqScreen(const SynthView &v, bool full) {
    if (v.countIn > 0) {
        drawCountIn(v, full || prev.countIn == 0);
        return;
    }
    // Uscendo dal preconteggio la schermata va ricostruita da zero.
    if (prev.countIn > 0) full = true;

    if (full) chrome(v.seqEditing ? "STEP EDIT" : "SEQUENCER", v.seqEditing ? WHITE : MAGENTA);

    const bool modeChanged = full || v.seqMode != prev.seqMode || v.seqEditing != prev.seqEditing;
    if (modeChanged) {
        clearBand(58, 22);
        const char *label = "STOP";
        uint16_t col = DARKGREY;
        if (v.seqMode == Sequencer::SEQ_RECORDING) {
            label = "REC";
            col = RED;
        } else if (v.seqMode == Sequencer::SEQ_PLAYING) {
            label = "PLAY";
            col = GREEN;
        }
        textCentered(label, 58, 2, col);
    }

    const bool patternChanged = full || modeChanged || v.seqRev != prev.seqRev;
    const bool marksMoved = v.seqStep != prev.seqStep || v.seqCursor != prev.seqCursor;

    if (patternChanged) {
        for (int i = 0; i < SEQ_STEPS; ++i) drawStepCell(v, i);
    } else if (marksMoved) {
        // A tempo alto la testina si sposta ogni 60 ms: ridisegnare tutte e 16
        // le celle sprecherebbe SPI. Bastano quelle che ha lasciato e quelle
        // che ha appena preso.
        drawStepCell(v, prev.seqStep);
        drawStepCell(v, prev.seqCursor);
        drawStepCell(v, v.seqStep);
        drawStepCell(v, v.seqCursor);
    }

    // Riga di dettaglio: in editing conta lo step sotto il cursore, altrimenti
    // la posizione nella battuta.
    if (patternChanged || marksMoved) {
        char buf[24];
        if (v.seqEditing) {
            const Sequencer::Step &s = Sequencer::stepAt(v.seqCursor);
            if (s.note == SEQ_TIE) {
                snprintf(buf, sizeof(buf), "%02d  LEGATO", v.seqCursor + 1);
            } else if (!validNote(s.note)) {
                snprintf(buf, sizeof(buf), "%02d  ---", v.seqCursor + 1);
            } else {
                snprintf(buf, sizeof(buf), "%02d  %s %+d", v.seqCursor + 1,
                         NOTE_NAMES_IT[s.note], (int)s.oct);
            }
        } else {
            snprintf(buf, sizeof(buf), "%02d/%d", v.seqStep + 1, SEQ_STEPS);
        }
        clearBand(140, 18);
        textCentered(buf, 140, 2, v.seqEditing ? WHITE : LIGHTGREY);
    }

    if (full || v.hold != prev.hold || v.arp != prev.arp || v.bpm != prev.bpm ||
        v.poly != prev.poly) {
        clearBand(166, 18);
        char buf[24];
        snprintf(buf, sizeof(buf), "BPM %3d", (int)v.bpm);
        textCentered(buf, 166, 2, YELLOW);
        clearBand(190, 16);
        char st[32];
        snprintf(st, sizeof(st), "HOLD %s ARP %s %s", v.hold ? "ON " : "OFF",
                 v.arp ? "ON " : "OFF", v.poly ? "POLI" : "MONO");
        textCentered(st, 190, 1, (v.hold || v.arp) ? CYAN : DARKGREY);
    }
}

void drawAdsrScreen(const SynthView &v, bool full) {
    if (full) chrome("ADSR EDIT", RED);

    struct Row {
        const char *label;
        float frac;
        char value[12];
        uint16_t color;
    } rows[4];

    // frazioni normalizzate sui range dichiarati, per le mini-barre
    rows[0].label = "A";
    rows[0].frac = logf(v.attackMs / 2.0f) / logf(250.0f);
    snprintf(rows[0].value, sizeof(rows[0].value), "%4d ms", (int)v.attackMs);
    rows[0].color = RED;

    rows[1].label = "D";
    rows[1].frac = (v.decayMs - 5.0f) / 995.0f;
    snprintf(rows[1].value, sizeof(rows[1].value), "%4d ms", (int)v.decayMs);
    rows[1].color = ORANGE;

    rows[2].label = "S";
    rows[2].frac = v.sustain;
    snprintf(rows[2].value, sizeof(rows[2].value), "%4d %%", (int)(v.sustain * 100.0f));
    rows[2].color = YELLOW;

    rows[3].label = "R";
    rows[3].frac = logf(v.releaseMs / 10.0f) / logf(200.0f);
    snprintf(rows[3].value, sizeof(rows[3].value), "%4d ms", (int)v.releaseMs);
    rows[3].color = GREEN;

    bool changed[4] = {
        full || fabsf(v.attackMs - prev.attackMs) > 0.5f,
        full || fabsf(v.decayMs - prev.decayMs) > 0.5f,
        full || fabsf(v.sustain - prev.sustain) > 0.005f,
        full || fabsf(v.releaseMs - prev.releaseMs) > 0.5f,
    };

    // Righe strette abbastanza da restare dentro l'area circolare anche in basso.
    const int y0 = 70, dy = 34;
    for (int i = 0; i < 4; ++i) {
        if (!changed[i]) continue;
        int y = y0 + i * dy;
        gfx->fillRect(30, y, 182, 24, BLACK);
        textAt(rows[i].label, 34, y + 4, 2, rows[i].color);
        drawBar(54, y, 104, 22, rows[i].frac, rows[i].color);
        textAt(rows[i].value, 166, y + 7, 1, WHITE);
    }
}

// -------------------------------------------------------------- VU meter
//
// Ago analogico con perno in basso: su un display tondo e' la forma che sfrutta
// meglio lo spazio. La scala e' in dBFS da -40 a 0, lineare sull'angolo.
//
// Geometria pensata perche' niente si sovrapponga, cosi' ogni elemento si puo'
// cancellare ridisegnandosi in nero senza rovinare quello che ha accanto:
//
//   raggio  92        punta dell'ago
//   raggio  93..97    indicatore di picco
//   raggio  98..110   tacche
//   raggio 110        arco della scala
//   raggio 121        numeri della scala
//
constexpr int VU_PX = 120;  // perno
constexpr int VU_PY = 194;
constexpr float VU_SWEEP = 0.9599f;  // 55 gradi per lato, in radianti
constexpr int VU_NEEDLE_R = 92;
constexpr int VU_MARK_IN = 93;
constexpr int VU_MARK_OUT = 97;
constexpr int VU_TICK_R = 98;
constexpr int VU_ARC_R = 110;
constexpr int VU_LABEL_R = 121;
constexpr float VU_DB_MIN = -40.0f;
constexpr float VU_RED_ZONE = 0.8f;  // -8 dBFS: da qui in su si rischia il clip

// Punto sulla corsa dell'ago: pos 0 = fondo scala sinistro, 1 = destro.
void vuPoint(float pos, int r, int &x, int &y) {
    const float a = (pos * 2.0f - 1.0f) * VU_SWEEP;
    x = VU_PX + (int)((float)r * sinf(a));
    y = VU_PY - (int)((float)r * cosf(a));
}

// Ampiezza lineare -> posizione sulla scala in dB.
float vuPos(float lin) {
    if (lin <= 0.0001f) return 0.0f;
    const float db = 20.0f * log10f(lin);
    if (db <= VU_DB_MIN) return 0.0f;
    if (db >= 0.0f) return 1.0f;
    return (db - VU_DB_MIN) / -VU_DB_MIN;
}

// Ago e indicatore di picco: la stessa funzione disegna e cancella (in nero), per
// costruzione tocca esattamente gli stessi pixel.
void vuNeedle(float pos, uint16_t color) {
    int x, y;
    vuPoint(pos, VU_NEEDLE_R, x, y);
    gfx->drawLine(VU_PX, VU_PY, x, y, color);
    gfx->drawLine(VU_PX - 1, VU_PY, x - 1, y, color);
    gfx->drawLine(VU_PX + 1, VU_PY, x + 1, y, color);
}

void vuMark(float pos, uint16_t color) {
    int x0, y0, x1, y1;
    vuPoint(pos, VU_MARK_IN, x0, y0);
    vuPoint(pos, VU_MARK_OUT, x1, y1);
    gfx->drawLine(x0, y0, x1, y1, color);
    gfx->drawLine(x0 + 1, y0, x1 + 1, y1, color);
}

void vuScale() {
    gfx->startWrite();
    int px, py;
    vuPoint(0.0f, VU_ARC_R, px, py);
    for (int i = 1; i <= 44; ++i) {
        const float p = (float)i / 44.0f;
        int x, y;
        vuPoint(p, VU_ARC_R, x, y);
        const uint16_t c = (p > VU_RED_ZONE) ? RED : GREEN;
        gfx->writeLine(px, py, x, y, c);
        gfx->writeLine(px, py - 1, x, y - 1, c);
        px = x;
        py = y;
    }
    gfx->endWrite();

    for (int i = 0; i <= 4; ++i) {
        const float p = (float)i / 4.0f;
        const uint16_t c = (p > VU_RED_ZONE) ? RED : LIGHTGREY;
        int x0, y0, x1, y1;
        vuPoint(p, VU_TICK_R, x0, y0);
        vuPoint(p, VU_ARC_R, x1, y1);
        gfx->drawLine(x0, y0, x1, y1, c);

        char lbl[6];
        snprintf(lbl, sizeof(lbl), "%d", (int)(VU_DB_MIN * (1.0f - p)));
        int lx, ly;
        vuPoint(p, VU_LABEL_R, lx, ly);
        textAt(lbl, lx - 3 * (int)strlen(lbl), ly - 4, 1, c);
    }
}

void drawVuScreen(const SynthView &, bool full) {
    static float lastNeedle = -1.0f;
    static float lastMark = -1.0f;
    static float peakHold = 0.0f;
    static uint32_t peakHoldAt = 0;
    static bool lastClip = false;
    static char lastRms[16] = "";
    static char lastPk[20] = "";

    if (full) {
        chrome("VU METER", GREEN);
        textAt("dBFS", 56, 58, 1, DARKGREY);
        textAt("CLIP", 146, 58, 1, DARKGREY);
        vuScale();
        gfx->fillCircle(VU_PX, VU_PY, 5, LIGHTGREY);
        lastNeedle = -1.0f;
        lastMark = -1.0f;
        peakHold = 0.0f;
        lastClip = true;  // forza il primo disegno della spia
        lastRms[0] = '\0';
        lastPk[0] = '\0';
        // Il picco si e' accumulato per tutto il tempo in cui la schermata non
        // era a video: si butta, altrimenti si entrerebbe sempre con la spia
        // di clip accesa.
        AudioEngine::peakLevel();
    }

    const float rms = AudioEngine::rmsLevel();
    // Lettura distruttiva: e' il picco degli ultimi 33 ms, non l'istante attuale.
    const float peak = AudioEngine::peakLevel();
    const float pos = vuPos(rms);

    // Il picco resta appeso mezzo secondo e poi ricade: e' l'unico modo di vedere
    // transienti che l'ago, per come e' smorzato, non fa in tempo a seguire.
    const uint32_t now = millis();
    const float ppos = vuPos(peak);
    if (ppos >= peakHold) {
        peakHold = ppos;
        peakHoldAt = now;
    } else if (now - peakHoldAt > 600) {
        peakHold -= 0.03f;
        if (peakHold < pos) peakHold = pos;
    }

    if (fabsf(pos - lastNeedle) > 0.002f) {
        if (lastNeedle >= 0.0f) vuNeedle(lastNeedle, BLACK);
        vuNeedle(pos, WHITE);
        gfx->fillCircle(VU_PX, VU_PY, 5, LIGHTGREY);  // il perno copre la base
        lastNeedle = pos;
    }
    if (fabsf(peakHold - lastMark) > 0.002f) {
        if (lastMark >= 0.0f) vuMark(lastMark, BLACK);
        vuMark(peakHold, (peakHold > VU_RED_ZONE) ? RED : YELLOW);
        lastMark = peakHold;
    }

    const bool clip = (peak >= 0.999f);
    if (clip != lastClip) {
        gfx->fillCircle(182, 62, 5, clip ? RED : DARKGREY);
        lastClip = clip;
    }

    char buf[16];
    if (rms > 0.0005f) {
        snprintf(buf, sizeof(buf), "%.1f dB", 20.0f * log10f(rms));
    } else {
        snprintf(buf, sizeof(buf), "-inf dB");
    }
    if (strcmp(buf, lastRms) != 0) {
        gfx->fillRect(50, 202, 140, 16, BLACK);
        textCentered(buf, 202, 2, WHITE);
        strncpy(lastRms, buf, sizeof(lastRms) - 1);
    }

    char pk[20];
    if (peakHold > 0.0f) {
        snprintf(pk, sizeof(pk), "pk %.0f dB", VU_DB_MIN * (1.0f - peakHold));
    } else {
        snprintf(pk, sizeof(pk), "pk --");
    }
    if (strcmp(pk, lastPk) != 0) {
        gfx->fillRect(72, 222, 96, 8, BLACK);
        textCentered(pk, 222, 1, DARKGREY);
        strncpy(lastPk, pk, sizeof(lastPk) - 1);
    }
}

// ---------------------------------------------------------- oscilloscopio
//
// Una colonna di pixel per campione: la finestra catturata dal motore audio e'
// lunga esattamente quanto il riquadro e' largo. Ogni fotogramma si cancella solo
// il segmento verticale che la traccia precedente occupava in quella colonna, si
// rimette la griglia dove passava, e si disegna il segmento nuovo: nessuna
// pulizia a tutto riquadro, quindi nessun lampeggio.
constexpr int SC_X0 = 30;
constexpr int SC_W = SCOPE_SAMPLES;  // 180
constexpr int SC_CY = 130;
constexpr int SC_H2 = 50;
constexpr uint16_t SC_GRID = 0x2104;  // grigio quasi nero
const int SC_GRID_Y[4] = {SC_CY - SC_H2, SC_CY - SC_H2 / 2, SC_CY + SC_H2 / 2, SC_CY + SC_H2};

void scopeGrid() {
    for (int g = 0; g < 4; ++g) gfx->drawFastHLine(SC_X0, SC_GRID_Y[g], SC_W, SC_GRID);
    // asse dello zero tratteggiato: si distingue dalle altre a colpo d'occhio
    for (int i = 0; i < SC_W; i += 6) gfx->drawFastHLine(SC_X0 + i, SC_CY, 3, DARKGREY);
}

inline bool scopeIsDash(int i) { return (i % 6) < 3; }

void drawScopeScreen(const SynthView &v, bool full) {
    static int8_t samples[SCOPE_SAMPLES];
    static int16_t colTop[SCOPE_SAMPLES];
    static uint8_t colH[SCOPE_SAMPLES];
    static bool traced = false;
    static float zoom = 1.0f;
    static char lastInfo[28] = "";

    if (full) {
        chrome("SCOPE", CYAN);
        scopeGrid();
        traced = false;
        zoom = 1.0f;
        lastInfo[0] = '\0';
    }

    // Finestra nuova o niente: senza aggancio fresco si tiene a video l'ultima.
    if (!AudioEngine::copyScope(samples)) return;

    int peak = 1;
    for (int i = 0; i < SCOPE_SAMPLES; ++i) {
        const int a = (samples[i] < 0) ? -samples[i] : samples[i];
        if (a > peak) peak = a;
    }
    // Zoom automatico: con il volume a meta' e la compensazione di polifonia la
    // traccia a scala fissa sarebbe una riga quasi piatta. Il fattore insegue il
    // valore giusto lentamente, altrimenti l'onda "respira" ad ogni fotogramma.
    float target = 110.0f / (float)peak;
    if (target > 8.0f) target = 8.0f;
    if (target < 1.0f) target = 1.0f;
    // Entrando nella schermata si parte gia' in scala; da li' in poi si insegue.
    zoom = traced ? (zoom + (target - zoom) * 0.15f) : target;

    const float scale = zoom * ((float)SC_H2 / 127.0f);

    gfx->startWrite();
    int prevY = SC_CY;
    for (int i = 0; i < SCOPE_SAMPLES; ++i) {
        int y = SC_CY - (int)((float)samples[i] * scale);
        if (y < SC_CY - SC_H2) y = SC_CY - SC_H2;
        if (y > SC_CY + SC_H2) y = SC_CY + SC_H2;
        if (i == 0) prevY = y;

        const int x = SC_X0 + i;

        if (traced) {
            gfx->writeFastVLine(x, colTop[i], colH[i], BLACK);
            // La cancellazione porta via anche la griglia: si rimette solo dove
            // passava davvero, un pixel alla volta.
            for (int g = 0; g < 4; ++g) {
                if (SC_GRID_Y[g] >= colTop[i] && SC_GRID_Y[g] < colTop[i] + colH[i]) {
                    gfx->writePixel(x, SC_GRID_Y[g], SC_GRID);
                }
            }
            if (scopeIsDash(i) && SC_CY >= colTop[i] && SC_CY < colTop[i] + colH[i]) {
                gfx->writePixel(x, SC_CY, DARKGREY);
            }
        }

        // Il segmento copre il salto rispetto alla colonna precedente: sui fronti
        // ripidi (quadra, dente di sega) la traccia resta continua.
        const int top = (y < prevY) ? y : prevY;
        const int h = ((y < prevY) ? (prevY - y) : (y - prevY)) + 1;
        gfx->writeFastVLine(x, top, h, GREENYELLOW);

        colTop[i] = (int16_t)top;
        colH[i] = (uint8_t)h;
        prevY = y;
    }
    gfx->endWrite();
    traced = true;

    char info[28];
    snprintf(info, sizeof(info), "%s  zoom x%.1f", WAVEFORM_NAMES[v.waveform], zoom);
    if (strcmp(info, lastInfo) != 0) {
        gfx->fillRect(20, 192, 200, 8, BLACK);
        textCentered(info, 192, 1, DARKGREY);
        strncpy(lastInfo, info, sizeof(lastInfo) - 1);
    }
}

// ------------------------------------------------------------ schermata rete

// Anticamera: la radio e' ancora spenta. Serve un gesto deliberato per
// accenderla, perche' da li' in poi il synth resta muto fino al riavvio.
void drawNetworkIdleScreen(const SynthView &, bool full) {
    if (!full) return;
    chrome("NETWORK", BLUE);
    textCentered("Aggiornamento", 76, 2, WHITE);
    textCentered("firmware via WiFi", 98, 2, WHITE);
    gfx->drawFastHLine(70, 124, 100, DARKGREY);
    textCentered("tieni premuto", 136, 1, DARKGREY);
    textCentered("SCORRI DISPLAY", 150, 2, CYAN);
    textCentered("per 1 secondo", 174, 1, DARKGREY);
    textCentered("il synth restera' muto", 194, 1, ORANGE);
}

// ---------------------------------------------------------------- QR code
//
// Versione 3 fissa (29x29 moduli) piu' 2 moduli di margine chiaro per lato: con
// 4 px per modulo il quadrato misura 132 px e sta comodamente dentro il cerchio
// del display, lasciando spazio alle scritte sotto.
constexpr uint8_t QR_VERSION = 3;
constexpr int QR_SCALE = 4;
constexpr int QR_QUIET = 2;
constexpr int QR_MODULES = 4 * QR_VERSION + 17;  // 29
// qrcode_getBufferSize() e' una funzione, non una macro: la dimensione la
// ricalcoliamo qui con la stessa formula della libreria, e la verifichiamo a
// runtime prima di scriverci dentro.
constexpr int QR_BUFFER_BYTES = (QR_MODULES * QR_MODULES + 7) / 8;

void drawQr(const char *text, int cy) {
    static uint8_t qrBuffer[QR_BUFFER_BYTES];
    QRCode qr;
    if (qrcode_getBufferSize(QR_VERSION) > sizeof(qrBuffer) ||
        qrcode_initText(&qr, qrBuffer, QR_VERSION, ECC_LOW, text) != 0) {
        textCentered("QR non generabile", cy, 1, RED);
        return;
    }

    const int side = (qr.size + 2 * QR_QUIET) * QR_SCALE;
    const int x0 = CX - side / 2;
    const int y0 = cy - side / 2;

    // Il margine chiaro fa parte del codice: senza, molti lettori rinunciano.
    gfx->fillRect(x0, y0, side, side, WHITE);

    const int mx = x0 + QR_QUIET * QR_SCALE;
    const int my = y0 + QR_QUIET * QR_SCALE;
    for (uint8_t y = 0; y < qr.size; ++y) {
        for (uint8_t x = 0; x < qr.size; ++x) {
            if (qrcode_getModule(&qr, x, y)) {
                gfx->fillRect(mx + x * QR_SCALE, my + y * QR_SCALE, QR_SCALE, QR_SCALE, BLACK);
            }
        }
    }
}

// ------------------------------------------------------------------- avvio
//
// Logo ArcadeVox: sole a fessure, orizzonte in prospettiva e wordmark con
// aberrazione cromatica. La scena e' tutta a primitive tranne le lettere — il
// font 6x8 di Adafruit_GFX non fa un logo — che arrivano da una maschera a 2 bit
// generata da tools/make_logo.py (1,6 kB in flash, vedi logo.h).
//
// Le due citazioni richieste stanno nella struttura, non appiccicate sopra: il
// sole con la griglia in fuga e' l'immaginario da sala giochi anni 80, e la
// linea dell'orizzonte e' una traccia da oscilloscopio che si appiattisce ai
// bordi. Provata anche una versione con joystick e altoparlante come icone
// laterali: a 20 px diventavano scarabocchi, quindi via.

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Interpolazione fra due colori, t = 0..1. Si lavora sui campi a 5/6/5 bit
// direttamente: convertire in 8 bit e tornare indietro perderebbe di piu'.
uint16_t mix565(uint16_t a, uint16_t b, float t) {
    const int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    return (uint16_t)((((int)(ar + (br - ar) * t)) << 11) |
                      (((int)(ag + (bg - ag) * t)) << 5) | ((int)(ab + (bb - ab) * t)));
}

// Colore attenuato a num/den: serve a rendere i livelli intermedi della
// maschera del wordmark, che e' antialiasata a 4 livelli.
uint16_t dim565(uint16_t c, int num, int den) {
    return (uint16_t)(((((c >> 11) & 0x1F) * num / den) << 11) |
                      ((((c >> 5) & 0x3F) * num / den) << 5) | ((c & 0x1F) * num / den));
}

const uint16_t LOGO_MAGENTA = rgb565(255, 32, 140);
const uint16_t LOGO_AMBER = rgb565(255, 196, 64);
const uint16_t LOGO_NEON = rgb565(0, 255, 255);
const uint16_t LOGO_DIMCYAN = rgb565(0, 90, 110);
const uint16_t LOGO_GRID = rgb565(0, 120, 150);
const uint16_t LOGO_ICE = rgb565(150, 240, 255);

constexpr int SUN_CX = 120;
constexpr int SUN_CY = 78;
constexpr int SUN_R = 38;
constexpr int HORIZON = 174;
constexpr int LOGO_X = CX - LOGO_W / 2;  // 18
constexpr int LOGO_Y = 124;

// Ritmo dell'animazione. Sta tutto qui perche' e' l'unica cosa che valga la pena
// ritoccare, e perche' la somma e' tempo vero fra l'accensione e il primo suono:
// allungare l'effetto si paga in attesa. Cosi' come sono, circa 2,3 s di
// animazione piu' la pausa finale. Azzerandoli tutti il logo compare e basta.
constexpr uint32_t PACE_STAR_MS = 12;      // ogni 3 stelle accese
constexpr uint32_t PACE_SUN_MS = 5;        // ogni 2 righe del disco
constexpr uint32_t PACE_SLIT_MS = 55;      // per fessura (6 in tutto)
constexpr uint32_t PACE_GRID_H_MS = 40;    // per orizzontale (6)
constexpr uint32_t PACE_GRID_V_MS = 22;    // per coppia di verticali (12)
constexpr uint32_t PACE_TRACE_US = 3000;   // ogni 2 colonne della traccia
constexpr uint32_t PACE_GHOST_MS = 220;    // pausa sulle sole eco cromatiche
constexpr uint32_t PACE_BAND_MS = 45;      // per banda di lettere (8)
constexpr uint32_t PACE_HOLD_MS = 400;     // logo fermo prima di cedere il display

// Cielo stellato. Le posizioni vengono da un generatore lineare con seme fisso:
// stessa scena ad ogni accensione, senza una tabella di coordinate in flash.
void logoStars() {
    uint32_t seed = 0x1234;
    int lit = 0;
    for (int i = 0; i < 64; ++i) {
        seed = seed * 1103515245u + 12345u;
        const int x = 16 + (int)((seed >> 8) % 208u);
        seed = seed * 1103515245u + 12345u;
        const int y = 16 + (int)((seed >> 8) % (uint32_t)(HORIZON - 30));

        const int dx = x - CX, dy = y - CY;
        if (dx * dx + dy * dy > 112 * 112) continue;  // fuori dal vetro
        const int sx = x - SUN_CX, sy = y - SUN_CY;
        if (sx * sx + sy * sy < (SUN_R + 6) * (SUN_R + 6)) continue;  // dentro al sole
        if (y >= LOGO_Y - 4 && y <= LOGO_Y + LOGO_H + 2 && x >= LOGO_X - 4 &&
            x <= LOGO_X + LOGO_W + 4) {
            continue;  // dietro alle lettere non si vedrebbero comunque
        }
        seed = seed * 1103515245u + 12345u;
        gfx->drawPixel(x, y, ((seed >> 16) & 3) ? LOGO_DIMCYAN : WHITE);
        if (++lit % 3 == 0) delay(PACE_STAR_MS);  // il cielo si accende un po' per volta
    }
}

// Sole in gradiente magenta -> ambra, poi le fessure orizzontali che si allargano
// verso il basso: e' quello che rende l'immagine riconoscibile al primo colpo.
void logoSun() {
    // Il disco cala dall'alto riga per riga invece di comparire tutto insieme:
    // e' la fase che regge l'attenzione mentre il resto della scena e' ancora
    // vuoto.
    for (int dy = -SUN_R; dy <= SUN_R; ++dy) {
        const int half = (int)sqrtf((float)(SUN_R * SUN_R - dy * dy));
        const float t = (float)(dy + SUN_R) / (float)(2 * SUN_R);
        gfx->drawFastHLine(SUN_CX - half, SUN_CY + dy, 2 * half + 1,
                           mix565(LOGO_MAGENTA, LOGO_AMBER, t));
        if ((dy & 1) == 0) delay(PACE_SUN_MS);
    }

    int y = SUN_CY - 4;
    int h = 1;
    while (y < SUN_CY + SUN_R) {
        gfx->startWrite();
        for (int k = 0; k < h; ++k) {
            const int dy = y + k - SUN_CY;
            if (dy < -SUN_R || dy > SUN_R) continue;
            const int half = (int)sqrtf((float)(SUN_R * SUN_R - dy * dy));
            gfx->writeFastHLine(SUN_CX - half, y + k, 2 * half + 1, BLACK);
        }
        gfx->endWrite();
        y += h + 5;
        ++h;
        delay(PACE_SLIT_MS);  // le fessure scendono una alla volta: il sole "tramonta"
    }
}

// Griglia in fuga verso il punto di orizzonte. Le orizzontali si diradano
// geometricamente scendendo, che e' quello che da' la profondita'.
void logoGrid() {
    int d = 2, step = 3;
    while (HORIZON + d < 236) {
        gfx->drawFastHLine(0, HORIZON + d, 240, (d < 18) ? dim565(LOGO_GRID, 3, 5) : LOGO_GRID);
        d += step;
        step = step * 3 / 2 + 1;
        delay(PACE_GRID_H_MS);
    }
    // Le verticali si aprono a ventaglio dal centro verso i lati: comparendo
    // tutte insieme, com'erano prima, la fase non si vedeva nemmeno.
    for (int k = 0; k <= 11; ++k) {
        gfx->drawLine(CX, HORIZON, CX + k * 30, 238, LOGO_GRID);
        if (k > 0) gfx->drawLine(CX, HORIZON, CX - k * 30, 238, LOGO_GRID);
        delay(PACE_GRID_V_MS);
    }
}

// La linea dell'orizzonte e' una traccia da oscilloscopio: oscilla al centro e
// si spegne verso i bordi, dove torna orizzonte. Disegnata dopo la griglia, cosi'
// le passa sopra.
void logoTrace() {
    int px = 0, py = 0;
    for (int x = 12; x < 229; ++x) {
        const float e = expf(-((float)(x - CX) / 58.0f) * ((float)(x - CX) / 58.0f));
        const int y =
            HORIZON - (int)lroundf(11.0f * e * sinf(2.0f * (float)M_PI * (float)(x - 12) / 46.0f));
        if (x > 12) {
            gfx->drawLine(px, py, x, y, LOGO_NEON);
            gfx->drawLine(px, py + 1, x, y + 1, dim565(LOGO_NEON, 2, 5));  // alone
        }
        px = x;
        py = y;
        if ((x & 1) == 0) delayMicroseconds(PACE_TRACE_US);
    }
}

// Una passata della maschera del wordmark, limitata alle righe [row0, row1).
// `minLevel` a 2 tiene solo i pixel ben coperti: e' quello che serve alle due
// eco cromatiche, che devono restare nette e non sbavare.
void logoWordmark(int ox, int oy, uint16_t top, uint16_t bottom, uint8_t minLevel, int row0,
                  int row1) {
    gfx->startWrite();
    for (int y = row0; y < row1; ++y) {
        const uint8_t *row = LOGO_MASK + y * LOGO_STRIDE;
        const uint16_t base = mix565(top, bottom, (float)y / (float)(LOGO_H - 1));
        for (int x = 0; x < LOGO_W; ++x) {
            const uint8_t level = (uint8_t)((pgm_read_byte(row + (x >> 2)) >> (6 - 2 * (x & 3))) & 3);
            if (level < minLevel) continue;
            gfx->writePixel(LOGO_X + x + ox, LOGO_Y + y + oy,
                            (level == 3) ? base : dim565(base, level, 3));
        }
    }
    gfx->endWrite();
}

// Schermata di accensione. Poco piu' di un secondo: il tempo di leggere la
// versione, che serve sapere prima di collegarsi al portale degli aggiornamenti.
void splash() {
    gfx->fillScreen(BLACK);
    gfx->drawCircle(CX, CY, 118, LOGO_NEON);
    gfx->drawCircle(CX, CY, 116, LOGO_DIMCYAN);
    logoStars();
    textCentered("v" FW_VERSION, 22, 1, LOGO_DIMCYAN);

    logoSun();
    logoGrid();
    logoTrace();

    // Prima le due eco laterali, poi il corpo bianco che scende riga per riga: le
    // lettere sembrano mettersi a fuoco. L'ordine conta, il corpo deve coprire le
    // eco e non il contrario.
    logoWordmark(-3, 2, LOGO_MAGENTA, LOGO_MAGENTA, 2, 0, LOGO_H);
    logoWordmark(3, 2, LOGO_NEON, LOGO_NEON, 2, 0, LOGO_H);
    delay(PACE_GHOST_MS);  // le eco da sole restano in vista: e' meta' dell'effetto
    for (int band = 0; band < LOGO_H; band += 4) {
        logoWordmark(0, 0, WHITE, LOGO_ICE, 1, band, band + 4);
        delay(PACE_BAND_MS);
    }

    delay(PACE_HOLD_MS);
}

}  // namespace

namespace Display {

void begin() {
    bus = new Arduino_ESP32SPI(PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_SCLK, PIN_TFT_MOSI,
                               GFX_NOT_DEFINED /* MISO non usato */);
    gfx = new Arduino_GC9A01(bus, PIN_TFT_RST, 0 /* rotation */, true /* IPS */);

    gfx->begin(40000000);
    splash();

    forceFull = true;
    prevValid = false;
}

void nextScreen() {
    screen = (screen + 1) % SCREEN_COUNT;
    forceFull = true;
}

uint8_t currentScreen() { return screen; }

void update(const SynthView &v) {
    if (!gfx) return;

    // Le due modalita' di edit scavalcano il ciclo delle schermate: mentre sono
    // attive si guarda per forza quella che serve, e all'uscita si torna
    // esattamente dov'eravamo.
    if (v.adsrEdit != inAdsrScreen) {
        inAdsrScreen = v.adsrEdit;
        forceFull = true;
    }
    // Durante preconteggio, registrazione e step edit si guarda per forza la
    // griglia: sono i momenti in cui serve vedere cosa sta finendo nel pattern.
    const bool seqOverride =
        v.seqEditing || v.countIn > 0 || v.seqMode == Sequencer::SEQ_RECORDING;
    if (seqOverride != inSeqOverride) {
        inSeqOverride = seqOverride;
        forceFull = true;
    }

    bool full = forceFull || !prevValid;
    forceFull = false;

    if (inAdsrScreen) {
        drawAdsrScreen(v, full);
    } else if (inSeqOverride) {
        drawSeqScreen(v, full);
    } else {
        switch (screen) {
            case 0: drawWaveScreen(v, full); break;
            case 1: drawOctaveScreen(v, full); break;
            case 2: drawLevelsScreen(v, full); break;
            case 3: drawSeqScreen(v, full); break;
            case SCREEN_VU: drawVuScreen(v, full); break;
            case SCREEN_SCOPE: drawScopeScreen(v, full); break;
            default: drawNetworkIdleScreen(v, full); break;
        }
    }

    prev = v;
    prevValid = true;
}

// ------------------------------------------------------------ modalita' rete

void updateNetwork() {
    if (!gfx) return;

    // Ridisegnare un QR da 841 moduli costa: si rifà solo quando cambia
    // davvero qualcosa.
    static NetPortal::Stage lastStage = NetPortal::NET_OFF;
    static char lastQr[96] = "";
    static char lastMsg[64] = "";
    static bool everDrawn = false;

    const NetPortal::Stage st = NetPortal::stage();
    const char *qr = NetPortal::qrPayload();
    const char *msg = NetPortal::message();

    if (everDrawn && st == lastStage && strcmp(qr, lastQr) == 0 && strcmp(msg, lastMsg) == 0) {
        return;
    }
    const bool qrChanged = !everDrawn || strcmp(qr, lastQr) != 0;

    lastStage = st;
    strncpy(lastQr, qr, sizeof(lastQr) - 1);
    strncpy(lastMsg, msg, sizeof(lastMsg) - 1);
    everDrawn = true;

    if (st == NetPortal::NET_UPDATING) {
        // Da qui in avanti comanda drawOtaProgress().
        return;
    }
    if (st == NetPortal::NET_FAILED) {
        // Un aggiornamento fallito lascia lo schermo sulla barra di
        // avanzamento: senza questo non si saprebbe mai com'e' andata.
        chrome("FALLITO", RED);
        textCentered(msg, 100, 2, WHITE);
        textCentered("il firmware attuale", 140, 1, DARKGREY);
        textCentered("e' rimasto intatto", 154, 1, DARKGREY);
        textCentered("PLAY per riavviare", 184, 1, CYAN);
        return;
    }

    if (qrChanged) {
        gfx->fillScreen(BLACK);
        drawQr(qr, 96);
    }

    // Tre righe sotto il codice: cosa sta succedendo, e le credenziali scritte
    // in chiaro per chi preferisce digitarle a mano.
    gfx->fillRect(10, 164, 220, 48, BLACK);
    textCentered(msg, 166, 1, CYAN);
    textCentered(NetPortal::ssid(), 180, 1, WHITE);

    char line[40];
    if (NetPortal::staIp()[0] != '\0') {
        snprintf(line, sizeof(line), "rete: %s", NetPortal::staIp());
    } else {
        snprintf(line, sizeof(line), "pass: %s", NetPortal::password());
    }
    textCentered(line, 194, 1, DARKGREY);
}

void drawOtaProgress(int pct) {
    if (!gfx) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    static int lastPct = -1;
    if (pct == lastPct) return;
    // Percentuale che torna indietro = trasferimento nuovo: si riparte da capo.
    if (lastPct < 0 || pct < lastPct) {
        chrome("UPDATE", ORANGE);
        textCentered("non spegnere", 190, 1, RED);
    }
    lastPct = pct;

    drawBar(45, 108, 150, 26, pct / 100.0f, ORANGE);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d %%", pct);
    gfx->fillRect(60, 146, 120, 24, BLACK);
    textCentered(buf, 146, 3, WHITE);
}

}  // namespace Display
