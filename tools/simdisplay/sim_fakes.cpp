// sim_fakes.cpp — i moduli che display.cpp interroga, ridotti alle loro risposte.
//
// Regola che vale per tutto il file: dove display.cpp legge una *stringa*, qui
// c'e' la stringa vera presa da src/. "trasferimento interrotto" e' lunga 24
// caratteri e a size 1 occupa 144 px; abbreviarla in "errore" farebbe passare
// un controllo che sul synth vero fallirebbe. Lo stesso vale per l'SSID
// ("ArcadeVox-" piu' quattro esadecimali) e per la password ("arcade" piu'
// quattro): sono generate dal MAC, quindi la lunghezza e' fissa e nota.

#include "sim_fakes.h"

#include <Arduino.h>

#include "../../src/audio_engine.h"
#include "../../src/net_portal.h"
#include "../../src/pinout.h"  // NOTE_COUNT
#include "../../src/sequencer.h"

// L'orologio finto. Parte da un valore diverso da zero perche' su ESP32 il primo
// millis() utile non e' mai 0 e display.cpp fa differenze fra istanti.
uint32_t simMillisNow = 1000;

// Il picco si riaccumula mentre il tempo passa. Vedi il commento su peakLevel():
// senza questo aggancio fra orologio e misura, la schermata VU nel simulatore si
// comporterebbe in modo diverso da quella vera.
void simRearmPeak();
void simAdvanceMillis(uint32_t ms) {
    simMillisNow += ms;
    if (ms > 0) simRearmPeak();
}

long gfxSimClippedWrites = 0;
SimSerial Serial;

// Le tabelle di nomi sono dichiarate in audio_engine.h e definite in
// audio_engine.cpp, che qui non si compila: le definizioni sono identiche a
// quelle vere. Vanno tenute allineate a mano — se una forma d'onda nuova
// comparisse solo di la', qui resterebbe un puntatore nullo e la schermata OSC
// andrebbe a scrivere il nulla.
const char *const WAVEFORM_NAMES[WAVE_COUNT] = {"SINE",  "SQUARE", "SAW",
                                                "TRIANG", "PULSE",  "NOISE"};

const char *const LFO_TARGET_NAMES[LFO_TARGET_COUNT] = {"SPENTO", "VIBRATO", "FILTRO",
                                                        "TREMOLO"};

namespace {

float gRms = 0.0f;
float gPeakTarget = 0.0f;  // livello che il segnale raggiunge fra un refresh e l'altro
float gPeakAccum = 0.0f;   // quello effettivamente accumulato adesso
uint8_t gScopeWave = WAVE_SINE;
float gScopeAmp = 0.0f;
bool gScopeFresh = true;

Sequencer::Step gPattern[SEQ_STEPS];

uint8_t gNetStage = NetPortal::NET_OFF;
// Valori come li produrrebbe un MAC qualsiasi: la forma conta, i byte no.
char gSsid[24] = "ArcadeVox-7C3A";
char gPass[16] = "arcade9F7C";
char gQr[80] = "";
char gMsg[48] = "";
char gStaIp[20] = "";
bool gUpdAvail = false;
char gUpdVer[16] = "";
char gPortal[32] = "http://192.168.4.1/";

}  // namespace

// ------------------------------------------------------------- motore audio
namespace AudioEngine {

float rmsLevel() { return gRms; }

// Distruttiva, come sul chip: restituisce il picco accumulato e riparte da zero.
// Questo dettaglio non e' pignoleria. drawVuScreen() entrando nella schermata
// chiama peakLevel() una volta per buttare via l'accumulo vecchio e poi subito
// una seconda volta per il valore da mostrare: sul synth vero la seconda lettura
// trova quasi zero, perche' fra le due non e' passato tempo di audio. Con uno
// stub che restituisce sempre lo stesso numero, la spia di clip si comporterebbe
// al contrario di come si comporta davvero (resterebbe spenta invece di
// accendersi al secondo fotogramma). L'accumulo torna a salire quando il tempo
// avanza — vedi simAdvanceMillis().
float peakLevel() {
    const float v = gPeakAccum;
    gPeakAccum = 0.0f;
    return v;
}

bool copyScope(int8_t *dst) {
    if (!gScopeFresh) return false;
    // Due cicli e mezzo nella finestra: e' la densita' che si vede sul synth
    // vero con una nota media, e mette in evidenza i fronti ripidi della quadra
    // e del dente di sega, che sono il caso peggiore per la traccia.
    for (int i = 0; i < SCOPE_SAMPLES; ++i) {
        const float t = (float)(i % 72) / 72.0f;
        float v;
        switch (gScopeWave) {
            case WAVE_SQUARE: v = (t < 0.5f) ? 1.0f : -1.0f; break;
            case WAVE_SAW: v = 2.0f * t - 1.0f; break;
            case WAVE_TRIANGLE: v = (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t); break;
            case WAVE_PULSE: v = (t < 0.25f) ? 1.0f : -1.0f; break;
            case WAVE_NOISE: {
                // Deterministico come l'icona: la traccia dev'essere la stessa a
                // ogni render, altrimenti due esecuzioni del simulatore
                // darebbero PNG diversi e il confronto non varrebbe piu'.
                uint32_t h = (uint32_t)i * 1664525u + 1013904223u;
                h ^= h >> 13;
                v = (float)((int32_t)((h >> 8) & 0xFFFF) - 32768) * (1.0f / 32768.0f);
                break;
            }
            default: v = sinf(2.0f * (float)M_PI * t); break;
        }
        float s = v * gScopeAmp;
        if (s > 127.0f) s = 127.0f;
        if (s < -127.0f) s = -127.0f;
        dst[i] = (int8_t)s;
    }
    return true;
}

// I nomi delle combinazioni dei fili audio invece servono davvero: settings.cpp
// e' il sorgente vero e li mette dentro le voci del menu.
const char *const AUDIO_ORDER_NAMES[AUDIO_ORDER_COUNT] = {
    "LRC BCK DIN", "BCK LRC DIN", "DIN BCK LRC",
    "DIN LRC BCK", "BCK DIN LRC", "LRC DIN BCK",
};

// Il resto dell'interfaccia esiste solo perche' il linker non protesti se un
// giorno si stubba anche main.cpp: nessuna di queste viene chiamata dal display.
void begin() {}
void shutdown() {}
bool voiceOn(uint8_t, float) { return true; }
bool voiceOff(uint8_t) { return true; }
bool voiceRetune(uint8_t, float) { return true; }
void allNotesOff() {}
void setWaveform(uint8_t) {}
void setCutoff(float) {}
void setResonance(float) {}
void setVolume(float) {}
void setDrive(float) {}
void setSubLevel(float) {}
void setDetune(float) {}
void setGlide(float) {}
void setAttack(float) {}
void setDecay(float) {}
void setSustain(float) {}
void setRelease(float) {}
void setCrush(bool) {}
void setCrushBits(uint8_t) {}
void setCrushDivider(uint8_t) {}
void setDelayTime(float) {}
void setDelayFeedback(float) {}
void setDelayMix(float) {}
void setLfoRate(float) {}
void setLfoDepth(float) {}
void setLfoTarget(uint8_t) {}
void setPinOrder(uint8_t) {}
uint8_t pinOrder() { return 0; }
void click(bool) {}
bool isSounding() { return false; }
uint8_t activeVoices() { return 0; }

}  // namespace AudioEngine

// ----------------------------------------------------------------- sequencer
namespace Sequencer {

const Step &stepAt(int index) {
    static Step fallback = {SEQ_REST, 0};
    if (index < 0 || index >= SEQ_STEPS) return fallback;
    return gPattern[index];
}

}  // namespace Sequencer

// --------------------------------------------------------------- portale rete
namespace NetPortal {

Stage stage() { return (Stage)gNetStage; }
const char *ssid() { return gSsid; }
const char *password() { return gPass; }
const char *qrPayload() { return gQr; }
const char *portalUrl() { return gPortal; }
const char *staIp() { return gStaIp; }
const char *message() { return gMsg; }
int progress() { return 0; }
bool updateAvailable() { return gUpdAvail; }
const char *updateVersion() { return gUpdVer; }
void installUpdate() {}

}  // namespace NetPortal

// ------------------------------------------------------------------ comandi
namespace Sim {

void setLevels(float rms, float peak) {
    gRms = rms;
    gPeakTarget = peak;
    gPeakAccum = peak;
}

void setScope(uint8_t waveform, float amp) {
    gScopeWave = waveform;
    gScopeAmp = amp;
}

void setScopeFresh(bool fresh) { gScopeFresh = fresh; }

void seqClear() {
    for (int i = 0; i < SEQ_STEPS; ++i) {
        gPattern[i].note = SEQ_REST;
        gPattern[i].oct = 0;
    }
}

// Pattern pieno "peggiore": tutte le ottave in gioco (quindi tutti e cinque i
// colori delle celle), qualche legato, nessuna pausa. E' lo stato in cui la
// griglia disegna il massimo numero di pixel.
void seqFill() {
    for (int i = 0; i < SEQ_STEPS; ++i) {
        gPattern[i].note = (int8_t)(i % NOTE_COUNT);
        gPattern[i].oct = (int8_t)((i % 5) - 2);
    }
    gPattern[3].note = SEQ_TIE;
    gPattern[11].note = SEQ_TIE;
}

void seqSetStep(int index, int8_t note, int8_t oct) {
    if (index < 0 || index >= SEQ_STEPS) return;
    gPattern[index].note = note;
    gPattern[index].oct = oct;
}

void netSet(uint8_t stage, const char *qr, const char *msg, const char *staIp) {
    gNetStage = stage;
    snprintf(gQr, sizeof(gQr), "%s", qr ? qr : "");
    snprintf(gMsg, sizeof(gMsg), "%s", msg ? msg : "");
    snprintf(gStaIp, sizeof(gStaIp), "%s", staIp ? staIp : "");
}

void netSetUpdate(bool available, const char *version) {
    gUpdAvail = available;
    strncpy(gUpdVer, version, sizeof(gUpdVer) - 1);
}

}  // namespace Sim

// Fuori dai namespace perche' la chiama simAdvanceMillis(), che vive
// nell'ambiente finto di Arduino e non sa niente di Sim.
void simRearmPeak() { gPeakAccum = gPeakTarget; }
