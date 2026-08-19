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

// Quanti suoni al massimo: tredici come i tasti. Serve a chi legge l'immagine
// dalla partizione, che deve sapere quando fermarsi prima di fidarsi di un
// numero arrivato dalla flash.
#define MEME_MAX 13

// Non un array ma un **puntatore**, e non una costante ma una variabile.
//
// I tredici suoni possono venire da due posti: i blob sintetizzati che il
// firmware si porta dentro, oppure l'immagine caricata nella partizione dati
// (vedi sample_store.h). All'accensione puntano ai primi; se la partizione ha
// qualcosa di valido, SampleStore::begin() li fa puntare li'.
//
// I punti che li usano non se ne accorgono: `MEME_SAMPLES[i]` e `MEME_COUNT` si
// scrivono uguale in tutti e due i casi, ed e' il motivo per cui questo cambio
// non ha toccato ne' il display ne' la logica dei tasti.
extern const MemeSample *MEME_SAMPLES;
extern uint8_t MEME_COUNT;

// I tredici sintetizzati, sempre presenti: sono la riserva, e sono anche cio' che
// suona una scheda appena programmata, su cui nessuno ha ancora caricato niente.
extern const MemeSample MEME_BUILTIN[];
extern const uint8_t MEME_BUILTIN_COUNT;
