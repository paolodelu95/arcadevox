// display.h — GC9A01 tondo 240x240 su SPI, schermate cicliche + quelle di edit.
#pragma once

#include <Arduino.h>

#include "fx_rows.h"
#include "settings.h"

// Un solo anello di otto schermate, percorso col joystick sinistra/destra.
//
// Nove erano troppe, e tre di quelle nove esistevano solo perche' un parametro
// non aveva trovato posto altrove. LEVELS spariva dentro SUONA — cutoff,
// risonanza e volume sono le manopole del suonare, e stavano su una schermata
// diversa da quella su cui si suona; SCOPE diventa la finestra centrale di
// SUONA, cosi' la forma d'onda che scegli la vedi davvero invece di vederne il
// ritratto disegnato a formule; FX si allarga in un elenco vero e cambia nome.
//
// La corona di posizione dice sempre dove sei — un settore colorato per pagina —
// e il ritorno a casa con la pressione lunga non serve piu': da qualunque punto
// dell'anello, casa e' al massimo quattro passi in una delle due direzioni.
#define SCREEN_SUONA 0      // onda vera, taglio, volume, risonanza: si suona qui
#define SCREEN_TIMBRI 1     // i quindici timbri, la scala e l'accordo
// I tredici tasti smettono di essere note e diventano tredici suoni. Sta subito
// dopo i timbri perche' risponde alla stessa domanda — "cosa succede quando premo
// un tasto" — e perche' e' la prima cosa che chiunque prova a far fare a uno
// strumento con venti tasti illuminati davanti a se'.
#define SCREEN_SUONI 2
#define SCREEN_INVILUPPO 3  // attacco, decadimento, sostegno, rilascio
#define SCREEN_EFFETTI 4    // elenco: grana, eco, LFO, arpeggio, corpo, filtro
#define SCREEN_RITMO 5      // sequencer: il cursore c'e' sempre, non si "entra"
#define SCREEN_LIVELLO 6    // VU meter ad ago
#define SCREEN_MENU 7       // impostazioni
#define SCREEN_COUNT 8

// Le schermate "a elenco" sono l'unica eccezione alla regola delle quattro
// manopole: la 1 sceglie la riga, la 2 ne cambia il valore. Su tutte le altre le
// quattro manopole sono i quattro parametri disegnati, nell'ordine in cui si
// leggono — e in ogni caso la fascia sotto la ghiera scrive sempre, a lettere,
// cosa fa ognuna delle quattro adesso. E' l'unica regola che resta da ricordare,
// ed e' scritta a schermo, quindi non c'e' nemmeno bisogno di ricordarla.
inline bool screenIsList(uint8_t s) {
    return s == SCREEN_MENU || s == SCREEN_EFFETTI;
}

// Fotografia dello stato del synth passata al display ad ogni refresh.
struct SynthView {
    uint8_t waveform;
    int8_t octave;
    float cutoffHz;
    float resonance;  // 0..1
    float volume;     // 0..1

    float attackMs;
    float decayMs;
    float sustain;  // 0..1
    float releaseMs;

    uint8_t seqMode;    // Sequencer::Mode
    uint8_t seqStep;    // 0..15, testina
    uint8_t seqCursor;  // 0..15, cursore dell'editor
    // Il cursore e' scrivibile: il giro e' fermo e sei sulla schermata RITMO.
    // Non e' piu' una modalita' in cui si entra e da cui si esce — era il modo
    // piu' sicuro per lasciare qualcuno dentro senza che se ne accorgesse.
    bool seqEditing;
    const char *seqNoteName;  // valore della manopola NOTA: "PAUSA", "DO#", "LEGATO"
    uint8_t countIn;    // movimenti mancanti al via (0 = non in preconteggio)
    uint16_t seqRev;    // revisione del pattern: cambia solo a scrittura avvenuta
    uint16_t bpm;
    bool hold;
    bool arp;
    uint8_t arpMode;
    const char *arpName;
    bool poly;          // false = MONO, true = POLIFONICO
    const char *chordName;
    uint8_t voices;     // voci che stanno suonando adesso

    // --- effetti ---
    bool crush;               // 8 BIT inserito
    const char *crushName;    // "8 BIT", "4 BIT"...
    float delayMix;
    float delayMs;
    float lfoDepth;
    float lfoRate;
    const char *lfoTargetName;
    float drive;
    float subLevel;
    float detuneCents;
    float glideMs;
    // --- elenco EFFETTI ---
    // I nomi delle righe stanno in fx_rows.h, che il display include: qui
    // viaggia solo cio' che cambia. Il valore e' testo gia' formattato perche'
    // "SPENTO", "220 ms" e "35 %" non hanno un formato comune, e la frazione
    // serve alla barretta.
    uint8_t fxCursor;                 // riga selezionata
    float fxFrac[FX_ROW_COUNT];       // 0..1 per la barretta di ogni riga
    char fxValue[FX_ROW_COUNT][10];   // valore scritto di ogni riga

    // --- tastiera ---
    const char *scaleName;
    const char *rootName;
    bool expanderOk;  // l'MCP23017 risponde: se no, la tastiera e' muta

    uint8_t setIndex[SETTING_COUNT];  // valori scelti nella schermata MENU
    uint8_t setCursor;                // riga selezionata
    // Riempimento 0..255 della barra di conferma delle azioni: le tre voci
    // rosse del menu e lo svuotamento del pattern non partono al tocco, si
    // caricano mentre tieni premuto. Vedere il gesto riempirsi e' cio' che
    // permette di cambiare idea, e trasforma una pressione lunga da trabocchetto
    // invisibile in una domanda a cui stai rispondendo.
    uint8_t holdFill;

    // Apprendimento dell'ordine dei LED: schermata a se', fuori dal ciclo.
    bool ledLearn;
    uint8_t ledLearnIndex;

    uint8_t timbro;       // timbro di fabbrica corrente, per la schermata TIMBRI
    uint8_t timbroCursor; // riga selezionata nell'elenco dei timbri

    // --- schermata SUONI ---
    int8_t memeLast;      // suono scelto (0..12), -1 se non se n'e' ancora scelto
    uint8_t memePlaying;  // quanti ne stanno suonando adesso

    // --- overlay ---
    // Quello che e' appena cambiato, mostrato sopra la schermata corrente per un
    // paio di secondi e poi tolto di mezzo, senza mai spostare la schermata che
    // stavi guardando.
    //
    // Esiste per una ragione precisa: le manopole e i tasti cambiano cose che
    // spesso *non sono disegnate dove ti trovi*. Prima toccava andare a cercare
    // la schermata che confermava il gesto; adesso il gesto si conferma da se'.
    const char *flashLabel;  // "OTTAVA", "VOLUME", nullptr se non c'e' overlay
    const char *flashValue;  // "+2", "70%", "SILENZIO"
    float flashFrac;         // 0..1 per la barra, negativo se non ha senso
    // Contatore che cambia ad ogni overlay nuovo. Serve a ridisegnare il
    // riquadro **una volta sola** invece che trenta volte al secondo, ed e' un
    // contatore e non un confronto fra puntatori per una ragione precisa: i
    // valori numerici si compongono sempre nello stesso buffer statico, quindi
    // l'indirizzo non cambia mai nemmeno quando il testo cambia del tutto.
    uint16_t flashRev;

};

namespace Display {

// L'intro dell'accensione dura tre secondi e li passa dentro delay(): il core 1
// e' occupato a disegnare e chiunque altro avesse qualcosa da fare in quei tre
// secondi — le luci dei tasti, per esempio — resterebbe fermo.
//
// Il pacer e' la via d'uscita: una funzione che l'intro chiama ad ogni pausa,
// invece di dormire. Va registrata **prima** di begin() e tolta subito dopo, e
// deve tornare in fretta (si auto-limita da se': viene chiamata ogni
// millisecondo). Il display non sa cosa faccia, e non deve saperlo — cosi' la
// dipendenza va in un verso solo e le luci restano fuori da questo file.
void setPacer(void (*fn)(uint32_t now));

void begin();
// L'anello si percorre nei due sensi: destra avanti, sinistra indietro.
void nextScreen();
void prevScreen();
uint8_t currentScreen();
void goTo(uint8_t s);  // REGISTRA e AVVIA portano su RITMO: si guarda cio' che si tocca
void update(const SynthView &v);  // ridisegna solo cio' che e' cambiato

// La corona dei comandi: cosa fa ogni manopola adesso, quanto vale, e quanto e'
// piena la sua corsa. La riempie main.cpp — l'unico che sappia cosa comanda cosa
// — e il display si limita a scriverla sotto l'arco della manopola giusta.
//
// E' il pezzo su cui si regge tutto lo schema di comandi: finche' ogni manopola
// dichiara da se' il proprio mestiere, non c'e' niente da ricordare a memoria e
// non serve un riquadro al centro dello schermo che venga a dirlo coprendo
// proprio la cosa che stai guardando.
//
//   label      undici caratteri al massimo, "-" se la manopola qui non fa niente
//   value      il valore, mostrato al posto del nome per 900 ms dopo uno scatto
//   frac       0..1, il riempimento dell'arco
//   flashValue true dopo uno scatto: fa partire i 900 ms del valore
void setKnob(int slot, const char *label, const char *value, float frac, bool flashValue);

// --- modalita' NETWORK (il synth e' muto, il loop normale non gira) ---
void updateNetwork();
void drawOtaProgress(int pct);
// L'anello che si riempie tenendo premuto AVVIA per installare l'aggiornamento
// che il synth ha trovato da solo.
void drawNetHold(uint8_t fill);

}  // namespace Display
