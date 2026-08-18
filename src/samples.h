// samples.h — i tredici suoni della schermata SUONI.
//
// Sono sintetizzati, non registrati, e non e' un ripiego: i suoni "meme" che
// girano in rete hanno quasi tutti un padrone, e questo repository e' pubblico.
// Ognuno e' costruito da cio' che lo rende riconoscibile — una trombetta da
// stadio sono tre lame scordate che calano di poco mentre suonano, un boom e'
// una sinusoide che scende sotto i quaranta hertz con davanti un colpo di
// rumore — quindi in flash costa il tempo che dura invece dei megabyte di una
// registrazione.
//
// Li fa tools/make_samples.py, che scrive src/samples.cpp. Quel file non si
// modifica a mano: si cambia una formula nello script e si rigenera.
#pragma once

#include <Arduino.h>

// Frequenza di campionamento dei blob. Il motore audio gira a 44100 e li rilegge
// con un accumulatore di fase: alzare la frequenza e' l'unica cosa che deve
// fare, e a queste durate l'interpolazione lineare basta e avanza.
#define MEME_RATE 16000

struct MemeSample {
    const char *name;   // come si chiama sul display
    const char *hint;   // una riga che dice cosa aspettarsi
    const uint8_t *data;  // 8 bit senza segno, 128 = silenzio
    uint32_t len;
};

extern const MemeSample MEME_SAMPLES[];
extern const uint8_t MEME_COUNT;
