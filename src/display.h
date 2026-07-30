// display.h — GC9A01 tondo 240x240 su SPI, 6 schermate cicliche + schermata ADSR.
#pragma once

#include <Arduino.h>

// Ordine del ciclo: WAVE, OCTAVE, LEVELS, SEQUENCER, VU, SCOPE, NETWORK.
#define SCREEN_COUNT 7    // schermate cicliche (la ADSR e' fuori dal ciclo)
#define SCREEN_VU 4       // VU meter ad ago
#define SCREEN_SCOPE 5    // oscilloscopio dell'uscita
#define SCREEN_NETWORK 6  // ultima del ciclo: da qui si accende la radio

// Fotografia dello stato del synth passata al display ad ogni refresh.
struct SynthView {
    uint8_t waveform;
    int8_t octave;
    float cutoffHz;
    float volume;   // 0..1

    bool adsrEdit;
    float attackMs;
    float decayMs;
    float sustain;  // 0..1
    float releaseMs;

    uint8_t seqMode;    // Sequencer::Mode
    uint8_t seqStep;    // 0..15, testina
    uint8_t seqCursor;  // 0..15, cursore dell'editor
    bool seqEditing;    // STEP EDIT attivo
    uint8_t countIn;    // movimenti mancanti al via (0 = non in preconteggio)
    uint16_t seqRev;    // revisione del pattern: cambia solo a scrittura avvenuta
    uint16_t bpm;
    bool hold;
    bool arp;
    bool poly;        // false = MONO, true = POLIFONICO
    uint8_t voices;   // voci che stanno suonando adesso
};

namespace Display {

void begin();
void nextScreen();          // pulsante "scorri display"
uint8_t currentScreen();
void update(const SynthView &v);  // ridisegna solo cio' che e' cambiato

// --- modalita' NETWORK (il synth e' muto, il loop normale non gira) ---
// Schermata a tutto display: QR, credenziali e stato del portale.
void updateNetwork();
// Barra di avanzamento dell'aggiornamento. Chiamata dal callback di Update,
// che tiene occupato il loop per tutta la durata del trasferimento.
void drawOtaProgress(int pct);

}  // namespace Display
