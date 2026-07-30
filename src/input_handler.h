// input_handler.h — scansione debounced di tutti i controlli del pannello.
#pragma once

#include <Arduino.h>

#include "pinout.h"

#define DEBOUNCE_MS 15
#define HOLD_LONG_PRESS_MS 600
#define REC_LONG_PRESS_MS 600
// Piu' lungo degli altri: da qui si accende la radio, e non deve poter succedere
// per una pressione distratta durante una session.
#define DISPLAY_LONG_PRESS_MS 1000

namespace Input {

void begin();
void update();  // da chiamare ad ogni giro di loop() sul core 1

// --- note ---
// Last-note-priority: -1 se nessun tasto e' premuto.
int currentNote();
// Note tenute in ordine di pressione (per l'arpeggiator).
int heldCount();
int heldNoteByOrder(int index);

// Coda degli attacchi di nota: restituisce il prossimo tasto premuto dall'ultima
// lettura, -1 se non ce ne sono. Serve a registrare e a scrivere in STEP EDIT,
// dove conta il singolo attacco e non la nota risultante dalla priorita'.
int consumeNoteOn();

// --- joystick (fronte di discesa + auto-repeat mentre e' tenuto) ---
bool joyUp();
bool joyDown();
bool joyLeft();
bool joyRight();

// --- pulsanti funzione (solo fronte di discesa) ---
bool playPressed();
bool arpPressed();
bool bpmPressed();
bool polyPressed();  // ex tasto DO': commuta MONO / POLIFONICO

// Nota tenuta adesso: true se il tasto `note` (0..NOTE_COUNT-1) e' premuto.
// Serve alla modalita' polifonica, dove non basta sapere qual e' l'ultima.
bool noteIsHeld(int note);

// --- pulsanti a doppia funzione ---
// Lo short-press scatta al rilascio (prima della soglia), il long-press appena la
// soglia viene superata, col tasto ancora premuto: cosi' le due funzioni non si
// pestano i piedi.
bool holdShortPress();  // HOLD latch
bool holdLongPress();   // ADSR edit mode
bool recShortPress();   // REC / stop registrazione
bool recLongPress();    // STEP EDIT mode
bool displayShortPress();  // scorre le schermate
bool displayLongPress();   // attiva la modalita' NETWORK (solo da quella schermata)

// Stato istantaneo di HOLD: durante il record fa da tasto di cancellazione.
bool holdIsDown();

// --- encoder rotativi ---
// Scatti (detent) accumulati dall'ultima chiamata: positivo = senso orario.
// La lettura azzera l'accumulo, quindi va chiamata una volta per giro di loop.
int enc1Delta();
int enc2Delta();

// Click dell'albero, se cablato (PIN_ENC*_SW >= 0): sempre false se scollegato.
bool enc1Click();
bool enc2Click();

}  // namespace Input
