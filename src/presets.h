// presets.h — i timbri di fabbrica.
//
// Un synth sottrattivo non *riproduce* un pianoforte: quello lo fa un
// campionatore, che di pianoforti ne ha registrati centoventi. Quello che fa —
// ed e' esattamente il mestiere per cui i synth hanno le manopole che hanno — e'
// riprodurne il **comportamento**: un attacco secco, un suono che si spegne da
// solo mentre tieni il tasto, il timbro che si scurisce mentre la nota muore.
// Con quelle tre cose insieme l'orecchio dice "pianoforte" anche se nessuna
// corda e' mai stata registrata. E' cosi' che si chiamano i preset di fabbrica
// di qualunque synth analogico dagli anni Settanta in poi.
//
// L'ingrediente che fa la differenza e' l'inviluppo di filtro: senza, un
// "pianoforte" resta un organo con l'attacco veloce.
#pragma once

#include <Arduino.h>

struct Preset {
    const char *name;
    const char *hint;  // una riga che dice cosa aspettarsi

    uint8_t wave;
    float cutoffHz;
    float resonance;

    float attackMs;
    float decayMs;
    float sustain;
    float releaseMs;

    float filtEnvAmount;  // quanto si apre il filtro all'attacco
    float filtEnvMs;      // e in quanto tempo si richiude

    float subLevel;
    float detuneCents;
    float drive;
    float glideMs;

    float delayMix;
    float delayMs;

    float lfoRate;
    float lfoDepth;
    uint8_t lfoTarget;

    bool crush;
    uint8_t crushPreset;
};

extern const Preset PRESETS[];
extern const uint8_t PRESET_COUNT;
