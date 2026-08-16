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
// Soglia unica per tutti i tasti funzione: sette tasti con due significati
// ciascuno diventano ingestibili se ognuno ha i suoi tempi.
#define FN_LONG_PRESS_MS 600
// Piu' lunga delle altre: da qui si svuota il pattern e si accende la radio, e
// non deve poter succedere per una pressione distratta.
#define FN_LONG_PRESS_SLOW_MS 900

// Indici dei tasti funzione, nell'ordine in cui stanno sul pannello.
enum {
    FN_ARP = 0,   // arpeggiator on/off      | lungo: modo dell'arpeggiator
    FN_CRUSH,     // 8 BIT on/off            | lungo: profondita' del crush
    FN_REC,       // registra                | lungo: STEP EDIT
    FN_PLAY,      // play/stop               | lungo: svuota il pattern
    FN_HOLD,      // hold/latch              | lungo: ADSR EDIT
    FN_POLY,      // mono/polifonico         | lungo: modo accordo
    FN_SCREEN     // schermata successiva    | lungo: menu impostazioni
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
// Lo short-press scatta al rilascio (prima della soglia), il long-press appena
// la soglia viene superata, col tasto ancora premuto: cosi' le due funzioni non
// si pestano i piedi.
bool fnShortPress(int fn);
bool fnLongPress(int fn);
bool fnIsDown(int fn);

// --- encoder rotativi (0..3) ---
// Scatti accumulati dall'ultima chiamata: positivo = senso orario. La lettura
// azzera l'accumulo, quindi va chiamata una volta per giro di loop.
int encDelta(int which);
// Click dell'albero (fronte di discesa).
bool encClick(int which);
bool encIsDown(int which);

}  // namespace Input
