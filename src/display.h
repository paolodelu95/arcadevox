// display.h — GC9A01 tondo 240x240 su SPI, 4 schermate cicliche + schermata ADSR.
#pragma once

#include <Arduino.h>

#define SCREEN_COUNT 4  // schermate cicliche (la ADSR e' fuori dal ciclo)

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

    uint8_t seqMode;   // Sequencer::Mode
    uint8_t seqStep;   // 0..15
    uint16_t bpm;
    bool hold;
    bool arp;
};

namespace Display {

void begin();
void nextScreen();          // pulsante "scorri display"
uint8_t currentScreen();
void update(const SynthView &v);  // ridisegna solo cio' che e' cambiato

}  // namespace Display
