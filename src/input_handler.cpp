// input_handler.cpp — matrice su MCP23017, 4 encoder in quadratura, joystick.
//
// Tre sorgenti diverse che arrivano tutte allo stesso debounce:
//
//   matrice 4x5  -> MCP23017 via I2C   (20 tasti: 13 note + 7 funzioni)
//   encoder A/B  -> GPIO con interrupt (nessuno scatto perso durante il redraw)
//   click e joy  -> MCP e GPIO         (contatti verso GND, pull-up)
//
// Il bus I2C lo tocca solo questo file, e solo dal core 1: nessun lock serve.

#include "input_handler.h"

#include <Wire.h>

namespace {

// ============================================================================
// MCP23017
// ============================================================================
// Registri con IOCON.BANK = 0 (default all'accensione).
constexpr uint8_t REG_IODIRA = 0x00;
constexpr uint8_t REG_IODIRB = 0x01;
constexpr uint8_t REG_GPPUA = 0x0C;
constexpr uint8_t REG_GPPUB = 0x0D;
constexpr uint8_t REG_GPIOA = 0x12;
constexpr uint8_t REG_GPIOB = 0x13;
constexpr uint8_t REG_OLATB = 0x15;

// Maschera delle colonne (GPB0..GPB3) e dei click encoder (GPB4..GPB7).
constexpr uint8_t COL_MASK = 0x0F;
constexpr uint8_t ENCSW_MASK = 0xF0;

bool mcpAlive = false;
uint32_t mcpErrors = 0;

bool mcpWrite(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(MCP_ADDR);
    Wire.write(reg);
    Wire.write(value);
    if (Wire.endTransmission() == 0) return true;
    ++mcpErrors;
    return false;
}

bool mcpRead(uint8_t reg, uint8_t &value) {
    Wire.beginTransmission(MCP_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        ++mcpErrors;
        return false;
    }
    if (Wire.requestFrom((uint8_t)MCP_ADDR, (uint8_t)1) != 1) {
        ++mcpErrors;
        return false;
    }
    value = (uint8_t)Wire.read();
    return true;
}

// Configurazione: porta A tutta in ingresso con pull-up (le 5 righe), porta B
// con le 4 colonne in uscita e i 4 click in ingresso con pull-up.
bool mcpConfigure() {
    bool ok = true;
    ok &= mcpWrite(REG_IODIRA, 0xFF);
    ok &= mcpWrite(REG_GPPUA, 0xFF);
    ok &= mcpWrite(REG_IODIRB, ENCSW_MASK);  // 0xF0: 0..3 uscite, 4..7 ingressi
    ok &= mcpWrite(REG_GPPUB, ENCSW_MASK);
    ok &= mcpWrite(REG_OLATB, COL_MASK);     // colonne a riposo alte
    return ok;
}

// Stato grezzo della matrice: bit a 1 = tasto premuto.
uint32_t matrixRaw = 0;
uint8_t encSwRaw = 0xF0;  // bit alti = non premuti

// Scansione completa: una colonna alla volta a livello basso, poi si legge la
// porta delle righe. Costo circa 300 us a 400 kHz.
//
// Una volta al millisecondo basta e avanza: il debounce e' di 12 ms, e il loop
// gira molto piu' spesso di cosi'. Scandire ad ogni giro terrebbe il bus I2C
// occupato per un terzo del tempo senza leggere un tasto in piu'.
constexpr uint32_t SCAN_INTERVAL_MS = 1;
uint32_t lastScan = 0;

// Quando il bus e' caduto non ha senso ritentare mille volte al secondo: ogni
// transazione fallita costa il timeout dell'I2C (circa 7 ms), quindi il loop
// rallenterebbe fino a inchiodarsi proprio nel momento in cui serve reggere —
// il display e il motore audio devono continuare a funzionare anche con la
// tastiera scollegata. Un tentativo al secondo basta a riprendersi da un
// contatto ballerino, e nel frattempo il resto del synth gira liscio.
constexpr uint32_t RETRY_INTERVAL_MS = 1000;
uint32_t nextRetry = 0;
bool reportedDown = false;

// Sul PCB **non ci sono resistenze di pull-up sull'I2C**: SDA e SCL vanno dal
// microcontrollore all'espansore e basta (verificato sulla netlist: quelle due
// reti hanno due soli membri). Il bus vive quindi sui pull-up interni
// dell'ESP32, che sono deboli — una quarantina di kiloohm — e a 400 kHz possono
// non farcela a tirare su la linea in tempo, specie con qualche centimetro di
// pista e uno zoccolo di mezzo.
//
// Invece di dare la colpa all'utente, ad ogni tentativo si alterna la velocita'
// piena e i 100 kHz: se il problema e' quello, il bus riparte da solo e la
// riga sulla seriale dice a che velocita' ha funzionato — che e' anche la
// diagnosi di "servono due resistenze da 4,7 k".
constexpr uint32_t I2C_FREQ_SLOW_HZ = 100000;
uint32_t busFreq = I2C_FREQ_HZ;

void busRestart() {
    Wire.end();
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, busFreq);
}

void matrixScan() {
    const uint32_t now = millis();
    if (!mcpAlive) {
        if ((int32_t)(now - nextRetry) < 0) return;
        nextRetry = now + RETRY_INTERVAL_MS;
    } else if (lastScan != 0 && (now - lastScan) < SCAN_INTERVAL_MS) {
        return;
    }
    lastScan = now;

    uint32_t bits = 0;
    bool ok = true;
    for (int c = 0; c < MATRIX_COLS; ++c) {
        // Solo la colonna `c` bassa. I bit alti sono ingressi: scriverli non fa
        // nulla, ma li lascio a 1 per non spegnere i pull-up per sbaglio.
        const uint8_t olat = (uint8_t)((COL_MASK & ~(1u << c)) | ENCSW_MASK);
        if (!mcpWrite(REG_OLATB, olat)) {
            ok = false;
            break;
        }
        uint8_t rows;
        if (!mcpRead(REG_GPIOA, rows)) {
            ok = false;
            break;
        }
        for (int r = 0; r < MATRIX_ROWS; ++r) {
            if ((rows & (1u << r)) == 0) bits |= 1u << KEY_AT(r, c);
        }
    }
    // I click degli encoder non dipendono dalla colonna attiva: una lettura per
    // giro basta e avanza.
    uint8_t portb;
    if (mcpRead(REG_GPIOB, portb)) {
        encSwRaw = (uint8_t)(portb & ENCSW_MASK);
    } else {
        ok = false;
    }
    mcpWrite(REG_OLATB, COL_MASK | ENCSW_MASK);  // riposo

    if (ok) {
        matrixRaw = bits;
        if (!mcpAlive) {
            mcpAlive = true;
            if (reportedDown) {
                Serial.print(F("MCP23017: bus tornato su a "));
                Serial.print(busFreq / 1000);
                Serial.println(F(" kHz, tastiera di nuovo attiva."));
                reportedDown = false;
            }
        }
    } else {
        // Bus caduto: nessun tasto risulta premuto — meglio muti che con una
        // nota appesa che nessuno rilascera' mai — e si riprova a configurare
        // l'espansore, ma solo al prossimo tentativo utile.
        if (mcpAlive || !reportedDown) {
            Serial.println(F("MCP23017: nessuna risposta sul bus I2C, riprovo ogni secondo "
                             "alternando 400 e 100 kHz."));
            reportedDown = true;
        }
        mcpAlive = false;
        matrixRaw = 0;
        encSwRaw = ENCSW_MASK;
        nextRetry = millis() + RETRY_INTERVAL_MS;
        busFreq = (busFreq == I2C_FREQ_HZ) ? I2C_FREQ_SLOW_HZ : I2C_FREQ_HZ;
        busRestart();
        mcpConfigure();
    }
}

// ============================================================================
// Debounce comune
// ============================================================================
struct Button {
    bool state;  // true = premuto (gia' debounced)
    bool rawLast;
    uint32_t lastChange;
    bool edgeDown;
    bool edgeUp;
    uint32_t pressedAt;
};

// Sorgenti: 20 tasti di matrice, 4 direzioni di joystick, 4 click di encoder.
enum {
    B_MATRIX0 = 0,
    B_JOY_UP = MATRIX_COLS * MATRIX_ROWS,
    B_JOY_DOWN,
    B_JOY_LEFT,
    B_JOY_RIGHT,
    B_ENC_SW0,
    B_COUNT = B_ENC_SW0 + 4
};

Button buttons[B_COUNT];

const uint8_t JOY_PINS[4] = {PIN_JOY_UP, PIN_JOY_DOWN, PIN_JOY_LEFT, PIN_JOY_RIGHT};

void feed(Button &b, bool raw, uint32_t now) {
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

bool consume(bool &flag) {
    bool v = flag;
    flag = false;
    return v;
}

// ============================================================================
// Encoder in quadratura
// ============================================================================
//
// Macchina a stati "full step" di Ben Buxton: un solo evento per detent e
// nessun conto sbagliato sui rimbalzi, che con gli encoder meccanici sono la
// norma. Ogni transizione su A o B genera un interrupt, cosi' nessuno scatto va
// perso nemmeno mentre il loop sta ridisegnando il display.

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
    {R_START, R_CW_BEGIN, R_CCW_BEGIN, R_START},
    {R_CW_NEXT, R_START, R_CW_FINAL, R_START | DIR_CW},
    {R_CW_NEXT, R_CW_BEGIN, R_START, R_START},
    {R_CW_NEXT, R_CW_BEGIN, R_CW_FINAL, R_START},
    {R_CCW_NEXT, R_START, R_CCW_BEGIN, R_START},
    {R_CCW_NEXT, R_CCW_FINAL, R_START, R_START | DIR_CCW},
    {R_CCW_NEXT, R_CCW_FINAL, R_CCW_BEGIN, R_START},
};

struct Encoder {
    uint8_t pinA;
    uint8_t pinB;
    volatile uint8_t state;
    volatile int32_t count;  // contatore monotono, mai azzerato dall'ISR
    int32_t lastRead;        // usato solo dal loop
};

Encoder encoders[4];

// Contatore monotono + lastRead: consumare il delta non ha bisogno di sezioni
// critiche e non puo' perdere scatti arrivati fra la lettura e l'azzeramento.
void IRAM_ATTR encoderStep(Encoder &e) {
    // A nel bit alto e B nel basso, al contrario di come lo scrive Buxton. La
    // tabella qui sopra non ha niente di sbagliato: e' il verso in cui gli EC11
    // stanno sul PCB: girando in senso orario le due fasi arrivano invertite
    // rispetto alla sua convenzione, e DIR_CW cadeva sulla rotazione antioraria.
    // Il risultato era che ogni manopola, ovunque, toglieva girando in avanti.
    //
    // Scambiare i due bit qui equivale a scambiare i fili A e B di tutti e
    // quattro gli encoder, ed e' l'unico punto che va toccato: da encDelta() in
    // giu' il firmware assume "positivo = senso orario = il valore sale", e
    // quell'assunzione adesso e' vera.
    uint8_t pinState = (uint8_t)((digitalRead(e.pinA) << 1) | digitalRead(e.pinB));
    e.state = ENC_TABLE[e.state & 0x0f][pinState];
    const uint8_t dir = e.state & 0x30;
    if (dir == DIR_CW) {
        ++e.count;
    } else if (dir == DIR_CCW) {
        --e.count;
    }
}

void IRAM_ATTR isrEnc0() { encoderStep(encoders[0]); }
void IRAM_ATTR isrEnc1() { encoderStep(encoders[1]); }
void IRAM_ATTR isrEnc2() { encoderStep(encoders[2]); }
void IRAM_ATTR isrEnc3() { encoderStep(encoders[3]); }

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
    const int32_t now = e.count;
    const int32_t delta = now - e.lastRead;
    e.lastRead = now;
    return (int)delta;
}

// ============================================================================
// Note e funzioni
// ============================================================================
const uint8_t NOTE_SLOT[NOTE_COUNT] = MATRIX_NOTE_SLOTS;
const uint8_t FN_SLOT[FN_COUNT] = MATRIX_FN_SLOTS;

int8_t pressOrder[NOTE_COUNT];
uint8_t pressCount = 0;

// Coda circolare degli attacchi. Piu' capiente del numero di tasti: in un giro
// di loop se ne possono accumulare al massimo NOTE_COUNT, ma cosi' resta spazio
// anche se il chiamante salta un giro.
constexpr uint8_t NOTE_QUEUE_SIZE = 32;
int8_t noteOnQueue[NOTE_QUEUE_SIZE];
uint8_t noteOnHead = 0;
uint8_t noteOnTail = 0;

void pushNoteOn(int note) {
    const uint8_t next = (uint8_t)((noteOnTail + 1) % NOTE_QUEUE_SIZE);
    if (next == noteOnHead) return;  // coda piena: vince l'evento piu' vecchio
    noteOnQueue[noteOnTail] = (int8_t)note;
    noteOnTail = next;
}

void notePressed(int note) {
    for (uint8_t i = 0; i < pressCount; ++i) {
        if (pressOrder[i] == note) return;
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

// ------------------------------------------------------------- auto-repeat joystick
constexpr uint32_t REPEAT_DELAY_MS = 400;
constexpr uint32_t REPEAT_RATE_MS = 90;
uint32_t joyNextRepeat[4] = {0, 0, 0, 0};
bool joyEvent[4] = {false, false, false, false};

// ------------------------------------------------------- pressione breve/lunga
struct PressTracker {
    uint8_t button;
    uint32_t longMs;
    bool longFired;
    bool shortEvent;
    bool longEvent;
};

PressTracker fnTrackers[FN_COUNT];

// Un tasto solo ha ancora la pressione lunga, ed e' quella che svuota il
// pattern: la soglia lenta serve a dare il tempo di pentirsi.
//
// Agli altri sei si da' una soglia irraggiungibile invece di una condizione in
// piu' nel ciclo. Non e' un trucco da risparmiare una riga: e' la traduzione
// esatta di "questo tasto non ha una funzione lunga". Il confronto
// (now - pressedAt) >= longMs non scatta mai, quindi longFired resta falso e il
// breve arriva **sempre** al rilascio, che il tasto sia stato sfiorato o tenuto
// giu' dieci secondi. Prima, con la soglia a 600 ms per tutti, tenere premuto un
// tasto senza funzione lunga voleva dire non far succedere niente — un tasto
// muto, senza nessuna spiegazione a schermo.
constexpr uint32_t FN_NO_LONG = 0xFFFFFFFFu;

uint32_t fnThreshold(int fn) {
    return (fn == FN_PLAY) ? FN_LONG_PRESS_SLOW_MS : FN_NO_LONG;
}

}  // namespace

namespace Input {

void begin() {
    for (int i = 0; i < B_COUNT; ++i) buttons[i] = Button{};

    for (int j = 0; j < 4; ++j) pinMode(JOY_PINS[j], INPUT_PULLUP);

    busRestart();
    mcpAlive = mcpConfigure();
    if (!mcpAlive) {
        // Un solo ritentativo subito, alla velocita' bassa: se il PCB e' quello
        // senza pull-up e i 400 kHz non passano, la tastiera funziona gia' al
        // primo avvio invece che dopo un secondo di errori.
        busFreq = I2C_FREQ_SLOW_HZ;
        busRestart();
        mcpAlive = mcpConfigure();
    }

    encoderBegin(encoders[0], PIN_ENC1_A, PIN_ENC1_B, isrEnc0);
    encoderBegin(encoders[1], PIN_ENC2_A, PIN_ENC2_B, isrEnc1);
    encoderBegin(encoders[2], PIN_ENC3_A, PIN_ENC3_B, isrEnc2);
    encoderBegin(encoders[3], PIN_ENC4_A, PIN_ENC4_B, isrEnc3);

    for (int i = 0; i < FN_COUNT; ++i) {
        fnTrackers[i] = PressTracker{(uint8_t)(B_MATRIX0 + FN_SLOT[i]), fnThreshold(i), false,
                                     false, false};
    }
}

void update() {
    const uint32_t now = millis();

    matrixScan();

    for (int k = 0; k < MATRIX_COLS * MATRIX_ROWS; ++k) {
        feed(buttons[B_MATRIX0 + k], (matrixRaw & (1u << k)) != 0, now);
    }
    for (int j = 0; j < 4; ++j) {
        feed(buttons[B_JOY_UP + j], digitalRead(JOY_PINS[j]) == LOW, now);
    }
    for (int e = 0; e < 4; ++e) {
        feed(buttons[B_ENC_SW0 + e], (encSwRaw & (1u << (MCP_ENC1_SW_BIT + e))) == 0, now);
    }

    // --- note: ordine di pressione e coda degli attacchi ---
    for (int n = 0; n < NOTE_COUNT; ++n) {
        Button &b = buttons[B_MATRIX0 + NOTE_SLOT[n]];
        if (consume(b.edgeDown)) {
            notePressed(n);
            pushNoteOn(n);
        }
        if (consume(b.edgeUp)) noteReleased(n);
    }

    // --- joystick: fronte + auto-repeat ---
    for (int j = 0; j < 4; ++j) {
        Button &b = buttons[B_JOY_UP + j];
        if (consume(b.edgeDown)) {
            joyEvent[j] = true;
            joyNextRepeat[j] = now + REPEAT_DELAY_MS;
        }
        if (b.state && (int32_t)(now - joyNextRepeat[j]) >= 0) {
            joyEvent[j] = true;
            joyNextRepeat[j] = now + REPEAT_RATE_MS;
        }
        consume(b.edgeUp);
    }

    // --- tasti funzione: breve vs lungo ---
    for (int i = 0; i < FN_COUNT; ++i) {
        PressTracker &t = fnTrackers[i];
        Button &b = buttons[t.button];
        if (consume(b.edgeDown)) t.longFired = false;
        if (b.state && !t.longFired && (now - b.pressedAt) >= t.longMs) {
            t.longFired = true;
            t.longEvent = true;
        }
        if (consume(b.edgeUp) && !t.longFired) t.shortEvent = true;
    }
}

bool expanderOk() { return mcpAlive; }
uint32_t expanderErrors() { return mcpErrors; }

int currentNote() { return (pressCount > 0) ? pressOrder[pressCount - 1] : -1; }
int heldCount() { return pressCount; }

int heldNoteByOrder(int index) {
    if (index < 0 || index >= (int)pressCount) return -1;
    return pressOrder[index];
}

bool noteIsHeld(int note) {
    if (note < 0 || note >= NOTE_COUNT) return false;
    return buttons[B_MATRIX0 + NOTE_SLOT[note]].state;
}

int consumeNoteOn() {
    if (noteOnHead == noteOnTail) return -1;
    const int n = noteOnQueue[noteOnHead];
    noteOnHead = (uint8_t)((noteOnHead + 1) % NOTE_QUEUE_SIZE);
    return n;
}

bool joyUp() { return consume(joyEvent[0]); }
bool joyDown() { return consume(joyEvent[1]); }
bool joyLeft() { return consume(joyEvent[2]); }
bool joyRight() { return consume(joyEvent[3]); }

bool fnShortPress(int fn) {
    if (fn < 0 || fn >= FN_COUNT) return false;
    return consume(fnTrackers[fn].shortEvent);
}

bool fnLongPress(int fn) {
    if (fn < 0 || fn >= FN_COUNT) return false;
    return consume(fnTrackers[fn].longEvent);
}

bool fnIsDown(int fn) {
    if (fn < 0 || fn >= FN_COUNT) return false;
    return buttons[B_MATRIX0 + FN_SLOT[fn]].state;
}

uint32_t fnHeldMs(int fn) {
    if (fn < 0 || fn >= FN_COUNT) return 0;
    const Button &b = buttons[B_MATRIX0 + FN_SLOT[fn]];
    return b.state ? (millis() - b.pressedAt) : 0;
}

int encDelta(int which) {
    if (which < 0 || which >= 4) return 0;
    return encoderConsume(encoders[which]);
}

bool encClick(int which) {
    if (which < 0 || which >= 4) return false;
    return consume(buttons[B_ENC_SW0 + which].edgeDown);
}

bool encRelease(int which) {
    if (which < 0 || which >= 4) return false;
    return consume(buttons[B_ENC_SW0 + which].edgeUp);
}

bool encIsDown(int which) {
    if (which < 0 || which >= 4) return false;
    return buttons[B_ENC_SW0 + which].state;
}

}  // namespace Input
