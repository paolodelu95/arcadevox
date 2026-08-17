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
};

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

// --- credenziali della rete di casa (modalita' NETWORK) ---
void saveWifi(const char *ssid, const char *pass);
bool loadWifi(String &ssid, String &pass);
void clearWifi();

// --- URL del manifest degli aggiornamenti ---
void saveManifestUrl(const char *url);
String loadManifestUrl(const char *fallback);

}  // namespace Storage
