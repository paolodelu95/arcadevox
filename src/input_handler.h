// input_handler.h — scansione debounced di tutti i controlli del pannello.
#pragma once

#include <Arduino.h>

#include "pinout.h"

#define DEBOUNCE_MS 15
#define HOLD_LONG_PRESS_MS 600

namespace Input {

void begin();
void update();  // da chiamare ad ogni giro di loop() sul core 1

// --- note ---
// Last-note-priority: -1 se nessun tasto e' premuto.
int currentNote();
// Note tenute in ordine di pressione (per l'arpeggiator).
int heldCount();
int heldNoteByOrder(int index);

// --- joystick (fronte di discesa + auto-repeat mentre e' tenuto) ---
bool joyUp();
bool joyDown();
bool joyLeft();
bool joyRight();

// --- pulsanti funzione (solo fronte di discesa) ---
bool displayPressed();
bool recPressed();
bool playPressed();
bool arpPressed();
bool bpmPressed();

// --- HOLD / ADSR edit ---
bool holdShortPress();  // rilasciato prima di 600 ms
bool holdLongPress();   // scatta appena si superano i 600 ms, ancora premuto

// --- encoder rotativi ---
// Scatti (detent) accumulati dall'ultima chiamata: positivo = senso orario.
// La lettura azzera l'accumulo, quindi va chiamata una volta per giro di loop.
int enc1Delta();
int enc2Delta();

// Click dell'albero, se cablato (PIN_ENC*_SW >= 0): sempre false se scollegato.
bool enc1Click();
bool enc2Click();

}  // namespace Input
