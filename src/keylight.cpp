// keylight.cpp — driver RMT per la catena di SK6812 e logica dei colori.
//
// Venti LED sono 480 bit: mandarli a mano col bit-banging vorrebbe dire
// disabilitare gli interrupt per 600 us ad ogni fotogramma, e il task audio se
// ne accorgerebbe subito. Con l'RMT il pattern lo spara l'hardware e il core 1
// torna a fare il suo lavoro dopo aver riempito un buffer.

#include "keylight.h"

#include <esp32-hal-rmt.h>
#include <string.h>

namespace {

// ============================================================================
// Driver
// ============================================================================
//
// Si passa dall'RMT dell'Arduino core e non dal driver IDF sottostante, e non e'
// un dettaglio: il LED di bordo (status_led.cpp) usa neopixelWrite(), che a sua
// volta chiama rmtInit(). I due allocatori non si parlano, quindi prendendo un
// canale "a mano" si finirebbe per litigare proprio con quello — due catene
// diverse pilotate dallo stesso hardware, e nessuna delle due funzionante.
// Chiedendo il canale allo stesso allocatore, invece, il conto torna.

// Un tick da 25 ns: i tempi dell'SK6812 diventano numeri interi piccoli.
constexpr float TICK_NS = 25.0f;
constexpr uint16_t T0H = 12;  // 300 ns
constexpr uint16_t T0L = 36;  // 900 ns
constexpr uint16_t T1H = 24;  // 600 ns
constexpr uint16_t T1L = 24;  // 600 ns

constexpr int BITS_PER_LED = 24;
rmt_data_t items[KEYLED_COUNT * BITS_PER_LED];

uint8_t frame[KEYLED_COUNT][3];  // GRB, gia' scalato di luminosita'
rmt_obj_t *rmtChain = nullptr;

void encodeFrame() {
    int k = 0;
    for (int led = 0; led < KEYLED_COUNT; ++led) {
        for (int c = 0; c < 3; ++c) {
            const uint8_t byte = frame[led][c];
            for (int b = 7; b >= 0; --b) {
                const bool one = (byte >> b) & 1;
                items[k].level0 = 1;
                items[k].duration0 = one ? T1H : T0H;
                items[k].level1 = 0;
                items[k].duration1 = one ? T1L : T0L;
                ++k;
            }
        }
    }
}

void flush() {
    if (!rmtChain) return;
    encodeFrame();
    // Non bloccante: la trasmissione dura 600 us e il fotogramma successivo
    // arriva 33 ms dopo, quindi non c'e' mai niente in coda da aspettare.
    rmtWrite(rmtChain, items, KEYLED_COUNT * BITS_PER_LED);
}

// ============================================================================
// Mappa tasto -> posizione nella catena
// ============================================================================
// Di fabbrica: la catena segue l'ordine della matrice. Dallo schematico si sa
// per certo solo che il primo LED e' quello del DO (la LEDDIN entra li'); il
// resto lo sistema la procedura di apprendimento.
uint8_t keyToLed[KEYLED_COUNT];

void defaultMap() {
    for (int i = 0; i < KEYLED_COUNT; ++i) keyToLed[i] = (uint8_t)i;
}

bool validMap(const uint8_t *m) {
    bool seen[KEYLED_COUNT] = {false};
    for (int i = 0; i < KEYLED_COUNT; ++i) {
        if (m[i] >= KEYLED_COUNT || seen[m[i]]) return false;
        seen[m[i]] = true;
    }
    return true;
}

// ============================================================================
// Apprendimento
// ============================================================================
bool learnActive = false;
uint8_t learnPos = 0;
uint8_t learnMap[KEYLED_COUNT];
bool learnTaken[KEYLED_COUNT];

// ============================================================================
// Colori
// ============================================================================
struct Rgb {
    uint8_t r, g, b;
};

// Quali dei 13 tasti nota sono alterazioni: DO# RE# FA# SOL# LA#.
const bool IS_SHARP[NOTE_COUNT] = {false, true,  false, true,  false, false, true,
                                   false, true,  false, true,  false, false};

const uint8_t NOTE_SLOT[NOTE_COUNT] = MATRIX_NOTE_SLOTS;
const uint8_t FN_SLOT[FN_COUNT] = MATRIX_FN_SLOTS;

// Un colore per funzione, scelto perche' si distinguano a colpo d'occhio anche
// di taglio: le due che fermano o cancellano qualcosa sono le uniche calde.
const Rgb FN_COLOR[FN_COUNT] = {
    {0, 200, 255},   // FN1 ARP     ciano
    {255, 90, 0},    // FN2 8 BIT   arancio
    {255, 0, 40},    // FN3 REC     rosso
    {0, 255, 90},    // FN4 PLAY    verde
    {255, 210, 0},   // FN5 TIENI   ambra
    {170, 0, 255},   // FN6 VOCI    viola
    // L'unico bianco della fila, e adesso e' anche l'unico tasto d'emergenza: si
    // trova di taglio, senza leggere e senza cercarlo. Prima questo colore stava
    // sotto il tasto che scorreva le schermate, cioe' sotto la funzione meno
    // urgente delle sette.
    {255, 255, 255}, // FN7 SILENZIO bianco
};

Rgb scale(Rgb c, uint16_t num, uint16_t den) {
    if (den == 0) return {0, 0, 0};
    return {(uint8_t)((uint32_t)c.r * num / den), (uint8_t)((uint32_t)c.g * num / den),
            (uint8_t)((uint32_t)c.b * num / den)};
}

// Colori grezzi del fotogramma, prima della luminosita' globale.
Rgb raw[KEYLED_COUNT];

void put(uint8_t slot, Rgb c) {
    if (slot >= KEYLED_COUNT) return;
    raw[slot] = c;
}

}  // namespace

namespace Keylight {

void begin() {
    defaultMap();
    memset(frame, 0, sizeof(frame));
    memset(raw, 0, sizeof(raw));

    // Due blocchi di memoria: la catena e' lunga 480 simboli e il driver la
    // ricarica a interrupt, ma con un blocco solo gli interrupt sarebbero il
    // doppio e cadrebbero proprio mentre il core 1 ridisegna il display.
    rmtChain = rmtInit(PIN_KEYLED_DATA, RMT_TX_MODE, RMT_MEM_128);
    if (rmtChain) rmtSetTick(rmtChain, TICK_NS);
    allOff();
}

void allOff() {
    memset(frame, 0, sizeof(frame));
    memset(raw, 0, sizeof(raw));
    flush();
}

void update(uint32_t now, const LightView &v) {
    if (!rmtChain) return;

    memset(raw, 0, sizeof(raw));

    if (learnActive) {
        // Un LED bianco alla volta, tutto il resto spento: non c'e' modo di
        // sbagliarsi su quale tasto stia chiedendo.
        const uint8_t pulse = (uint8_t)(160 + 95 * ((now / 250) % 2));
        for (int i = 0; i < KEYLED_COUNT; ++i) {
            if (keyToLed[i] == learnPos) raw[i] = {pulse, pulse, pulse};
        }
    } else {
        // --- riposo: le note disegnano la tastiera, spente ma leggibili ---
        for (int n = 0; n < NOTE_COUNT; ++n) {
            const uint8_t slot = NOTE_SLOT[n];
            const bool inScale = (v.scaleRoot < 0) || (v.scaleMask & (1u << n));
            Rgb base = IS_SHARP[n] ? Rgb{40, 0, 60} : Rgb{0, 30, 40};
            if (!inScale) base = {6, 0, 0};  // fuori scala: quasi spento, rossastro
            if (v.crush) base = IS_SHARP[n] ? Rgb{50, 20, 0} : Rgb{40, 30, 0};

            Rgb c = base;
            if (v.noteSound & (1u << n)) {
                c = IS_SHARP[n] ? Rgb{255, 60, 255} : Rgb{80, 255, 255};
            } else if (v.noteHeld & (1u << n)) {
                c = IS_SHARP[n] ? Rgb{160, 30, 200} : Rgb{40, 180, 200};
            }
            if (v.seqNote == n) {
                // La nota della sequenza si distingue da quella suonata a mano:
                // verde, il colore del trasporto.
                c = {0, 255, 120};
            }
            // Il respiro dell'invito: un ciclo lento di tre secondi, e solo sui
            // tasti che non hanno gia' qualcosa da dire. Un tasto che sta
            // suonando — perche' lo tieni premuto, perche' lo suona la sequenza
            // o perche' gli arriva una nota dal cavo MIDI — ha una notizia piu'
            // importante di "puoi suonarmi", e l'invito non deve coprirgliela.
            //
            // Un triangolo e non un seno: a venti LED e otto bit la differenza
            // non si vede, e una moltiplicazione intera costa molto meno.
            const bool busy = (v.noteSound & (1u << n)) || (v.noteHeld & (1u << n)) ||
                              (v.seqNote == n);
            if (v.invite && !busy) {
                const uint32_t t = now % 3000;
                const uint16_t k = (t < 1500) ? (uint16_t)(t / 6) : (uint16_t)((3000 - t) / 6);
                const Rgb glow = IS_SHARP[n] ? Rgb{120, 40, 200} : Rgb{40, 200, 220};
                c = scale(glow, k, 250);
            }
            put(slot, c);
        }

        // --- funzioni: accese quando la loro funzione e' inserita ---
        for (int f = 0; f < FN_COUNT; ++f) {
            const Rgb col = FN_COLOR[f];
            Rgb c = scale(col, 1, 12);  // sempre un filo accese: si trovano al buio
            if (v.fnActive & (1u << f)) c = col;
            if (v.fnPending & (1u << f)) {
                const bool on = ((now / 200) % 2) == 0;
                c = on ? col : scale(col, 1, 6);
            }
            // AVVIA batte il tempo anche da fermo: e' il metronomo che lo
            // strumento non ha, e insegna cos'e' il BPM a chi non lo sa. Tace
            // pero' quando il tasto ha gia' qualcosa da dire — il preconteggio
            // lampeggia sullo stesso LED, e due battiti sovrapposti non si
            // leggono ne' come l'uno ne' come l'altro.
            const bool busy = ((v.fnActive | v.fnPending) & (1u << f)) != 0;
            if (f == FN_PLAY && v.tempoPulse && !busy) {
                c = scale(col, 1, 3);
            }
            put(FN_SLOT[f], c);
        }
    }

    // Luminosita' globale: 0 spegne davvero, 8 e' il massimo. La scala e'
    // quadratica perche' l'occhio non e' lineare e a meta' corsa un LED lineare
    // sembrerebbe gia' al massimo.
    const uint16_t b = (v.brightness > 8) ? 8 : v.brightness;
    const uint16_t num = (uint16_t)(b * b);
    for (int i = 0; i < KEYLED_COUNT; ++i) {
        const uint8_t led = keyToLed[i];
        const Rgb c = scale(raw[i], num, 64);
        frame[led][0] = c.g;  // SK6812: ordine GRB
        frame[led][1] = c.r;
        frame[led][2] = c.b;
    }
    flush();
}

// ------------------------------------------------------------ apprendimento
void startLearn() {
    learnActive = true;
    learnPos = 0;
    memset(learnTaken, 0, sizeof(learnTaken));
    for (int i = 0; i < KEYLED_COUNT; ++i) learnMap[i] = 0xFF;
    // Durante l'apprendimento la mappa in uso torna quella di fabbrica: e' la
    // sola che garantisce di accendere un LED per volta anche se quella salvata
    // era sbagliata.
    defaultMap();
}

bool learning() { return learnActive; }
uint8_t learnIndex() { return learnPos; }

void learnAssign(uint8_t key) {
    if (!learnActive || key >= KEYLED_COUNT) return;
    if (learnMap[key] != 0xFF) return;  // tasto gia' usato: si ignora
    learnMap[key] = learnPos;
    learnTaken[learnPos] = true;
    ++learnPos;
    if (learnPos >= KEYLED_COUNT) {
        // Finita: se per qualche motivo manca un tasto, i buchi si riempiono
        // con le posizioni rimaste, cosi' la mappa resta una permutazione
        // valida invece di diventare inservibile.
        bool used[KEYLED_COUNT] = {false};
        for (int i = 0; i < KEYLED_COUNT; ++i) {
            if (learnMap[i] != 0xFF) used[learnMap[i]] = true;
        }
        uint8_t spare = 0;
        for (int i = 0; i < KEYLED_COUNT; ++i) {
            if (learnMap[i] == 0xFF) {
                while (spare < KEYLED_COUNT && used[spare]) ++spare;
                learnMap[i] = (spare < KEYLED_COUNT) ? spare++ : 0;
            }
        }
        memcpy(keyToLed, learnMap, sizeof(keyToLed));
        learnActive = false;
    }
}

void cancelLearn() { learnActive = false; }

const uint8_t *map() { return keyToLed; }

void setMap(const uint8_t *m) {
    if (m && validMap(m)) memcpy(keyToLed, m, KEYLED_COUNT);
}

void resetMap() { defaultMap(); }

}  // namespace Keylight
