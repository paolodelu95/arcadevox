// sequencer.cpp — 16 step, editor passo-passo, record quantizzato in overdub.
//
// Durata di uno step = un sedicesimo = 60000 / bpm / 4 ms. Il ricalcolo e'
// immediato: cambiando tempo anche a sequenza in corso, il tick successivo usa
// gia' la nuova durata.
//
// La testina (`stepIndex`) indica lo step che sta suonando adesso. Alla partenza
// viene messa sull'ultimo step, cosi' il primo tick la porta sullo 0.

#include "sequencer.h"

#include "audio_engine.h"
#include "pinout.h"  // NOTE_COUNT

namespace {

const int BPM_PRESETS[BPM_PRESET_COUNT] = {40, 60, 80, 100, 120, 140, 160, 180};

Sequencer::Step steps[SEQ_STEPS];
Sequencer::Mode currentMode = Sequencer::SEQ_IDLE;

int bpmValue = 120;
uint8_t stepIndex = SEQ_STEPS - 1;
uint32_t nextTickAt = 0;
uint8_t countInSteps = 0;

int8_t seqNote = -1;
int8_t seqOct = 0;
bool triggerFlag = false;

bool editActive = false;
uint8_t cursorPos = 0;
uint16_t rev = 0;

void clearPattern() {
    for (int i = 0; i < SEQ_STEPS; ++i) {
        steps[i].note = SEQ_REST;
        steps[i].oct = 0;
    }
    ++rev;
}

// Manda in uscita lo step sotto la testina.
void playCurrentStep() {
    const Sequencer::Step &s = steps[stepIndex];
    if (s.note == SEQ_TIE) {
        // Legato: la nota in corso prosegue senza che l'inviluppo riparta.
        return;
    }
    if (s.note == SEQ_REST) {
        seqNote = -1;
        return;
    }
    seqNote = s.note;
    seqOct = s.oct;
    triggerFlag = true;
}

void startTransport(Sequencer::Mode m, uint32_t now) {
    currentMode = m;
    stepIndex = SEQ_STEPS - 1;  // il primo tick porta sullo 0
    seqNote = -1;
    triggerFlag = false;
    nextTickAt = now;
}

}  // namespace

namespace Sequencer {

void begin() {
    clearPattern();
    stop();
}

int stepDurationMs() {
    int d = 60000 / bpmValue / SEQ_PER_BEAT;
    return (d < 1) ? 1 : d;
}

void update(uint32_t now, bool erase) {
    if (currentMode == SEQ_IDLE) return;
    if ((int32_t)(now - nextTickAt) < 0) return;

    const int dur = stepDurationMs();
    // Griglia ancorata al tick precedente: niente deriva accumulata. Se pero'
    // siamo in ritardo di piu' di uno step (BPM appena alzato, refresh lungo del
    // display) ci si riaggancia al presente invece di rincorrere i tick persi.
    nextTickAt += (uint32_t)dur;
    if ((int32_t)(now - nextTickAt) > dur) nextTickAt = now + (uint32_t)dur;

    stepIndex = (uint8_t)((stepIndex + 1) % SEQ_STEPS);

    if (currentMode == SEQ_COUNTIN) {
        if (countInSteps > 0) --countInSteps;
        if (countInSteps == 0) currentMode = SEQ_RECORDING;
    } else if (currentMode == SEQ_RECORDING && erase) {
        steps[stepIndex].note = SEQ_REST;
        steps[stepIndex].oct = 0;
        ++rev;
    }

    playCurrentStep();

    // Click sui movimenti: guida il preconteggio e la registrazione, muto in play.
    if (currentMode != SEQ_PLAYING && (stepIndex % SEQ_PER_BEAT) == 0) {
        AudioEngine::click(stepIndex == 0);
    }
}

void noteEvent(uint32_t now, int note, int8_t oct) {
    if (currentMode != SEQ_RECORDING) return;
    if (note < 0 || note >= NOTE_COUNT) return;

    // Quantizzazione al sedicesimo *piu' vicino*: se manca meno di mezzo step
    // alla prossima linea di griglia, la nota le appartiene gia'. E' la
    // differenza fra dover essere perfetti e poter suonare come un umano.
    const int dur = stepDurationMs();
    const int32_t toNext = (int32_t)(nextTickAt - now);
    uint8_t target = stepIndex;
    if (toNext < dur / 2) target = (uint8_t)((stepIndex + 1) % SEQ_STEPS);

    steps[target].note = (int8_t)note;
    steps[target].oct = oct;
    ++rev;
}

void toggleRecord() {
    const uint32_t now = millis();
    switch (currentMode) {
        case SEQ_RECORDING:
            // Fine dell'overdub: il loop prosegue con quello che c'e'.
            currentMode = SEQ_PLAYING;
            break;
        case SEQ_COUNTIN:
            stop();  // ripensamento durante il preconteggio
            break;
        case SEQ_PLAYING:
            // Il loop sta gia' girando e si sente: si entra in overdub subito,
            // senza preconteggio.
            currentMode = SEQ_RECORDING;
            break;
        case SEQ_IDLE:
        default:
            // Da fermo serve un riferimento: una battuta di click, durante la
            // quale il pattern esistente suona gia'.
            startTransport(SEQ_COUNTIN, now);
            countInSteps = SEQ_STEPS;
            break;
    }
}

void togglePlay() {
    if (currentMode == SEQ_IDLE) {
        startTransport(SEQ_PLAYING, millis());
    } else {
        stop();
    }
}

void stop() {
    currentMode = SEQ_IDLE;
    stepIndex = SEQ_STEPS - 1;
    countInSteps = 0;
    seqNote = -1;
    triggerFlag = false;
}

// ------------------------------------------------------------------- editor

void setEditing(bool on) {
    if (on == editActive) return;
    editActive = on;
    // Entrando, il cursore si posiziona dove si sta guardando: sulla testina se
    // il loop gira, altrimenti all'inizio del pattern.
    if (on) cursorPos = (currentMode == SEQ_IDLE) ? 0 : stepIndex;
}

void toggleEditing() { setEditing(!editActive); }

bool editing() { return editActive; }

int cursor() { return cursorPos; }

void moveCursor(int delta) {
    int p = ((int)cursorPos + delta) % SEQ_STEPS;
    if (p < 0) p += SEQ_STEPS;
    cursorPos = (uint8_t)p;
}

void writeAtCursor(int note, int8_t oct) {
    steps[cursorPos].note = (int8_t)note;
    steps[cursorPos].oct = (note < 0) ? 0 : oct;
    ++rev;
    // Step input: il cursore avanza da solo, cosi' si scrive una melodia
    // premendo un tasto dopo l'altro senza altri comandi.
    moveCursor(1);
}

// -------------------------------------------------------------------- tempo

void cycleBpm() {
    // Primo preset piu' veloce di dove siamo adesso, poi si riparte dal basso:
    // la leva resta prevedibile anche dopo una regolazione fine con l'encoder.
    for (int i = 0; i < BPM_PRESET_COUNT; ++i) {
        if (BPM_PRESETS[i] > bpmValue) {
            setBpm(BPM_PRESETS[i]);
            return;
        }
    }
    setBpm(BPM_PRESETS[0]);
}

void nudgeBpm(int steps_) { setBpm(bpmValue + steps_); }

void setBpm(int value) {
    if (value < BPM_MIN) value = BPM_MIN;
    if (value > BPM_MAX) value = BPM_MAX;
    bpmValue = value;

    // Se il nuovo step e' piu' corto dell'attesa residua, il tick va anticipato:
    // il cambio di tempo si sente subito invece che dal movimento successivo.
    if (currentMode != SEQ_IDLE) {
        const uint32_t now = millis();
        const uint32_t dur = (uint32_t)stepDurationMs();
        if ((int32_t)(nextTickAt - now) > (int32_t)dur) nextTickAt = now + dur;
    }
}

int bpm() { return bpmValue; }

// -------------------------------------------------------------------- stato

Mode mode() { return currentMode; }

int currentStep() { return stepIndex; }

int countInBeats() {
    if (currentMode != SEQ_COUNTIN) return 0;
    return (countInSteps + SEQ_PER_BEAT - 1) / SEQ_PER_BEAT;
}

const Step &stepAt(int index) { return steps[index & (SEQ_STEPS - 1)]; }

uint16_t revision() { return rev; }

int outputNote() { return (currentMode == SEQ_IDLE) ? -1 : seqNote; }

int8_t outputOctave() { return seqOct; }

bool consumeTrigger() {
    bool v = triggerFlag;
    triggerFlag = false;
    return v;
}

// -------------------------------------------------------------- persistenza

void *patternData() { return steps; }

size_t patternSize() { return sizeof(steps); }

void clearAll() {
    stop();
    for (int i = 0; i < SEQ_STEPS; ++i) {
        steps[i].note = SEQ_REST;
        steps[i].oct = 0;
    }
    cursorPos = 0;
    ++rev;
}

void patternChanged() { ++rev; }

void sanitizePattern() {
    for (int i = 0; i < SEQ_STEPS; ++i) {
        Step &s = steps[i];
        if (s.note >= NOTE_COUNT || s.note < SEQ_TIE) {
            s.note = SEQ_REST;
            s.oct = 0;
        }
        if (s.oct < -2) s.oct = -2;
        if (s.oct > 2) s.oct = 2;
    }
    ++rev;
}

}  // namespace Sequencer
