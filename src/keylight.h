// keylight.h — i 20 LED RGB (SK6812) dentro i tasti Cherry MX.
//
// Ogni tasto della scheda nuova ha il suo LED indirizzabile, tutti in catena su
// un filo solo. Qui dentro c'e' il driver (RMT, scritto a mano: non serve una
// libreria per venti LED) e la logica che decide di che colore devono essere.
//
// L'ordine della catena non e' deducibile dallo schematico oltre al primo LED,
// quindi non lo si indovina: c'e' una procedura di apprendimento che accende un
// LED alla volta e aspetta che venga premuto il tasto giusto. Il risultato
// finisce in NVS e non si rifa' mai piu'.
#pragma once

#include <Arduino.h>

#include "input_handler.h"  // gli indici dei tasti funzione
#include "pinout.h"

// Colori delle famiglie di tasti, esposti perche' il display li usa nelle
// legende: quello che si vede sotto le dita e quello che si legge a schermo
// devono essere la stessa cosa.
enum KeyRole : uint8_t {
    ROLE_NATURAL = 0,  // tasti bianchi (DO RE MI FA SOL LA SI DO')
    ROLE_SHARP,        // tasti neri (le alterazioni)
    ROLE_FN,           // tasti funzione
    ROLE_COUNT
};

// Fotografia dello stato che decide i colori, passata ad ogni refresh.
struct LightView {
    uint16_t noteHeld;    // bit 0..12: nota premuta adesso
    uint16_t noteSound;   // bit 0..12: nota che sta effettivamente suonando
    uint8_t fnActive;     // bit 0..6: la funzione del tasto e' inserita
    uint8_t fnPending;    // bit 0..6: funzione che lampeggia (in attesa/attiva a tempo)
    int8_t seqNote;       // nota che la sequenza sta suonando, -1 se ferma
    bool seqRunning;
    bool crush;           // 8 BIT inserito: cambia la tinta di tutto il pannello
    uint8_t brightness;   // 0..8, dalla schermata impostazioni
    int8_t scaleRoot;     // tonica della scala (0..11), -1 = cromatica libera
    uint16_t scaleMask;   // bit 0..12: nota compresa nella scala scelta
    // Nessuno ha ancora premuto un tasto da quando la scheda si e' accesa: i
    // tredici tasti nota respirano insieme, piano. Non e' un lampeggio — il
    // lampeggio dice urgenza, il respiro dice possibilita' — e si spegne da solo
    // alla prima nota. E' un invito a suonare che non ha una schermata da
    // chiudere, non si ripete e non occupa un pixel di display: per chi non ha
    // mai visto un synth, e' la differenza fra un oggetto acceso e un oggetto
    // che sta aspettando lui.
    bool invite;
    // AVVIA pulsa sul movimento anche a pattern fermo: chi non ha mai visto un
    // sequencer capisce cos'e' il tempo guardando un tasto battere, e girando la
    // manopola TEMPO lo vede cambiare prima ancora di premere qualcosa.
    bool tempoPulse;
    // Sulla schermata SUONI i tredici tasti non sono piu' una tastiera: sono
    // tredici cose diverse, e disegnarli come bianchi e neri direbbe una cosa
    // falsa. Prendono tredici tinte in fila, cosi' si vede a colpo d'occhio che
    // qui ogni tasto e' per conto suo.
    bool memeMode;
};

namespace Keylight {

void begin();

// Colori e refresh. Da chiamare a ritmo di display (~30 fps): il driver RMT
// impiega 600 us a scrivere la catena, il resto e' aritmetica.
void update(uint32_t now, const LightView &v);

// Spegne tutto (ingresso in modalita' rete, spegnimento ordinato).
void allOff();

// --- apprendimento dell'ordine della catena ---
// Accende un LED alla volta: il chiamante deve dire quale tasto e' stato premuto
// e la mappa si costruisce da sola.
void startLearn();
bool learning();
uint8_t learnIndex();          // LED acceso adesso, 0..KEYLED_COUNT-1
void learnAssign(uint8_t key); // il tasto `key` (indice di matrice) e' quello acceso
void cancelLearn();

// Mappa tasto -> posizione nella catena. Serve a storage per salvarla.
const uint8_t *map();
void setMap(const uint8_t *m);  // da NVS; ignora le mappe non valide
void resetMap();                // torna all'ordine di fabbrica

}  // namespace Keylight
