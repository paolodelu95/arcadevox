// settings.h — il menu delle impostazioni, condiviso fra logica e display.
//
// Le voci non salvano il valore ma l'**indice** nella loro scala: cosi' una
// release futura puo' ritoccare i numeri senza che le schede gia' in giro si
// ritrovino con una sensibilita' o una luminosita' assurda.
#pragma once

#include <Arduino.h>

// --- sensibilita' degli encoder ---
#define SETTING_VOL 0
#define SETTING_CUTOFF 1
#define SETTING_ADSR 2
#define SETTING_FINE 3
// --- impostazioni musicali ---
#define SETTING_SCALE 4
#define SETTING_ROOT 5
// --- luci sotto i tasti ---
#define SETTING_LED 6
#define SETTING_LEDLEARN 7  // azione: impara l'ordine della catena
// --- uscita audio e MIDI ---
#define SETTING_AUDIO 8
#define SETTING_MIDIOUT 9
// --- rete ---
#define SETTING_NET 10  // azione: accende la radio

// Fin qui il menu. Il TIMBRO sta dopo apposta: resta una voce a tutti gli
// effetti — ha un indice, un valore di fabbrica, un posto in NVS e risponde al
// program change — ma **non si sceglie piu' da qui**. Ha una schermata sua,
// perche' e' il punto da cui si comincia a suonare e non una preferenza da
// impostare una volta e dimenticare.
//
// Spostarlo in coda non tocca la memoria: lo stato salva ogni impostazione in un
// campo col suo nome (`setTimbro`), non un array indicizzato, quindi
// rinumerare qui non sposta niente di quello che c'e' gia' nelle schede in giro.
#define SETTING_MENU_COUNT 11
#define SETTING_TIMBRO 11
#define SETTING_COUNT 12

namespace Settings {

struct Entry {
    // Intestazione di categoria, oppure nullptr per proseguire quella di sopra.
    const char *category;
    const char *label;
    uint8_t count;                   // quanti valori ha, 0 se e' un'azione
    uint8_t byDefault;               // indice di partenza
    const char *const *valueLabels;  // testo mostrato per ogni valore
};

extern const Entry ENTRIES[SETTING_COUNT];

// Quante posizioni ha la voce (il TIMBRO lo chiede a presets.cpp).
uint8_t valueCount(uint8_t which);

// Una voce d'azione si esegue, non si regola: gli encoder non la toccano.
inline bool isAction(uint8_t which) { return ENTRIES[which].count == 0; }

// Indice riportato dentro i limiti della sua voce: quello che arriva dalla NVS
// non e' fidato, puo' venire da una versione con una scala piu' lunga.
uint8_t clampIndex(uint8_t which, uint8_t index);

const char *valueLabel(uint8_t which, uint8_t index);

// Passo per scatto di encoder, in frazione di corsa (0..1). Vale per volume,
// cutoff e ADSR, che si muovono tutti su una posizione normalizzata.
float step(uint8_t which, uint8_t index);

// Divisore del passo fine (col click dell'encoder): 2, 4, 8 o 16.
float fineDivider(uint8_t index);

// --- scale ---
// Semitono suonato dall'`degree`-esimo tasto (0..NOTE_COUNT-1) della scala
// `scaleIdx`, senza contare la tonica. Sulla scala cromatica e' l'identita';
// sulle altre i 13 tasti coprono quasi due ottave, perche' i gradi sono meno di
// dodici e la tastiera si allunga da sola.
int scaleSemitone(uint8_t scaleIdx, int degree);
// True se quel tasto e' una tonica (la nota che da' il nome alla scala): serve
// al display e alle luci per far vedere dove ricomincia l'ottava.
bool scaleIsRoot(uint8_t scaleIdx, int degree);
// Nome italiano della tonica scelta ("DO", "DO#", ...).
const char *rootName(uint8_t rootIdx);

}  // namespace Settings
