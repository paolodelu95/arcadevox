// display.cpp — GC9A01 240x240 IPS su SPI hardware (Arduino_GFX).
//
// Le schermate cicliche sono 4 (pulsante GPIO 18). Quando l'ADSR EDIT MODE e'
// attivo si passa automaticamente a una quinta schermata dedicata, che bypassa il
// ciclo; all'uscita si torna esattamente alla schermata di prima.
//
// Per evitare flicker si ridisegna la parte statica solo al cambio schermata, e i
// valori dinamici solo quando cambiano davvero.

#include "display.h"

#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

#include "audio_engine.h"
#include "pinout.h"
#include "sequencer.h"

namespace {

constexpr int CX = 120;  // centro del display tondo
constexpr int CY = 120;

Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;

uint8_t screen = 0;
bool inAdsrScreen = false;
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
        gfx->fillRect(50, 88, 140, 64, BLACK);
        drawWaveIcon(v.waveform, 120, CYAN);
        clearBand(170, 24);
        textCentered(WAVEFORM_NAMES[v.waveform], 170, 3, WHITE);
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

void drawSeqScreen(const SynthView &v, bool full) {
    if (full) chrome("SEQUENCER", MAGENTA);

    bool modeChanged = full || v.seqMode != prev.seqMode;
    if (modeChanged) {
        clearBand(64, 24);
        const char *label = "STOP";
        uint16_t col = DARKGREY;
        if (v.seqMode == Sequencer::SEQ_RECORDING) {
            label = "REC";
            col = RED;
        } else if (v.seqMode == Sequencer::SEQ_PLAYING) {
            label = "PLAY";
            col = GREEN;
        }
        textCentered(label, 64, 3, col);
    }

    // griglia 16 step: 2 righe da 8
    if (full || v.seqStep != prev.seqStep || modeChanged) {
        const int x0 = 44, y0 = 100, cell = 18, gap = 4;
        bool active = (v.seqMode != Sequencer::SEQ_IDLE);
        for (int i = 0; i < 16; ++i) {
            int col = i % 8, row = i / 8;
            int x = x0 + col * (cell + gap);
            int y = y0 + row * (cell + gap);
            bool on = active && (i == v.seqStep);
            uint16_t c = on ? ((v.seqMode == Sequencer::SEQ_RECORDING) ? RED : GREEN) : NAVY;
            gfx->fillRect(x, y, cell, cell, c);
        }
        char buf[12];
        snprintf(buf, sizeof(buf), "%02d/16", active ? (v.seqStep + 1) : 0);
        clearBand(148, 16);
        textCentered(buf, 148, 2, WHITE);
    }

    if (full || v.hold != prev.hold || v.arp != prev.arp || v.bpm != prev.bpm) {
        clearBand(174, 18);
        char buf[24];
        snprintf(buf, sizeof(buf), "BPM %3d", (int)v.bpm);
        textCentered(buf, 174, 2, YELLOW);
        clearBand(198, 16);
        char st[24];
        snprintf(st, sizeof(st), "HOLD %s   ARP %s", v.hold ? "ON " : "OFF", v.arp ? "ON " : "OFF");
        textCentered(st, 198, 1, v.hold || v.arp ? CYAN : DARKGREY);
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

}  // namespace

namespace Display {

void begin() {
    bus = new Arduino_ESP32SPI(PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_SCLK, PIN_TFT_MOSI,
                               GFX_NOT_DEFINED /* MISO non usato */);
    gfx = new Arduino_GC9A01(bus, PIN_TFT_RST, 0 /* rotation */, true /* IPS */);

    gfx->begin(40000000);
    gfx->fillScreen(BLACK);

    textCentered("SprigSynth", 100, 3, CYAN);
    textCentered("ESP32-S3", 140, 2, DARKGREY);
    delay(900);

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

    // La schermata ADSR bypassa il ciclo normale finche' l'edit mode e' attivo.
    if (v.adsrEdit != inAdsrScreen) {
        inAdsrScreen = v.adsrEdit;
        forceFull = true;
    }

    bool full = forceFull || !prevValid;
    forceFull = false;

    if (inAdsrScreen) {
        drawAdsrScreen(v, full);
    } else {
        switch (screen) {
            case 0: drawWaveScreen(v, full); break;
            case 1: drawOctaveScreen(v, full); break;
            case 2: drawLevelsScreen(v, full); break;
            default: drawSeqScreen(v, full); break;
        }
    }

    prev = v;
    prevValid = true;
}

}  // namespace Display
