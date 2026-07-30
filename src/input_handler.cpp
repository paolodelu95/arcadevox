// input_handler.cpp — debounce, last-note-priority, press order, encoder in quadratura.
//
// Tutti i contatti sono verso GND con pull-up interno: premuto = LOW.

#include "input_handler.h"

namespace {

// ------------------------------------------------------------------- debounce
struct Button {
    int8_t pin;  // -1 = non cablato
    bool state;  // true = premuto (gia' debounced)
    bool rawLast;
    uint32_t lastChange;
    bool edgeDown;  // fronte premuto, consumato dal chiamante
    bool edgeUp;
    uint32_t pressedAt;
};

// Indici nell'array `buttons`.
enum {
    B_NOTE0 = 0,  // NOTE_COUNT note consecutive: DO..SI
    B_JOY_UP = NOTE_COUNT,
    B_JOY_DOWN,
    B_JOY_LEFT,
    B_JOY_RIGHT,
    B_DISPLAY,
    B_REC,
    B_PLAY,
    B_HOLD,
    B_ARP,
    B_BPM,
    B_POLY,
    B_ENC1_SW,
    B_ENC2_SW,
    B_COUNT
};

Button buttons[B_COUNT];

const int8_t BUTTON_PINS[B_COUNT] = {
    PIN_NOTE_DO,  PIN_NOTE_RE,  PIN_NOTE_MI,   PIN_NOTE_FA,
    PIN_NOTE_SOL, PIN_NOTE_LA,  PIN_NOTE_SI,
    PIN_JOY_UP,   PIN_JOY_DOWN, PIN_JOY_LEFT,  PIN_JOY_RIGHT,
    PIN_BTN_DISPLAY, PIN_BTN_REC, PIN_BTN_PLAY, PIN_BTN_HOLD,
    PIN_LEVER_ARP, PIN_LEVER_BPM, PIN_BTN_POLY,
    PIN_ENC1_SW,  PIN_ENC2_SW,
};

// ---------------------------------------------------------- encoder (quadratura)
//
// Macchina a stati "full step" di Ben Buxton: emette un solo evento per detent e
// ignora i rimbalzi dei contatti, che con gli encoder meccanici sono la norma.
// Ogni transizione su A o B genera un interrupt, cosi' nessuno scatto va perso
// nemmeno mentre il loop e' occupato a ridisegnare il display.

#define R_START 0x0
#define R_CW_FINAL 0x1
#define R_CW_BEGIN 0x2
#define R_CW_NEXT 0x3
#define R_CCW_BEGIN 0x4
#define R_CCW_FINAL 0x5
#define R_CCW_NEXT 0x6

#define DIR_CW 0x10
#define DIR_CCW 0x20

const uint8_t ENC_TABLE[7][4] = {
    // R_START
    {R_START, R_CW_BEGIN, R_CCW_BEGIN, R_START},
    // R_CW_FINAL
    {R_CW_NEXT, R_START, R_CW_FINAL, R_START | DIR_CW},
    // R_CW_BEGIN
    {R_CW_NEXT, R_CW_BEGIN, R_START, R_START},
    // R_CW_NEXT
    {R_CW_NEXT, R_CW_BEGIN, R_CW_FINAL, R_START},
    // R_CCW_BEGIN
    {R_CCW_NEXT, R_START, R_CCW_BEGIN, R_START},
    // R_CCW_FINAL
    {R_CCW_NEXT, R_CCW_FINAL, R_START, R_START | DIR_CCW},
    // R_CCW_NEXT
    {R_CCW_NEXT, R_CCW_FINAL, R_CCW_BEGIN, R_START},
};

struct Encoder {
    uint8_t pinA;
    uint8_t pinB;
    volatile uint8_t state;
    volatile int32_t count;  // contatore monotono, mai azzerato dall'ISR
    int32_t lastRead;        // usato solo dal loop
};

Encoder encoders[2];

// Contatore monotono + lastRead: il consumo del delta non ha bisogno di sezioni
// critiche e non puo' perdere scatti arrivati tra la lettura e l'azzeramento.
void IRAM_ATTR encoderStep(Encoder &e) {
    uint8_t pinState = (uint8_t)((digitalRead(e.pinB) << 1) | digitalRead(e.pinA));
    e.state = ENC_TABLE[e.state & 0x0f][pinState];
    uint8_t dir = e.state & 0x30;
    if (dir == DIR_CW) {
        ++e.count;
    } else if (dir == DIR_CCW) {
        --e.count;
    }
}

void IRAM_ATTR isrEnc0() { encoderStep(encoders[0]); }
void IRAM_ATTR isrEnc1() { encoderStep(encoders[1]); }

void encoderBegin(Encoder &e, uint8_t pinA, uint8_t pinB, void (*isr)()) {
    e.pinA = pinA;
    e.pinB = pinB;
    e.state = R_START;
    e.count = 0;
    e.lastRead = 0;
    pinMode(pinA, INPUT_PULLUP);
    pinMode(pinB, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(pinA), isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(pinB), isr, CHANGE);
}

int encoderConsume(Encoder &e) {
    int32_t now = e.count;
    int32_t delta = now - e.lastRead;
    e.lastRead = now;
    return (int)delta;
}

// -------------------------------------------------------- ordine di pressione note
int8_t pressOrder[NOTE_COUNT];
uint8_t pressCount = 0;

// Coda circolare degli attacchi. Piu' capiente del numero di tasti: in un solo
// giro di loop se ne possono accumulare comunque solo NOTE_COUNT, ma cosi' resta
// spazio anche se il chiamante salta un giro.
constexpr uint8_t NOTE_QUEUE_SIZE = 16;
int8_t noteOnQueue[NOTE_QUEUE_SIZE];
uint8_t noteOnHead = 0;
uint8_t noteOnTail = 0;

void pushNoteOn(int note) {
    uint8_t next = (uint8_t)((noteOnTail + 1) % NOTE_QUEUE_SIZE);
    if (next == noteOnHead) return;  // coda piena: l'evento piu' vecchio ha la precedenza
    noteOnQueue[noteOnTail] = (int8_t)note;
    noteOnTail = next;
}

// ------------------------------------------------------------- auto-repeat joystick
constexpr uint32_t REPEAT_DELAY_MS = 400;  // prima ripetizione
constexpr uint32_t REPEAT_RATE_MS = 90;    // ripetizioni successive
uint32_t joyNextRepeat[4] = {0, 0, 0, 0};
bool joyEvent[4] = {false, false, false, false};

// ------------------------------------------------------- pressione breve/lunga
// Stessa meccanica per HOLD, REC e DISPLAY: ognuno porta due funzioni distinte
// senza bisogno di altri pulsanti sul pannello, che sono finiti.
struct PressTracker {
    uint8_t button;    // indice in `buttons`
    uint32_t longMs;   // soglia
    bool longFired;    // il long-press di questa pressione e' gia' scattato
    bool shortEvent;
    bool longEvent;
};

PressTracker pressTrackers[] = {
    {B_HOLD, HOLD_LONG_PRESS_MS, false, false, false},
    {B_REC, REC_LONG_PRESS_MS, false, false, false},
    {B_DISPLAY, DISPLAY_LONG_PRESS_MS, false, false, false},
};

enum { T_HOLD = 0, T_REC, T_DISPLAY, T_COUNT };

void notePressed(int note) {
    for (uint8_t i = 0; i < pressCount; ++i) {
        if (pressOrder[i] == note) return;  // gia' presente
    }
    if (pressCount < NOTE_COUNT) pressOrder[pressCount++] = (int8_t)note;
}

void noteReleased(int note) {
    for (uint8_t i = 0; i < pressCount; ++i) {
        if (pressOrder[i] == note) {
            for (uint8_t j = i; j + 1 < pressCount; ++j) pressOrder[j] = pressOrder[j + 1];
            --pressCount;
            return;
        }
    }
}

bool consume(bool &flag) {
    bool v = flag;
    flag = false;
    return v;
}

}  // namespace

namespace Input {

void begin() {
    for (int i = 0; i < B_COUNT; ++i) {
        buttons[i].pin = BUTTON_PINS[i];
        buttons[i].state = false;
        buttons[i].rawLast = false;
        buttons[i].lastChange = 0;
        buttons[i].edgeDown = false;
        buttons[i].edgeUp = false;
        buttons[i].pressedAt = 0;
        if (buttons[i].pin >= 0) pinMode((uint8_t)buttons[i].pin, INPUT_PULLUP);
    }

    encoderBegin(encoders[0], PIN_ENC1_A, PIN_ENC1_B, isrEnc0);
    encoderBegin(encoders[1], PIN_ENC2_A, PIN_ENC2_B, isrEnc1);
}

void update() {
    const uint32_t now = millis();

    for (int i = 0; i < B_COUNT; ++i) {
        Button &b = buttons[i];
        if (b.pin < 0) continue;  // click encoder non cablato
        bool raw = (digitalRead((uint8_t)b.pin) == LOW);
        if (raw != b.rawLast) {
            b.rawLast = raw;
            b.lastChange = now;
        }
        if (raw != b.state && (now - b.lastChange) >= DEBOUNCE_MS) {
            b.state = raw;
            if (raw) {
                b.edgeDown = true;
                b.pressedAt = now;
            } else {
                b.edgeUp = true;
            }
        }
    }

    // --- note: aggiorno l'ordine di pressione ---
    for (int n = 0; n < NOTE_COUNT; ++n) {
        Button &b = buttons[B_NOTE0 + n];
        if (consume(b.edgeDown)) {
            notePressed(n);
            pushNoteOn(n);
        }
        if (consume(b.edgeUp)) noteReleased(n);
    }

    // --- joystick: fronte + auto-repeat (utile per Decay/Sustain in edit mode) ---
    for (int j = 0; j < 4; ++j) {
        Button &b = buttons[B_JOY_UP + j];
        if (consume(b.edgeDown)) {
            joyEvent[j] = true;
            joyNextRepeat[j] = now + REPEAT_DELAY_MS;
        }
        if (b.state) {
            if ((int32_t)(now - joyNextRepeat[j]) >= 0) {
                joyEvent[j] = true;
                joyNextRepeat[j] = now + REPEAT_RATE_MS;
            }
        }
        consume(b.edgeUp);
    }

    // --- pulsanti a doppia funzione: breve vs long-press ---
    for (int i = 0; i < T_COUNT; ++i) {
        PressTracker &t = pressTrackers[i];
        Button &b = buttons[t.button];
        if (consume(b.edgeDown)) t.longFired = false;
        if (b.state && !t.longFired && (now - b.pressedAt) >= t.longMs) {
            t.longFired = true;
            t.longEvent = true;
        }
        if (consume(b.edgeUp)) {
            if (!t.longFired) t.shortEvent = true;
        }
    }
}

int currentNote() { return (pressCount > 0) ? pressOrder[pressCount - 1] : -1; }

int heldCount() { return pressCount; }

int heldNoteByOrder(int index) {
    if (index < 0 || index >= (int)pressCount) return -1;
    return pressOrder[index];
}

bool noteIsHeld(int note) {
    if (note < 0 || note >= NOTE_COUNT) return false;
    return buttons[B_NOTE0 + note].state;
}

int consumeNoteOn() {
    if (noteOnHead == noteOnTail) return -1;
    int n = noteOnQueue[noteOnHead];
    noteOnHead = (uint8_t)((noteOnHead + 1) % NOTE_QUEUE_SIZE);
    return n;
}

bool joyUp() { return consume(joyEvent[0]); }
bool joyDown() { return consume(joyEvent[1]); }
bool joyLeft() { return consume(joyEvent[2]); }
bool joyRight() { return consume(joyEvent[3]); }

bool playPressed() { return consume(buttons[B_PLAY].edgeDown); }
bool arpPressed() { return consume(buttons[B_ARP].edgeDown); }
bool bpmPressed() { return consume(buttons[B_BPM].edgeDown); }
bool polyPressed() { return consume(buttons[B_POLY].edgeDown); }

bool holdShortPress() { return consume(pressTrackers[T_HOLD].shortEvent); }
bool holdLongPress() { return consume(pressTrackers[T_HOLD].longEvent); }
bool recShortPress() { return consume(pressTrackers[T_REC].shortEvent); }
bool recLongPress() { return consume(pressTrackers[T_REC].longEvent); }
bool displayShortPress() { return consume(pressTrackers[T_DISPLAY].shortEvent); }
bool displayLongPress() { return consume(pressTrackers[T_DISPLAY].longEvent); }

bool holdIsDown() { return buttons[B_HOLD].state; }

int enc1Delta() { return encoderConsume(encoders[0]); }
int enc2Delta() { return encoderConsume(encoders[1]); }

bool enc1Click() { return consume(buttons[B_ENC1_SW].edgeDown); }
bool enc2Click() { return consume(buttons[B_ENC2_SW].edgeDown); }

}  // namespace Input
