// display.cpp — GC9A01 240x240 IPS su SPI hardware (Arduino_GFX).
//
// Il vetro e' tondo, e per due versioni l'interfaccia ha fatto finta di no:
// rettangoli centrati, elenchi allineati a sinistra, fasce di pulizia larghe
// quanto il quadrato che poi si mangiavano la ghiera perche' il cerchio, li',
// era gia' rientrato. Le quattro mezzelune laterali restavano vuote e i commenti
// del file erano pieni di note su quel pixel e quell'altro che sbordava.
//
// Adesso il sistema e' radiale, e la differenza non e' estetica: in un layout a
// raggi gli angoli non esistono, quindi non c'e' piu' niente da sprecare e
// nessun bordo da difendere caso per caso. Ogni raggio ha un mestiere fisso,
// uguale su tutte le schermate:
//
//   r 117/118   anello esterno, nell'accento della schermata
//   r 112..115  corona di posizione: sette settori, uno per schermata
//   r 100..110  corona dei comandi: i quattro archi delle manopole
//   r 84        didascalie: cosa fa ogni manopola, e per un attimo quanto vale
//   r <= 54     disco dei contenuti, centro (120,122): cio' di cui parla la
//               schermata
//
// Chi aggiunge una schermata non deve decidere dove mettere le cose: le mette
// dove sono sempre. Ed e' anche il motivo per cui non serve piu' ricordare cosa
// fanno le manopole — c'e' scritto sotto ognuna, sempre, su ogni schermata.
//
// Per evitare flicker si ridisegna la parte statica solo al cambio schermata e i
// valori dinamici solo quando cambiano davvero. La traccia e l'ago del VU fanno
// eccezione: cambiano ad ogni fotogramma, quindi cancellano e ridisegnano solo i
// pixel che occupavano prima invece di ripulire aree intere. L'overlay segue la
// stessa regola, e prima non la seguiva: e' quello, e solo quello, il motivo per
// cui vibrava.

#include "display.h"

#include <Arduino_GFX_Library.h>
#include <math.h>
#include <qrcode.h>
#include <string.h>

#include "audio_engine.h"
#include "logo.h"
#include "net_portal.h"
#include "pinout.h"
#include "presets.h"
#include "sequencer.h"
#include "settings.h"
#include "version.h"

namespace {

// ------------------------------------------------------------------ geometria
constexpr int CX = 120;  // centro del vetro
constexpr int CY = 120;

// Il disco dei contenuti sta un po' piu' in alto del centro del vetro: sopra ci
// vuole spazio per il titolo e per la fila delle targhette, sotto ci sono gli
// archi delle manopole e le loro didascalie.
constexpr int DISC_CY = 112;
constexpr int DISC_R = 42;
constexpr int DISC_CX_LEFT = CX - DISC_R + 2;

constexpr int RING_R = 118;      // anello esterno
constexpr int POSRING_IN = 112;  // corona di posizione
constexpr int POSRING_OUT = 115;
constexpr int KNOB_IN = 100;  // corona dei comandi
constexpr int KNOB_OUT = 110;
constexpr int CAPTION_R = 84;  // didascalie delle manopole

// Fascia delle targhette di stato, subito sotto la riga di separazione. Sta a 56
// e non a 50 per due pixel di corda in piu': a quella quota la corona di
// posizione passa a x=27 e x=213, e le targhette devono starci dentro senza
// sfiorarla.
constexpr int CHIP_Y = 56;
constexpr int CHIP_X0 = 32;
constexpr int CHIP_X1 = 208;
// Compatibilita' con le schermate fuori dall'anello (rete, aggiornamento), che
// scrivono ancora dall'alto in giu' come una pagina.
constexpr int CONTENT_TOP = 54;

// I raggi di sicurezza, ed e' la cosa piu' importante di tutto il file.
//
// In un'interfaccia radiale ogni anello e' un vicino di casa degli altri, e una
// pulizia larga quanto sembra ragionevole ne cancella tre. La vecchia regola
// ("non superare la corda del cerchio") bastava quando dentro non c'era niente;
// adesso dentro c'e' la corona di posizione a 113, la corona dei comandi a 100 e
// le didascalie a 84, e una gomma che arriva fin li' se le porta via — senza
// ridisegnarle, perche' il telaio si rifa' solo al cambio di pagina.
//
//   FLASH_R   104  la banda dell'overlay: sotto la corona di posizione, e i suoi
//                  quattro angoli cadono appena fuori dal ventaglio dei comandi
//   CONTENT_R  96  tutto cio' che disegnano le sette schermate dell'anello
//
// Le pagine fuori dall'anello — avvio, rete, aggiornamento — non hanno corone e
// si ridisegnano tutte: li' vale ancora la vecchia regola del solo cerchio.
constexpr int FLASH_R = 104;
constexpr int CONTENT_R = 96;
// E una quota: sotto questa riga cominciano gli archi delle manopole (il loro
// pixel piu' alto sta a y=156), quindi il contenuto di una schermata non ci
// arriva mai.
constexpr int CONTENT_BOTTOM = 154;

Arduino_DataBus *bus = nullptr;
Arduino_GFX *gfx = nullptr;

uint8_t screen = 0;
bool inSeqOverride = false;  // preconteggio o registrazione: RITMO scavalca
bool inLedLearn = false;
bool forceFull = true;

SynthView prev;
bool prevValid = false;

// ------------------------------------------------------------------- colore
//
// Fondo nero, struttura fredda, accenti caldi. La regola di leggibilita' che
// viene prima dello stile e non si tocca: i *valori* si scrivono chiari e grandi
// su nero, e il colore lo portano etichette e strutture, mai il numero che devi
// leggere.
//
// Il rosso e' uscito dalla ruota degli accenti. Prima era il colore della
// schermata ADSR, dove non c'era niente da segnalare, e quel prestito costava
// caro: se il rosso e' anche una decorazione, il rosso della registrazione e
// quello del clip pesano meno di quanto dovrebbero. Da qui in poi, se e' rosso
// c'e' qualcosa che non va o che non si torna indietro.

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Interpolazione fra due colori, t = 0..1. Si lavora sui campi a 5/6/5 bit
// direttamente: convertire in 8 bit e tornare indietro perderebbe di piu'.
uint16_t mix565(uint16_t a, uint16_t b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    return (uint16_t)((((int)(ar + (br - ar) * t)) << 11) |
                      (((int)(ag + (bg - ag) * t)) << 5) | ((int)(ab + (bb - ab) * t)));
}

// Colore attenuato a num/den, per ricavare tracce e stati spenti da un accento.
uint16_t dim565(uint16_t c, int num, int den) {
    return (uint16_t)(((((c >> 11) & 0x1F) * num / den) << 11) |
                      ((((c >> 5) & 0x3F) * num / den) << 5) | ((c & 0x1F) * num / den));
}

// Il fantasma di un accento: la sua versione spenta, sempre ricavata allo stesso
// modo. E' la regola che chiude la tavolozza — non si sceglie a mano il colore
// di uno stato spento, lo si deriva — ed e' il motivo per cui l'anello di
// posizione si legge come un'unica cosa invece che come sette colori accostati.
inline uint16_t ghost(uint16_t c) { return dim565(c, 1, 4); }

const uint16_t HUD_NEON = rgb565(0, 255, 255);      // telaio, accento di SUONA
const uint16_t HUD_MAGENTA = rgb565(255, 32, 140);  // selezione, accento di EFFETTI
const uint16_t HUD_AMBER = rgb565(255, 196, 64);    // valori, accento di INVILUPPO
const uint16_t HUD_ICE = rgb565(210, 250, 255);     // i numeri da leggere
const uint16_t HUD_LIME = rgb565(120, 255, 120);    // sta andando, accento di RITMO
const uint16_t HUD_VIOLET = rgb565(150, 110, 255);  // accento di TIMBRI
const uint16_t HUD_ORANGE = rgb565(255, 120, 0);    // 8 BIT inserito
const uint16_t HUD_RED = rgb565(255, 60, 60);       // solo allarme: mai un accento
// Era rgb565(0,150,180) ed e' il colore piu' usato del file: troppo scuro per un
// testo, giusto per una struttura. Alzandolo le etichette si leggono davvero, e
// il vecchio valore torna utile come HUD_DIM, che e' il mestiere che gli riesce.
const uint16_t HUD_LABEL = rgb565(96, 186, 204);  // etichette
const uint16_t HUD_DIM = rgb565(0, 96, 120);      // secondario: righe non scelte
const uint16_t HUD_TRACK = rgb565(8, 58, 72);     // segmenti spenti

// Un colore per ottava, lo stesso ovunque: celle del sequencer, targhetta
// d'ottava del telaio, LED sotto i tasti. Un colore che vuol dire la stessa cosa
// in tre posti diversi si impara da solo.
const uint16_t OCT_COLORS[5] = {rgb565(150, 110, 255), rgb565(80, 170, 255), rgb565(0, 230, 230),
                                rgb565(140, 240, 120), rgb565(255, 190, 70)};

uint16_t octColor(int8_t oct) {
    int i = oct + 2;
    if (i < 0) i = 0;
    if (i > 4) i = 4;
    return OCT_COLORS[i];
}

// L'accento di ogni schermata dell'anello. La corona di posizione li mostra
// tutti e sette insieme: la ghiera diventa la mappa a colori dello strumento e
// dice dove sei prima che tu legga il titolo.
const uint16_t SCREEN_ACCENT[SCREEN_COUNT] = {
    HUD_NEON,    // SUONA
    HUD_VIOLET,  // TIMBRI
    HUD_AMBER,   // INVILUPPO
    HUD_MAGENTA, // EFFETTI
    HUD_LIME,    // RITMO
    HUD_ICE,     // LIVELLO
    HUD_LABEL,   // MENU: il piu' silenzioso della ruota, qui non si suona
};

const char *const SCREEN_TITLE[SCREEN_COUNT] = {"SUONA", "TIMBRI", "INVIL.",  "EFFETTI",
                                                "RITMO", "LIVELLO", "MENU"};

// ------------------------------------------------------------------- helper
void textAt(const char *s, int x, int y, uint8_t size, uint16_t color) {
    gfx->setTextSize(size);
    gfx->setTextColor(color);
    gfx->setCursor(x, y);
    gfx->print(s);
}

void textCentered(const char *s, int y, uint8_t size, uint16_t color) {
    int w = 6 * size * (int)strlen(s);
    textAt(s, CX - w / 2, y, size, color);
}

// Testo allineato a destra: i valori numerici che cambiano di cifre restano
// ancorati al bordo invece di ballare.
void textRight(const char *s, int xEnd, int y, uint8_t size, uint16_t color) {
    textAt(s, xEnd - 6 * size * (int)strlen(s), y, size, color);
}

// Testo centrato su un punto qualsiasi, che e' quello che serve per le
// didascalie disposte lungo un arco. Le lettere restano dritte: il font 6x8 non
// ruota, e ruotarlo a mano vorrebbe dire disegnare i glifi pixel per pixel. Il
// tondo lo fanno gli archi; le parole si limitano a disporsi lungo di loro.
void textAtPoint(const char *s, int cx, int cy, uint8_t size, uint16_t color) {
    textAt(s, cx - 3 * size * (int)strlen(s), cy - 4 * size, size, color);
}

// La gomma unica del sistema. Cancella una fascia calcolando la mezza corda su
// OGNI riga, non sulla piu' stretta: cosi' non morde mai la ghiera e non lascia
// mai un pixel acceso di quello che c'era prima.
//
// I morsi nella cornice sono il difetto piu' insidioso di un vetro tondo, perche'
// non si richiudono da soli: l'anello si ridisegna solo al cambio schermata,
// quindi un fillRect largo quanto il quadrato lascia due tacche nella ghiera che
// restano li' finche' non cambi pagina. Con la corda vera non puo' succedere.
void radiusFill(int y, int h, int r, uint16_t color) {
    if (h <= 0) return;
    gfx->startWrite();
    for (int yy = y; yy < y + h; ++yy) {
        const int dy = yy - CY;
        const int d2 = r * r - dy * dy;
        if (d2 <= 0) continue;
        const int half = (int)sqrtf((float)d2);
        gfx->writeFastHLine(CX - half, yy, 2 * half, color);
    }
    gfx->endWrite();
}

// La gomma delle sette schermate dell'anello: si ferma prima di ogni corona.
void contentFill(int y, int h, uint16_t color = BLACK) {
    if (y + h > CONTENT_BOTTOM + 1) h = CONTENT_BOTTOM + 1 - y;
    radiusFill(y, h, CONTENT_R, color);
}

// ------------------------------------------------------------- primitive radiali
//
// Gli angoli si misurano in gradi dal 12 in punto, positivi in senso orario: e'
// la convenzione dei quadranti, non quella della trigonometria, perche' qui si
// ragiona su un display tondo e non su un piano cartesiano.
inline void polar(float deg, float r, int &x, int &y) {
    const float a = deg * (float)M_PI / 180.0f;
    x = CX + (int)(r * sinf(a) + 0.5f);
    y = CY - (int)(r * cosf(a) + 0.5f);
}

// Un blocco d'arco pieno fra due angoli e due raggi: il mattone di tutto il
// resto. Disegnato come raggi radiali a passo di un grado dentro un solo
// startWrite/endWrite, cosi' e' streaming puro sull'SPI.
void arcSeg(float a0, float a1, int rIn, int rOut, uint16_t color) {
    if (a1 < a0) {
        const float t = a0;
        a0 = a1;
        a1 = t;
    }
    gfx->startWrite();
    // Mezzo grado di passo e non uno: a raggio 115 un grado sono due pixel di
    // arco, e a passo intero il blocco esce rigato.
    for (float a = a0; a <= a1; a += 0.5f) {
        int x0, y0, x1, y1;
        polar(a, (float)rIn, x0, y0);
        polar(a, (float)rOut, x1, y1);
        gfx->writeLine(x0, y0, x1, y1, color);
    }
    gfx->endWrite();
}

// L'arco a segmenti che sostituisce la barra rettangolare, ed e' quello che
// rende visibile ogni manopola su ogni schermata. Il valore si legge come
// frazione di corsa e non come lunghezza assoluta: a colpo d'occhio "sono a tre
// quarti" e' molto piu' facile da stimare di "sono a 4200 Hz".
constexpr int KNOB_SEGS = 10;
constexpr float KNOB_SPAN = 30.0f;  // gradi occupati da un arco di manopola
// Da sinistra a destra nell'ordine fisico delle manopole. 180 gradi e' il fondo
// del vetro: le quattro si aprono a ventaglio attorno a li'.
const float KNOB_CENTER[4] = {234.0f, 198.0f, 162.0f, 126.0f};

void arcGauge(int slot, float frac, uint16_t lo, uint16_t hi) {
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const float a0 = KNOB_CENTER[slot] + KNOB_SPAN / 2.0f;  // orario = verso il basso
    const int lit = (int)(frac * KNOB_SEGS + 0.5f);
    const float seg = KNOB_SPAN / (float)KNOB_SEGS;
    for (int i = 0; i < KNOB_SEGS; ++i) {
        // Il primo segmento sta dal lato esterno del ventaglio, cosi' i quattro
        // archi si riempiono tutti "verso il centro" e la simmetria si legge.
        const float s0 = a0 - (float)(i + 1) * seg + 0.6f;
        const float s1 = a0 - (float)i * seg - 0.6f;
        const uint16_t c =
            (i < lit) ? mix565(lo, hi, (float)i / (float)(KNOB_SEGS - 1)) : HUD_TRACK;
        arcSeg(s0, s1, KNOB_IN, KNOB_OUT, c);
    }
}

// n tacche radiali distribuite fra due angoli, le prime `lit` accese.
void radialTicks(int rIn, int rOut, int n, float a0, float a1, int lit, uint16_t on,
                 uint16_t off) {
    for (int i = 0; i < n; ++i) {
        const float a = a0 + (a1 - a0) * (float)i / (float)(n - 1);
        int x0, y0, x1, y1;
        polar(a, (float)rIn, x0, y0);
        polar(a, (float)rOut, x1, y1);
        gfx->drawLine(x0, y0, x1, y1, (i < lit) ? on : off);
    }
}

// ---------------------------------------------------------------- targhette
//
// Etichetta su fondo pieno: si usa per gli stati (AVVIA, REC, TIENI) dove conta
// riconoscere il colore prima ancora di leggere la parola.
//
// Il colore del testo si sceglie sulla luminanza del fondo invece di essere
// sempre nero. Non e' un raffinamento: con il nero d'ufficio, "8 BIT SPENTO"
// scritto su HUD_TRACK — rgb(8,58,72) — era nero su quasi-nero, cioe' invisibile,
// e per due versioni la riga piu' importante della schermata effetti non si e'
// letta.
uint16_t chipText(uint16_t bg) {
    const int r = (bg >> 11) & 0x1F, g = (bg >> 5) & 0x3F, b = bg & 0x1F;
    const int luma = (2 * r * 2 + 5 * g + b * 2) / 8;  // pesi 2/5/1 su scala ~63
    return (luma > 30) ? BLACK : HUD_ICE;
}

int hudChipWidth(const char *text, uint8_t size) { return 6 * size * (int)strlen(text) + 10; }

void hudChip(int x, int y, const char *text, uint16_t bg, uint8_t size) {
    const int w = hudChipWidth(text, size);
    const int h = 8 * size + 6;
    gfx->fillRect(x, y, w, h, bg);
    textAt(text, x + 5, y + 3, size, chipText(bg));
}

void hudChipCentered(int y, const char *text, uint16_t bg, uint8_t size) {
    hudChip(CX - hudChipWidth(text, size) / 2, y, text, bg, size);
}

// Targhetta di uno stato spento: contorno invece di pieno. Acceso e spento si
// distinguono per pieno contro vuoto, non per sfumatura di colore — che su un
// vetro piccolo e' una differenza che non si vede.
void hudChipOff(int x, int y, const char *text, uint8_t size) {
    const int w = hudChipWidth(text, size);
    const int h = 8 * size + 6;
    gfx->fillRect(x, y, w, h, BLACK);
    gfx->drawRect(x, y, w, h, HUD_TRACK);
    textAt(text, x + 5, y + 3, size, HUD_DIM);
}

// Barra rettangolare: sopravvive per l'aggiornamento del firmware, che e' una
// pagina e non una schermata dello strumento.
void hudBar(int x, int y, int w, int h, float frac, uint16_t lo, uint16_t hi) {
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    constexpr int SEG = 6, GAP = 2;
    const int count = (w + GAP) / (SEG + GAP);
    const int lit = (int)(frac * count + 0.5f);
    for (int i = 0; i < count; ++i) {
        const uint16_t c =
            (i < lit) ? mix565(lo, hi, (count > 1) ? (float)i / (float)(count - 1) : 0.0f)
                      : HUD_TRACK;
        gfx->fillRect(x + i * (SEG + GAP), y, SEG, h, c);
    }
}

// ------------------------------------------------------------------- telaio
//
// Quello che c'e' su ogni schermata, sempre nello stesso posto. Si ridisegna
// solo al cambio di pagina: da li' in poi si aggiornano i quattro archi delle
// manopole e nient'altro.

// Compatibilita' con le pagine fuori dall'anello (rete, aggiornamento, avvio):
// li' il telaio radiale non serve, serve un titolo e un fondo pulito.
void chrome(const char *title, uint16_t accent) {
    gfx->fillScreen(BLACK);
    gfx->drawCircle(CX, CY, 118, dim565(accent, 1, 3));
    gfx->drawCircle(CX, CY, 117, dim565(accent, 1, 6));

    textCentered(title, 22, 2, accent);

    // Parentesi ai lati del titolo. La lunghezza si adatta: con un titolo lungo
    // lo spazio dentro il cerchio finisce, e un trattino che sborda si vedrebbe
    // tagliato dalla cornice tonda.
    const int tw = 12 * (int)strlen(title);
    const uint16_t bracket = dim565(accent, 2, 3);
    const int inner = tw / 2 + 6;
    // Il punto critico e' la cima del trattino verticale, a y=24, cioe' 96 px
    // sopra il centro: perche' resti dentro l'area utile la mezza larghezza li'
    // vale al massimo sqrt(116^2 - 96^2) = 65.
    const int outer = 65;
    if (outer - inner >= 10) {
        for (int side = 0; side < 2; ++side) {
            const int x0 = (side == 0) ? (CX - outer) : (CX + inner);
            gfx->drawFastHLine(x0, 30, outer - inner, bracket);
            gfx->drawFastVLine((side == 0) ? x0 : (x0 + outer - inner - 1), 24, 7, bracket);
        }
    }

    gfx->drawFastHLine(40, 46, 160, dim565(accent, 1, 4));
    gfx->drawFastHLine(CX - 30, 46, 60, accent);
    gfx->drawFastHLine(CX - 30, 47, 60, dim565(accent, 1, 2));
}

// La corona di posizione: sette settori, uno per schermata, ognuno del proprio
// accento. Quello dove sei e' acceso pieno, gli altri sei sono il loro fantasma.
//
// Serve a rispondere alla domanda che l'anello di schermate poneva e non
// risolveva: con nove pagine percorribili nei due sensi, l'unico segnale di dove
// ti trovassi era il titolo, e per sapere quanto mancava alla prossima bisognava
// ricordarsi l'ordine. Sette settori colorati si contano con la coda dell'occhio,
// e siccome ogni settore ha il colore della sua pagina, dopo due giri la ghiera
// e' una mappa: il viola e' i timbri, il lime e' il ritmo.
void drawPosRing(uint8_t current) {
    constexpr float STEP = 360.0f / (float)SCREEN_COUNT;
    constexpr float GAP = 6.0f;
    for (int i = 0; i < SCREEN_COUNT; ++i) {
        const float a0 = (float)i * STEP + GAP / 2.0f;
        const float a1 = (float)(i + 1) * STEP - GAP / 2.0f;
        const uint16_t c = SCREEN_ACCENT[i];
        // I settori spenti sono a due quinti e non al quarto del fantasma
        // standard: qui non stanno dicendo "questo e' disattivato", stanno
        // disegnando una mappa, e una mappa che non si legge non e' una mappa.
        // Restano comunque molto piu' scuri dell'attivo, che e' pieno.
        arcSeg(a0, a1, POSRING_IN, POSRING_OUT, (i == current) ? c : dim565(c, 2, 5));
    }
}

// La targhetta dell'ottava, in alto a sinistra, su ogni schermata e in ogni
// modalita'. E' il motivo per cui il joystick verticale non ha piu' bisogno di
// far comparire un riquadro al centro: un valore sempre a video non va
// annunciato, va guardato. Il fondo e' il colore dell'ottava, lo stesso che
// prendono le celle del sequencer e i LED sotto i tasti.
void drawOctaveChip(int8_t oct) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%+d", (int)oct);
    // Ventisei pixel di gomma e non quarantaquattro: il testo e' sempre di due
    // caratteri (da "-2" a "+2"), quindi la targhetta e' larga 22 e finisce a
    // x=50. Le targhette di stato, allineate a destra, cominciano a x=60: una
    // gomma piu' larga ne staccherebbe la prima lettera ad ogni cambio d'ottava,
    // e siccome l'ottava si disegna **dopo** il contenuto, il morso resterebbe.
    gfx->fillRect(26, CHIP_Y, 26, 14, BLACK);
    hudChip(28, CHIP_Y, buf, octColor(oct), 1);
}

// La corona dei comandi. Quattro archi in basso, uno per manopola, da sinistra a
// destra come stanno sul pannello, e sotto ognuno la sua didascalia.
//
// E' il pezzo su cui si regge tutto il resto: finche' ogni manopola dichiara da
// se' cosa fa e quanto vale, non serve un riquadro che lo venga a dire al centro
// dello schermo coprendo proprio la cosa che stai guardando. La didascalia mostra
// il NOME quando la manopola e' ferma e il VALORE per i novecento millisecondi
// dopo uno scatto: il nome insegna, il valore conferma, e nessuno dei due ruba
// spazio all'altro.
struct KnobSlot {
    // Undici caratteri piu' il terminatore: e' la riga piu' lunga dell'elenco
    // EFFETTI ("PROFONDITA") e la piu' lunga del menu ("IMPARA LUCI"). Con otto
    // byte venivano tagliate a sette e la didascalia diceva "IMPARA " — che in
    // una schermata il cui unico scopo e' non far ricordare niente e' proprio il
    // difetto che non ci si puo' permettere.
    char label[12];
    char value[12];
    float frac;
    uint32_t valueUntil;
};

KnobSlot knobSlot[4];

// Pulizia della didascalia, larga quanto il testo e non quanto il caso peggiore.
//
// Le due didascalie di mezzo hanno i centri a soli 52 px l'uno dall'altro: un
// riquadro fisso da undici caratteri sarebbe largo 66 e i due si sovrapporrebbero
// di quattordici pixel, cioe' riscrivendo VOL si mangerebbe la coda di
// "PROFONDITA" senza ridisegnarla. Cancellando la larghezza vera il problema non
// si pone, perche' quella di mezzo a destra e' sempre corta (VOL, SOST) e le due
// esterne stanno su un'altra riga.
void clearCaption(int slot, int chars) {
    int cx, cy;
    polar(KNOB_CENTER[slot], (float)CAPTION_R, cx, cy);
    const int w = 6 * chars + 4;
    gfx->fillRect(cx - w / 2, cy - 5, w, 10, BLACK);
}

void drawCaption(int slot, uint32_t now) {
    int cx, cy;
    polar(KNOB_CENTER[slot], (float)CAPTION_R, cx, cy);
    const bool showValue = (knobSlot[slot].valueUntil != 0) && (now < knobSlot[slot].valueUntil);
    const char *s = showValue ? knobSlot[slot].value : knobSlot[slot].label;
    textAtPoint(s, cx, cy, 1, showValue ? HUD_ICE : HUD_LABEL);
}

// Il numero della manopola, disegnato una volta sola al cambio schermata. Un "1"
// accanto al primo ventaglio e' quello che lega la cosa disegnata alla cosa che
// hai sotto le dita.
//
// Sta **dentro** l'arco, a raggio 94, e non fuori: a 116 il glifo arrivava a
// raggio 117,4, cioe' esattamente addosso all'anello — e siccome l'anello si
// ridisegna solo al cambio di pagina, quella cifra ci restava appoggiata sopra
// come una sbavatura. Fra la didascalia (84) e l'arco (100) di spazio ce n'e', e
// li' il numero e' anche piu' vicino a cio' che nomina.
void drawKnobIndex(int slot) {
    int cx, cy;
    polar(KNOB_CENTER[slot], 94.0f, cx, cy);
    char n[2] = {(char)('1' + slot), '\0'};
    textAtPoint(n, cx, cy, 1, HUD_DIM);
}

// ------------------------------------------------------- disco dei contenuti
//
// La gomma del disco: come chordFill, ma sulla corda del disco invece che su
// quella del vetro. Serve perche' dentro il disco si cancella spesso, e cancellare
// sulla corda grande porterebbe via gli archi delle manopole che stanno appena
// piu' in basso.
void discFill(int y, int h, uint16_t color = BLACK) {
    if (h <= 0) return;
    gfx->startWrite();
    for (int yy = y; yy < y + h; ++yy) {
        const int dy = yy - DISC_CY;
        const int d2 = DISC_R * DISC_R - dy * dy;
        if (d2 <= 0) continue;
        const int half = (int)sqrtf((float)d2);
        gfx->writeFastHLine(CX - half, yy, 2 * half, color);
    }
    gfx->endWrite();
}

// ------------------------------------------------------------------- SUONA
//
// La schermata su cui si sta di piu' era la piu' povera dell'anello: usava tre
// campi su oltre trenta e lasciava sessanta righe di pixel vuote proprio dove il
// vetro e' piu' largo. Disegnava anche l'onda scelta, ma la disegnava a formule —
// un ritratto, non la cosa.
//
// Adesso al centro c'e' la traccia VERA dell'uscita, e dietro, come orizzonte, la
// risposta del filtro con la gobba della risonanza. Sono le quattro manopole di
// questa pagina messe in figura: giri la prima e la forma cambia, giri la seconda
// e l'orizzonte scorre, giri la quarta e gli si alza la gobba. Non c'e' niente da
// annunciare con un riquadro, perche' e' gia' tutto sotto gli occhi.
constexpr int TR_X0 = DISC_CX_LEFT;
constexpr int TR_W = 2 * DISC_R - 3;

int16_t curveY[TR_W];  // l'orizzonte: y della risposta del filtro, colonna per colonna

// Risposta del filtro a due poli, la stessa del motore audio, campionata sulle
// colonne della traccia. Con la risonanza a zero e' la solita discesa dolce,
// alzandola compare la gobba sul taglio: e' l'unico modo di far vedere cosa fa
// quella manopola senza doverla sentire.
void computeFilterCurve(float cutoffHz, float resonance) {
    const float decades = logf(100.0f);
    const float damp = 2.0f - 1.94f * resonance;
    for (int i = 0; i < TR_W; ++i) {
        const int x = TR_X0 + i;
        const int dx = x - CX;
        const int d2 = DISC_R * DISC_R - dx * dx;
        if (d2 <= 0) {
            curveY[i] = -1;
            continue;
        }
        const int half = (int)sqrtf((float)d2);
        const float f = 80.0f * expf(decades * (float)i / (float)(TR_W - 1));
        const float r = f / cutoffHz;
        const float den = (1.0f - r * r);
        float mag = 1.0f / sqrtf(den * den + (damp * r) * (damp * r));
        mag = mag / (1.0f + mag * 0.35f);  // il picco puo' andare a venti volte
        int y = DISC_CY + half - (int)(mag * (float)(2 * half - 2));
        if (y < DISC_CY - half) y = DISC_CY - half;
        if (y > DISC_CY + half) y = DISC_CY + half;
        curveY[i] = (int16_t)y;
    }
}

void drawFilterCurve(uint16_t color) {
    for (int i = 0; i < TR_W; ++i) {
        if (curveY[i] >= 0) gfx->drawPixel(TR_X0 + i, curveY[i], color);
    }
}

void drawSuonaScreen(const SynthView &v, bool full) {
    static int8_t samples[SCOPE_SAMPLES];
    static int16_t colTop[TR_W];
    static uint8_t colH[TR_W];
    static bool traced = false;
    static float zoom = 1.0f;

    const uint16_t horizon = ghost(HUD_MAGENTA);

    if (full) {
        // La fila delle targhette: quattro interruttori che si leggono per pieno
        // contro vuoto. Sono gli stessi quattro tasti che hanno la parola
        // stampata sopra, e portano lo stesso colore del LED che li illumina: la
        // targhetta e il tasto sono la stessa cosa, e non c'e' bisogno di dirlo.
        gfx->fillRect(58, CHIP_Y, CHIP_X1 - 58, 14, BLACK);
        struct Flag {
            const char *label;
            bool on;
            uint16_t color;
        } flags[4] = {
            {"ARP", v.arp, HUD_NEON},
            {"8BIT", v.crush, HUD_ORANGE},
            {"TIENI", v.hold, HUD_AMBER},
            {v.poly ? "POLI" : "MONO", v.poly, HUD_LIME},
        };
        int total = 0;
        for (int i = 0; i < 4; ++i) total += hudChipWidth(flags[i].label, 1) + 4;
        int x = CHIP_X1 - (total - 4);
        for (int i = 0; i < 4; ++i) {
            if (flags[i].on) {
                hudChip(x, CHIP_Y, flags[i].label, flags[i].color, 1);
            } else {
                hudChipOff(x, CHIP_Y, flags[i].label, 1);
            }
            x += hudChipWidth(flags[i].label, 1) + 4;
        }

        discFill(DISC_CY - DISC_R, 2 * DISC_R);
        computeFilterCurve(v.cutoffHz, v.resonance);
        drawFilterCurve(horizon);
        traced = false;
        zoom = 1.0f;
    } else {
        // Le targhette non hanno un valore che scivola: cambiano di scatto, e
        // quando cambiano si rifa' la fila intera. Costa 158x14 px una volta ogni
        // pressione di tasto, cioe' niente.
        if (v.arp != prev.arp || v.crush != prev.crush || v.hold != prev.hold ||
            v.poly != prev.poly) {
            drawSuonaScreen(v, true);
            return;
        }
        if (fabsf(v.cutoffHz - prev.cutoffHz) > 5.0f ||
            fabsf(v.resonance - prev.resonance) > 0.005f) {
            drawFilterCurve(BLACK);
            computeFilterCurve(v.cutoffHz, v.resonance);
            drawFilterCurve(horizon);
        }
    }

    // La corona delle voci: sedici tacche sull'arco basso, accese quante ne stanno
    // suonando. Il colore dice la modalita' — lime in polifonico, ambra in mono —
    // quindi premere VOCI si vede qui prima ancora che nella targhetta.
    if (full || v.voices != prev.voices || v.poly != prev.poly) {
        radialTicks(48, 55, 16, 130.0f, 230.0f, v.voices,
                    v.poly ? HUD_LIME : HUD_AMBER, HUD_TRACK);
    }

    // Finestra nuova o niente: senza aggancio fresco si tiene a video l'ultima.
    if (!AudioEngine::copyScope(samples)) return;

    int peak = 1;
    for (int i = 0; i < SCOPE_SAMPLES; ++i) {
        const int a = (samples[i] < 0) ? -samples[i] : samples[i];
        if (a > peak) peak = a;
    }
    // Zoom automatico: a volume dimezzato e con la compensazione di polifonia una
    // traccia a scala fissa sarebbe una riga quasi piatta. Il fattore insegue il
    // valore giusto lentamente, altrimenti l'onda "respira" ad ogni fotogramma.
    float target = 110.0f / (float)peak;
    if (target > 8.0f) target = 8.0f;
    if (target < 1.0f) target = 1.0f;
    zoom = traced ? (zoom + (target - zoom) * 0.15f) : target;

    gfx->startWrite();
    int prevY = DISC_CY;
    bool clipped = false;
    for (int i = 0; i < TR_W; ++i) {
        const int x = TR_X0 + i;
        const int dx = x - CX;
        const int d2 = DISC_R * DISC_R - dx * dx;
        if (d2 <= 0) continue;
        // Ogni colonna e' tosata sulla corda del disco: e' cosi' che la traccia
        // prende la forma tonda invece di stare dentro un riquadro.
        const int half = (int)sqrtf((float)d2);

        // 105 colonne da 180 campioni: si prende il campione piu' vicino, che a
        // questa densita' non si distingue da un'interpolazione.
        const int s = samples[(i * SCOPE_SAMPLES) / TR_W];
        int y = DISC_CY - (int)((float)s * zoom * ((float)DISC_R / 127.0f));
        if (y < DISC_CY - half) {
            y = DISC_CY - half;
            clipped = true;
        }
        if (y > DISC_CY + half) {
            y = DISC_CY + half;
            clipped = true;
        }
        if (i == 0) prevY = y;

        if (traced) {
            gfx->writeFastVLine(x, colTop[i], colH[i], BLACK);
            // La cancellazione porta via anche l'orizzonte: si rimette solo dove
            // passava davvero, un pixel alla volta. E' la stessa tecnica con cui
            // la vecchia schermata SCOPE rimetteva la griglia, ed e' la ragione
            // per cui qui non lampeggia niente.
            if (curveY[i] >= colTop[i] && curveY[i] < colTop[i] + colH[i]) {
                gfx->writePixel(x, curveY[i], horizon);
            }
        }

        // Il segmento copre il salto rispetto alla colonna precedente: sui fronti
        // ripidi (quadra, dente di sega) la traccia resta continua.
        const int top = (y < prevY) ? y : prevY;
        const int h = ((y < prevY) ? (prevY - y) : (y - prevY)) + 1;
        colTop[i] = (int16_t)top;
        colH[i] = (uint8_t)h;
        prevY = y;
    }
    // Il colore si decide dopo aver guardato tutte le colonne: la traccia e' una
    // cosa sola e non puo' cambiare tinta a meta'. In arancione vuol dire che sta
    // toccando i bordi — prima un clip a schermo si vedeva identico a un'onda
    // quadra, e non c'era nessun modo di accorgersene.
    const uint16_t tc = clipped ? HUD_ORANGE : HUD_LIME;
    for (int i = 0; i < TR_W; ++i) {
        const int dx = TR_X0 + i - CX;
        if (DISC_R * DISC_R - dx * dx <= 0) continue;
        gfx->writeFastVLine(TR_X0 + i, colTop[i], colH[i], tc);
    }
    gfx->endWrite();
    traced = true;
}

// ------------------------------------------------------------------- ritratti
//
// Mezzo ciclo dell'onda e profilo dell'inviluppo: due disegnini che dicono di un
// timbro molto piu' del suo nome. Servono a TIMBRI, e il profilo serve anche a
// INVILUPPO, dove e' grande e si deforma mentre giri.

// Valore -1..+1 della forma d'onda alla fase t (0..1). Stessa forma del motore.
float waveValue(uint8_t wave, float t) {
    switch (wave) {
        case WAVE_SQUARE: return (t < 0.5f) ? 1.0f : -1.0f;
        case WAVE_SAW: return 2.0f * t - 1.0f;
        case WAVE_TRIANGLE: return (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
        case WAVE_PULSE: return (t < 0.25f) ? 1.0f : -1.0f;
        case WAVE_NOISE: {
            // Rumore *riproducibile*: a parita' di t esce sempre lo stesso valore.
            // Con un random vero l'icona cambierebbe ad ogni fotogramma e
            // sembrerebbe rotta invece che rumorosa.
            uint32_t h = (uint32_t)(t * 64.0f) * 1664525u + 1013904223u;
            h ^= h >> 13;
            return (float)((int32_t)((h >> 8) & 0xFFFF) - 32768) * (1.0f / 32768.0f);
        }
        case WAVE_SINE:
        default: return sinf(2.0f * (float)M_PI * t);
    }
}

void drawWaveShape(int x, int y, int w, int h, uint8_t wave, uint16_t color) {
    const int cy = y + h / 2;
    int prevY = cy;
    gfx->startWrite();
    for (int i = 0; i < w; ++i) {
        const float t = (float)i / (float)(w - 1) * 2.0f;  // due cicli
        const int yy = cy - (int)(waveValue(wave, t - (float)(int)t) * (float)(h / 2 - 1));
        if (i == 0) prevY = yy;
        const int top = (yy < prevY) ? yy : prevY;
        const int hh = ((yy < prevY) ? (prevY - yy) : (yy - prevY)) + 1;
        gfx->writeFastVLine(x + i, top, hh, color);
        prevY = yy;
    }
    gfx->endWrite();
}

// Il profilo A/D/S/R disegnato dai valori veri. I tempi vanno in scala
// logaritmica come le frazioni delle manopole, altrimenti l'attacco — che va da 2
// a 500 ms — occuperebbe un pixel per tre quarti della sua corsa.
//
// E' l'unica cosa che INVILUPPO descriveva senza farla vedere: quattro numeri e
// quattro barre per una forma che si capisce in un colpo d'occhio se solo la
// disegni. Con i colori divisi per tratto — e gli stessi quattro colori sugli
// archi delle manopole — si vede anche quale manopola comanda quale pezzo.
void drawEnvelope(int x, int y, int w, int h, float aMs, float dMs, float sus, float rMs,
                  bool colorful) {
    const float fa = logf(aMs / 2.0f) / logf(250.0f);
    const float fd = logf(dMs / 5.0f) / logf(200.0f);
    const float fr = logf(rMs / 10.0f) / logf(200.0f);
    // Le quattro fasi si spartiscono la larghezza in proporzione, con un minimo
    // perche' nessun tratto sparisca del tutto: un attacco a 2 ms resta una
    // parete verticale, ma deve restare visibile che c'e'.
    const float tot = fa + fd + 0.9f + fr + 0.4f;
    const int wa = (int)((fa + 0.1f) / tot * w);
    const int wd = (int)((fd + 0.1f) / tot * w);
    const int wr = (int)((fr + 0.1f) / tot * w);
    int ws = w - wa - wd - wr;
    if (ws < 4) ws = 4;

    const int yBot = y + h - 1;
    const int ySus = yBot - (int)(sus * (float)(h - 1));
    int px = x, py = yBot;
    struct Leg {
        int x, y;
        uint16_t c;
    } legs[4] = {
        {x + wa, y, colorful ? HUD_RED : HUD_AMBER},
        {x + wa + wd, ySus, colorful ? HUD_MAGENTA : HUD_AMBER},
        {x + wa + wd + ws, ySus, colorful ? HUD_AMBER : HUD_AMBER},
        {x + wa + wd + ws + wr, yBot, colorful ? HUD_LIME : HUD_AMBER},
    };
    for (int i = 0; i < 4; ++i) {
        gfx->drawLine(px, py, legs[i].x, legs[i].y, legs[i].c);
        gfx->drawLine(px, py + 1, legs[i].x, legs[i].y + 1, legs[i].c);
        px = legs[i].x;
        py = legs[i].y;
    }
    if (colorful) {
        // Un pallino sui vertici, del colore del suo parametro: lega la manopola
        // al punto della curva che muove.
        for (int i = 0; i < 4; ++i) gfx->fillCircle(legs[i].x, legs[i].y, 3, legs[i].c);
    }
}

// ------------------------------------------------------------------ TIMBRI
//
// Non piu' un elenco rettangolare piantato dentro un cerchio — con le righe che
// sulla prima riga sbordavano e staccavano due morsi dalla ghiera — ma una
// scheda: il nome scelto grande al centro, i vicini piccoli sopra e sotto, e in
// mezzo il ritratto del timbro, che prima non c'era. Due livelli di importanza
// dove ce n'era uno solo, ed e' la ragione per cui la voce scelta si vede senza
// bisogno di un riquadro che venga a dirlo.
constexpr float LIST_A0 = 35.0f;  // arco di posizione dell'elenco, sul fianco destro
constexpr float LIST_A1 = 100.0f;

void drawListArc(int index, int count, uint16_t accent) {
    if (count < 1) return;
    const float span = (LIST_A1 - LIST_A0) / (float)count;
    for (int i = 0; i < count; ++i) {
        const float a0 = LIST_A0 + span * (float)i + 0.4f;
        const float a1 = LIST_A0 + span * (float)(i + 1) - 0.4f;
        arcSeg(a0, a1, KNOB_IN, KNOB_OUT, (i == index) ? accent : ghost(accent));
    }
}

void drawTimbriScreen(const SynthView &v, bool full) {
    if (!full && v.timbroCursor == prev.timbroCursor && v.timbro == prev.timbro) return;

    const int sel = (v.timbroCursor < PRESET_COUNT) ? v.timbroCursor : 0;
    const Preset &p = PRESETS[sel];

    contentFill(68, 88);

    if (sel > 0) textCentered(PRESETS[sel - 1].name, 70, 1, HUD_DIM);
    if (sel + 1 < (int)PRESET_COUNT) textCentered(PRESETS[sel + 1].name, 146, 1, HUD_DIM);

    // Il pallino dice quale sta *suonando adesso*, che con la manopola che scorre
    // liberamente non e' per forza quello sotto il cursore.
    textCentered(p.name, 82, 2, HUD_ICE);
    if (sel == (int)v.timbro) gfx->fillCircle(CX - 6 * (int)strlen(p.name) - 10, 90, 3, HUD_VIOLET);

    textCentered(p.hint, 102, 1, HUD_LABEL);

    // Il ritratto: com'e' fatta la voce e come si spegne. Un timbro si sceglie
    // per come suona, ma vedere che il pianoforte ha la coda che scende e
    // l'organo no e' esattamente cio' che insegna a cosa servono le altre
    // schermate.
    drawWaveShape(CX - 48, 114, 96, 14, p.wave, HUD_NEON);
    drawEnvelope(CX - 48, 130, 96, 14, p.attackMs, p.decayMs, p.sustain, p.releaseMs, false);

    drawListArc(sel, PRESET_COUNT, HUD_VIOLET);
}

// --------------------------------------------------------------- INVILUPPO
//
// Le quattro righe A/D/S/R sono sparite: erano quattro etichette, quattro barre e
// quattro numeri per dire quello che adesso dicono le quattro manopole della
// corona, che stanno sempre li' e nello stesso ordine in cui si leggono. Il disco
// se lo prende tutto la forma, che finalmente c'e': era l'unica schermata che
// descriveva una figura senza disegnarla.
void drawInviluppoScreen(const SynthView &v, bool full) {
    if (!full && fabsf(v.attackMs - prev.attackMs) < 0.5f &&
        fabsf(v.decayMs - prev.decayMs) < 0.5f && fabsf(v.sustain - prev.sustain) < 0.005f &&
        fabsf(v.releaseMs - prev.releaseMs) < 0.5f) {
        return;
    }

    contentFill(66, 88);

    // I quattro numeri, piccoli e in alto: il valore esatto serve quando lo
    // cerchi, non mentre giri — mentre giri lo dice gia' la didascalia della
    // manopola, che sta nel punto dove hai la mano.
    char buf[40];
    snprintf(buf, sizeof(buf), "%d  %d  %d%%  %d", (int)v.attackMs, (int)v.decayMs,
             (int)(v.sustain * 100.0f + 0.5f), (int)v.releaseMs);
    textCentered(buf, 68, 1, HUD_DIM);

    // Il tratteggio verticale sotto la curva le da' massa senza costare come un
    // pieno: una riga ogni sei pixel, in fantasma.
    const int x0 = CX - 52, w = 104, y0 = 84, h = 68;
    for (int i = 0; i < w; i += 6) {
        gfx->drawFastVLine(x0 + i, y0 + h - 14, 13, ghost(HUD_AMBER));
    }
    drawEnvelope(x0, y0, w, h, v.attackMs, v.decayMs, v.sustain, v.releaseMs, true);
}

// ---------------------------------------------------------- elenchi a carosello
//
// TIMBRI, EFFETTI e MENU sono tre volte la stessa cosa: una voce scelta grande al
// centro con il suo valore, due vicine piccole e spente, e un arco che dice a che
// punto dell'elenco sei. Prima erano tre implementazioni incompatibili — tre
// passi verticali diversi, tre larghezze di pulizia diverse, tre corpi diversi per
// il valore — ed era la mancanza di astrazione che costava di piu' a chi doveva
// ridisegnare.
struct CarouselItem {
    const char *category;
    const char *label;
    const char *value;
    bool action;
};

void drawCarousel(int cursor, int count, uint16_t accent, float frac,
                  CarouselItem (*fetch)(int)) {
    contentFill(66, 90);

    const CarouselItem cur = fetch(cursor);

    // L'intestazione di categoria della voce scelta: dice in che famiglia sei
    // senza costringerti a scorrere all'indietro per ritrovarla. Se la voce non
    // ne apre una nuova si risale finche' non se ne trova una.
    const char *cat = nullptr;
    for (int i = cursor; i >= 0 && !cat; --i) cat = fetch(i).category;
    if (cat) textCentered(cat, 68, 1, HUD_LABEL);

    if (cursor > 0) textCentered(fetch(cursor - 1).label, 80, 1, HUD_DIM);
    if (cursor + 1 < count) textCentered(fetch(cursor + 1).label, 146, 1, HUD_DIM);

    textCentered(cur.label, 92, 2, cur.action ? HUD_RED : accent);
    // Il corpo si sceglie sulla lunghezza. A corpo 3 un valore da undici caratteri
    // — "LRC BCK DIN", l'ordine dei fili dell'uscita audio — sarebbe largo 198 px
    // su una corda che li' ne offre 188: sborderebbe dal cerchio e resterebbe
    // fuori anche dalla fascia che lo cancella.
    const size_t vn = strlen(cur.value);
    const uint8_t vs = (vn <= 8) ? 3 : (vn <= 12) ? 2 : 1;
    textCentered(cur.value, 112 + (24 - 8 * vs) / 2, vs, cur.action ? HUD_RED : HUD_ICE);

    if (!cur.action) {
        // Barra a segmenti larga 120: la frazione di corsa si legge prima del
        // numero, ed e' quello che serve mentre giri.
        hudBar(CX - 60, 138, 120, 5, frac, ghost(accent), accent);
    }

    drawListArc(cursor, count, accent);
}

// ------------------------------------------------------------------ EFFETTI
const SynthView *carouselView = nullptr;

CarouselItem fxItem(int i) {
    CarouselItem it;
    it.category = FX_ROWS[i].category;
    it.label = FX_ROWS[i].label;
    it.value = carouselView->fxValue[i];
    it.action = false;
    return it;
}

void drawEffettiScreen(const SynthView &v, bool full) {
    if (!full && v.fxCursor == prev.fxCursor &&
        fabsf(v.fxFrac[v.fxCursor] - prev.fxFrac[v.fxCursor]) < 0.0005f &&
        strcmp(v.fxValue[v.fxCursor], prev.fxValue[v.fxCursor]) == 0) {
        return;
    }
    carouselView = &v;
    drawCarousel(v.fxCursor, FX_ROW_COUNT, HUD_MAGENTA, v.fxFrac[v.fxCursor], fxItem);
}

// --------------------------------------------------------------------- MENU
CarouselItem menuItem(int i) {
    CarouselItem it;
    it.category = Settings::ENTRIES[i].category;
    it.label = Settings::ENTRIES[i].label;
    it.action = Settings::isAction(i);
    if (it.action) {
        it.value = "TIENI";
    } else {
        it.value = Settings::valueLabel(i, carouselView->setIndex[i]);
    }
    return it;
}

void drawMenuScreen(const SynthView &v, bool full) {
    if (!full && v.setCursor == prev.setCursor &&
        v.setIndex[v.setCursor] == prev.setIndex[v.setCursor]) {
        return;
    }
    carouselView = &v;
    const uint8_t c = v.setCursor;
    const float frac = Settings::isAction(c)
                           ? 0.0f
                           : ((Settings::valueCount(c) > 1)
                                  ? (float)v.setIndex[c] / (float)(Settings::valueCount(c) - 1)
                                  : 0.0f);
    drawCarousel(c, SETTING_MENU_COUNT, HUD_LABEL, frac, menuItem);
}

// ------------------------------------------------------------------- RITMO
//
// La griglia 2x8 resta rettangolare, e va detto perche': sedici celle leggibili
// su un anello non ci stanno, il fondo del vetro l'hanno gia' preso le manopole,
// e la griglia com'e' e' la cosa migliore del firmware. Si e' spostata dove il
// cerchio e' piu' largo, cosi' nessun morso e' piu' possibile, e la forma tonda
// del giro la da' la testina che orbita nel telaio.
//
// Quello che e' sparito e' lo STEP EDIT come modalita': il cursore c'e' sempre,
// la prima manopola lo muove e la seconda scrive. Non si entra piu' in niente, e
// quindi non ci si puo' piu' restare dentro senza accorgersene.
const char *const NOTE_LETTERS[NOTE_COUNT] = {"C", "c", "D", "d", "E", "F", "f",
                                              "G", "g", "A", "a", "B", "C"};

inline bool validNote(int8_t n) { return n >= 0 && n < NOTE_COUNT; }

constexpr int GRID_X0 = 30;
constexpr int GRID_Y0 = 74;
constexpr int GRID_CELL = 20;
constexpr int GRID_GAP = 3;

void gridCellPos(int i, int &x, int &y) {
    x = GRID_X0 + (i % 8) * (GRID_CELL + GRID_GAP);
    y = GRID_Y0 + (i / 8) * (GRID_CELL + GRID_GAP);
}

void drawStepCell(const SynthView &v, int i) {
    int x, y;
    gridCellPos(i, x, y);

    const Sequencer::Step &s = Sequencer::stepAt(i);
    const bool playhead = (v.seqMode != Sequencer::SEQ_IDLE) && (i == v.seqStep);
    const bool cursor = v.seqEditing && (i == v.seqCursor);

    if (s.note == SEQ_TIE) {
        // Legato: nessuna nota nuova, solo il proseguimento della precedente.
        gfx->fillRect(x, y, GRID_CELL, GRID_CELL, BLACK);
        gfx->drawRect(x, y, GRID_CELL, GRID_CELL, HUD_TRACK);
        gfx->fillRect(x + 3, y + GRID_CELL / 2 - 1, GRID_CELL - 6, 3, HUD_ICE);
    } else if (!validNote(s.note)) {
        // Pausa: casella vuota col solo contorno. Piena, com'era prima, la griglia
        // sembrava scritta anche dove non c'era niente.
        gfx->fillRect(x, y, GRID_CELL, GRID_CELL, BLACK);
        gfx->drawRect(x, y, GRID_CELL, GRID_CELL, HUD_TRACK);
    } else {
        gfx->fillRect(x, y, GRID_CELL, GRID_CELL, octColor(s.oct));
        gfx->setTextSize(1);
        gfx->setTextColor(BLACK);
        gfx->setCursor(x + 7, y + 6);
        gfx->print(NOTE_LETTERS[s.note]);
    }

    // Battere: le caselle 1, 5, 9 e 13 portano un trattino sopra, cosi' i quattro
    // movimenti si contano senza doverli cercare.
    if (i % 4 == 0) gfx->drawFastHLine(x, y - 2, GRID_CELL, HUD_LABEL);

    // La testina passa sopra al contenuto senza cancellarlo: cornice spessa.
    if (playhead) {
        const uint16_t c = (v.seqMode == Sequencer::SEQ_RECORDING) ? HUD_RED : HUD_LIME;
        gfx->drawRect(x, y, GRID_CELL, GRID_CELL, c);
        gfx->drawRect(x + 1, y + 1, GRID_CELL - 2, GRID_CELL - 2, c);
    }
    if (cursor) gfx->drawRect(x, y, GRID_CELL, GRID_CELL, WHITE);
}

// La testina che orbita nel telaio: sedici posizioni su un giro completo, a
// raggio 94. E' il giro visto come giro — un sequencer e' un anello, e su un
// display tondo dirlo costa un pallino.
int8_t orbitDrawn = -1;

void drawOrbit(const SynthView &v, bool force) {
    const bool running = (v.seqMode != Sequencer::SEQ_IDLE);
    const int8_t want = running ? (int8_t)v.seqStep : (int8_t)-1;
    if (!force && want == orbitDrawn) return;
    if (orbitDrawn >= 0) {
        int x, y;
        polar((float)orbitDrawn * 22.5f, 94.0f, x, y);
        gfx->fillCircle(x, y, 3, BLACK);
    }
    if (want >= 0) {
        int x, y;
        polar((float)want * 22.5f, 94.0f, x, y);
        gfx->fillCircle(x, y, 3,
                        (v.seqMode == Sequencer::SEQ_RECORDING) ? HUD_RED : HUD_LIME);
    }
    orbitDrawn = want;
}

// Preconteggio: il pattern gira gia', ma qui conta solo sapere quando si parte.
void drawCountIn(const SynthView &v, bool full) {
    if (!full && v.countIn == prev.countIn) return;
    contentFill(66, 90);
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", (int)v.countIn);
    textCentered(buf, 74, 8, HUD_RED);
    radialTicks(48, 56, 4, 150.0f, 210.0f, v.countIn, HUD_RED, HUD_TRACK);
    textCentered("SUONA AL VIA", 144, 1, HUD_LABEL);
}

void drawRitmoScreen(const SynthView &v, bool full) {
    if (v.countIn > 0) {
        drawCountIn(v, full || prev.countIn == 0);
        drawOrbit(v, full);
        return;
    }
    if (prev.countIn > 0) full = true;

    // Lo stato del trasporto e' una targhetta piena: il colore lo riconosci prima
    // di aver letto la parola, che e' quello che serve mentre suoni.
    if (full || v.seqMode != prev.seqMode || v.seqEditing != prev.seqEditing) {
        gfx->fillRect(58, CHIP_Y, CHIP_X1 - 58, 14, BLACK);
        const char *label = "STOP";
        uint16_t col = HUD_LABEL;
        if (v.seqMode == Sequencer::SEQ_RECORDING) {
            label = "REC";
            col = HUD_RED;
        } else if (v.seqMode == Sequencer::SEQ_PLAYING) {
            label = "PLAY";
            col = HUD_LIME;
        }
        hudChip(62, CHIP_Y, label, col, 1);
        // A giro fermo i tredici tasti scrivono invece di limitarsi a suonare:
        // e' l'unico momento in cui una tastiera fa due cose, e va detto.
        if (v.seqEditing) {
            hudChip(CHIP_X1 - hudChipWidth("SCRIVI", 1), CHIP_Y, "SCRIVI", HUD_ICE, 1);
        }
    }

    const bool patternChanged = full || v.seqRev != prev.seqRev || v.seqMode != prev.seqMode ||
                                v.seqEditing != prev.seqEditing;
    const bool marksMoved = v.seqStep != prev.seqStep || v.seqCursor != prev.seqCursor;

    if (patternChanged) {
        for (int i = 0; i < SEQ_STEPS; ++i) drawStepCell(v, i);
    } else if (marksMoved) {
        // A tempo alto la testina si sposta ogni 60 ms: ridisegnare tutte e sedici
        // le celle sprecherebbe SPI. Bastano quelle che ha lasciato e quelle che
        // ha appena preso.
        drawStepCell(v, prev.seqStep);
        drawStepCell(v, prev.seqCursor);
        drawStepCell(v, v.seqStep);
        drawStepCell(v, v.seqCursor);
    }

    // Il grande leggio: il passo sotto il cursore e cosa c'e' scritto dentro. E'
    // il valore della manopola NOTA, che da sola sostituisce i tre gesti nascosti
    // con cui prima si scriveva una pausa, un legato o una nota.
    if (patternChanged || marksMoved ||
        (v.seqNoteName != prev.seqNoteName && v.seqNoteName && prev.seqNoteName &&
         strcmp(v.seqNoteName, prev.seqNoteName) != 0)) {
        contentFill(126, 26);
        char buf[24];
        snprintf(buf, sizeof(buf), "%02d %s", v.seqCursor + 1,
                 v.seqNoteName ? v.seqNoteName : "");
        textCentered(buf, 128, 3, v.seqEditing ? WHITE : HUD_ICE);
    }

    drawOrbit(v, full);
}

// ----------------------------------------------------------------- LIVELLO
//
// Il VU diventa concentrico al vetro invece di avere il perno sul fondo: su un
// display tondo un quadrante che gira attorno al centro e' la forma che il vetro
// chiede, e la corsa passa da 110 a 200 gradi.
//
// La geometria resta a raggi separati, che era gia' la cosa giusta: niente si
// sovrappone, quindi ogni elemento si cancella ridisegnandosi in nero senza
// rovinare quello che ha accanto.
//
//   r <= 44   ago
//   r 46..52  indicatore di picco
//   r 52..60  tacche
//   r 60      arco della scala
//   r 72      numeri della scala
//
// Tutto sta dentro il raggio 78, cioe' sotto le didascalie delle manopole: un
// quadrante che si allargasse fino al bordo se le mangerebbe, e su questa
// schermata la manopola viva e' proprio quella del volume.
constexpr int VU_PX = 120;
constexpr int VU_PY = 120;
constexpr float VU_SWEEP = 100.0f;  // gradi per lato
constexpr int VU_NEEDLE_R = 44;
constexpr int VU_MARK_IN = 46;
constexpr int VU_MARK_OUT = 52;
constexpr int VU_TICK_R = 52;
constexpr int VU_ARC_R = 60;
constexpr int VU_LABEL_R = 72;
constexpr float VU_DB_MIN = -40.0f;
constexpr float VU_RED_ZONE = 0.8f;  // -8 dBFS: da qui in su si rischia il clip

// Punto sulla corsa dell'ago: pos 0 = fondo scala sinistro, 1 = destro.
void vuPoint(float pos, int r, int &x, int &y) {
    const float deg = (pos * 2.0f - 1.0f) * VU_SWEEP;
    const float a = deg * (float)M_PI / 180.0f;
    x = VU_PX + (int)((float)r * sinf(a));
    y = VU_PY - (int)((float)r * cosf(a));
}

float vuPos(float lin) {
    if (lin <= 0.0001f) return 0.0f;
    const float db = 20.0f * log10f(lin);
    if (db <= VU_DB_MIN) return 0.0f;
    if (db >= 0.0f) return 1.0f;
    return (db - VU_DB_MIN) / -VU_DB_MIN;
}

// Ago e picco: la stessa funzione disegna e cancella (in nero), per costruzione
// tocca esattamente gli stessi pixel.
void vuNeedle(float pos, uint16_t color) {
    int x, y;
    vuPoint(pos, VU_NEEDLE_R, x, y);
    gfx->drawLine(VU_PX, VU_PY, x, y, color);
    gfx->drawLine(VU_PX - 1, VU_PY, x - 1, y, color);
    gfx->drawLine(VU_PX + 1, VU_PY, x + 1, y, color);
}

void vuMark(float pos, uint16_t color) {
    int x0, y0, x1, y1;
    vuPoint(pos, VU_MARK_IN, x0, y0);
    vuPoint(pos, VU_MARK_OUT, x1, y1);
    gfx->drawLine(x0, y0, x1, y1, color);
    gfx->drawLine(x0 + 1, y0, x1 + 1, y1, color);
}

void vuScale() {
    gfx->startWrite();
    int px, py;
    vuPoint(0.0f, VU_ARC_R, px, py);
    for (int i = 1; i <= 60; ++i) {
        const float p = (float)i / 60.0f;
        int x, y;
        vuPoint(p, VU_ARC_R, x, y);
        const uint16_t c = (p > VU_RED_ZONE) ? HUD_RED : HUD_LIME;
        gfx->writeLine(px, py, x, y, c);
        gfx->writeLine(px, py - 1, x, y - 1, c);
        px = x;
        py = y;
    }
    gfx->endWrite();

    // Sei valori marcati, e nessuno in cima: a 12 in punto il numero cadrebbe
    // sulla riga di separazione. Restano -40, -32, -24 a sinistra e -16, -8, 0 a
    // destra, con lo zero rosso al fondo scala, dove serve.
    const float marks[6] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
    for (int i = 0; i < 6; ++i) {
        const float p = marks[i];
        const uint16_t c = (p > VU_RED_ZONE) ? HUD_RED : HUD_ICE;
        int x0, y0, x1, y1;
        vuPoint(p, VU_TICK_R, x0, y0);
        vuPoint(p, VU_ARC_R, x1, y1);
        gfx->drawLine(x0, y0, x1, y1, c);

        char lbl[6];
        snprintf(lbl, sizeof(lbl), "%d", (int)(VU_DB_MIN * (1.0f - p)));
        int lx, ly;
        vuPoint(p, VU_LABEL_R, lx, ly);
        textAtPoint(lbl, lx, ly, 1, c);
    }
}

void drawLivelloScreen(const SynthView &, bool full) {
    static float lastNeedle = -1.0f;
    static float lastMark = -1.0f;
    static float peakHold = 0.0f;
    static uint32_t peakHoldAt = 0;
    static bool lastClip = false;
    static char lastRms[16] = "";
    static char lastPk[20] = "";

    if (full) {
        vuScale();
        gfx->fillCircle(VU_PX, VU_PY, 5, HUD_ICE);
        lastNeedle = -1.0f;
        lastMark = -1.0f;
        peakHold = 0.0f;
        lastClip = true;  // forza il primo disegno della spia
        lastRms[0] = '\0';
        lastPk[0] = '\0';
        // Il picco si e' accumulato per tutto il tempo in cui la schermata non era
        // a video: si butta, altrimenti si entrerebbe sempre col clip acceso.
        AudioEngine::peakLevel();
    }

    const float rms = AudioEngine::rmsLevel();
    // Lettura distruttiva: e' il picco degli ultimi 33 ms, non l'istante attuale.
    const float peak = AudioEngine::peakLevel();
    const float pos = vuPos(rms);

    // Il picco resta appeso mezzo secondo e poi ricade: e' l'unico modo di vedere
    // transienti che l'ago, per come e' smorzato, non fa in tempo a seguire.
    const uint32_t now = millis();
    const float ppos = vuPos(peak);
    if (ppos >= peakHold) {
        peakHold = ppos;
        peakHoldAt = now;
    } else if (now - peakHoldAt > 600) {
        peakHold -= 0.03f;
        if (peakHold < pos) peakHold = pos;
    }

    if (fabsf(pos - lastNeedle) > 0.002f) {
        if (lastNeedle >= 0.0f) vuNeedle(lastNeedle, BLACK);
        vuNeedle(pos, HUD_ICE);
        lastNeedle = pos;
    }
    if (fabsf(peakHold - lastMark) > 0.002f) {
        if (lastMark >= 0.0f) vuMark(lastMark, BLACK);
        vuMark(peakHold, (peakHold > VU_RED_ZONE) ? HUD_RED : HUD_AMBER);
        lastMark = peakHold;
    }

    // La spia di clip e' il perno stesso: e' il punto dove l'occhio guarda gia'.
    const bool clip = (peak >= 0.999f);
    if (clip != lastClip || fabsf(pos - lastNeedle) < 0.0001f) {
        gfx->fillCircle(VU_PX, VU_PY, 5, clip ? HUD_RED : HUD_ICE);
        lastClip = clip;
    }

    char buf[16];
    if (rms > 0.0005f) {
        snprintf(buf, sizeof(buf), "%.1f dB", 20.0f * log10f(rms));
    } else {
        snprintf(buf, sizeof(buf), "-inf dB");
    }
    if (strcmp(buf, lastRms) != 0) {
        // A 136 e non a 132: i due numeri di fondo scala stanno a raggio 72 sui
        // fianchi, cioe' sulle righe 128..135, e una gomma che partisse da li' se
        // ne porterebbe via la meta' inferiore ad ogni cambio di livello — un
        // difetto che poi non si richiude, perche' la scala si disegna una volta
        // sola entrando nella schermata.
        contentFill(136, 17);
        textCentered(buf, 136, 2, HUD_ICE);
        strncpy(lastRms, buf, sizeof(lastRms) - 1);
    }

    char pk[20];
    if (peakHold > 0.0f) {
        snprintf(pk, sizeof(pk), "pk %.0f dB", VU_DB_MIN * (1.0f - peakHold));
    } else {
        snprintf(pk, sizeof(pk), "pk --");
    }
    if (strcmp(pk, lastPk) != 0) {
        // Fascia stretta e centrata a mano: a questa quota gli archi delle
        // manopole passano gia' ai due fianchi, e una gomma sulla corda ne
        // porterebbe via le punte.
        gfx->fillRect(CX - 40, 155, 80, 8, BLACK);
        textCentered(pk, 155, 1, HUD_LABEL);
        strncpy(lastPk, pk, sizeof(lastPk) - 1);
    }
}
// ---------------------------------------------------------------- QR code
//
// Versione 3 fissa (29x29 moduli) piu' 2 moduli di margine chiaro per lato: con
// 4 px per modulo il quadrato misura 132 px e sta comodamente dentro il cerchio
// del display, lasciando spazio alle scritte sotto.
constexpr uint8_t QR_VERSION = 3;
constexpr int QR_SCALE = 4;
constexpr int QR_QUIET = 2;
constexpr int QR_MODULES = 4 * QR_VERSION + 17;  // 29
// qrcode_getBufferSize() e' una funzione, non una macro: la dimensione la
// ricalcoliamo qui con la stessa formula della libreria, e la verifichiamo a
// runtime prima di scriverci dentro.
constexpr int QR_BUFFER_BYTES = (QR_MODULES * QR_MODULES + 7) / 8;

void drawQr(const char *text, int cy) {
    static uint8_t qrBuffer[QR_BUFFER_BYTES];
    QRCode qr;
    if (qrcode_getBufferSize(QR_VERSION) > sizeof(qrBuffer) ||
        qrcode_initText(&qr, qrBuffer, QR_VERSION, ECC_LOW, text) != 0) {
        textCentered("QR non generabile", cy, 1, RED);
        return;
    }

    const int side = (qr.size + 2 * QR_QUIET) * QR_SCALE;
    const int x0 = CX - side / 2;
    const int y0 = cy - side / 2;

    // Il margine chiaro fa parte del codice: senza, molti lettori rinunciano.
    gfx->fillRect(x0, y0, side, side, WHITE);

    const int mx = x0 + QR_QUIET * QR_SCALE;
    const int my = y0 + QR_QUIET * QR_SCALE;
    for (uint8_t y = 0; y < qr.size; ++y) {
        for (uint8_t x = 0; x < qr.size; ++x) {
            if (qrcode_getModule(&qr, x, y)) {
                gfx->fillRect(mx + x * QR_SCALE, my + y * QR_SCALE, QR_SCALE, QR_SCALE, BLACK);
            }
        }
    }
}

// ------------------------------------------------------------------- avvio
//
// Logo ArcadeVox: sole a fessure, orizzonte in prospettiva e wordmark con
// aberrazione cromatica. La scena e' tutta a primitive tranne le lettere — il
// font 6x8 di Adafruit_GFX non fa un logo — che arrivano da una maschera a 2 bit
// generata da tools/make_logo.py (1,6 kB in flash, vedi logo.h).
//
// Le due citazioni richieste stanno nella struttura, non appiccicate sopra: il
// sole con la griglia in fuga e' l'immaginario da sala giochi anni 80, e la
// linea dell'orizzonte e' una traccia da oscilloscopio che si appiattisce ai
// bordi. Provata anche una versione con joystick e altoparlante come icone
// laterali: a 20 px diventavano scarabocchi, quindi via.

// La tavolozza del logo e' quella dell'interfaccia (vedi la sezione colore in
// cima): qui restano solo i due toni che servono alla sola scena di avvio.
const uint16_t LOGO_MAGENTA = HUD_MAGENTA;
const uint16_t LOGO_AMBER = HUD_AMBER;
const uint16_t LOGO_NEON = HUD_NEON;
const uint16_t LOGO_ICE = HUD_ICE;
const uint16_t LOGO_DIMCYAN = rgb565(0, 90, 110);
const uint16_t LOGO_GRID = rgb565(0, 120, 150);

constexpr int SUN_CX = 120;
constexpr int SUN_CY = 78;
constexpr int SUN_R = 38;
constexpr int HORIZON = 174;
constexpr int LOGO_X = CX - LOGO_W / 2;  // 18
constexpr int LOGO_Y = 124;

// Ritmo dell'animazione. Sta tutto qui perche' e' l'unica cosa che valga la pena
// ritoccare, e perche' la somma e' tempo vero fra l'accensione e il primo suono:
// allungare l'effetto si paga in attesa. Cosi' come sono, circa 2,3 s di
// animazione piu' la pausa finale. Azzerandoli tutti il logo compare e basta.
constexpr uint32_t PACE_STAR_MS = 12;      // ogni 3 stelle accese
constexpr uint32_t PACE_SUN_MS = 5;        // ogni 2 righe del disco
constexpr uint32_t PACE_SLIT_MS = 55;      // per fessura (6 in tutto)
constexpr uint32_t PACE_GRID_H_MS = 40;    // per orizzontale (6)
constexpr uint32_t PACE_GRID_V_MS = 22;    // per coppia di verticali (12)
constexpr uint32_t PACE_TRACE_US = 3000;   // ogni 2 colonne della traccia
constexpr uint32_t PACE_GHOST_MS = 220;    // pausa sulle sole eco cromatiche
constexpr uint32_t PACE_BAND_MS = 45;      // per banda di lettere (8)
constexpr uint32_t PACE_HOLD_MS = 400;     // logo fermo prima di cedere il display

// Cielo stellato. Le posizioni vengono da un generatore lineare con seme fisso:
// stessa scena ad ogni accensione, senza una tabella di coordinate in flash.
void logoStars() {
    uint32_t seed = 0x1234;
    int lit = 0;
    for (int i = 0; i < 64; ++i) {
        seed = seed * 1103515245u + 12345u;
        const int x = 16 + (int)((seed >> 8) % 208u);
        seed = seed * 1103515245u + 12345u;
        const int y = 16 + (int)((seed >> 8) % (uint32_t)(HORIZON - 30));

        const int dx = x - CX, dy = y - CY;
        if (dx * dx + dy * dy > 112 * 112) continue;  // fuori dal vetro
        const int sx = x - SUN_CX, sy = y - SUN_CY;
        if (sx * sx + sy * sy < (SUN_R + 6) * (SUN_R + 6)) continue;  // dentro al sole
        if (y >= LOGO_Y - 4 && y <= LOGO_Y + LOGO_H + 2 && x >= LOGO_X - 4 &&
            x <= LOGO_X + LOGO_W + 4) {
            continue;  // dietro alle lettere non si vedrebbero comunque
        }
        seed = seed * 1103515245u + 12345u;
        gfx->drawPixel(x, y, ((seed >> 16) & 3) ? LOGO_DIMCYAN : WHITE);
        if (++lit % 3 == 0) delay(PACE_STAR_MS);  // il cielo si accende un po' per volta
    }
}

// Sole in gradiente magenta -> ambra, poi le fessure orizzontali che si allargano
// verso il basso: e' quello che rende l'immagine riconoscibile al primo colpo.
void logoSun() {
    // Il disco cala dall'alto riga per riga invece di comparire tutto insieme:
    // e' la fase che regge l'attenzione mentre il resto della scena e' ancora
    // vuoto.
    for (int dy = -SUN_R; dy <= SUN_R; ++dy) {
        const int half = (int)sqrtf((float)(SUN_R * SUN_R - dy * dy));
        const float t = (float)(dy + SUN_R) / (float)(2 * SUN_R);
        gfx->drawFastHLine(SUN_CX - half, SUN_CY + dy, 2 * half + 1,
                           mix565(LOGO_MAGENTA, LOGO_AMBER, t));
        if ((dy & 1) == 0) delay(PACE_SUN_MS);
    }

    int y = SUN_CY - 4;
    int h = 1;
    while (y < SUN_CY + SUN_R) {
        gfx->startWrite();
        for (int k = 0; k < h; ++k) {
            const int dy = y + k - SUN_CY;
            if (dy < -SUN_R || dy > SUN_R) continue;
            const int half = (int)sqrtf((float)(SUN_R * SUN_R - dy * dy));
            gfx->writeFastHLine(SUN_CX - half, y + k, 2 * half + 1, BLACK);
        }
        gfx->endWrite();
        y += h + 5;
        ++h;
        delay(PACE_SLIT_MS);  // le fessure scendono una alla volta: il sole "tramonta"
    }
}

// Griglia in fuga verso il punto di orizzonte. Le orizzontali si diradano
// geometricamente scendendo, che e' quello che da' la profondita'.
void logoGrid() {
    int d = 2, step = 3;
    while (HORIZON + d < 236) {
        gfx->drawFastHLine(0, HORIZON + d, 240, (d < 18) ? dim565(LOGO_GRID, 3, 5) : LOGO_GRID);
        d += step;
        step = step * 3 / 2 + 1;
        delay(PACE_GRID_H_MS);
    }
    // Le verticali si aprono a ventaglio dal centro verso i lati: comparendo
    // tutte insieme, com'erano prima, la fase non si vedeva nemmeno.
    for (int k = 0; k <= 11; ++k) {
        gfx->drawLine(CX, HORIZON, CX + k * 30, 238, LOGO_GRID);
        if (k > 0) gfx->drawLine(CX, HORIZON, CX - k * 30, 238, LOGO_GRID);
        delay(PACE_GRID_V_MS);
    }
}

// La linea dell'orizzonte e' una traccia da oscilloscopio: oscilla al centro e
// si spegne verso i bordi, dove torna orizzonte. Disegnata dopo la griglia, cosi'
// le passa sopra.
void logoTrace() {
    int px = 0, py = 0;
    for (int x = 12; x < 229; ++x) {
        const float e = expf(-((float)(x - CX) / 58.0f) * ((float)(x - CX) / 58.0f));
        const int y =
            HORIZON - (int)lroundf(11.0f * e * sinf(2.0f * (float)M_PI * (float)(x - 12) / 46.0f));
        if (x > 12) {
            gfx->drawLine(px, py, x, y, LOGO_NEON);
            gfx->drawLine(px, py + 1, x, y + 1, dim565(LOGO_NEON, 2, 5));  // alone
        }
        px = x;
        py = y;
        if ((x & 1) == 0) delayMicroseconds(PACE_TRACE_US);
    }
}

// Una passata della maschera del wordmark, limitata alle righe [row0, row1).
// `minLevel` a 2 tiene solo i pixel ben coperti: e' quello che serve alle due
// eco cromatiche, che devono restare nette e non sbavare.
void logoWordmark(int ox, int oy, uint16_t top, uint16_t bottom, uint8_t minLevel, int row0,
                  int row1) {
    gfx->startWrite();
    for (int y = row0; y < row1; ++y) {
        const uint8_t *row = LOGO_MASK + y * LOGO_STRIDE;
        const uint16_t base = mix565(top, bottom, (float)y / (float)(LOGO_H - 1));
        for (int x = 0; x < LOGO_W; ++x) {
            const uint8_t level = (uint8_t)((pgm_read_byte(row + (x >> 2)) >> (6 - 2 * (x & 3))) & 3);
            if (level < minLevel) continue;
            gfx->writePixel(LOGO_X + x + ox, LOGO_Y + y + oy,
                            (level == 3) ? base : dim565(base, level, 3));
        }
    }
    gfx->endWrite();
}

// Schermata di accensione. Poco piu' di un secondo: il tempo di leggere la
// versione, che serve sapere prima di collegarsi al portale degli aggiornamenti.
void splash() {
    gfx->fillScreen(BLACK);
    gfx->drawCircle(CX, CY, 118, LOGO_NEON);
    gfx->drawCircle(CX, CY, 116, LOGO_DIMCYAN);
    logoStars();
    textCentered("v" FW_VERSION, 22, 1, LOGO_DIMCYAN);

    logoSun();
    logoGrid();
    logoTrace();

    // Griglia e traccia arrivano fino al bordo del quadrato, ed e' voluto: sul
    // vetro tondo spariscono dietro la ghiera, che e' proprio l'effetto oblo'
    // che si vuole. Quello che non e' voluto e' che passando ci cancellino la
    // cornice — le sei orizzontali la tagliano in dodici punti, e sui tre archi
    // alti il neon diventa quasi spento. Si ripassano i due anelli sopra: costa
    // due righe, e la griglia torna a sembrare che passi sotto il telaio.
    gfx->drawCircle(CX, CY, 118, LOGO_NEON);
    gfx->drawCircle(CX, CY, 116, LOGO_DIMCYAN);

    // Prima le due eco laterali, poi il corpo bianco che scende riga per riga: le
    // lettere sembrano mettersi a fuoco. L'ordine conta, il corpo deve coprire le
    // eco e non il contrario.
    logoWordmark(-3, 2, LOGO_MAGENTA, LOGO_MAGENTA, 2, 0, LOGO_H);
    logoWordmark(3, 2, LOGO_NEON, LOGO_NEON, 2, 0, LOGO_H);
    delay(PACE_GHOST_MS);  // le eco da sole restano in vista: e' meta' dell'effetto
    for (int band = 0; band < LOGO_H; band += 4) {
        logoWordmark(0, 0, WHITE, LOGO_ICE, 1, band, band + 4);
        delay(PACE_BAND_MS);
    }

    delay(PACE_HOLD_MS);
}


// ------------------------------------------- apprendimento dei LED dei tasti
//
// Fuori dall'anello: niente corona di posizione e niente corona di comandi, perche'
// qui le manopole sono morte davvero — il synth e' muto e l'unica cosa da fare e'
// premere il tasto che si e' acceso.
void drawLedLearnScreen(const SynthView &v, bool full) {
    if (full) {
        chrome("LUCI", HUD_LIME);
        textCentered("PREMI IL TASTO ACCESO", CONTENT_TOP + 12, 1, HUD_LABEL);
        hudChipCentered(190, "SINISTRA PER USCIRE", HUD_TRACK, 1);
    }
    if (!full && v.ledLearnIndex == prev.ledLearnIndex) return;

    contentFill(84, 68);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)v.ledLearnIndex + 1);
    textCentered(buf, 86, 7, HUD_ICE);
    snprintf(buf, sizeof(buf), "di %d", KEYLED_COUNT);
    textCentered(buf, 144, 1, HUD_LABEL);

    // L'arco si riempie di venti segmenti, uno per LED assegnato: si vede quanto
    // manca senza contare le pressioni. Sta sul ventaglio basso, che qui e' libero
    // perche' le manopole non comandano niente.
    for (int i = 0; i < KEYLED_COUNT; ++i) {
        const float span = 120.0f / (float)KEYLED_COUNT;
        arcSeg(120.0f + span * i + 0.4f, 120.0f + span * (i + 1) - 0.4f, KNOB_IN, KNOB_OUT,
               (i < v.ledLearnIndex) ? HUD_LIME : HUD_TRACK);
    }
}

// ----------------------------------------------------------------- overlay
//
// La banda che dice cosa e' appena successo. Vibrava, e le cause erano tre, tutte
// vere insieme.
//
// (1) Si ridisegnava ad ogni fotogramma. La chiamata era `if (c'e' un messaggio)
//     disegnalo`, senza memoria di cosa fosse gia' a video: per una sessantina di
//     fotogrammi di fila il pannello veniva rifatto da zero, e la prima cosa che
//     faceva era spegnere ventimila pixel. A 40 MHz sono circa 8 ms di nero su un
//     fotogramma da 33, cioe' uno strobo a 30 Hz. Adesso c'e' un contatore di
//     revisione e si disegna una volta sola. Il confronto fra puntatori non
//     funzionerebbe: i valori numerici si compongono sempre nello stesso buffer,
//     quindi l'indirizzo non cambia mai nemmeno quando il testo cambia del tutto.
// (2) La schermata sotto combatteva per gli stessi pixel. Traccia, ago e celle
//     ridisegnavano trenta volte al secondo proprio dove stava la banda. Adesso,
//     finche' la banda e' a video, il contenuto sotto sta fermo.
// (3) Si mangiava la ghiera. Il rettangolo andava da x=8 a x=231, cioe' sopra
//     l'anello, e gli staccava quattro morsi che restavano li' per tutta la
//     durata: la cornice "respirava" insieme alla banda. Adesso la pulizia e' per
//     corda e gli angoli sono arrotondati, quindi non arriva mai a toccarla.
//
// E dura 900 ms invece di 2000. Due secondi sono un'eternita' quando hai le mani
// sulle manopole, e la banda copre proprio la zona che vorresti guardare.
// La banda: y 88..151, riquadro da x=22 a x=217 con gli angoli arrotondati.
//
// La larghezza non e' scelta a occhio, e' quella che la gomma riesce a
// cancellare **su tutte** le sue righe. radiusFill lavora sulla corda, che si
// stringe allontanandosi dal centro: sulla riga piu' estrema della banda —
// trentadue pixel sopra il centro — la corda a raggio 104 vale 98,9 px per lato,
// cioe' da x=21 a x=219. Un riquadro piu' largo di cosi' avrebbe i fianchi fuori
// dall'area che viene ripulita, e alla scadenza resterebbero due barrette
// colorate appese ai lati fino al cambio di schermata.
//
// Cosi' i quattro spigoli cadono a raggio 103: dentro la corona di posizione,
// che sta a 113, e appena fuori dal ventaglio delle manopole, che comincia a 111
// gradi mentre lo spigolo sta a 108. Quelli di prima cadevano a raggio 121, cioe'
// fuori dal vetro — la cornice che sembrava rettangolare era gia' tagliata sui
// quattro angoli, e il rettangolo pieno sotto staccava quattro morsi dalla ghiera.
constexpr int FLASH_Y = 88;
constexpr int FLASH_H = 64;
constexpr int FLASH_X = 22;
constexpr int FLASH_W = 196;

void drawFlash(const char *label, const char *value, float frac, uint16_t accent) {
    radiusFill(FLASH_Y, FLASH_H, FLASH_R, BLACK);
    gfx->drawRoundRect(FLASH_X, FLASH_Y, FLASH_W, FLASH_H, 10, accent);
    gfx->drawRoundRect(FLASH_X + 1, FLASH_Y + 1, FLASH_W - 2, FLASH_H - 2, 9,
                       dim565(accent, 1, 3));

    if (label) textCentered(label, FLASH_Y + 7, 1, HUD_LABEL);

    if (value) {
        // Il corpo si sceglie sulla lunghezza: "SILENZIO" a corpo 4 uscirebbe dal
        // tondo, "+2" a corpo 2 sarebbe minuscolo in mezzo a tutto quel nero.
        const size_t n = strlen(value);
        const uint8_t size = (n <= 4) ? 4 : (n <= 8) ? 3 : 2;
        textCentered(value, FLASH_Y + 18 + (32 - 8 * size) / 2, size, HUD_ICE);
    }

    // La barra c'e' solo per i parametri continui: su "SILENZIO" non vuol dire
    // niente, e disegnarla vuota sembrerebbe un valore a zero.
    if (frac >= 0.0f) {
        hudBar(CX - 60, FLASH_Y + 50, 120, 6, frac, ghost(accent), accent);
    }
}

// La ghiera che si riempie: e' la conferma dei gesti che non si tornano indietro
// — svuotare il pattern, accendere la radio, rifare la mappa delle luci. Tenendo
// premuto la vedi caricarsi, e mollando prima non e' successo niente.
//
// Sta sulla ghiera e non su una barra al centro per una ragione precisa: e' il
// bordo stesso dello strumento che si riempie, non si puo' non vederlo, e non
// copre niente di quello che stavi guardando.
//
// Si prende in prestito la **corona di posizione**, non l'anello esterno, e non
// e' un dettaglio: la corona la disegna arcSeg, quindi riempirla e rimetterla a
// posto tocca esattamente gli stessi pixel. L'anello esterno invece lo disegna
// drawCircle, che segue la variante a ellisse della libreria: un arco radiale
// sovrapposto non ne ricopre gli stessi punti, e alla fine sarebbero restati dei
// puntini rossi appesi alla cornice che nessuno avrebbe piu' tolto.
uint8_t holdDrawn = 0;

void drawHoldRing(uint8_t fill, uint8_t currentScreenIdx) {
    if (fill == holdDrawn) return;
    if (fill == 0) {
        drawPosRing(currentScreenIdx);  // stessa primitiva, stessi pixel
    } else {
        const float a = 360.0f * (float)fill / 255.0f;
        arcSeg(0.0f, a, POSRING_IN, POSRING_OUT, HUD_RED);
    }
    holdDrawn = fill;
}

}  // namespace

namespace Display {

void begin() {
    bus = new Arduino_ESP32SPI(PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_SCLK, PIN_TFT_MOSI,
                               GFX_NOT_DEFINED /* MISO non usato */);
    gfx = new Arduino_GC9A01(bus, PIN_TFT_RST, 0 /* rotation */, true /* IPS */);

    gfx->begin(40000000);
    splash();

    forceFull = true;
    prevValid = false;
}

void nextScreen() {
    screen = (screen + 1) % SCREEN_COUNT;
    forceFull = true;
}

void prevScreen() {
    screen = (uint8_t)((screen + SCREEN_COUNT - 1) % SCREEN_COUNT);
    forceFull = true;
}

void goTo(uint8_t s) {
    if (s >= SCREEN_COUNT || s == screen) return;
    screen = s;
    forceFull = true;
}

uint8_t currentScreen() { return screen; }

// Il telaio: si ridisegna solo al cambio di pagina.
static void drawFrame(const SynthView &v, uint8_t s) {
    const uint16_t accent = SCREEN_ACCENT[s];
    gfx->fillScreen(BLACK);
    gfx->drawCircle(CX, CY, 118, dim565(accent, 1, 3));
    gfx->drawCircle(CX, CY, 117, dim565(accent, 1, 6));
    holdDrawn = 0;
    orbitDrawn = -1;

    drawPosRing(s);

    textCentered(SCREEN_TITLE[s], 18, 2, accent);
    gfx->drawFastHLine(40, 44, 160, dim565(accent, 1, 4));
    gfx->drawFastHLine(CX - 30, 44, 60, accent);
    gfx->drawFastHLine(CX - 30, 45, 60, dim565(accent, 1, 2));

    drawOctaveChip(v.octave);

    for (int i = 0; i < 4; ++i) drawKnobIndex(i);
}

void update(const SynthView &v) {
    if (!gfx) return;

    const uint32_t now = millis();

    // L'apprendimento delle luci si prende lo schermo intero: mentre e' in corso
    // la tastiera non suona, quindi non c'e' niente altro da guardare.
    if (v.ledLearn != inLedLearn) {
        inLedLearn = v.ledLearn;
        forceFull = true;
    }
    if (inLedLearn) {
        drawLedLearnScreen(v, forceFull || !prevValid);
        forceFull = false;
        prev = v;
        prevValid = true;
        return;
    }

    // Durante preconteggio e registrazione si guarda per forza la griglia: sono i
    // momenti in cui serve vedere cosa sta finendo nel pattern. Lo step edit non
    // scavalca piu' niente, perche' non e' piu' una modalita'.
    const bool seqOverride = (v.countIn > 0 || v.seqMode == Sequencer::SEQ_RECORDING);
    if (seqOverride != inSeqOverride) {
        inSeqOverride = seqOverride;
        forceFull = true;
    }
    const uint8_t s = inSeqOverride ? SCREEN_RITMO : screen;
    const uint16_t accent = SCREEN_ACCENT[s];

    const bool newFrame = forceFull || !prevValid;
    forceFull = false;
    if (newFrame) drawFrame(v, s);

    bool full = newFrame;

    const bool flashOn = (v.flashLabel != nullptr || v.flashValue != nullptr);
    const bool flashNew = flashOn && (v.flashRev != prev.flashRev || newFrame);
    const bool flashGone = (!flashOn) && (prev.flashLabel != nullptr || prev.flashValue != nullptr);

    // Quando la banda se ne va si cancella solo lei e si rifa' il contenuto. Il
    // telaio sta tutto fuori dalla banda e non l'ha mai sfiorato, quindi non c'e'
    // niente da ricostruire: prima si rifaceva l'intera schermata con un
    // fillScreen — cinquantasettemila pixel, un fotogramma intero saltato e la
    // ghiera che riappariva di colpo, che era poi il lampo nero che si vedeva
    // ogni volta che un messaggio scadeva.
    if (flashGone) {
        radiusFill(FLASH_Y, FLASH_H, FLASH_R, BLACK);
        full = true;
    }

    // Finche' la banda e' a video il contenuto sta fermo: e' l'ultima sorgente di
    // sfarfallio, quella per cui sotto si disegnava e sopra si cancellava trenta
    // volte al secondo negli stessi pixel.
    if (!flashOn || full) {
        switch (s) {
            case SCREEN_SUONA: drawSuonaScreen(v, full); break;
            case SCREEN_TIMBRI: drawTimbriScreen(v, full); break;
            case SCREEN_INVILUPPO: drawInviluppoScreen(v, full); break;
            case SCREEN_EFFETTI: drawEffettiScreen(v, full); break;
            case SCREEN_RITMO: drawRitmoScreen(v, full); break;
            case SCREEN_LIVELLO: drawLivelloScreen(v, full); break;
            default: drawMenuScreen(v, full); break;
        }

        if (full || v.octave != prev.octave) drawOctaveChip(v.octave);

        // La corona dei comandi: gli archi si rifanno solo dove il valore e'
        // cambiato davvero, le didascalie solo quando cambia il testo. Il
        // confronto sul testo e' uno strcmp e non un confronto di puntatori,
        // perche' i valori si compongono sempre nello stesso buffer.
        for (int i = 0; i < 4; ++i) {
            const bool showValue = (knobSlot[i].valueUntil != 0) && (now < knobSlot[i].valueUntil);
            static bool wasShowing[4] = {false, false, false, false};
            static char drawn[4][12] = {"", "", "", ""};
            const char *want = showValue ? knobSlot[i].value : knobSlot[i].label;
            if (full || showValue != wasShowing[i] || strncmp(drawn[i], want, 11) != 0) {
                // Si cancella il piu' largo fra quello che c'era e quello che
                // arriva: cancellare solo il nuovo lascerebbe la coda del vecchio.
                const int a = (int)strlen(drawn[i]), b = (int)strlen(want);
                clearCaption(i, (a > b) ? a : b);
                drawCaption(i, now);
                strncpy(drawn[i], want, 11);
                drawn[i][11] = '\0';
                wasShowing[i] = showValue;
            }
            static float drawnFrac[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
            if (full || fabsf(knobSlot[i].frac - drawnFrac[i]) > 0.004f) {
                arcGauge(i, knobSlot[i].frac, ghost(accent), accent);
                drawnFrac[i] = knobSlot[i].frac;
            }
        }
    }

    // La banda sta sopra a tutto e non chiede il permesso a nessuna schermata, ma
    // la disegna una volta sola: e' il senso di tutta la riparazione.
    if (flashNew) drawFlash(v.flashLabel, v.flashValue, v.flashFrac, accent);

    drawHoldRing(v.holdFill, s);

    prev = v;
    prevValid = true;
}

// La corona dei comandi la riempie main.cpp, che e' l'unico a sapere cosa comanda
// cosa: il display si limita a scriverla.
void setKnob(int slot, const char *label, const char *value, float frac, bool flashValue) {
    if (slot < 0 || slot >= 4) return;
    strncpy(knobSlot[slot].label, label ? label : "-", sizeof(knobSlot[slot].label) - 1);
    knobSlot[slot].label[sizeof(knobSlot[slot].label) - 1] = '\0';
    if (value) {
        strncpy(knobSlot[slot].value, value, sizeof(knobSlot[slot].value) - 1);
        knobSlot[slot].value[sizeof(knobSlot[slot].value) - 1] = '\0';
    }
    knobSlot[slot].frac = frac;
    // Il valore si mostra per i novecento millisecondi dopo uno scatto e poi
    // lascia il posto al nome: il nome insegna, il valore conferma, e nessuno dei
    // due ruba spazio all'altro.
    if (flashValue) knobSlot[slot].valueUntil = millis() + 900;
}
// ------------------------------------------------------------ modalita' rete

void updateNetwork() {
    if (!gfx) return;

    // Ridisegnare un QR da 841 moduli costa: si rifà solo quando cambia
    // davvero qualcosa.
    static NetPortal::Stage lastStage = NetPortal::NET_OFF;
    static char lastQr[96] = "";
    static char lastMsg[64] = "";
    static char lastIp[20] = "";
    static bool everDrawn = false;

    const NetPortal::Stage st = NetPortal::stage();
    const char *qr = NetPortal::qrPayload();
    const char *msg = NetPortal::message();

    // L'indirizzo entra nel confronto: arriva quando il rientro automatico va a
    // buon fine, senza che ne' lo stato ne' il messaggio cambino.
    if (everDrawn && st == lastStage && strcmp(qr, lastQr) == 0 && strcmp(msg, lastMsg) == 0 &&
        strcmp(NetPortal::staIp(), lastIp) == 0) {
        return;
    }
    strncpy(lastIp, NetPortal::staIp(), sizeof(lastIp) - 1);
    const bool qrChanged = !everDrawn || strcmp(qr, lastQr) != 0;

    lastStage = st;
    strncpy(lastQr, qr, sizeof(lastQr) - 1);
    strncpy(lastMsg, msg, sizeof(lastMsg) - 1);
    everDrawn = true;

    if (st == NetPortal::NET_UPDATING) {
        // Da qui in avanti comanda drawOtaProgress().
        return;
    }
    if (st == NetPortal::NET_FAILED) {
        // Un aggiornamento fallito lascia lo schermo sulla barra di
        // avanzamento: senza questo non si saprebbe mai com'e' andata.
        chrome("FALLITO", HUD_RED);
        // I messaggi del portale arrivano a 24 caratteri ("trasferimento
        // interrotto"): a size 2 fanno 288 px su 240, il testo partirebbe da
        // x=-24, si perderebbero le prime due lettere e le ultime andrebbero a
        // capo da sole spezzando la parola. A questa quota ce ne stanno 18, di
        // piu' non entrano: oltre si scende a size 1. Piccolo, ma intero e
        // leggibile, che e' l'unica cosa che conta quando c'e' scritto FALLITO.
        const bool lungo = strlen(msg) > 18;
        textCentered(msg, lungo ? 100 : 96, lungo ? 1 : 2, HUD_ICE);
        textCentered("il firmware attuale", 136, 1, HUD_LABEL);
        textCentered("e' rimasto intatto", 148, 1, HUD_LABEL);
        hudChipCentered(180, "JOYSTICK A SINISTRA", HUD_NEON, 1);
        return;
    }

    if (qrChanged) {
        gfx->fillScreen(BLACK);
        drawQr(qr, 90);
    }

    // Sotto il codice: cosa sta succedendo, e le credenziali per chi le digita a
    // mano. La password e' scritta grande di proposito: se la fotocamera non
    // aggancia il QR — e capita, dipende dal telefono — questa riga e' l'unica
    // via d'uscita, e va letta da mezzo metro con il synth appoggiato al tavolo.
    gfx->fillRect(10, 156, 220, 70, BLACK);

    // Una volta in rete la riga di stato porta l'indirizzo, che dal telefono e'
    // una scorciatoia per il portale. Le credenziali dell'access point restano
    // comunque a video: sono l'unica via d'ingresso se il telefono non e' sulla
    // stessa rete di casa.
    char head[40];
    if (NetPortal::staIp()[0] != '\0') {
        snprintf(head, sizeof(head), "in rete: %s", NetPortal::staIp());
    } else {
        snprintf(head, sizeof(head), "%s", msg);
    }
    textCentered(head, 160, 1, HUD_NEON);
    textCentered(NetPortal::ssid(), 172, 1, HUD_ICE);
    textCentered(NetPortal::password(), 184, 2, HUD_AMBER);
    // Dall'access point il portale si apre senza login. La finestra utente e
    // password compare solo a chi arriva dall'indirizzo di casa, quindi la
    // riga si scrive solo quando quell'indirizzo esiste: altrimenti annuncia
    // un ostacolo che sulla strada del QR non c'e'.
    if (NetPortal::staIp()[0] != '\0') {
        textCentered("da casa, utente: " NET_AUTH_USER, 204, 1, HUD_LABEL);
    }
    // La via d'uscita va scritta sulla schermata da cui si vuole uscire. Finora
    // stava solo nel manuale, e da qui il synth sembrava un vicolo cieco.
    // Quattordici caratteri: a y=216 il glifo arriva fino alla riga 223, e li'
    // siamo a centotre pixel dal centro — la corda utile vale 106 px, cioe'
    // diciassette caratteri a corpo 1. Con "JOYSTICK A SINISTRA: ESCI" la riga
    // partiva da x=45 e le prime lettere finivano oltre il bordo del vetro, dove
    // non esistono proprio.
    textCentered("SINISTRA: ESCI", 216, 1, HUD_LIME);
}

void drawOtaProgress(int pct) {
    if (!gfx) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    static int lastPct = -1;
    if (pct == lastPct) return;
    // Percentuale che torna indietro = trasferimento nuovo: si riparte da capo.
    if (lastPct < 0 || pct < lastPct) {
        chrome("UPDATE", HUD_AMBER);
        textCentered("SCRITTURA IN CORSO", CONTENT_TOP, 1, HUD_LABEL);
        // Il blocco dell'avvertenza sale di 12 px. A x=34 (86 px dal centro) il
        // vetro finisce a y=202: la barretta rossa alta 22 che partiva da 186
        // usciva dal tondo per le ultime cinque righe e prima ancora passava
        // sopra l'anello. Alzandola a 174 il suo ultimo pixel sta a raggio 114,
        // dentro. L'ascissa resta 34 perche' e' la stessa della barretta gemella
        // sulla schermata NETWORK e le due devono restare allineate; sopra c'e'
        // spazio, il blocco della percentuale finisce a y=165.
        gfx->fillRect(34, 174, 3, 22, HUD_RED);
        textAt("NON SPEGNERE", 46, 178, 1, HUD_RED);
        textAt("l'aggiornamento e' a meta'", 46, 190, 1, HUD_LABEL);
    }
    lastPct = pct;

    hudBar(45, 104, 150, 26, pct / 100.0f, HUD_AMBER, HUD_LIME);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d %%", pct);
    gfx->fillRect(60, 142, 120, 24, BLACK);
    textCentered(buf, 142, 3, HUD_ICE);
}

}  // namespace Display
