// fx_rows.h — l'elenco della schermata EFFETTI, condiviso fra logica e display.
//
// Prima questi parametri stavano tutti sul quarto encoder, e per sceglierne uno
// bisognava sapere che *un altro* encoder, su un'altra schermata, ne cambiava il
// bersaglio. Era il comando meno trovabile dello strumento: nove parametri
// dietro una manopola che non diceva di comandarli.
//
// Adesso sono righe di un elenco, come le impostazioni: la prima manopola
// sceglie la riga, la seconda ne cambia il valore, e la riga selezionata e'
// scritta in chiaro col suo nome. In cambio della manopola persa si guadagnano
// cinque parametri che prima non aveva nessuno — il ritorno dell'eco, il
// bersaglio dell'LFO, il modo dell'arpeggiator e le due manopole dell'inviluppo
// di filtro, che secondo presets.h sono l'ingrediente che distingue un
// pianoforte da un organo e non erano toccabili in nessun modo.
//
// Qui stanno solo i **nomi**: il valore lo calcola main.cpp, che e' l'unico a
// sapere cosa sono, e il disegno lo fa display.cpp, che non ha bisogno di
// saperlo. E' la stessa divisione del lavoro di SynthView.
#pragma once

#include <Arduino.h>

enum FxRowId : uint8_t {
    FX_GRANA = 0,   // profondita' dell'8 BIT: 12, 8, 6, 4 bit
    FX_ECO_MIX,     // quanto eco si sente
    FX_ECO_TEMPO,   // distanza fra le ripetizioni
    FX_ECO_RITORNO, // quante volte ritorna
    FX_LFO_SU,      // cosa fa oscillare: niente, altezza, filtro, volume
    FX_LFO_VELOC,   // quanto in fretta
    FX_LFO_PROF,    // di quanto
    FX_MODO_ARP,    // su, giu', su/giu', casuale, ordine
    FX_SUB,         // l'ottava sotto
    FX_DETUNE,      // il battimento fra le voci
    FX_DRIVE,       // la saturazione
    FX_GLIDE,       // quanto ci mette a scivolare sulla nota dopo
    FX_APERTURA,    // di quanto il filtro si spalanca all'attacco
    FX_CHIUSURA,    // e in quanto tempo si richiude
    FX_ROW_COUNT
};

struct FxRowInfo {
    // Intestazione di categoria, oppure nullptr per proseguire quella di sopra.
    const char *category;
    // Massimo dieci caratteri: oltre, la colonna del valore non ci sta piu'
    // dentro il cerchio alle quote basse dell'elenco.
    const char *label;
};

// La tabella sta in fx_rows.cpp: qui basta sapere che esiste.
extern const FxRowInfo FX_ROWS[FX_ROW_COUNT];
