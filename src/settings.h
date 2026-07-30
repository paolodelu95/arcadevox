// settings.h — sensibilita' degli encoder, condivisa fra logica e display.
//
// Gli encoder sono incrementali: quanto muove uno scatto e' una scelta di gusto,
// e finora era cablata nel codice. Con il volume a 1/50 di corsa per scatto
// servivano due giri e mezzo di manopola per andare da zero a fondo, che al banco
// e' un'eternita'.
//
// Le voci non salvano il passo ma l'**indice** nella scala: cosi' una release
// futura puo' ritoccare i valori senza che le schede gia' in giro si ritrovino
// con una sensibilita' assurda.
#pragma once

#include <Arduino.h>

#define SETTING_VOL 0
#define SETTING_CUTOFF 1
#define SETTING_ADSR 2
#define SETTING_FINE 3
#define SETTING_COUNT 4

namespace Settings {

struct Entry {
    const char *label;
    uint8_t count;                  // quanti valori ha questa voce
    uint8_t byDefault;              // indice di partenza
    const char *const *valueLabels; // testo mostrato per ogni valore
};

extern const Entry ENTRIES[SETTING_COUNT];

// Indice riportato dentro i limiti della sua voce: quello che arriva dalla NVS
// non e' fidato, puo' venire da una versione con una scala piu' lunga.
uint8_t clampIndex(uint8_t which, uint8_t index);

const char *valueLabel(uint8_t which, uint8_t index);

// Passo per scatto di encoder, in frazione di corsa (0..1). Vale per volume,
// cutoff e ADSR, che si muovono tutti su una posizione normalizzata.
float step(uint8_t which, uint8_t index);

// Divisore del passo fine (col click dell'encoder): 2, 4, 8 o 16.
float fineDivider(uint8_t index);

}  // namespace Settings
