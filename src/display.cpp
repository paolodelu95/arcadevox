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
#include "settings.h"
#include "version.h"

namespace {

constexpr int CX = 120;  // centro del display tondo
constexpr int CY = 120;

Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;

uint8_t screen = 0;
bool inAdsrScreen = false;
bool inSeqOverride = false;  // STEP EDIT o preconteggio: la SEQ scavalca il ciclo
bool inLedLearn = false;     // apprendimento dell'ordine dei LED sotto i tasti
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

// Testo allineato a destra: i valori numerici che cambiano di cifre restano
// ancorati al bordo invece di ballare.
void textRight(const char *s, int xEnd, int y, uint8_t size, uint16_t color) {
    textAt(s, xEnd - 6 * size * (int)strlen(s), y, size, color);
}

// Cancella una fascia orizzontale (usata prima di riscrivere un valore dinamico).
//
// La fascia non puo' essere un rettangolo fisso largo quanto il quadrato. Il
// vetro e' tondo: allontanandosi dal centro il cerchio rientra, e appena si
// superano i 37 px di distanza — sopra y=83 o sotto y=157 — l'anello passa a
// x <= 8, cioe' dentro il fillRect da 8 a 231, che se lo mangia aprendo due
// tagli, uno per fianco. Non sono tagli che si richiudono: la cornice si
// ridisegna solo al cambio schermata, quindi restano li' finche' non si esce.
// Percio' la mezza larghezza si stringe seguendo il cerchio.
//
// Il raggio di riferimento e' 116 e non 118: l'anello e' spesso due pixel (117
// e 118) e sotto ci vuole un pixel di nero, altrimenti la fascia arriva a
// sbavare sulla cornice invece di mangiarsela. Comanda la riga piu' lontana dal
// centro, che e' quella dove il cerchio e' piu' stretto. Il tetto a 112 e' la
// mezza larghezza di sempre: verso il centro non serve allargarsi di piu'.
void clearBand(int y, int h) {
    int top = y - CY, bot = y + h - 1 - CY;
    if (top < 0) top = -top;
    if (bot < 0) bot = -bot;
    const int dy = (top > bot) ? top : bot;
    int half = (dy < 116) ? (int)sqrtf((float)(116 * 116 - dy * dy)) : 0;
    if (half > 112) half = 112;
    gfx->fillRect(CX - half, y, 2 * half, h, BLACK);
}

// ------------------------------------------------------------------- colore
//
// Tutto il display parla la stessa lingua della schermata di avvio: fondo nero,
// struttura in ciano, accenti magenta e ambra. La regola di leggibilita' che
// viene prima dello stile: i *valori* si scrivono chiari e grandi su nero, e il
// colore lo portano etichette e strutture, mai il numero che devi leggere.

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Interpolazione fra due colori, t = 0..1. Si lavora sui campi a 5/6/5 bit
// direttamente: convertire in 8 bit e tornare indietro perderebbe di piu'.
uint16_t mix565(uint16_t a, uint16_t b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    return (uint16_t)((((int)(ar + (br - ar) * t)) << 11) |
                      (((int)(ag + (bg - ag) * t)) << 5) | ((int)(ab + (bb - ab) * t)));
}

// Colore attenuato a num/den, per ricavare tracce e stati spenti da un accento.
uint16_t dim565(uint16_t c, int num, int den) {
    return (uint16_t)(((((c >> 11) & 0x1F) * num / den) << 11) |
                      ((((c >> 5) & 0x3F) * num / den) << 5) | ((c & 0x1F) * num / den));
}

const uint16_t HUD_NEON = rgb565(0, 255, 255);      // struttura, titoli
const uint16_t HUD_MAGENTA = rgb565(255, 32, 140);  // accento caldo
const uint16_t HUD_AMBER = rgb565(255, 196, 64);    // valori in salita
const uint16_t HUD_ICE = rgb565(210, 250, 255);     // testo dei valori
const uint16_t HUD_LIME = rgb565(120, 255, 120);    // stato buono
const uint16_t HUD_RED = rgb565(255, 60, 60);       // allarme
const uint16_t HUD_TRACK = rgb565(0, 46, 58);       // segmenti spenti
const uint16_t HUD_LABEL = rgb565(0, 150, 180);     // etichette piccole

// ------------------------------------------------------------------- chrome
//
// Cornice comune a tutte le schermate: anello, titolo fra parentesi HUD e una
// riga di separazione accesa al centro. Sotto la riga, da y=54, comincia l'area
// dei contenuti.
constexpr int CONTENT_TOP = 54;

void chrome(const char *title, uint16_t accent) {
    gfx->fillScreen(BLACK);
    gfx->drawCircle(CX, CY, 118, dim565(accent, 1, 3));
    gfx->drawCircle(CX, CY, 117, dim565(accent, 1, 6));

    textCentered(title, 22, 2, accent);

    // Parentesi ai lati del titolo. La lunghezza si adatta: con un titolo lungo
    // lo spazio dentro il cerchio finisce, e un trattino che sborda si vedrebbe
    // tagliato dalla cornice tonda. Da nove caratteri in su (SEQUENCER, STEP
    // EDIT) il braccio scenderebbe sotto i 10 px e allora si rinuncia: meglio
    // niente parentesi che due monconi appiccicati al titolo.
    const int tw = 12 * (int)strlen(title);
    const uint16_t bracket = dim565(accent, 2, 3);
    // Sei pixel d'aria fra titolo e parentesi e non otto: qui sotto si vede che
    // di spazio ce n'e' poco, e due pixel restituiti al braccio sono la
    // differenza fra tenere le parentesi su SETTINGS e COUNT IN o perderle.
    const int inner = tw / 2 + 6;
    // Il punto critico e' la cima del trattino verticale, a y=24, cioe' 96 px
    // sopra il centro. Perche' resti dentro l'area dei contenuti (raggio 116,
    // sotto l'anello) la mezza larghezza li' vale al massimo
    // sqrt(116^2 - 96^2) = 65.1. Con il 74 di prima il trattino usciva
    // addirittura dal vetro: la sua punta stava a raggio 121.2 e i tre pixel
    // piu' alti, oltre 119.5, sul pannello tondo non esistono proprio; gli
    // altri quattro finivano appoggiati all'anello.
    const int outer = 65;
    if (outer - inner >= 10) {
        for (int side = 0; side < 2; ++side) {
            const int x0 = (side == 0) ? (CX - outer) : (CX + inner);
            gfx->drawFastHLine(x0, 30, outer - inner, bracket);
            gfx->drawFastVLine((side == 0) ? x0 : (x0 + outer - inner - 1), 24, 7, bracket);
        }
    }

    gfx->drawFastHLine(40, 46, 160, dim565(accent, 1, 4));
    gfx->drawFastHLine(CX - 30, 46, 60, accent);
    gfx->drawFastHLine(CX - 30, 47, 60, dim565(accent, 1, 2));
}

// --------------------------------------------------------------- primitive HUD

// Barra a segmenti. I segmenti spenti restano visibili come traccia: il valore
// si legge come frazione della corsa e non come lunghezza assoluta, che a colpo
// d'occhio e' molto piu' facile da stimare. Il colore scorre da `lo` a `hi`
// lungo la barra, cosi' anche la zona alta si riconosce senza leggere il numero.
void hudBar(int x, int y, int w, int h, float frac, uint16_t lo, uint16_t hi) {
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    constexpr int SEG = 6, GAP = 2;
    const int count = (w + GAP) / (SEG + GAP);
    const int lit = (int)(frac * count + 0.5f);

    for (int i = 0; i < count; ++i) {
        const uint16_t c =
            (i < lit) ? mix565(lo, hi, (count > 1) ? (float)i / (float)(count - 1) : 0.0f)
                      : HUD_TRACK;
        gfx->fillRect(x + i * (SEG + GAP), y, SEG, h, c);
    }
}

// Etichetta piccola su fondo pieno: si usa per gli stati (PLAY, REC, HOLD) dove
// conta riconoscere il colore prima ancora di leggere la parola.
void hudChip(int x, int y, const char *text, uint16_t bg, uint8_t size) {
    const int w = 6 * size * (int)strlen(text) + 10;
    const int h = 8 * size + 6;
    gfx->fillRect(x, y, w, h, bg);
    textAt(text, x + 5, y + 3, size, BLACK);
}

int hudChipWidth(const char *text, uint8_t size) { return 6 * size * (int)strlen(text) + 10; }

void hudChipCentered(int y, const char *text, uint16_t bg, uint8_t size) {
    hudChip(CX - hudChipWidth(text, size) / 2, y, text, bg, size);
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
// Sotto ci passa una griglia appena accennata, la stessa dell'oscilloscopio: le
// due schermate mostrano la stessa cosa e devono somigliarsi.
void drawWaveIcon(uint8_t wave, int cy, uint16_t color) {
    constexpr int HALF_W = 60;
    constexpr float AMP = 26.0f;
    constexpr int PERIOD = 60;  // px per ciclo -> 2 cicli in 120 px

    for (int dy = -26; dy <= 26; dy += 13) {
        if (dy == 0) continue;
        gfx->drawFastHLine(CX - HALF_W, cy + dy, 2 * HALF_W, HUD_TRACK);
    }
    for (int x = CX - HALF_W; x < CX + HALF_W; x += 6) gfx->drawFastHLine(x, cy, 3, HUD_LABEL);

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
}

// Otto caselle, una per voce del pool: quante ne stanno suonando si conta a
// colpo d'occhio, senza leggere un numero.
void drawVoiceSlots(int y, uint8_t voices, bool poly) {
    constexpr int SLOT = 14, GAP = 4;
    constexpr int total = MAX_VOICES * SLOT + (MAX_VOICES - 1) * GAP;
    const int x0 = CX - total / 2;
    for (int i = 0; i < MAX_VOICES; ++i) {
        const int x = x0 + i * (SLOT + GAP);
        if (i < voices) {
            gfx->fillRect(x, y, SLOT, SLOT, poly ? HUD_LIME : HUD_AMBER);
        } else {
            gfx->drawRect(x, y, SLOT, SLOT, HUD_TRACK);
        }
    }
}

// ----------------------------------------------------------------- schermate

void drawWaveScreen(const SynthView &v, bool full) {
    if (full) {
        chrome("OSC", HUD_NEON);
        textCentered("JOY < > CAMBIA ONDA", CONTENT_TOP, 1, HUD_LABEL);
    }
    if (full || v.waveform != prev.waveform) {
        gfx->fillRect(56, 72, 128, 60, BLACK);
        drawWaveIcon(v.waveform, 102, HUD_NEON);
        clearBand(142, 24);
        textCentered(WAVEFORM_NAMES[v.waveform], 142, 3, HUD_ICE);
    }
    // La schermata della voce e' il posto giusto per dire quante ne suonano.
    if (full || v.poly != prev.poly || v.voices != prev.voices) {
        gfx->fillRect(40, 176, 160, 14, BLACK);
        drawVoiceSlots(176, v.voices, v.poly);
        clearBand(198, 8);
        textCentered(v.poly ? "POLIFONICO" : "MONOFONICO", 198, 1,
                     v.poly ? HUD_LIME : HUD_LABEL);
    }
}

void drawOctaveScreen(const SynthView &v, bool full) {
    if (full) {
        chrome("OCTAVE", HUD_MAGENTA);
        textCentered("JOY SU GIU CAMBIA OTTAVA", CONTENT_TOP, 1, HUD_LABEL);
    }
    if (!full && v.octave == prev.octave) return;

    char buf[8];
    snprintf(buf, sizeof(buf), "%+d", (int)v.octave);
    gfx->fillRect(20, 70, 200, 66, BLACK);
    textCentered(buf, 70, 8, (v.octave == 0) ? HUD_ICE : HUD_MAGENTA);

    // Selettore a cinque caselle: si vede subito dove sei nella corsa e quanto
    // margine resta, cosa che il solo numero non dice.
    constexpr int CHIP_W = 26, CHIP_H = 20, CHIP_GAP = 8;
    constexpr int total = 5 * CHIP_W + 4 * CHIP_GAP;
    const int x0 = CX - total / 2;
    for (int i = 0; i < 5; ++i) {
        const int x = x0 + i * (CHIP_W + CHIP_GAP);
        const bool here = (i - 2) == v.octave;
        char lbl[4];
        snprintf(lbl, sizeof(lbl), "%+d", i - 2);
        if (here) {
            gfx->fillRect(x, 148, CHIP_W, CHIP_H, HUD_MAGENTA);
            textAt(lbl, x + 7, 154, 1, BLACK);
        } else {
            // Ripulire prima: la casella accesa un attimo fa era piena, e il solo
            // contorno non basta a portarne via il fondo.
            gfx->fillRect(x, 148, CHIP_W, CHIP_H, BLACK);
            gfx->drawRect(x, 148, CHIP_W, CHIP_H, HUD_TRACK);
            textAt(lbl, x + 7, 154, 1, HUD_LABEL);
        }
    }

    // Il moltiplicatore in chiaro: e' quello che senti, l'esponente e' un modo
    // indiretto di dirlo.
    static const char *const MUL[5] = {"x 0.25", "x 0.50", "x 1.00", "x 2.00", "x 4.00"};
    int idx = v.octave + 2;
    if (idx < 0) idx = 0;
    if (idx > 4) idx = 4;
    clearBand(180, 16);
    textCentered(MUL[idx], 180, 2, HUD_LABEL);
}

// Risposta del passa-basso one-pole disegnata in piccolo: |H| = 1/sqrt(1+(f/fc)^2)
// su asse logaritmico 80 Hz..8 kHz, lo stesso della barra qui sopra. Serve a
// vedere *dove* taglia, non solo a che numero e' arrivata la manopola.
// Risposta del filtro a due poli, quello vero del motore: con la risonanza a
// zero e' la solita discesa dolce, alzandola compare la gobba sul taglio. E'
// l'unico modo di far capire cosa fa la seconda manopola senza suonare.
void drawFilterCurve(int x, int y, int w, int h, float cutoffHz, float resonance) {
    gfx->fillRect(x, y, w, h, BLACK);
    gfx->drawFastHLine(x, y + h - 1, w, HUD_TRACK);

    const float decades = logf(100.0f);
    const int cx = x + (int)((float)w * logf(cutoffHz / 80.0f) / decades);
    if (cx > x && cx < x + w) {
        for (int yy = y; yy < y + h; yy += 3) gfx->drawPixel(cx, yy, dim565(HUD_MAGENTA, 1, 2));
    }

    // Stesso smorzamento usato dal motore audio: 2 senza risonanza, verso zero
    // quando il filtro sta per mettersi a fischiare.
    const float damp = 2.0f - 1.94f * resonance;

    int prevY = y;
    for (int i = 0; i < w; ++i) {
        const float f = 80.0f * expf(decades * (float)i / (float)(w - 1));
        const float r = f / cutoffHz;
        const float den = (1.0f - r * r);
        float mag = 1.0f / sqrtf(den * den + (damp * r) * (damp * r));
        // Il picco puo' andare a venti volte: si comprime, altrimenti la gobba
        // esce dal riquadro e con lei il resto della curva.
        mag = mag / (1.0f + mag * 0.35f);
        int yy = y + h - 1 - (int)(mag * (float)(h - 2));
        if (yy < y) yy = y;
        if (i == 0) prevY = yy;
        const int top = (yy < prevY) ? yy : prevY;
        const int hh = ((yy < prevY) ? (prevY - yy) : (yy - prevY)) + 1;
        gfx->drawFastVLine(x + i, top, hh, HUD_MAGENTA);
        prevY = yy;
    }
}

void drawLevelsScreen(const SynthView &v, bool full) {
    constexpr int BX = 44, BW = 152;
    if (full) {
        chrome("LEVELS", HUD_AMBER);
        textAt("CUTOFF", BX, 54, 1, HUD_LABEL);
        textAt("RISONANZA", BX, 100, 1, HUD_LABEL);
        textAt("VOLUME", BX, 146, 1, HUD_LABEL);
        textCentered("1 CUT  2 RIS  3 VOL", 196, 1, HUD_LABEL);
    }

    // cutoff: mappatura log 80..8000 Hz per una barra percettivamente lineare
    const float cf = logf(v.cutoffHz / 80.0f) / logf(100.0f);
    if (full || fabsf(v.cutoffHz - prev.cutoffHz) > 5.0f) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d Hz", (int)v.cutoffHz);
        gfx->fillRect(BX + 66, 50, BW - 66, 16, BLACK);
        textRight(buf, BX + BW, 50, 2, HUD_ICE);
        hudBar(BX, 68, BW, 20, cf, HUD_NEON, HUD_MAGENTA);
    }

    // La risonanza cambia la forma della curva, non solo un numero: la si
    // ridisegna insieme alla barra, perche' e' li' che si capisce cosa fa.
    if (full || fabsf(v.resonance - prev.resonance) > 0.005f ||
        fabsf(v.cutoffHz - prev.cutoffHz) > 5.0f) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d %%", (int)(v.resonance * 100.0f + 0.5f));
        gfx->fillRect(BX + 66, 96, BW - 66, 16, BLACK);
        textRight(buf, BX + BW, 96, 2, v.resonance > 0.85f ? HUD_RED : HUD_ICE);
        hudBar(BX, 114, BW, 20, v.resonance, HUD_AMBER, HUD_RED);
        drawFilterCurve(BX, 172, BW, 26, v.cutoffHz, v.resonance);
    }

    if (full || fabsf(v.volume - prev.volume) > 0.01f) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d %%", (int)(v.volume * 100.0f + 0.5f));
        gfx->fillRect(BX + 66, 142, BW - 66, 16, BLACK);
        textRight(buf, BX + BW, 142, 2, HUD_ICE);
        hudBar(BX, 160, BW, 10, v.volume, HUD_LIME, HUD_RED);
    }
}

// --------------------------------------------------------- schermata EFFETTI
//
// Sette parametri che prima non esistevano, tutti sul quarto encoder: la
// schermata serve soprattutto a ricordare quale sta comandando adesso, ed e'
// per questo che la riga selezionata e' l'unica scritta in chiaro.
void drawFxRow(int y, const char *label, float frac, const char *value, bool selected,
               uint16_t lo, uint16_t hi) {
    gfx->fillRect(38, y - 1, 164, 19, BLACK);
    if (selected) gfx->fillRect(34, y, 3, 16, HUD_MAGENTA);
    textAt(label, 42, y + 1, 1, selected ? HUD_ICE : HUD_LABEL);
    hudBar(100, y + 1, 58, 8, frac, lo, hi);
    textRight(value, 200, y + 1, 1, selected ? HUD_AMBER : HUD_LABEL);
}

void drawFxScreen(const SynthView &v, bool full) {
    if (full) chrome("FX", HUD_MAGENTA);

    // Il riquadro dell'8 BIT sta in cima e cambia colore: e' l'effetto che si
    // accende e si spegne con un tasto, e deve vedersi da lontano.
    if (full || v.crush != prev.crush || v.crushName != prev.crushName) {
        gfx->fillRect(40, 44, 160, 24, BLACK);
        hudChipCentered(46, v.crush ? v.crushName : "8 BIT OFF",
                        v.crush ? rgb565(255, 120, 0) : HUD_TRACK, 2);
    }

    // Riga i-esima -> valore di Enc4Target che la evidenzia. La riga dell'LFO ne
    // ha due (velocita' e profondita'), perche' e' l'unica con due manopole che
    // guardano la stessa barra.
    struct FxRow {
        int y;
        const char *label;
        uint8_t e4a;
        uint8_t e4b;
        uint16_t lo;
        uint16_t hi;
    };
    static const FxRow ROWS[7] = {
        {74, "ECO", 1, 1, HUD_NEON, HUD_MAGENTA},
        {92, "TEMPO", 2, 2, HUD_NEON, HUD_MAGENTA},
        {110, "LFO", 3, 4, HUD_LIME, HUD_AMBER},
        {128, "DRIVE", 5, 5, HUD_AMBER, HUD_RED},
        {146, "SUB", 6, 6, HUD_NEON, HUD_LIME},
        {164, "DETUNE", 7, 7, HUD_NEON, HUD_LIME},
        {182, "GLIDE", 8, 8, HUD_NEON, HUD_LIME},
    };

    const float frac[7] = {
        v.delayMix, (v.delayMs - 20.0f) / 375.0f, v.lfoDepth, v.drive,
        v.subLevel, v.detuneCents / 50.0f,        v.glideMs / 500.0f,
    };
    const float fracPrev[7] = {
        prev.delayMix, (prev.delayMs - 20.0f) / 375.0f, prev.lfoDepth, prev.drive,
        prev.subLevel, prev.detuneCents / 50.0f,        prev.glideMs / 500.0f,
    };

    char buf[16];
    for (int i = 0; i < 7; ++i) {
        const bool sel = (v.enc4Index == ROWS[i].e4a) || (v.enc4Index == ROWS[i].e4b);
        const bool wasSel = (prev.enc4Index == ROWS[i].e4a) || (prev.enc4Index == ROWS[i].e4b);
        if (!full && sel == wasSel && fabsf(frac[i] - fracPrev[i]) < 0.005f &&
            !(i == 2 && v.lfoTargetName != prev.lfoTargetName)) {
            continue;
        }
        switch (i) {
            case 0:
            case 3:
            case 4:
                snprintf(buf, sizeof(buf), "%d%%", (int)(frac[i] * 100.0f + 0.5f));
                break;
            case 1:
                snprintf(buf, sizeof(buf), "%dms", (int)v.delayMs);
                break;
            case 2:
                snprintf(buf, sizeof(buf), "%s", v.lfoTargetName ? v.lfoTargetName : "");
                break;
            case 5:
                snprintf(buf, sizeof(buf), "%dc", (int)v.detuneCents);
                break;
            default:
                if (v.glideMs <= 0.0f) {
                    snprintf(buf, sizeof(buf), "OFF");
                } else {
                    snprintf(buf, sizeof(buf), "%dms", (int)v.glideMs);
                }
                break;
        }
        drawFxRow(ROWS[i].y, ROWS[i].label, frac[i], buf, sel, ROWS[i].lo, ROWS[i].hi);
    }
}

// Iniziali usate dentro le celle: una lettera sola deve bastare, e in notazione
// italiana SOL e SI collidono. La riga di dettaglio sotto la griglia scrive
// comunque il nome per esteso, come sul pannello.
// Con tredici tasti le alterazioni entrano in scena: nella cella ci sta una
// lettera sola, quindi i tasti neri prendono la minuscola della nota che
// alterano (c = DO#, d = RE#...). Si distingue a colpo d'occhio senza aggiungere
// un secondo carattere che non ci starebbe.
const char NOTE_LETTERS[NOTE_COUNT] = {'C', 'c', 'D', 'd', 'E', 'F', 'f',
                                       'G', 'g', 'A', 'a', 'B', 'C'};
const char *const NOTE_NAMES_IT[NOTE_COUNT] = {"DO",  "DO#", "RE",  "RE#", "MI",
                                               "FA",  "FA#", "SOL", "SOL#", "LA",
                                               "LA#", "SI",  "DO'"};

// Le note non sono piu' una potenza di due: l'indice va controllato, non mascherato.
inline bool validNote(int8_t n) { return n >= 0 && n < NOTE_COUNT; }

// Colore della cella in base all'ottava con cui lo step e' stato scritto
// (indice = oct + 2): il registro si legge a colpo d'occhio. Sono tutti toni
// abbastanza chiari da reggere l'iniziale nera scritta sopra — con l'indaco e il
// viola di prima, in basso, la lettera spariva.
const uint16_t OCT_COLORS[5] = {rgb565(150, 110, 255), rgb565(80, 170, 255), rgb565(0, 230, 230),
                                rgb565(140, 240, 120), rgb565(255, 190, 70)};

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
        gfx->fillRect(x, y, GRID_CELL, GRID_CELL, BLACK);
        gfx->drawRect(x, y, GRID_CELL, GRID_CELL, HUD_TRACK);
        gfx->fillRect(x + 3, y + GRID_CELL / 2 - 1, GRID_CELL - 6, 3, HUD_ICE);
    } else if (!validNote(s.note)) {
        // Pausa: casella vuota con il solo contorno. Lasciandola piena, com'era
        // prima, la griglia sembrava scritta anche dove non c'era niente.
        gfx->fillRect(x, y, GRID_CELL, GRID_CELL, BLACK);
        gfx->drawRect(x, y, GRID_CELL, GRID_CELL, HUD_TRACK);
    } else {
        gfx->fillRect(x, y, GRID_CELL, GRID_CELL, octColor(s.oct));
        gfx->setTextSize(1);
        gfx->setTextColor(BLACK);
        gfx->setCursor(x + 7, y + 6);
        gfx->print(NOTE_LETTERS[s.note]);
    }

    // Battere: le caselle 1, 5, 9 e 13 portano un trattino sopra, cosi' i quattro
    // movimenti si contano senza doverli cercare.
    if (i % 4 == 0) gfx->drawFastHLine(x, y - 2, GRID_CELL, HUD_LABEL);

    // La testina passa sopra al contenuto senza cancellarlo: cornice spessa.
    if (playhead) {
        uint16_t c = (v.seqMode == Sequencer::SEQ_RECORDING) ? HUD_RED : HUD_LIME;
        gfx->drawRect(x, y, GRID_CELL, GRID_CELL, c);
        gfx->drawRect(x + 1, y + 1, GRID_CELL - 2, GRID_CELL - 2, c);
    }
    if (cursor) {
        gfx->drawRect(x, y, GRID_CELL, GRID_CELL, WHITE);
    }
}

// Preconteggio: il pattern gira gia', ma qui conta solo sapere quando si parte.
// Oltre alla cifra ci sono quattro pallini che si spengono: il conto alla
// rovescia si segue con la coda dell'occhio, senza rileggere il numero.
void drawCountIn(const SynthView &v, bool full) {
    if (full) chrome("COUNT IN", HUD_RED);
    if (!full && v.countIn == prev.countIn) return;

    char buf[4];
    snprintf(buf, sizeof(buf), "%d", (int)v.countIn);
    gfx->fillRect(60, 76, 120, 84, BLACK);
    textCentered(buf, 76, 10, HUD_RED);

    constexpr int DOT_R = 8, DOT_GAP = 30;
    const int x0 = CX - (3 * DOT_GAP) / 2;
    for (int i = 0; i < 4; ++i) {
        const int x = x0 + i * DOT_GAP;
        if (i < v.countIn) {
            gfx->fillCircle(x, 178, DOT_R, HUD_RED);
        } else {
            gfx->fillCircle(x, 178, DOT_R, BLACK);
            gfx->drawCircle(x, 178, DOT_R, HUD_TRACK);
        }
    }

    clearBand(200, 8);
    textCentered("SUONA AL VIA", 200, 1, HUD_LABEL);
}

void drawSeqScreen(const SynthView &v, bool full) {
    // Svuotare 16 step con una pressione lunga e' un'azione che non lascia
    // traccia: senza una conferma esplicita si resta col dubbio di aver solo
    // fermato il loop. Resta a schermo un secondo e mezzo.
    const bool cleared = (v.clearedAgo > 0 && v.clearedAgo < 1500);
    const bool wasCleared = (prev.clearedAgo > 0 && prev.clearedAgo < 1500);
    if (cleared != wasCleared) full = true;

    if (v.countIn > 0) {
        drawCountIn(v, full || prev.countIn == 0);
        return;
    }
    // Uscendo dal preconteggio la schermata va ricostruita da zero.
    if (prev.countIn > 0) full = true;

    if (full) {
        chrome(v.seqEditing ? "STEP EDIT" : "SEQUENCER", v.seqEditing ? HUD_ICE : HUD_MAGENTA);
        // In step edit i tasti nota scrivono invece di suonare e la leva ARP
        // diventa il legato: e' il momento in cui una legenda serve di piu'.
        textCentered(v.seqEditing ? "TASTI SCRIVONO   ARP: LEGATO"
                                  : "TIENI REC: EDIT   PLAY: SVUOTA",
                     180, 1, HUD_LABEL);
    }

    if (cleared) {
        clearBand(56, 22);
        hudChipCentered(56, "PATTERN VUOTO", HUD_RED, 1);
    }

    const bool modeChanged =
        (!cleared) && (full || v.seqMode != prev.seqMode || v.seqEditing != prev.seqEditing);
    if (modeChanged) {
        // Lo stato del trasporto e' una targhetta piena: il colore lo riconosci
        // prima di aver letto la parola, che e' quello che serve mentre suoni.
        clearBand(56, 22);
        const char *label = "STOP";
        uint16_t col = HUD_LABEL;
        if (v.seqMode == Sequencer::SEQ_RECORDING) {
            label = "REC";
            col = HUD_RED;
        } else if (v.seqMode == Sequencer::SEQ_PLAYING) {
            label = "PLAY";
            col = HUD_LIME;
        }
        hudChipCentered(56, label, col, 2);
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
        textCentered(buf, 140, 2, v.seqEditing ? WHITE : HUD_ICE);
    }

    if (full || v.hold != prev.hold || v.arp != prev.arp || v.bpm != prev.bpm ||
        v.poly != prev.poly) {
        // Fascia stretta sul solo BPM: con i 18 px di prima la pulizia arrivava a
        // toccare la legenda qui sotto e ne mangiava la prima riga ad ogni
        // cambio di tempo.
        clearBand(162, 16);
        char buf[24];
        snprintf(buf, sizeof(buf), "%d BPM", (int)v.bpm);
        textCentered(buf, 162, 2, HUD_AMBER);

        // Tre targhette invece di una riga di testo: acceso e spento si
        // distinguono per pieno contro contorno, non per sfumatura di grigio.
        clearBand(190, 15);
        struct Flag {
            const char *label;
            bool on;
            uint16_t color;
        } flags[3] = {
            {"HOLD", v.hold, HUD_NEON},
            {"ARP", v.arp, HUD_MAGENTA},
            {v.poly ? "POLI" : "MONO", v.poly, HUD_LIME},
        };
        int total = 0;
        for (int i = 0; i < 3; ++i) total += hudChipWidth(flags[i].label, 1) + 6;
        int x = CX - (total - 6) / 2;
        for (int i = 0; i < 3; ++i) {
            const int w = hudChipWidth(flags[i].label, 1);
            if (flags[i].on) {
                hudChip(x, 190, flags[i].label, flags[i].color, 1);
            } else {
                gfx->drawRect(x, 190, w, 14, HUD_TRACK);
                textAt(flags[i].label, x + 5, 193, 1, HUD_LABEL);
            }
            x += w + 6;
        }
    }
}

void drawAdsrScreen(const SynthView &v, bool full) {
    if (full) chrome("ADSR", HUD_RED);

    struct Row {
        const char *label;
        float frac;
        char value[12];
        uint16_t color;
    } rows[4];

    // frazioni normalizzate sui range dichiarati, per le mini-barre
    rows[0].label = "A";
    rows[0].frac = logf(v.attackMs / 2.0f) / logf(250.0f);
    snprintf(rows[0].value, sizeof(rows[0].value), "%d ms", (int)v.attackMs);
    rows[0].color = HUD_RED;

    rows[1].label = "D";
    rows[1].frac = (v.decayMs - 5.0f) / 995.0f;
    snprintf(rows[1].value, sizeof(rows[1].value), "%d ms", (int)v.decayMs);
    rows[1].color = HUD_MAGENTA;

    rows[2].label = "S";
    rows[2].frac = v.sustain;
    snprintf(rows[2].value, sizeof(rows[2].value), "%d %%", (int)(v.sustain * 100.0f + 0.5f));
    rows[2].color = HUD_AMBER;

    rows[3].label = "R";
    rows[3].frac = logf(v.releaseMs / 10.0f) / logf(200.0f);
    snprintf(rows[3].value, sizeof(rows[3].value), "%d ms", (int)v.releaseMs);
    rows[3].color = HUD_LIME;

    bool changed[4] = {
        full || fabsf(v.attackMs - prev.attackMs) > 0.5f,
        full || fabsf(v.decayMs - prev.decayMs) > 0.5f,
        full || fabsf(v.sustain - prev.sustain) > 0.005f,
        full || fabsf(v.releaseMs - prev.releaseMs) > 0.5f,
    };

    // Con quattro encoder la storia delle modalita' da ricordare finisce qui:
    // un parametro per manopola, nello stesso ordine in cui sono scritti.
    if (full) {
        clearBand(196, 10);
        textCentered("1=A  2=D  3=S  4=R", 196, 1, HUD_LABEL);
    }

    // Righe strette abbastanza da restare dentro l'area circolare anche in basso.
    // La lettera del parametro sta su una targhetta piena: quattro barre uguali
    // una sopra l'altra si confondono, il quadrato colorato le ancora.
    const int y0 = 62, dy = 34;
    for (int i = 0; i < 4; ++i) {
        if (!changed[i]) continue;
        const int y = y0 + i * dy;
        gfx->fillRect(28, y, 186, 26, BLACK);
        gfx->fillRect(30, y, 18, 18, rows[i].color);
        textAt(rows[i].label, 33, y + 1, 2, BLACK);
        // Il numero e' l'unica cosa che leggi mentre giri la manopola, e a size 1
        // sul vetro da 1.28" e' alto un millimetro scarso. Va a size 2 come i
        // valori di LEVELS e SETTINGS; lo spazio glielo cede la barra, che
        // scende da 12 a 7 segmenti senza perdere niente — dice una frazione di
        // corsa, non una misura. y+1 e non y+5 perche' il testo ora e' alto 16
        // px dentro una targhetta di 18.
        hudBar(54, y, 60, 18, rows[i].frac, dim565(rows[i].color, 1, 2), rows[i].color);
        textRight(rows[i].value, 212, y + 1, 2, HUD_ICE);
    }
}

// ------------------------------------------------------------- impostazioni
//
// Quattro righe, una selezionata. Encoder 1 sceglie la riga, encoder 2 cambia il
// valore: i due encoder qui non fanno cutoff e volume, e va bene cosi' — sei
// fermo su una schermata di regolazione, il suono puo' aspettare.
//
// I valori sono scritti in **giri di manopola** e non in frazioni di corsa:
// "2.4 giri" dice quello che ti interessa davvero, "1/48 di corsa per scatto" no.
// Due stati. Fuori dal menu si leggono solo i valori, con l'invito a entrare;
// dentro compare il cursore e i comandi cambiano significato: il tasto che
// normalmente scorre le schermate qui scorre le voci, e tenendolo premuto si
// esce. Le voci sono raggruppate per categoria perche' la rete e la sensibilita'
// degli encoder non hanno niente a che vedere fra loro.
constexpr int SET_TOP = 50;
constexpr int SET_HEADER_H = 15;
constexpr int SET_ROW_H = 22;
// Le voci sono dieci e il vetro e' tondo: cinque per volta sono quante ne
// stanno senza che la prima e l'ultima finiscano sotto la ghiera. La finestra
// segue il cursore invece di paginare, cosi' scorrendo non si perde il contesto.
constexpr int SET_VISIBLE = 5;

int settingsFirst = 0;
int settingsFirstDrawn = -1;

// Prima voce della finestra, calcolata perche' il cursore ci stia dentro.
int settingsWindowStart(int cursor) {
    int first = settingsFirst;
    if (cursor < first) first = cursor;
    if (cursor > first + SET_VISIBLE - 1) first = cursor - SET_VISIBLE + 1;
    if (first > SETTING_COUNT - SET_VISIBLE) first = SETTING_COUNT - SET_VISIBLE;
    if (first < 0) first = 0;
    return first;
}

// Quota della riga i-esima *dentro la finestra corrente*, contando le
// intestazioni di categoria che la precedono.
int settingsRowY(int which) {
    int y = SET_TOP;
    for (int i = settingsFirst; i <= which; ++i) {
        if (Settings::ENTRIES[i].category) y += SET_HEADER_H;
        if (i < which) y += SET_ROW_H;
    }
    return y;
}

void drawSettingsScreen(const SynthView &v, bool full) {
    const bool editing = v.setEditing;

    settingsFirst = settingsWindowStart(editing ? v.setCursor : 0);
    const int last = settingsFirst + SET_VISIBLE - 1;

    if (full || editing != prev.setEditing || settingsFirst != settingsFirstDrawn) {
        chrome("SETTINGS", HUD_NEON);
        for (int i = settingsFirst; i <= last; ++i) {
            if (!Settings::ENTRIES[i].category) continue;
            const int hy = settingsRowY(i) - SET_HEADER_H;
            textAt(Settings::ENTRIES[i].category, 30, hy + 2, 1, HUD_LABEL);
            const int lx = 30 + 6 * (int)strlen(Settings::ENTRIES[i].category) + 6;
            gfx->drawFastHLine(lx, hy + 6, 200 - lx, dim565(HUD_NEON, 1, 4));
        }
        // Due frecce ai lati dicono che sopra o sotto c'e' dell'altro: senza,
        // una finestra su dieci voci sembra il menu intero.
        if (settingsFirst > 0) gfx->fillTriangle(212, 60, 206, 68, 218, 68, HUD_LABEL);
        if (last < SETTING_COUNT - 1) gfx->fillTriangle(212, 186, 206, 178, 218, 178, HUD_LABEL);
        settingsFirstDrawn = settingsFirst;
        full = true;
    }

    for (int i = settingsFirst; i <= last; ++i) {
        const bool sel = editing && (i == v.setCursor);
        const bool wasSel = prev.setEditing && (i == prev.setCursor);
        if (!full && v.setIndex[i] == prev.setIndex[i] && sel == wasSel) continue;

        const int y = settingsRowY(i);
        // L'ultima voce (MODALITA' WIFI) sta a y=174 e la sua riga arriva a
        // y=193: li' il cerchio si e' gia' stretto e l'anello passa a x=29 e
        // x=211. Il rettangolo di pulizia largo 26..215 se lo portava via, e la
        // barretta del cursore a x=28 finiva a ridipingerne un pixel di
        // magenta. Con 31..209 e la barretta a 32 restano due pixel di nero fra
        // la riga e la cornice, e sulle righe alte non cambia niente perche'
        // li' di spazio ce n'era d'avanzo.
        gfx->fillRect(31, y, 179, SET_ROW_H - 2, BLACK);
        if (sel) gfx->fillRect(32, y + 1, 4, SET_ROW_H - 4, HUD_MAGENTA);

        textAt(Settings::ENTRIES[i].label, 42, y + 6, 1, sel ? HUD_ICE : HUD_LABEL);
        if (Settings::isAction(i)) {
            textRight("ATTIVA", 208, y + 3, 2, sel ? HUD_AMBER : dim565(HUD_AMBER, 1, 2));
        } else {
            textRight(Settings::valueLabel(i, v.setIndex[i]), 208, y + 3, 2,
                      sel ? HUD_AMBER : HUD_LABEL);
        }
    }

    // Riga di aiuto: dice sempre cosa fa il tasto adesso, perche' lo stesso
    // pulsante ha tre significati diversi a seconda di dove sei.
    //
    // Nessuna di queste stringhe puo' superare i 24 caratteri. E' la quota che
    // detta il limite: a y=208, l'ultima riga di pixel del glifo, il cerchio
    // dell'area dei contenuti lascia solo 75 px per lato, e la fascia che
    // ripulisce la riga si stringe di conseguenza. Con i 28 caratteri di prima
    // il testo partiva da x=36, cioe' fuori dal vetro, e la prima e l'ultima
    // lettera finivano sotto la ghiera.
    const char *hint;
    if (!editing) {
        hint = "TIENI PREMUTO: MODIFICA";
    } else if (Settings::isAction(v.setCursor)) {
        hint = "PREMI: ESEGUI  ENC1: SU";
    } else {
        hint = "PREMI: GIU'  TIENI: ESCI";
    }
    static const char *lastHint = nullptr;
    if (full || hint != lastHint) {
        // Alta 8 e non 10: il glifo size 1 occupa y 202..208 e non serve altro.
        // Due righe in meno vogliono dire una fascia piu' larga di 6 px, che e'
        // esattamente quello che ci vuole per starci dentro con 24 caratteri.
        clearBand(202, 8);
        textCentered(hint, 202, 1, Settings::isAction(v.setCursor) && editing ? HUD_RED
                                                                             : HUD_LABEL);
        lastHint = hint;
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
        const uint16_t c = (p > VU_RED_ZONE) ? HUD_RED : HUD_LIME;
        gfx->writeLine(px, py, x, y, c);
        gfx->writeLine(px, py - 1, x, y - 1, c);
        px = x;
        py = y;
    }
    gfx->endWrite();

    for (int i = 0; i <= 4; ++i) {
        const float p = (float)i / 4.0f;
        const uint16_t c = (p > VU_RED_ZONE) ? HUD_RED : HUD_ICE;
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
        chrome("VU", HUD_LIME);
        textAt("dBFS", 56, 58, 1, HUD_LABEL);
        textAt("CLIP", 146, 58, 1, HUD_LABEL);
        vuScale();
        gfx->fillCircle(VU_PX, VU_PY, 5, HUD_ICE);
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
        vuNeedle(pos, HUD_ICE);
        gfx->fillCircle(VU_PX, VU_PY, 5, HUD_ICE);  // il perno copre la base
        lastNeedle = pos;
    }
    if (fabsf(peakHold - lastMark) > 0.002f) {
        if (lastMark >= 0.0f) vuMark(lastMark, BLACK);
        vuMark(peakHold, (peakHold > VU_RED_ZONE) ? HUD_RED : HUD_AMBER);
        lastMark = peakHold;
    }

    const bool clip = (peak >= 0.999f);
    if (clip != lastClip) {
        gfx->fillCircle(182, 62, 5, clip ? HUD_RED : HUD_TRACK);
        lastClip = clip;
    }

    char buf[16];
    if (rms > 0.0005f) {
        snprintf(buf, sizeof(buf), "%.1f dB", 20.0f * log10f(rms));
    } else {
        snprintf(buf, sizeof(buf), "-inf dB");
    }
    if (strcmp(buf, lastRms) != 0) {
        // 140 px di pulizia per 8 caratteri ("-66.0 dB" e' il piu' lungo, 96 px
        // da x=72 a x=167) erano una fascia larga il doppio del necessario, e a
        // y=217 arrivava a mangiarsi l'anello: li' il cerchio passa a x=53 e
        // x=187, dentro il vecchio 50..189. Con 66..173 il testo e' coperto con
        // sei pixel di margine per lato e la cornice resta intera.
        gfx->fillRect(66, 202, 108, 16, BLACK);
        textCentered(buf, 202, 2, HUD_ICE);
        strncpy(lastRms, buf, sizeof(lastRms) - 1);
    }

    char pk[20];
    if (peakHold > 0.0f) {
        snprintf(pk, sizeof(pk), "pk %.0f dB", VU_DB_MIN * (1.0f - peakHold));
    } else {
        snprintf(pk, sizeof(pk), "pk --");
    }
    if (strcmp(pk, lastPk) != 0) {
        // Quota bassissima, a 109 px dal centro: qui il vetro lascia poco piu' di
        // 80 px di corda. "pk -40 dB" sono 9 caratteri, 54 px da x=93 a x=146:
        // basta e avanza una fascia da 88 a 159. Con la vecchia 72..167 si
        // spegnevano i pixel di cornice del fondo, dove l'anello passa a x=77 e
        // x=162.
        gfx->fillRect(88, 222, 72, 8, BLACK);
        textCentered(pk, 222, 1, HUD_LABEL);
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
constexpr uint16_t SC_GRID = HUD_TRACK;
const int SC_GRID_Y[4] = {SC_CY - SC_H2, SC_CY - SC_H2 / 2, SC_CY + SC_H2 / 2, SC_CY + SC_H2};

void scopeGrid() {
    for (int g = 0; g < 4; ++g) gfx->drawFastHLine(SC_X0, SC_GRID_Y[g], SC_W, SC_GRID);
    // asse dello zero tratteggiato: si distingue dalle altre a colpo d'occhio
    for (int i = 0; i < SC_W; i += 6) gfx->drawFastHLine(SC_X0 + i, SC_CY, 3, HUD_LABEL);
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
        chrome("SCOPE", HUD_NEON);
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
                gfx->writePixel(x, SC_CY, HUD_LABEL);
            }
        }

        // Il segmento copre il salto rispetto alla colonna precedente: sui fronti
        // ripidi (quadra, dente di sega) la traccia resta continua.
        const int top = (y < prevY) ? y : prevY;
        const int h = ((y < prevY) ? (prevY - y) : (y - prevY)) + 1;
        gfx->writeFastVLine(x, top, h, HUD_LIME);

        colTop[i] = (int16_t)top;
        colH[i] = (uint8_t)h;
        prevY = y;
    }
    gfx->endWrite();
    traced = true;

    char info[28];
    snprintf(info, sizeof(info), "%s  zoom x%.1f", WAVEFORM_NAMES[v.waveform], zoom);
    if (strcmp(info, lastInfo) != 0) {
        // La riga piu' lunga possibile e' "TRIANGLE  zoom x8.0": 19 caratteri,
        // 114 px da x=63 a x=176. I 200 px di prima erano quasi tutto il
        // quadrato e a y=199 tagliavano l'anello di netto su tutti e due i
        // fianchi, dove passa a x=33 e x=206. 128 px bastano con sette pixel di
        // margine per lato.
        gfx->fillRect(56, 192, 128, 8, BLACK);
        textCentered(info, 192, 1, HUD_LABEL);
        strncpy(lastInfo, info, sizeof(lastInfo) - 1);
    }
}

// ------------------------------------------------------------ schermata rete

// Anticamera: la radio e' ancora spenta. Serve un gesto deliberato per
// accenderla, perche' da li' in poi il synth resta muto fino al riavvio.
void drawNetworkIdleScreen(const SynthView &, bool full) {
    if (!full) return;
    chrome("NETWORK", HUD_NEON);
    textCentered("AGGIORNAMENTO FIRMWARE", CONTENT_TOP, 1, HUD_LABEL);
    textCentered("via WiFi", 70, 2, HUD_ICE);

    // Il gesto e' il contenuto della schermata: sta su una targhetta, non in
    // mezzo a una frase.
    textCentered("tieni premuto", 104, 1, HUD_LABEL);
    hudChipCentered(118, "SCORRI DISPLAY", HUD_NEON, 1);
    textCentered("per 1 secondo", 140, 1, HUD_LABEL);

    // Avvertenza in fondo, con la barra rossa a sinistra: e' l'unica cosa della
    // schermata che vale la pena leggere due volte.
    // Il blocco sale di 6 px per la stessa ragione del gemello su UPDATE: a x=34
    // siamo a 86 px dal centro, e li' l'anello interno passa gia' a y=199. La
    // barretta che finiva a 201 gli si appoggiava sopra per le ultime tre
    // righe. Finendo a 195 resta a raggio 114, con il suo margine di nero.
    gfx->fillRect(34, 162, 3, 34, HUD_RED);
    textAt("DA QUI IN POI", 46, 164, 1, HUD_RED);
    textAt("il synth resta muto", 46, 176, 1, HUD_LABEL);
    textAt("fino al riavvio", 46, 188, 1, HUD_LABEL);
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

// La tavolozza del logo e' quella dell'interfaccia (vedi la sezione colore in
// cima): qui restano solo i due toni che servono alla sola scena di avvio.
const uint16_t LOGO_MAGENTA = HUD_MAGENTA;
const uint16_t LOGO_AMBER = HUD_AMBER;
const uint16_t LOGO_NEON = HUD_NEON;
const uint16_t LOGO_ICE = HUD_ICE;
const uint16_t LOGO_DIMCYAN = rgb565(0, 90, 110);
const uint16_t LOGO_GRID = rgb565(0, 120, 150);

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

    // Griglia e traccia arrivano fino al bordo del quadrato, ed e' voluto: sul
    // vetro tondo spariscono dietro la ghiera, che e' proprio l'effetto oblo'
    // che si vuole. Quello che non e' voluto e' che passando ci cancellino la
    // cornice — le sei orizzontali la tagliano in dodici punti, e sui tre archi
    // alti il neon diventa quasi spento. Si ripassano i due anelli sopra: costa
    // due righe, e la griglia torna a sembrare che passi sotto il telaio.
    gfx->drawCircle(CX, CY, 118, LOGO_NEON);
    gfx->drawCircle(CX, CY, 116, LOGO_DIMCYAN);

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

// ------------------------------------------- apprendimento dei LED dei tasti
//
// Venti tasti, venti LED, e nessun documento che dica in che ordine sono
// collegati fra loro: la scheda accende un LED e chiede di premere il tasto che
// si e' illuminato. Venti pressioni e la mappa e' fatta per sempre.
void drawLedLearnScreen(const SynthView &v, bool full) {
    if (full) {
        chrome("LUCI", HUD_LIME);
        textCentered("PREMI IL TASTO", 74, 2, HUD_ICE);
        textCentered("CHE SI E' ACCESO", 96, 2, HUD_ICE);
        textCentered("FN7 A LUNGO: ANNULLA", 194, 1, HUD_LABEL);
    }

    if (full || v.ledLearnIndex != prev.ledLearnIndex) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d / %d", v.ledLearnIndex + 1, KEYLED_COUNT);
        gfx->fillRect(50, 126, 140, 24, BLACK);
        textCentered(buf, 126, 3, HUD_AMBER);
        hudBar(50, 158, 140, 12, (float)v.ledLearnIndex / (float)KEYLED_COUNT, HUD_LIME,
               HUD_NEON);
    }
}

// --------------------------------------------------------------- messaggini
// Una fascia in basso con dentro il nome di quello che e' appena cambiato.
// Sta sopra qualunque schermata: quando premi un tasto vuoi sapere cosa hai
// fatto, non cercare la pagina che te lo dice.
void drawToast(const char *text) {
    const int w = hudChipWidth(text, 2);
    const int x = CX - w / 2;
    gfx->fillRect(x - 3, 174, w + 6, 24, BLACK);
    gfx->drawRect(x - 3, 174, w + 6, 24, dim565(HUD_MAGENTA, 1, 2));
    hudChip(x, 177, text, HUD_MAGENTA, 2);
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

    // L'apprendimento delle luci si prende lo schermo intero: mentre e' in corso
    // la tastiera non suona, quindi non c'e' niente altro da guardare.
    if (v.ledLearn != inLedLearn) {
        inLedLearn = v.ledLearn;
        forceFull = true;
    }
    if (inLedLearn) {
        drawLedLearnScreen(v, forceFull || !prevValid);
        forceFull = false;
        prev = v;
        prevValid = true;
        return;
    }

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
            case SCREEN_OSC: drawWaveScreen(v, full); break;
            case SCREEN_OCTAVE: drawOctaveScreen(v, full); break;
            case SCREEN_LEVELS: drawLevelsScreen(v, full); break;
            case SCREEN_FX: drawFxScreen(v, full); break;
            case SCREEN_SEQ: drawSeqScreen(v, full); break;
            case SCREEN_VU: drawVuScreen(v, full); break;
            case SCREEN_SCOPE: drawScopeScreen(v, full); break;
            default: drawSettingsScreen(v, full); break;
        }
    }

    // Il messaggio in sovrimpressione sta sopra a tutto e non chiede il permesso
    // a nessuna schermata: quando sparisce, quella sotto si ridisegna intera,
    // che e' l'unico modo per non lasciare un buco nero dove stava la scritta.
    if (v.toast) {
        drawToast(v.toast);
    } else if (prev.toast) {
        forceFull = true;
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
    static char lastIp[20] = "";
    static bool everDrawn = false;

    const NetPortal::Stage st = NetPortal::stage();
    const char *qr = NetPortal::qrPayload();
    const char *msg = NetPortal::message();

    // L'indirizzo entra nel confronto: arriva quando il rientro automatico va a
    // buon fine, senza che ne' lo stato ne' il messaggio cambino.
    if (everDrawn && st == lastStage && strcmp(qr, lastQr) == 0 && strcmp(msg, lastMsg) == 0 &&
        strcmp(NetPortal::staIp(), lastIp) == 0) {
        return;
    }
    strncpy(lastIp, NetPortal::staIp(), sizeof(lastIp) - 1);
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
        chrome("FALLITO", HUD_RED);
        // I messaggi del portale arrivano a 24 caratteri ("trasferimento
        // interrotto"): a size 2 fanno 288 px su 240, il testo partirebbe da
        // x=-24, si perderebbero le prime due lettere e le ultime andrebbero a
        // capo da sole spezzando la parola. A questa quota ce ne stanno 18, di
        // piu' non entrano: oltre si scende a size 1. Piccolo, ma intero e
        // leggibile, che e' l'unica cosa che conta quando c'e' scritto FALLITO.
        const bool lungo = strlen(msg) > 18;
        textCentered(msg, lungo ? 100 : 96, lungo ? 1 : 2, HUD_ICE);
        textCentered("il firmware attuale", 136, 1, HUD_LABEL);
        textCentered("e' rimasto intatto", 148, 1, HUD_LABEL);
        hudChipCentered(180, "PLAY PER RIAVVIARE", HUD_NEON, 1);
        return;
    }

    if (qrChanged) {
        gfx->fillScreen(BLACK);
        drawQr(qr, 90);
    }

    // Sotto il codice: cosa sta succedendo, e le credenziali per chi le digita a
    // mano. La password e' scritta grande di proposito: se la fotocamera non
    // aggancia il QR — e capita, dipende dal telefono — questa riga e' l'unica
    // via d'uscita, e va letta da mezzo metro con il synth appoggiato al tavolo.
    gfx->fillRect(10, 156, 220, 70, BLACK);

    // Una volta in rete la riga di stato porta l'indirizzo, che dal telefono e'
    // una scorciatoia per il portale. Le credenziali dell'access point restano
    // comunque a video: sono l'unica via d'ingresso se il telefono non e' sulla
    // stessa rete di casa.
    char head[40];
    if (NetPortal::staIp()[0] != '\0') {
        snprintf(head, sizeof(head), "in rete: %s", NetPortal::staIp());
    } else {
        snprintf(head, sizeof(head), "%s", msg);
    }
    textCentered(head, 160, 1, HUD_NEON);
    textCentered(NetPortal::ssid(), 172, 1, HUD_ICE);
    textCentered(NetPortal::password(), 184, 2, HUD_AMBER);
    // Dall'access point il portale si apre senza login. La finestra utente e
    // password compare solo a chi arriva dall'indirizzo di casa, quindi la
    // riga si scrive solo quando quell'indirizzo esiste: altrimenti annuncia
    // un ostacolo che sulla strada del QR non c'e'.
    if (NetPortal::staIp()[0] != '\0') {
        textCentered("da casa, utente: " NET_AUTH_USER, 204, 1, HUD_LABEL);
    }
    // La via d'uscita va scritta sulla schermata da cui si vuole uscire. Finora
    // stava solo nel manuale, e da qui il synth sembrava un vicolo cieco.
    textCentered("PLAY per uscire", 216, 1, HUD_LIME);
}

void drawOtaProgress(int pct) {
    if (!gfx) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    static int lastPct = -1;
    if (pct == lastPct) return;
    // Percentuale che torna indietro = trasferimento nuovo: si riparte da capo.
    if (lastPct < 0 || pct < lastPct) {
        chrome("UPDATE", HUD_AMBER);
        textCentered("SCRITTURA IN CORSO", CONTENT_TOP, 1, HUD_LABEL);
        // Il blocco dell'avvertenza sale di 12 px. A x=34 (86 px dal centro) il
        // vetro finisce a y=202: la barretta rossa alta 22 che partiva da 186
        // usciva dal tondo per le ultime cinque righe e prima ancora passava
        // sopra l'anello. Alzandola a 174 il suo ultimo pixel sta a raggio 114,
        // dentro. L'ascissa resta 34 perche' e' la stessa della barretta gemella
        // sulla schermata NETWORK e le due devono restare allineate; sopra c'e'
        // spazio, il blocco della percentuale finisce a y=165.
        gfx->fillRect(34, 174, 3, 22, HUD_RED);
        textAt("NON SPEGNERE", 46, 178, 1, HUD_RED);
        textAt("l'aggiornamento e' a meta'", 46, 190, 1, HUD_LABEL);
    }
    lastPct = pct;

    hudBar(45, 104, 150, 26, pct / 100.0f, HUD_AMBER, HUD_LIME);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d %%", pct);
    gfx->fillRect(60, 142, 120, 24, BLACK);
    textCentered(buf, 142, 3, HUD_ICE);
}

}  // namespace Display
