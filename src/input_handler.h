// input_handler.h — scansione della matrice, dei 4 encoder e del joystick.
//
// Rispetto alla scheda vecchia cambia la sorgente, non l'idea: i tasti non sono
// piu' un GPIO ciascuno ma una matrice 4x5 su un MCP23017, e gli encoder sono
// quattro invece di due. Il resto del firmware continua a chiedere "quale nota
// e' premuta" senza sapere da dove arriva la risposta.
#pragma once

#include <Arduino.h>

#include "pinout.h"

#define DEBOUNCE_MS 12
// L'unica soglia rimasta, ed e' quella del solo gesto che non si torna indietro:
// svuotare i sedici passi. Novecento millisecondi non sono un'attesa, sono il
// tempo di cambiare idea — e adesso si vedono, perche' l'anello esterno si
// riempie mentre tieni premuto.
#define FN_LONG_PRESS_SLOW_MS 900

// Indici dei tasti funzione, nell'ordine in cui stanno sul pannello.
//
// Una parola stampata sopra, una cosa sola sotto. Prima ognuno dei sette ne
// faceva due — una breve e una lunga — e le seconde erano invisibili: nessun
// tasto ti dice che tenendolo premuto cambia mestiere, e chi non lo sapeva non
// aveva modo di scoprirlo. Le sei funzioni nascoste non sono sparite, sono
// andate dove si vedono: il modo dell'arpeggiator e la grana dell'8 BIT
// nell'elenco EFFETTI, lo step edit dentro la schermata RITMO (che adesso ha
// sempre il cursore), il modo accordo sulla quarta manopola di TIMBRI, e il
// panico allo scoperto su FN7 — che prima duplicava il joystick.
//
// L'unica pressione lunga superstite e' su AVVIA, e non e' una seconda
// funzione: e' una richiesta di conferma. Svuotare sedici passi e' l'unico
// gesto distruttivo dello strumento, e l'attesa serve a poter cambiare idea.
enum {
    FN_ARP = 0,   // arpeggiator acceso/spento
    FN_CRUSH,     // 8 BIT acceso/spento
    FN_REC,       // registra
    FN_PLAY,      // avvia/ferma          | lungo: svuota il pattern
    FN_HOLD,      // tieni (latch)
    FN_POLY,      // mono/polifonico
    FN_SILENCE    // panico: zittisce tutto
};

namespace Input {

void begin();
void update();  // da chiamare ad ogni giro di loop() sul core 1

// L'espansore risponde? Se il bus I2C cade, tastiera ed encoder-click muoiono
// insieme: meglio dirlo sul display che lasciar credere a una scheda rotta.
bool expanderOk();
// Quante volte la scansione ha dovuto ritentare: solo per la schermata di
// diagnostica.
uint32_t expanderErrors();

// --- note ---
// Last-note-priority: -1 se nessun tasto e' premuto.
int currentNote();
// Note tenute in ordine di pressione (per l'arpeggiator).
int heldCount();
int heldNoteByOrder(int index);
// Coda degli attacchi: il prossimo tasto premuto dall'ultima lettura, -1 se
// non ce ne sono. Serve a registrare e a scrivere in STEP EDIT.
int consumeNoteOn();
// Nota tenuta adesso (serve alla modalita' polifonica).
bool noteIsHeld(int note);

// --- joystick (fronte di discesa + auto-repeat mentre e' tenuto) ---
bool joyUp();
bool joyDown();
bool joyLeft();
bool joyRight();

// --- tasti funzione ---
// Su sei tasti su sette lo short-press scatta al rilascio e basta, comunque a
// lungo li si sia tenuti: un tasto che non fa niente perche' l'hai premuto
// "troppo" e' esattamente il genere di sorpresa che questo schema toglie di
// mezzo. Solo su AVVIA c'e' anche il long-press, che scatta alla soglia col
// tasto ancora giu' e in quel caso il breve non arriva.
bool fnShortPress(int fn);
bool fnLongPress(int fn);
bool fnIsDown(int fn);
// Da quanti millisecondi e' premuto, 0 se non lo e'. Serve a far vedere il
// caricamento della conferma: una pressione lunga che non mostra di essere in
// corso e' indistinguibile da un tasto che non funziona.
uint32_t fnHeldMs(int fn);

// --- encoder rotativi (0..3) ---
// Scatti accumulati dall'ultima chiamata: positivo = senso orario. La lettura
// azzera l'accumulo, quindi va chiamata una volta per giro di loop.
int encDelta(int which);
// Click dell'albero (fronte di discesa).
bool encClick(int which);
// Rilascio dell'albero (fronte di salita). Il ripristino di un parametro deve
// scattare quando lasci, non quando premi: finche' tieni giu' il click la
// manopola e' in passo fine, e se il ripristino partisse subito i due gesti si
// pesterebbero i piedi ad ogni tentativo di regolazione precisa.
bool encRelease(int which);
bool encIsDown(int which);

// --- inattivita' ---
// Millisecondi dall'ultimo comando toccato, di qualunque genere: tasti, joystick,
// alberi, manopole. Un tasto **tenuto** conta come attivita' finche' e' giu'.
// Serve al ritorno automatico alla schermata SUONA, e sta qui perche' questo e'
// l'unico file che vede tutti i comandi insieme.
uint32_t idleMs();

}  // namespace Input
