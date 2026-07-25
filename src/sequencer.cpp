// sequencer.cpp — 16 step quantizzati, record in tempo reale, loop di playback.
//
// Durata di uno step = sedicesimo di nota = 60000 / bpm / 4 ms.
// Il ricalcolo e' immediato: cambiando preset BPM anche a sequenza in corso, il
// tick successivo usa gia' la nuova durata.

#include "sequencer.h"

namespace {

const int BPM_PRESETS[BPM_PRESET_COUNT] = {60, 80, 100, 120, 140, 160, 180};

int8_t steps[SEQ_STEPS];
Sequencer::Mode currentMode = Sequencer::SEQ_IDLE;
uint8_t bpmIndex = 3;  // 120 BPM di default
uint8_t stepIndex = 0;
uint32_t nextTickAt = 0;
int8_t seqNote = -1;
bool tickFlag = false;

void clearPattern() {
    for (int i = 0; i < SEQ_STEPS; ++i) steps[i] = -1;
}

void startPlayback(uint32_t now) {
    currentMode = Sequencer::SEQ_PLAYING;
    stepIndex = 0;
    seqNote = -1;
    nextTickAt = now;  // primo step immediato
}

void stopAll() {
    currentMode = Sequencer::SEQ_IDLE;
    seqNote = -1;
    stepIndex = 0;
    tickFlag = false;
}

}  // namespace

namespace Sequencer {

void begin() {
    clearPattern();
    stopAll();
}

int stepDurationMs() { return 60000 / BPM_PRESETS[bpmIndex] / 4; }

void update(uint32_t now, int liveNote) {
    if (currentMode == SEQ_IDLE) return;
    if ((int32_t)(now - nextTickAt) < 0) return;

    nextTickAt = now + (uint32_t)stepDurationMs();
    tickFlag = true;

    if (currentMode == SEQ_RECORDING) {
        // Registra cio' che e' tenuto premuto nel preciso istante del tick.
        steps[stepIndex] = (int8_t)liveNote;
        ++stepIndex;
        if (stepIndex >= SEQ_STEPS) {
            // Pattern completo: parte subito il playback in loop, suonando
            // gia' lo step 0 su questo stesso tick.
            startPlayback(now);
            seqNote = steps[0];
            stepIndex = 1;
            nextTickAt = now + (uint32_t)stepDurationMs();
        }
        return;
    }

    // SEQ_PLAYING
    seqNote = steps[stepIndex];
    stepIndex = (stepIndex + 1) % SEQ_STEPS;
}

void toggleRecord() {
    uint32_t now = millis();
    if (currentMode == SEQ_RECORDING) {
        // Stop manuale anticipato: gli step rimanenti restano silenzio,
        // poi si passa comunque in PLAY.
        for (uint8_t i = stepIndex; i < SEQ_STEPS; ++i) steps[i] = -1;
        startPlayback(now);
        return;
    }
    clearPattern();
    currentMode = SEQ_RECORDING;
    stepIndex = 0;
    seqNote = -1;
    nextTickAt = now;  // il primo tick campiona subito
}

void togglePlay() {
    if (currentMode == SEQ_IDLE) {
        startPlayback(millis());
    } else {
        // Ferma sia il playback che un'eventuale registrazione in corso.
        stopAll();
    }
}

void cycleBpm() {
    bpmIndex = (bpmIndex + 1) % BPM_PRESET_COUNT;
    // La nuova durata vale gia' dal prossimo tick.
    uint32_t now = millis();
    if (currentMode != SEQ_IDLE) {
        uint32_t dur = (uint32_t)stepDurationMs();
        if ((int32_t)(nextTickAt - now) > (int32_t)dur) nextTickAt = now + dur;
    }
}

Mode mode() { return currentMode; }

int currentStep() {
    if (currentMode == SEQ_RECORDING) return stepIndex;
    // In play `stepIndex` punta gia' allo step successivo: mostro quello suonato.
    return (stepIndex + SEQ_STEPS - 1) % SEQ_STEPS;
}

int bpm() { return BPM_PRESETS[bpmIndex]; }

int outputNote() { return (currentMode == SEQ_PLAYING) ? seqNote : -1; }

bool consumeTick() {
    bool v = tickFlag;
    tickFlag = false;
    return v;
}

}  // namespace Sequencer
