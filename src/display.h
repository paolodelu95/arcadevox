// display.h — GC9A01 tondo 240x240 su SPI, schermate cicliche + quelle di edit.
#pragma once

#include <Arduino.h>

#include "settings.h"

// Ordine del ciclo: OSC, OTTAVA, LIVELLI, EFFETTI, SEQUENCER, VU, SCOPE,
// SETTINGS. Le schermate di edit (ADSR, step edit, luci) stanno fuori dal ciclo:
// ci si entra da un tasto e si torna esattamente dov'eravamo.
#define SCREEN_OSC 0
#define SCREEN_OCTAVE 1
#define SCREEN_LEVELS 2
#define SCREEN_FX 3        // 8 BIT, eco, LFO, drive, sub, detune, glide
#define SCREEN_SEQ 4
#define SCREEN_VU 5        // VU meter ad ago
#define SCREEN_SCOPE 6     // oscilloscopio dell'uscita
#define SCREEN_SETTINGS 7  // ultima del ciclo: encoder, tastiera, luci, rete
#define SCREEN_COUNT 8

// Fotografia dello stato del synth passata al display ad ogni refresh.
struct SynthView {
    uint8_t waveform;
    int8_t octave;
    float cutoffHz;
    float resonance;  // 0..1
    float volume;     // 0..1

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
    uint8_t arpMode;
    const char *arpName;
    bool poly;          // false = MONO, true = POLIFONICO
    const char *chordName;
    uint8_t voices;     // voci che stanno suonando adesso

    // --- effetti ---
    bool crush;               // 8 BIT inserito
    const char *crushName;    // "8 BIT", "4 BIT"...
    float delayMix;
    float delayMs;
    float lfoDepth;
    float lfoRate;
    const char *lfoTargetName;
    float drive;
    float subLevel;
    float detuneCents;
    float glideMs;
    const char *enc4Name;     // cosa comanda adesso il quarto encoder
    uint8_t enc4Index;        // indice dello stesso, per evidenziare la riga giusta

    // --- tastiera ---
    const char *scaleName;
    const char *rootName;
    bool expanderOk;  // l'MCP23017 risponde: se no, la tastiera e' muta

    uint8_t setIndex[SETTING_COUNT];  // valori scelti nella schermata SETTINGS
    uint8_t setCursor;                // riga selezionata
    bool setEditing;                  // dentro al menu: il cursore e' visibile

    // Millisecondi da quando il pattern e' stato svuotato, 0 se non e' successo.
    uint32_t clearedAgo;

    // Apprendimento dell'ordine dei LED: schermata a se', fuori dal ciclo.
    bool ledLearn;
    uint8_t ledLearnIndex;

    // Messaggio breve in sovrimpressione, nullptr se non ce n'e' uno.
    const char *toast;
};

namespace Display {

void begin();
void nextScreen();          // tasto FN7
uint8_t currentScreen();
void update(const SynthView &v);  // ridisegna solo cio' che e' cambiato

// --- modalita' NETWORK (il synth e' muto, il loop normale non gira) ---
void updateNetwork();
void drawOtaProgress(int pct);

}  // namespace Display
