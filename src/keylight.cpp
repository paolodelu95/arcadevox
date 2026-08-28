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

// I colori di riposo della tastiera: spenti abbastanza da non dare fastidio al
// buio, accesi abbastanza da far vedere dove sono i tasti. Hanno un nome perche'
// non li usa piu' solo il fotogramma di riposo: e' li' che il gioco di luci
// dell'accensione deve consegnare il pannello, e le due cose devono restare lo
// stesso colore anche fra un anno.
const Rgb REST_NATURAL = {0, 30, 40};
const Rgb REST_SHARP = {40, 0, 60};

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

// Manda a video raw[]: applica la mappa della catena e la luminosita' globale.
//
// La scala della luminosita' e' quadratica perche' l'occhio non e' lineare: con
// una scala diritta, a meta' corsa un LED sembrerebbe gia' al massimo. Zero
// spegne davvero, otto e' il massimo.
void present(uint8_t brightness) {
    const uint16_t b = (brightness > 8) ? 8 : brightness;
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

// ============================================================================
// Geometria del pannello
// ============================================================================
//
// I venti tasti non sono una fila: sono un pezzo di pianoforte con sopra una
// riga di funzioni. Per far correre una luce "da sinistra a destra", o dal
// centro verso i lati, serve sapere dove sta ogni tasto **sul pannello** — e
// l'indice di matrice non lo dice: quello e' un fatto di cablaggio, e mette il
// DO' acuto accanto a FN1 solo perche' cosi' cadeva la quarta riga.
//
//   riga 0   le sette funzioni, in alto
//   riga 1   i cinque tasti neri, incastrati fra i bianchi giusti
//   riga 2   gli otto tasti bianchi, in basso
//
// La x va da 0 (a sinistra) a 255 (a destra) e per le note e' quella vera del
// pianoforte: i neri stanno a meta' strada fra i due bianchi che separano, e
// dove il tasto nero non esiste — fra MI e FA, fra SI e DO' — la scala cromatica
// salta di due mezzi passi invece che di uno.
constexpr uint8_t ROW_FN = 0;
constexpr uint8_t ROW_SHARP = 1;
constexpr uint8_t ROW_NATURAL = 2;
constexpr uint8_t ROW_COUNT = 3;

uint8_t spotX[KEYLED_COUNT];
uint8_t spotRow[KEYLED_COUNT];
Rgb spotRest[KEYLED_COUNT];  // il colore di riposo, quello a cui l'avvio consegna

void buildSpots() {
    for (int i = 0; i < KEYLED_COUNT; ++i) {
        spotX[i] = 128;
        spotRow[i] = ROW_FN;
        spotRest[i] = {0, 0, 0};
    }
    for (int n = 0; n < NOTE_COUNT; ++n) {
        const uint8_t slot = NOTE_SLOT[n];
        if (slot >= KEYLED_COUNT) continue;
        const int half = n + (n >= 5 ? 1 : 0) + (n >= 12 ? 1 : 0);  // in mezzi tasti bianchi
        spotX[slot] = (uint8_t)(half * 255 / 14);
        spotRow[slot] = IS_SHARP[n] ? ROW_SHARP : ROW_NATURAL;
        spotRest[slot] = IS_SHARP[n] ? REST_SHARP : REST_NATURAL;
    }
    for (int f = 0; f < FN_COUNT; ++f) {
        const uint8_t slot = FN_SLOT[f];
        if (slot >= KEYLED_COUNT) continue;
        spotX[slot] = (uint8_t)(f * 255 / (FN_COUNT - 1));
        spotRow[slot] = ROW_FN;
        spotRest[slot] = scale(FN_COLOR[f], 1, 12);
    }
}

// ============================================================================
// Il gioco di luci dell'accensione
// ============================================================================
//
// Gira mentre il display disegna l'intro (vedi Display::setPacer): quei tre
// secondi erano tempo in cui il pannello restava nero, e un pannello nero
// all'accensione non dice se le venti luci ci sono davvero.
//
// Fa due lavori in uno, e nessuno dei due e' un pretesto per l'altro.
//
// **Il collaudo.** Le prime tre passate accendono ogni LED a rosso pieno, poi a
// verde pieno, poi a blu pieno, e il lampo bianco che le chiude li accende tutti
// e tre insieme. Un LED morto resta nero mentre i suoi vicini si accendono; un
// canale morto si vede perche' quella passata li' salta un tasto e le altre due
// no. E' l'unica prova che copre i sessanta canali della catena, e costa meno di
// un secondo all'avvio.
//
// **La scena.** Le tre passate sono anche l'inizio di un racconto, ed e' lo
// stesso racconto che sta andando sul vetro tondo: il sole che sale e tramonta a
// fasce, la griglia che si apre a ventaglio dal centro, la traccia di
// oscilloscopio che corre sull'orizzonte, le due eco cromatiche del logo e il
// bianco che le mette a fuoco. Stessi tempi e stessi colori del display, presi
// dalla sua tavolozza: quello che si vede sotto le dita e quello che si vede
// sullo schermo sono la stessa cosa, che e' la regola di questo file.
//
// L'ultimo fotogramma non e' un nero: e' esattamente il fotogramma di riposo che
// disegnera' update() un istante dopo, cosi' fra l'intro e lo strumento acceso
// non c'e' nessuno scatto.

// Tavolozza del display (display.cpp, HUD_*): gli stessi tre colori dell'intro.
const Rgb SHOW_MAGENTA = {255, 32, 140};
const Rgb SHOW_AMBER = {255, 196, 64};
const Rgb SHOW_NEON = {0, 255, 255};
const Rgb SHOW_WHITE = {255, 255, 255};
// La griglia in fuga: un ciano molto piu' basso, che sotto la traccia deve fare
// da fondo e non da protagonista.
const Rgb SHOW_GRID = {0, 55, 70};

// Le tre passate di collaudo, un canale per volta.
const Rgb SHOW_CHANNEL[3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};

// Tempi, in millisecondi dall'accensione. Sommati stanno dentro la durata
// dell'intro del display (~3 s): se l'intro finisse prima, il gioco di luci
// viene semplicemente troncato e il fotogramma di riposo lo mette a video il
// primo giro di loop.
constexpr uint32_t T_SWEEP = 200;                    // una passata di collaudo
constexpr uint32_t T_CHECK = T_SWEEP * 3;            //  600  rosso, verde, blu
constexpr uint32_t T_FLASH = T_CHECK + 240;          //  840  tutti e tre insieme
constexpr uint32_t T_SUN = T_FLASH + 520;            // 1360  il sole sale e cala
constexpr uint32_t T_GRID = T_SUN + 400;             // 1760  il ventaglio
constexpr uint32_t T_TRACE = T_GRID + 350;           // 2110  la traccia
constexpr uint32_t T_WORD = T_TRACE + 360;           // 2470  le eco e il bianco
constexpr uint32_t T_REST = T_WORD + 260;            // 2730  consegna al riposo
constexpr uint32_t SHOW_FRAME_MS = 20;               // ~50 fotogrammi al secondo

uint32_t showStart = 0;
uint32_t showLast = 0;
uint8_t showBright = 5;
bool showDone = true;

// Miscela: k a 0 e' `a`, k a 255 e' `b`.
Rgb blend(Rgb a, Rgb b, uint8_t k) {
    return {(uint8_t)(((255 - k) * a.r + k * b.r) / 255),
            (uint8_t)(((255 - k) * a.g + k * b.g) / 255),
            (uint8_t)(((255 - k) * a.b + k * b.b) / 255)};
}

// Quanto e' acceso un punto che dista |x - head| da un fronte largo `w`.
uint8_t frontFade(int x, int head, int w) {
    int d = x - head;
    if (d < 0) d = -d;
    if (d >= w) return 0;
    return (uint8_t)(255 - d * 255 / w);
}

// Come sopra, ma solo dietro al fronte: e' la scia della cometa.
uint8_t tailFade(int x, int head, int len) {
    const int d = head - x;
    if (d < 0 || d >= len) return 0;
    return (uint8_t)(255 - d * 255 / len);
}

// Rampa lineare 0..255 di `u` dentro [from, from + span).
uint8_t ramp(uint32_t u, uint32_t from, uint32_t span) {
    if (u <= from) return 0;
    if (u >= from + span) return 255;
    return (uint8_t)((u - from) * 255 / span);
}

// --- 1) collaudo: tre passate, un canale per volta -------------------------
void showCheck(uint32_t t) {
    const int pass = (int)(t / T_SWEEP);
    const uint32_t u = t % T_SWEEP;
    // Le passate si alternano di verso: la seconda torna indietro invece di
    // saltare da capo, e la cosa si legge come un movimento solo invece che come
    // tre partenze.
    const bool ltr = (pass & 1) == 0;
    int head = (int)(u * 320 / T_SWEEP) - 32;  // entra ed esce dai due bordi
    if (!ltr) head = 255 - head;

    const Rgb before = (pass == 0) ? Rgb{0, 0, 0} : SHOW_CHANNEL[pass - 1];
    for (int i = 0; i < KEYLED_COUNT; ++i) {
        const int x = spotX[i];
        const bool painted = ltr ? (x <= head) : (x >= head);
        // Il fronte e' bianco e il colore resta dietro di lui: a fine passata il
        // pannello e' tutto di quel canale, ed e' li' che si contano i buchi.
        raw[i] = blend(painted ? SHOW_CHANNEL[pass] : before, SHOW_WHITE,
                       frontFade(x, head, 36));
    }
}

// --- 2) il lampo bianco: i tre canali insieme ------------------------------
void showFlash(uint32_t t) {
    const uint32_t u = t - T_CHECK;
    const uint8_t k = (u < 80) ? 255 : (uint8_t)(255 - ramp(u, 80, 160));
    const Rgb c = scale(SHOW_WHITE, k, 255);
    for (int i = 0; i < KEYLED_COUNT; ++i) raw[i] = c;
}

// --- 3) il sole: sale dal basso, poi cala a fasce --------------------------
void showSun(uint32_t t) {
    const uint32_t u = t - T_FLASH;
    // Il disco del display e' un gradiente magenta in alto, ambra in basso: qui
    // le tre righe del pannello sono le tre altezze di quel gradiente.
    const Rgb ROW_COLOR[ROW_COUNT] = {SHOW_MAGENTA, blend(SHOW_MAGENTA, SHOW_AMBER, 128),
                                      SHOW_AMBER};
    for (int i = 0; i < KEYLED_COUNT; ++i) {
        const uint8_t row = spotRow[i];
        uint8_t k;
        if (u < 260) {
            // Sale: prima i bianchi in basso, per ultime le funzioni in alto.
            k = ramp(u, (uint32_t)(ROW_NATURAL - row) * 70u, 120);
        } else {
            // Cala, e in ordine rovesciato. La dissolvenza e' a quattro gradini
            // invece che continua: sono le fessure che tagliano il disco
            // nell'intro, ed e' quello che rende il tramonto riconoscibile.
            k = (uint8_t)(255 - ramp(u, 260 + (uint32_t)row * 70u, 120));
            k = (uint8_t)(k & 0xC0);
        }
        raw[i] = scale(ROW_COLOR[row], k, 255);
    }
}

// --- 4) la griglia: un ventaglio che si apre dal centro --------------------
void showGrid(uint32_t t) {
    const uint32_t u = t - T_SUN;
    const int edge = (int)(u * 150 / (T_GRID - T_SUN));  // fin dove e' arrivato
    for (int i = 0; i < KEYLED_COUNT; ++i) {
        int d = (int)spotX[i] - 128;
        if (d < 0) d = -d;
        raw[i] = (d > edge) ? Rgb{0, 0, 0} : blend(SHOW_GRID, SHOW_NEON, frontFade(d, edge, 30));
    }
}

// --- 5) la traccia: una cometa che ondeggia sull'orizzonte -----------------
void showTrace(uint32_t t) {
    const uint32_t u = t - T_GRID;
    const int head = (int)(u * 300 / (T_TRACE - T_GRID)) - 20;

    // Dove sta il fronte in verticale, in centesimi di riga (0 = funzioni, 200 =
    // bianchi): oscilla attorno alla riga di mezzo come la traccia del display
    // oscilla attorno all'orizzonte. Un'onda triangolare, che a tre righe e'
    // indistinguibile da un seno e non costa una libreria di matematica.
    const int hx = (head < 0) ? 0 : (head > 255 ? 255 : head);
    int tri = (hx * 6) & 511;
    if (tri > 255) tri = 511 - tri;
    const int wave = 100 + (tri - 128) * 100 / 128;

    for (int i = 0; i < KEYLED_COUNT; ++i) {
        const int x = spotX[i];
        // La griglia resta accesa sotto: la traccia nell'intro le passa sopra.
        Rgb c = SHOW_GRID;
        int dv = (int)spotRow[i] * 100 - wave;
        if (dv < 0) dv = -dv;
        // Il fronte non sta su una riga sola: sfuma su quella accanto, o a tre
        // righe l'oscillazione sarebbe un saltello fra tre stati e non un'onda.
        if (dv < 130) {
            const uint16_t vert = (uint16_t)(255 - dv * 255 / 130);
            const uint16_t k = (uint16_t)tailFade(x, head, 90) * vert / 255;
            c = blend(c, SHOW_NEON, (uint8_t)k);
            // La testa e' piu' chiara della scia: e' quello che la fa sembrare
            // in movimento invece che una barra che si allunga.
            c = blend(c, SHOW_WHITE, (uint8_t)((uint16_t)frontFade(x, head, 28) * vert / 510));
        }
        raw[i] = c;
    }
}

// --- 6) il logo: le due eco, poi il bianco che mette a fuoco ---------------
void showWord(uint32_t t) {
    const uint32_t u = t - T_TRACE;
    for (int i = 0; i < KEYLED_COUNT; ++i) {
        // Le eco del wordmark sono due copie sfalsate di tre pixel, una magenta a
        // sinistra e una ciano a destra: sul pannello diventano le sue due meta'.
        const Rgb ghost = (spotX[i] < 128) ? SHOW_MAGENTA : SHOW_NEON;
        uint8_t k = (u < 160) ? ramp(u, 0, 90) : 255;
        Rgb c = scale(ghost, (uint16_t)k * 3 / 5, 255);
        // Poi il corpo bianco scende riga per riga, come le bande di lettere.
        const uint8_t white = ramp(u, 160 + (uint32_t)spotRow[i] * 55u, 70);
        raw[i] = blend(c, SHOW_WHITE, white);
    }
}

// --- 7) consegna: il bianco si posa sui colori di riposo -------------------
void showRest(uint32_t t) {
    const uint8_t k = ramp(t - T_WORD, 0, T_REST - T_WORD);
    for (int i = 0; i < KEYLED_COUNT; ++i) raw[i] = blend(SHOW_WHITE, spotRest[i], k);
}

}  // namespace

namespace Keylight {

void begin() {
    defaultMap();
    buildSpots();
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
            Rgb base;
            if (v.memeMode) {
                // Tredici tinte lungo la ruota dei colori, una per tasto. Qui la
                // scala non vuol dire niente — nessuno di questi tasti suona una
                // nota — e nemmeno l'8 BIT, che sui campioni si sente ma non
                // riguarda la tastiera: percio' questo ramo e' esclusivo e non si
                // fa sovrascrivere da nessuno dei due, com'e' successo finche' era
                // solo il primo di una fila di if.
                //
                // Il giro si fa a mano con tre rampe invece che con una
                // conversione HSV: per tredici valori fissi non vale la pena, e
                // cosi' resta tutto in aritmetica intera. Il canale spento va a
                // zero e non a un fondo fisso, o le tinte agli innesti delle rampe
                // finirebbero a due gradi l'una dall'altra, cioe' uguali.
                const int h = (n * 255) / NOTE_COUNT;
                const int k = (h % 85) * 60 / 85;
                if (h < 85) base = {(uint8_t)(60 - k), (uint8_t)k, 0};
                else if (h < 170) base = {0, (uint8_t)(60 - k), (uint8_t)k};
                else base = {(uint8_t)k, 0, (uint8_t)(60 - k)};
            } else {
                base = IS_SHARP[n] ? REST_SHARP : REST_NATURAL;

                // `scaleMask` segna le **toniche**, non le note "comprese nella
                // scala": lo riempie scaleIsRoot(), che e' vera dove il grado e'
                // un multiplo della lunghezza della scala.
                //
                // Qui veniva letto come "questa nota appartiene alla scala" e
                // tutto il resto finiva dipinto di rosso. Su una pentatonica le
                // toniche fra i tredici tasti sono tre, quindi dieci tasti su
                // tredici diventavano rossi appena si sceglieva una scala — e
                // restavano tali, perche' il rosso era il colore di riposo:
                // premendo il tasto tornava quello giusto e al rilascio si
                // ripresentava.
                //
                // Un tasto "fuori scala" non esiste su questa tastiera, e non e'
                // un dettaglio: scaleSemitone() mappa **ognuno** dei tredici su
                // un grado valido, per questo su una pentatonica gli stessi tasti
                // coprono due ottave abbondanti. Una nota fuori dalla scala non
                // si puo' proprio suonare, quindi non c'e' niente da segnalare.
                //
                // La tonica invece vale la pena vederla: dice dove ricomincia
                // l'ottava, ed e' l'unico riferimento rimasto su una tastiera i
                // cui tasti hanno smesso di essere bianchi e neri. Ambra, che sta
                // lontana sia dal turchese dei naturali sia dal viola delle
                // alterazioni e non si confonde con nessuno dei due.
                if (v.scaleRoot >= 0 && (v.scaleMask & (1u << n))) {
                    base = {60, 34, 0};
                }

                if (v.crush) base = IS_SHARP[n] ? Rgb{50, 20, 0} : Rgb{40, 30, 0};
            }

            Rgb c = base;
            if (v.memeMode) {
                // Premuto: la stessa tinta, ma piena. Non un colore diverso —
                // qui il colore *e'* il nome del suono, e cambiarlo mentre suona
                // vorrebbe dire cambiargli nome sotto il dito.
                if (v.noteHeld & (1u << n)) c = scale(base, 4, 1);
            } else if (v.noteSound & (1u << n)) {
                c = IS_SHARP[n] ? Rgb{255, 60, 255} : Rgb{80, 255, 255};
            } else if (v.noteHeld & (1u << n)) {
                c = IS_SHARP[n] ? Rgb{160, 30, 200} : Rgb{40, 180, 200};
            }
            // La sequenza e l'invito non hanno niente da dire fra i suoni: li'
            // nessun tasto suona una nota, e il respiro coprirebbe le tredici
            // tinte che sono l'unica cosa da guardare.
            if (!v.memeMode && v.seqNote == n) {
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
            if (v.invite && !busy && !v.memeMode) {
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

    present(v.brightness);
}

// ------------------------------------------------------------------- avvio
void bootBegin(uint8_t brightness) {
    showStart = millis();
    showLast = showStart - SHOW_FRAME_MS;
    showBright = brightness;
    // Luci spente e' una scelta, non una svista: chi ha messo LUCI a SPENTE non
    // vuole vedere il pannello accendersi, e non lo vuole nemmeno per un secondo
    // all'avvio. Il collaudo delle luci ha senso solo per chi le luci le tiene
    // accese; per gli altri il gioco non parte proprio, invece di partire e non
    // farsi vedere.
    showDone = (brightness == 0) || (rmtChain == nullptr);
}

bool bootTick(uint32_t now) {
    if (showDone) return false;
    if ((uint32_t)(now - showLast) < SHOW_FRAME_MS) return true;
    showLast = now;

    const uint32_t t = now - showStart;
    if (t < T_CHECK) showCheck(t);
    else if (t < T_FLASH) showFlash(t);
    else if (t < T_SUN) showSun(t);
    else if (t < T_GRID) showGrid(t);
    else if (t < T_TRACE) showTrace(t);
    else if (t < T_WORD) showWord(t);
    else showRest(t);

    present(showBright);
    if (t >= T_REST) {
        // L'ultimo fotogramma e' gia' quello di riposo: il primo giro di loop
        // ridisegnera' esattamente questo, e il passaggio non si vede.
        showDone = true;
        return false;
    }
    return true;
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
