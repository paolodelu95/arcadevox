// sequencer.h — step-sequencer a 16 step con editing passo-passo e record quantizzato.
//
// Due assi indipendenti, come sui sequencer veri (Roland, Elektron, Akai):
//
//   - il TRASPORTO   (Mode: fermo / count-in / record / play)
//   - l'EDITOR       (attivo o no, con un cursore su uno step)
//
// Sono ortogonali: si edita il pattern anche mentre il loop sta suonando.
#pragma once

#include <Arduino.h>

#define SEQ_STEPS 16
#define SEQ_PER_BEAT 4  // step per movimento: la griglia e' in sedicesimi
#define BPM_PRESET_COUNT 8

#define BPM_MIN 40
#define BPM_MAX 240

// Valori speciali del campo `note` di uno step.
#define SEQ_REST (-1)  // silenzio
#define SEQ_TIE (-2)   // legato: tiene la nota precedente senza ritriggerare

namespace Sequencer {

// Uno step: nota della scala (0..7) piu' l'ottava in cui e' stato scritto.
//
// L'ottava e' memorizzata in modo *assoluto*, non come scostamento da quella
// globale: cio' che si scrive e' cio' che si risente, adesso e fra un'ora. Il
// joystick su/giu' resta l'ottava della tastiera dal vivo e, in STEP EDIT,
// sceglie il registro dello step che si sta per scrivere.
struct Step {
    int8_t note;  // 0..7, oppure SEQ_REST / SEQ_TIE
    int8_t oct;   // -2..+2
};

enum Mode : uint8_t {
    SEQ_IDLE = 0,
    SEQ_COUNTIN,    // una battuta di preconteggio, poi si registra
    SEQ_RECORDING,  // overdub in loop
    SEQ_PLAYING
};

void begin();

// Da chiamare ad ogni giro di loop(). `erase` = tasto di cancellazione tenuto
// premuto: svuota gli step che passano sotto la testina durante il record.
void update(uint32_t now, bool erase);

// --- trasporto ---
void toggleRecord();  // REC breve
void togglePlay();    // PLAY/STOP
void stop();

// --- registrazione dal vivo ---
// Da chiamare sul fronte di attacco di ogni nota suonata (arpeggiator incluso).
// La nota viene agganciata alla linea di griglia piu' vicina, non a quella
// appena passata: e' cio' che rende la registrazione tollerabile da un umano.
void noteEvent(uint32_t now, int note, int8_t oct);

// --- editor passo-passo ---
void setEditing(bool on);
void toggleEditing();
bool editing();
int cursor();
void moveCursor(int delta);
void writeAtCursor(int note, int8_t oct);  // nota, SEQ_REST o SEQ_TIE

// --- tempo ---
void cycleBpm();          // leva BPM: preset successivo
void nudgeBpm(int steps); // encoder: BPM continuo entro BPM_MIN..BPM_MAX
int bpm();
void setBpm(int value);
int stepDurationMs();

// --- stato ---
Mode mode();
int currentStep();  // 0..15, testina di riproduzione
// Movimenti che mancano alla fine del preconteggio (4..1), 0 se non in count-in.
int countInBeats();

const Step &stepAt(int index);
// Cambia ad ogni scrittura sul pattern: il display se ne serve per ridisegnare
// la griglia solo quando serve davvero.
uint16_t revision();

// Nota che la sequenza vuole suonare adesso (-1 = silenzio / non in play) e la
// sua ottava.
int outputNote();
int8_t outputOctave();
// True una sola volta per tick, e solo se lo step va ritriggerato (i legati no).
bool consumeTrigger();

// --- persistenza (usate da storage.*) ---
void *patternData();
size_t patternSize();
void patternChanged();  // dopo un caricamento da NVS
// Riporta in scala gli step arrivati da NVS: note fuori range diventano pause,
// ottave fuori range vengono limitate.
void sanitizePattern();

}  // namespace Sequencer
