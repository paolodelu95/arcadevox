// sequencer.h — step-sequencer quantizzato a 16 step, stile drum machine.
#pragma once

#include <Arduino.h>

#define SEQ_STEPS 16
#define BPM_PRESET_COUNT 7

namespace Sequencer {

enum Mode : uint8_t {
    SEQ_IDLE = 0,
    SEQ_RECORDING,
    SEQ_PLAYING
};

void begin();

// Da chiamare ogni giro: `liveNote` e' la nota suonata dal vivo in questo istante
// (-1 = silenzio), usata per il record in tempo reale.
void update(uint32_t now, int liveNote);

void toggleRecord();  // REC
void togglePlay();    // PLAY/STOP
void cycleBpm();      // leva BPM: passa al preset successivo

Mode mode();
int currentStep();     // 0..15
int bpm();
int stepDurationMs();

// Nota che la sequenza vuole suonare adesso (-1 = silenzio / non in play).
int outputNote();
// True una sola volta per ogni tick della griglia (per ritriggerare l'inviluppo).
bool consumeTick();

}  // namespace Sequencer
