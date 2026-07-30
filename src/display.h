// display.h — GC9A01 tondo 240x240 su SPI, 8 schermate cicliche + schermata ADSR.
#pragma once

#include <Arduino.h>

#include "settings.h"

// Ordine del ciclo: OSC, OCTAVE, LEVELS, SEQUENCER, VU, SCOPE, SETTINGS, NETWORK.
#define SCREEN_COUNT 8     // schermate cicliche (la ADSR e' fuori dal ciclo)
#define SCREEN_VU 4        // VU meter ad ago
#define SCREEN_SCOPE 5     // oscilloscopio dell'uscita
#define SCREEN_SETTINGS 6  // sensibilita' degli encoder
#define SCREEN_NETWORK 7   // ultima del ciclo: da qui si accende la radio

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

    uint8_t setIndex[SETTING_COUNT];  // valori scelti nella schermata SETTINGS
    uint8_t setCursor;                // riga selezionata

    // Millisecondi da quando il pattern e' stato svuotato, 0 se non e' successo:
    // serve a tenere a schermo la conferma per un attimo e poi toglierla.
    uint32_t clearedAgo;
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
