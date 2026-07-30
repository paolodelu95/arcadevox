// settings.cpp — le scale di sensibilita' degli encoder.

#include "settings.h"

namespace {

// Scatti di detent per percorrere l'intera corsa del parametro. Una manopola
// EC11 ne fa 20 per giro, quindi l'etichetta mostra direttamente i giri: e' la
// grandezza che senti sotto le dita, molto piu' dei "1/64 di corsa per scatto"
// che c'erano nel codice.
//
// La scala e' scritta in ordine CRESCENTE di giri, e non e' un dettaglio di
// stile. Il menu somma l'indice al movimento dell'encoder, sempre, per ogni
// voce: se qui i numeri scendessero mentre l'indice sale, girando la manopola in
// senso orario il numero a video calerebbe, e la manopola sembrerebbe montata al
// contrario. E' esattamente com'era fino alla 1.11.0. La regola che tiene tutto
// insieme e' una sola: l'indice cresce nella stessa direzione del numero che si
// legge sullo schermo, e allora il senso orario aumenta ovunque senza casi
// particolari nel codice.
//
// Attenzione a chi ritocca questa tabella: gli indici finiscono in NVS. Se
// l'ordine cambia di nuovo, va alzato SCALE_REV in storage.h e aggiunta la
// conversione, altrimenti le schede gia' in giro si ritrovano la sensibilita'
// ribaltata al primo avvio.
const uint16_t DETENTS[6] = {12, 20, 32, 48, 64, 100};
const char *const TURNS[6] = {"0.6 giri", "1.0 giri", "1.6 giri",
                              "2.4 giri", "3.2 giri", "5.0 giri"};

const float FINE[4] = {2.0f, 4.0f, 8.0f, 16.0f};
const char *const FINE_LABELS[4] = {"1/2", "1/4", "1/8", "1/16"};

}  // namespace

namespace Settings {

// I default riproducono esattamente il comportamento di prima che questa
// schermata esistesse: chi aggiorna non trova le manopole cambiate sotto le dita
// finche' non decide lui.
const Entry ENTRIES[SETTING_COUNT] = {
    {"ENCODER", "VOLUME", 6, 3, TURNS},  // era 1/50 di corsa per scatto -> 2.4 giri
    {nullptr, "CUTOFF", 6, 4, TURNS},    // era 1/64 -> 3.2 giri
    {nullptr, "ADSR", 6, 3, TURNS},      // era 1/48, su attack e release -> 2.4 giri
    {nullptr, "PASSO FINE", 4, 1, FINE_LABELS},
    {"RETE", "MODALITA' WIFI", 0, 0, nullptr},
};

uint8_t clampIndex(uint8_t which, uint8_t index) {
    if (which >= SETTING_COUNT || ENTRIES[which].count == 0) return 0;
    const uint8_t n = ENTRIES[which].count;
    return (index < n) ? index : ENTRIES[which].byDefault;
}

const char *valueLabel(uint8_t which, uint8_t index) {
    if (which >= SETTING_COUNT || ENTRIES[which].count == 0) return "";
    return ENTRIES[which].valueLabels[clampIndex(which, index)];
}

float step(uint8_t which, uint8_t index) {
    return 1.0f / (float)DETENTS[clampIndex(which, index)];
}

float fineDivider(uint8_t index) { return FINE[clampIndex(SETTING_FINE, index)]; }

}  // namespace Settings
