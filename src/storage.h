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
};

void begin();

// Rilegge stato e pattern. False se la NVS e' vuota o di una versione diversa:
// in quel caso `s` non viene toccato e restano validi i default di main.cpp.
bool load(SynthState &s);

void markDirty();
// True quando c'e' una modifica in sospeso e il tempo di calma e' scaduto: solo
// allora il chiamante si prende la briga di fotografare lo stato.
bool savePending(uint32_t now);
void flush(const SynthState &s);  // scrittura immediata

// --- credenziali della rete di casa (modalita' NETWORK) ---
void saveWifi(const char *ssid, const char *pass);
bool loadWifi(String &ssid, String &pass);
void clearWifi();

// --- URL del manifest degli aggiornamenti ---
void saveManifestUrl(const char *url);
String loadManifestUrl(const char *fallback);

}  // namespace Storage
