// presets.cpp — i timbri di fabbrica, uno per riga.
//
// I nomi promettono uno strumento e mantengono un carattere: e' il patto di
// qualunque synth. "PIANOFORTE" non suonera' come uno Steinway, ma si comporta
// come un pianoforte — attacco secco, coda che muore da sola, timbro che si
// scurisce mentre la nota si spegne — e su una tastiera si suona come tale.
//
// Ordine dei campi: onda, cutoff, risonanza | A D S R | filtro: quanto, quanto
// dura | sub, detune, drive, glide | eco: mix, tempo | LFO: velocita',
// profondita', bersaglio | 8 bit.

#include "presets.h"

#include "audio_engine.h"

const Preset PRESETS[] = {
    // Il primo e' il suono "neutro" con cui il synth si e' sempre acceso: chi
    // non vuole timbri prefatti resta qui e non se ne accorge nemmeno.
    {"BASE", "il suono di sempre", WAVE_SAW, 4000.0f, 0.0f,
     10.0f, 150.0f, 0.70f, 250.0f,
     0.0f, 300.0f,
     0.0f, 0.0f, 0.0f, 0.0f,
     0.0f, 220.0f,
     5.0f, 0.0f, LFO_OFF,
     false, 1},

    // Martelletto: attacco immediato, nessun sustain, il filtro si chiude
    // mentre la corda muore. Il sub sotto da' il corpo dei bassi del pianoforte.
    {"PIANOFORTE", "attacco secco, coda che muore", WAVE_TRIANGLE, 1100.0f, 0.12f,
     2.0f, 700.0f, 0.12f, 320.0f,
     0.62f, 520.0f,
     0.35f, 4.0f, 0.05f, 0.0f,
     0.10f, 180.0f,
     5.0f, 0.0f, LFO_OFF,
     false, 1},

    // Corda pizzicata: come il pianoforte ma piu' nasale e piu' corta, con un
    // filo di drive che imita la cassa.
    {"CHITARRA", "pizzicata, un po' sporca", WAVE_SAW, 900.0f, 0.30f,
     3.0f, 420.0f, 0.05f, 220.0f,
     0.70f, 300.0f,
     0.15f, 8.0f, 0.28f, 0.0f,
     0.12f, 160.0f,
     5.0f, 0.0f, LFO_OFF,
     false, 1},

    // Nessun inviluppo di filtro e sustain pieno: parte e finisce col tasto,
    // che e' esattamente cio' che fa una canna d'organo.
    {"ORGANO", "parte e finisce col tasto", WAVE_SQUARE, 3200.0f, 0.05f,
     6.0f, 60.0f, 1.00f, 70.0f,
     0.0f, 300.0f,
     0.55f, 0.0f, 0.0f, 0.0f,
     0.0f, 220.0f,
     5.0f, 0.0f, LFO_OFF,
     false, 1},

    {"BASSO", "corto e profondo", WAVE_SAW, 320.0f, 0.45f,
     4.0f, 260.0f, 0.25f, 140.0f,
     0.55f, 190.0f,
     0.75f, 0.0f, 0.20f, 25.0f,
     0.0f, 220.0f,
     5.0f, 0.0f, LFO_OFF,
     false, 1},

    // La scordatura fra le due onde e' tutto: e' quel battimento a far sembrare
    // molti strumenti invece di uno.
    {"ARCHI", "entra piano, riempie", WAVE_SAW, 2200.0f, 0.15f,
     320.0f, 400.0f, 0.90f, 620.0f,
     0.20f, 900.0f,
     0.20f, 22.0f, 0.0f, 0.0f,
     0.32f, 260.0f,
     4.5f, 0.06f, LFO_PITCH,
     false, 1},

    {"FLAUTO", "sinusoide con un filo di vibrato", WAVE_SINE, 2600.0f, 0.0f,
     90.0f, 200.0f, 0.95f, 200.0f,
     0.0f, 300.0f,
     0.0f, 0.0f, 0.0f, 0.0f,
     0.15f, 200.0f,
     5.5f, 0.16f, LFO_PITCH,
     false, 1},

    {"CAMPANE", "colpo lungo, si spegne da solo", WAVE_SINE, 5000.0f, 0.10f,
     2.0f, 1000.0f, 0.0f, 1500.0f,
     0.45f, 800.0f,
     0.0f, 34.0f, 0.0f, 0.0f,
     0.45f, 320.0f,
     5.0f, 0.0f, LFO_OFF,
     false, 1},

    {"CLAVICEMBALO", "pizzicato metallico", WAVE_PULSE, 2400.0f, 0.22f,
     2.0f, 300.0f, 0.0f, 160.0f,
     0.50f, 260.0f,
     0.0f, 6.0f, 0.10f, 0.0f,
     0.10f, 140.0f,
     5.0f, 0.0f, LFO_OFF,
     false, 1},

    {"VIBRAFONO", "morbido, ondeggia", WAVE_SINE, 3000.0f, 0.0f,
     6.0f, 850.0f, 0.0f, 700.0f,
     0.30f, 600.0f,
     0.25f, 0.0f, 0.0f, 0.0f,
     0.25f, 240.0f,
     5.5f, 0.55f, LFO_AMP,
     false, 1},

    // Il 303: risonanza alta, filtro che si chiude in fretta, glide fra le note.
    {"ACIDO", "il basso che squittisce", WAVE_SAW, 400.0f, 0.85f,
     3.0f, 200.0f, 0.10f, 120.0f,
     0.72f, 230.0f,
     0.0f, 0.0f, 0.40f, 60.0f,
     0.18f, 180.0f,
     5.0f, 0.0f, LFO_OFF,
     false, 1},

    {"ARCADE", "sala giochi, 8 bit", WAVE_PULSE, 4200.0f, 0.10f,
     2.0f, 130.0f, 0.22f, 80.0f,
     0.25f, 160.0f,
     0.0f, 0.0f, 0.0f, 0.0f,
     0.12f, 120.0f,
     5.0f, 0.0f, LFO_OFF,
     true, 1},

    {"PAD SPAZIALE", "arriva da lontano", WAVE_TRIANGLE, 1500.0f, 0.25f,
     420.0f, 600.0f, 0.95f, 1500.0f,
     0.30f, 2000.0f,
     0.30f, 16.0f, 0.0f, 0.0f,
     0.50f, 380.0f,
     0.6f, 0.45f, LFO_CUTOFF,
     false, 1},

    // Il rumore con un inviluppo cortissimo e' una percussione: la nota decide
    // l'intonazione del corpo, non l'altezza vera.
    {"TAMBURO", "rumore percosso", WAVE_NOISE, 1800.0f, 0.35f,
     2.0f, 130.0f, 0.0f, 70.0f,
     0.80f, 90.0f,
     0.0f, 0.0f, 0.25f, 0.0f,
     0.0f, 220.0f,
     5.0f, 0.0f, LFO_OFF,
     false, 1},

    {"LASER", "scende e sparisce", WAVE_SAW, 700.0f, 0.75f,
     2.0f, 180.0f, 0.0f, 120.0f,
     1.00f, 140.0f,
     0.0f, 0.0f, 0.30f, 180.0f,
     0.30f, 140.0f,
     5.0f, 0.0f, LFO_OFF,
     false, 1},
};

const uint8_t PRESET_COUNT = (uint8_t)(sizeof(PRESETS) / sizeof(PRESETS[0]));
