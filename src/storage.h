// storage.h — persistenza in NVS di pattern, parametri e credenziali WiFi.
//
// Il salvataggio non e' immediato: si segna lo stato come "sporco" e si scrive
// una sola volta, qualche secondo dopo l'ultima modifica. Girando un encoder si
// generano centinaia di variazioni al secondo, ma sulla flash ne arriva una.
#pragma once

#include <Arduino.h>

#define STORAGE_SAVE_DELAY_MS 3000

namespace Storage {

// Tutto cio' che deve sopravvivere allo spegnimento, in un blocco solo: una
// scrittura NVS invece di una per parametro.
struct SynthState {
    uint32_t magic;
    uint8_t waveform;
    int8_t octave;
    uint16_t bpm;
    bool poly;
    float cutoffHz;
    float volume;
    float attackMs;
    float decayMs;
    float sustain;
    float releaseMs;

    // Sensibilita' degli encoder: indici nelle tabelle di main.cpp, non valori.
    // Salvare l'indice invece del passo vero permette di ritoccare le tabelle in
    // una release futura senza che le schede gia' in giro si ritrovino con una
    // sensibilita' assurda.
    uint8_t stepVol;
    uint8_t stepCutoff;
    uint8_t stepAdsr;
    uint8_t stepFine;

    // Orientamento della scala a cui i tre indici qui sopra si riferiscono. Un
    // indice da solo non dice niente se la tabella nel frattempo e' stata
    // rovesciata: e' proprio la promessa fatta due commenti piu' su — ritoccare
    // le tabelle senza rovinare le schede in giro — e questo e' il campo che la
    // mantiene. Zero significa "blob scritto prima della 1.12.0", quando la
    // scala scendeva dai giri alti ai bassi.
    uint8_t scaleRev;

    // --- 2.0.0: scheda nuova (13 note, 4 encoder, luci, effetti) ---
    // Tutto quello che segue e' in coda apposta: un blob della 1.x e' piu'
    // corto, si rilegge lo stesso e questi campi restano a zero, che i
    // chiamanti riportano ai valori di fabbrica. Chi aggiorna il firmware
    // ritrova onda, ottava, cutoff, volume e ADSR come li aveva lasciati.
    float resonance;
    float drive;
    float subLevel;
    float detuneCents;
    float glideMs;

    float delayMs;
    float delayFb;
    float delayMix;

    float lfoRate;
    float lfoDepth;
    uint8_t lfoTarget;

    uint8_t crushOn;      // 8 BIT inserito all'ultimo spegnimento
    uint8_t crushPreset;  // quale profondita' di crush
    uint8_t arpMode;
    uint8_t chordMode;
    uint8_t enc4Assign;   // cosa comanda il quarto encoder

    uint8_t setScale;  // indici delle voci nuove del menu impostazioni
    uint8_t setRoot;
    uint8_t setLed;
    uint8_t setAudio;
    uint8_t setTimbro;  // l'ultimo timbro di fabbrica caricato
    uint8_t setMidiOut;

    // Marcatore di revisione della struttura, e non e' ridondante rispetto al
    // controllo sulla lunghezza.
    //
    // Il trucco della migrazione — "un blob piu' corto e' una versione
    // precedente" — ha un buco: i campi in coda sono uint8_t e la struttura ne
    // contiene di float, quindi il compilatore la riempie fino al multiplo di
    // quattro. Aggiungere un byte puo' quindi lasciare `sizeof` **identico**, il
    // controllo passa, e il campo nuovo si legge dal riempimento: zero. Un
    // valore che per SETTING_MIDIOUT vuol dire "spento", e infatti il MIDI OUT
    // nasceva gia' disinserito senza che nessuno l'avesse chiesto.
    //
    // Questo campo lo smaschera: un firmware nuovo ci scrive sempre
    // STORAGE_STATE_REV, quindi leggere zero significa per forza "blob scritto
    // prima che questo campo esistesse", e i campi nuovi vanno ignorati.
    uint8_t stateRev;

    // --- 2.1.0: l'inviluppo di filtro diventa regolabile ---
    // Due parametri che il motore aveva da sempre e che nessun comando
    // raggiungeva: li scrivevano solo i timbri di fabbrica. Adesso hanno due
    // righe nell'elenco EFFETTI, quindi vanno anche ricordati — altrimenti
    // sarebbero gli unici due che si azzerano ad ogni spegnimento.
    //
    // Stanno in coda per la stessa ragione di tutto il resto, e stavolta senza
    // il rischio del riempimento: sono float, la struttura cresce di otto byte
    // pieni e nessuna lunghezza vecchia puo' coincidere con quella nuova.
    float filtEnvAmount;
    float filtEnvMs;

    // --- 2.8.0: il pattern ha uno strumento suo ---
    // Senza questo, un giro di batteria registrato ieri tornava suonato dal
    // synth alla riaccensione: il pattern sopravviveva allo spegnimento e lo
    // strumento con cui era stato scritto no, che e' il modo piu' sicuro di
    // rendere inutile l'averlo salvato.
    uint8_t seqInstrument;
};

// Alzata a 4 perche' seqInstrument e' un singolo byte in coda a una struttura
// piena di float: il riempimento puo' lasciare sizeof identico, e il controllo
// sulla sola lunghezza non se ne accorgerebbe. E' esattamente il buco che questo
// campo di revisione esiste per tappare, ed e' gia' successo una volta.
#define STORAGE_STATE_REV 4

// Orientamento attuale: la scala sale, l'indice cresce col numero a video.
#define STORAGE_SCALE_REV 1

void begin();

// Rilegge stato e pattern. False se la NVS e' vuota o di una versione diversa:
// in quel caso `s` non viene toccato e restano validi i default di main.cpp.
bool load(SynthState &s);

void markDirty();
// True quando c'e' una modifica in sospeso e il tempo di calma e' scaduto: solo
// allora il chiamante si prende la briga di fotografare lo stato.
bool savePending(uint32_t now);
void flush(const SynthState &s);  // scrittura immediata

// --- ordine della catena di LED sotto i tasti ---
// Venti byte: per ogni tasto, la sua posizione nella catena. Lo schematico non
// lo dice, lo impara la scheda (vedi keylight.h) e da li' in poi resta scritto.
void saveLedMap(const uint8_t *map, size_t len);
bool loadLedMap(uint8_t *map, size_t len);

// --- le reti conosciute (modalita' NETWORK) ---
//
// Sono piu' di una perche' lo strumento si sposta. Una sola rete voleva dire che
// portandolo altrove — a provare, a suonare da qualcun altro — l'aggiornamento
// automatico smetteva di funzionare e bisognava rifare tutto il giro col
// telefono; e tornando a casa bisognava rifarlo un'altra volta, perche' nel
// frattempo l'unica casella era stata sovrascritta.
//
// Cinque bastano per casa, lavoro, il posto delle prove e l'hotspot del
// telefono, che e' poi la rete che ci si porta dietro ovunque.
#define WIFI_SLOTS 5

// Le reti sono tenute in ordine di ultimo uso: la prima e' quella piu' recente.
// Serve a due cose — decidere quale provare quando ce ne sono due in portata, e
// sapere quale buttare quando le caselle finiscono.
void wifiRemember(const char *ssid, const char *pass);
uint8_t wifiCount();
bool wifiAt(uint8_t i, String &ssid, String &pass);
// Password di una rete gia' conosciuta, "" se non la conosce.
bool wifiPasswordFor(const char *ssid, String &pass);
void wifiForget(const char *ssid);
void wifiForgetAll();

// --- URL del manifest degli aggiornamenti ---
void saveManifestUrl(const char *url);
String loadManifestUrl(const char *fallback);

}  // namespace Storage
