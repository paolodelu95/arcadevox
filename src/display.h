// display.h — GC9A01 tondo 240x240 su SPI, schermate cicliche + quelle di edit.
#pragma once

#include <Arduino.h>

#include "settings.h"

// Un solo anello di schermate, percorso col joystick sinistra/destra.
//
// Non ci sono piu' modi nascosti dietro una pressione lunga: l'ADSR era una
// modalita' in cui si entrava tenendo premuto FN5, adesso e' una schermata come
// le altre e i suoi quattro parametri stanno sui quattro encoder esattamente
// come ovunque. La vecchia schermata OTTAVA e' sparita: mostrava un numero che
// interessa solo nell'istante in cui lo cambi, e per quello adesso c'e'
// l'overlay, che lo fa vedere sopra qualunque schermata senza spostarti.
#define SCREEN_HOME 0      // onda, ottava, scala, stato: la schermata del suonare
#define SCREEN_TIMBRI 1    // i quindici timbri, fuori dalle impostazioni
#define SCREEN_LEVELS 2    // cutoff, risonanza, volume
#define SCREEN_ADSR 3      // attack, decay, sustain, release
#define SCREEN_FX 4        // 8 BIT, eco, LFO, drive, sub, detune, glide
#define SCREEN_SEQ 5
#define SCREEN_VU 6        // VU meter ad ago
#define SCREEN_SCOPE 7     // oscilloscopio dell'uscita
#define SCREEN_SETTINGS 8  // ultima dell'anello: encoder, tastiera, luci, rete
#define SCREEN_COUNT 9

// Le schermate "a elenco" sono l'unica eccezione alla regola delle quattro
// manopole, e la seconda e ultima regola del sistema: encoder 1 scorre le voci,
// encoder 2 cambia il valore di quella selezionata. Su tutte le altre i quattro
// encoder sono i quattro parametri disegnati, nell'ordine in cui si leggono.
inline bool screenIsList(uint8_t s) {
    return s == SCREEN_SETTINGS || s == SCREEN_TIMBRI;
}

// Fotografia dello stato del synth passata al display ad ogni refresh.
struct SynthView {
    uint8_t waveform;
    int8_t octave;
    float cutoffHz;
    float resonance;  // 0..1
    float volume;     // 0..1

    bool adsrEdit;
    float attackMs;
    float decayMs;
    float sustain;  // 0..1
    float releaseMs;

    uint8_t seqMode;    // Sequencer::Mode
    uint8_t seqStep;    // 0..15, testina
    uint8_t seqCursor;  // 0..15, cursore dell'editor
    bool seqEditing;    // STEP EDIT attivo
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
    const char *enc4Name;     // cosa comanda adesso il quarto encoder
    uint8_t enc4Index;        // indice dello stesso, per evidenziare la riga giusta

    // --- tastiera ---
    const char *scaleName;
    const char *rootName;
    bool expanderOk;  // l'MCP23017 risponde: se no, la tastiera e' muta

    uint8_t setIndex[SETTING_COUNT];  // valori scelti nella schermata SETTINGS
    uint8_t setCursor;                // riga selezionata
    bool setEditing;                  // dentro al menu: il cursore e' visibile

    // Millisecondi da quando il pattern e' stato svuotato, 0 se non e' successo.
    uint32_t clearedAgo;

    // Apprendimento dell'ordine dei LED: schermata a se', fuori dal ciclo.
    bool ledLearn;
    uint8_t ledLearnIndex;

    uint8_t timbro;       // timbro di fabbrica corrente, per la schermata TIMBRI
    uint8_t timbroCursor; // riga selezionata nell'elenco dei timbri

    // --- overlay ---
    // Quello che e' appena cambiato, mostrato sopra la schermata corrente per un
    // paio di secondi e poi tolto di mezzo, senza mai spostare la schermata che
    // stavi guardando.
    //
    // Esiste per una ragione precisa: le manopole e i tasti cambiano cose che
    // spesso *non sono disegnate dove ti trovi*. Prima toccava andare a cercare
    // la schermata che confermava il gesto; adesso il gesto si conferma da se'.
    const char *flashLabel;  // "OTTAVA", "CUTOFF", nullptr se non c'e' overlay
    const char *flashValue;  // "+2", "SAW", "PIANOFORTE"
    float flashFrac;         // 0..1 per la barra, negativo se non ha senso
};

namespace Display {

void begin();
// L'anello si percorre nei due sensi: destra avanti, sinistra indietro. Poterlo
// fare all'indietro non e' un vezzo — con nove schermate, tornare a quella
// appena lasciata costava otto passi.
void nextScreen();
void prevScreen();
void goHome();  // FN7 tenuto premuto: la via di casa da qualunque schermata
uint8_t currentScreen();
void update(const SynthView &v);  // ridisegna solo cio' che e' cambiato

// --- modalita' NETWORK (il synth e' muto, il loop normale non gira) ---
void updateNetwork();
void drawOtaProgress(int pct);

}  // namespace Display
