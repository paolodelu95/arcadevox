// settings.cpp — le scale di sensibilita' degli encoder.

#include "settings.h"

namespace {

// Scatti di detent per percorrere l'intera corsa del parametro. Una manopola
// EC11 ne fa 20 per giro, quindi l'etichetta mostra direttamente i giri: e' la
// grandezza che senti sotto le dita, molto piu' dei "1/64 di corsa per scatto"
// che c'erano nel codice.
const uint16_t DETENTS[6] = {100, 64, 48, 32, 20, 12};
const char *const TURNS[6] = {"5.0 giri", "3.2 giri", "2.4 giri",
                              "1.6 giri", "1.0 giri", "0.6 giri"};

const float FINE[4] = {2.0f, 4.0f, 8.0f, 16.0f};
const char *const FINE_LABELS[4] = {"1/2", "1/4", "1/8", "1/16"};

}  // namespace

namespace Settings {

// I default riproducono esattamente il comportamento di prima che questa
// schermata esistesse: chi aggiorna non trova le manopole cambiate sotto le dita
// finche' non decide lui.
const Entry ENTRIES[SETTING_COUNT] = {
    {"VOLUME", 6, 2, TURNS},      // era 1/50 di corsa per scatto
    {"CUTOFF", 6, 1, TURNS},      // era 1/64
    {"ADSR", 6, 2, TURNS},        // era 1/48, su attack e release
    {"PASSO FINE", 4, 1, FINE_LABELS},
};

uint8_t clampIndex(uint8_t which, uint8_t index) {
    if (which >= SETTING_COUNT) return 0;
    const uint8_t n = ENTRIES[which].count;
    return (index < n) ? index : ENTRIES[which].byDefault;
}

const char *valueLabel(uint8_t which, uint8_t index) {
    if (which >= SETTING_COUNT) return "";
    return ENTRIES[which].valueLabels[clampIndex(which, index)];
}

float step(uint8_t which, uint8_t index) {
    return 1.0f / (float)DETENTS[clampIndex(which, index)];
}

float fineDivider(uint8_t index) { return FINE[clampIndex(SETTING_FINE, index)]; }

}  // namespace Settings
