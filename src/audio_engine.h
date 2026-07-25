// audio_engine.h — oscillatore monofonico + filtro + ADSR, su task dedicato core 0.
#pragma once

#include <Arduino.h>

#define SAMPLE_RATE 44100

// Forme d'onda disponibili (ordine usato dal joystick sinistra/destra).
enum Waveform : uint8_t {
    WAVE_SINE = 0,
    WAVE_SQUARE,
    WAVE_SAW,
    WAVE_TRIANGLE,
    WAVE_COUNT
};

extern const char *const WAVEFORM_NAMES[WAVE_COUNT];

// Stati dell'inviluppo (esposti per il display / debug).
enum EnvStage : uint8_t {
    ENV_IDLE = 0,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE
};

namespace AudioEngine {

// Inizializza I2S e lancia il task audio pinnato sul core 0.
void begin();

// --- controllo nota (chiamati dal core 1) ---
void noteOn(float freq);   // ritriggera sempre l'inviluppo (ATTACK)
void setFrequency(float freq);  // cambia intonazione senza ritriggerare
void noteOff();            // passa in RELEASE

// --- parametri timbrici ---
void setWaveform(uint8_t wave);
void setCutoff(float hz);      // 80..8000 Hz
void setVolume(float vol);     // 0..1

// --- parametri ADSR ---
void setAttack(float ms);      // 2..500 ms
void setDecay(float ms);       // 5..1000 ms
void setSustain(float level);  // 0..1
void setRelease(float ms);     // 10..2000 ms

// --- stato ---
EnvStage envStage();
bool isSounding();

}  // namespace AudioEngine
