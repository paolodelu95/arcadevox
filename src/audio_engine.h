// audio_engine.h — oscillatori + filtro + ADSR, su task dedicato core 0.
//
// Il motore ha un pool di voci ed e' polifonico per costruzione; la modalita'
// MONO non e' un motore diverso, e' semplicemente il chiamante che ne usa una
// sola (vedi main.cpp). Ogni voce ha fase, inviluppo e filtro suoi: una nota
// nuova non eredita lo stato di quella precedente.
#pragma once

#include <Arduino.h>

#define SAMPLE_RATE 44100

// 7 tasti nota + 1 voce per il sequencer: il massimo che puo' suonare insieme.
// Essendo esattamente quante servono, ogni identificativo ha la sua voce
// dedicata: niente allocazione, niente voice stealing, comportamento sempre
// prevedibile.
#define MAX_VOICES 8

// Identificativi di voce. I tasti (e l'arpeggiator, che suona una nota alla
// volta fra quelle tenute) usano il proprio indice di nota, 0..6.
#define VOICE_MONO 0  // in modalita' mono esiste una sola voce logica
#define VOICE_SEQ 7   // in poli la sequenza suona *sotto* le dita, non al posto loro

// Campioni di una finestra dell'oscilloscopio: uno per colonna di pixel della
// traccia sul display (~4 ms a 44,1 kHz, poco piu' di un ciclo di un DO4).
#define SCOPE_SAMPLES 180

// Forme d'onda disponibili (ordine usato dal joystick sinistra/destra).
enum Waveform : uint8_t {
    WAVE_SINE = 0,
    WAVE_SQUARE,
    WAVE_SAW,
    WAVE_TRIANGLE,
    WAVE_COUNT
};

extern const char *const WAVEFORM_NAMES[WAVE_COUNT];

// Stati dell'inviluppo (esposti per il display / debug).
enum EnvStage : uint8_t {
    ENV_IDLE = 0,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE
};

namespace AudioEngine {

// Inizializza I2S e lancia il task audio pinnato sul core 0.
void begin();

// Ferma il task audio e libera l'I2S. Serve alla modalita' NETWORK: lo stack
// WiFi gira sullo stesso core 0 con priorita' molto piu' alta e farebbe
// sottoscorrere il DMA. Dopo questa chiamata il synth e' muto fino al reboot.
void shutdown();

// --- controllo delle voci (chiamati dal core 1) ---
// Gli eventi passano da una coda FreeRTOS: i due core non toccano mai lo stesso
// stato, e il task audio li consuma all'inizio di ogni blocco.
//
// Restituiscono false se la coda e' piena (non dovrebbe mai succedere: 32 posti
// e uno svuotamento ogni ~3 ms). Il chiamante deve tenerne conto e non
// aggiornare la propria idea di cosa sta suonando: un voiceOff perso senza
// accorgersene lascerebbe una nota appesa per sempre.
bool voiceOn(uint8_t id, float freq);   // attacca o ritriggera la voce `id`
bool voiceOff(uint8_t id);              // manda in RELEASE la voce `id`
bool voiceRetune(uint8_t id, float freq);  // reintona senza ritriggerare
void allNotesOff();                     // rilascia tutto (cambio di modalita')

// --- parametri timbrici ---
void setWaveform(uint8_t wave);
void setCutoff(float hz);      // 80..8000 Hz
void setVolume(float vol);     // 0..1

// --- parametri ADSR (comuni a tutte le voci) ---
void setAttack(float ms);      // 2..500 ms
void setDecay(float ms);       // 5..1000 ms
void setSustain(float level);  // 0..1
void setRelease(float ms);     // 10..2000 ms

// --- metronomo ---
// Voce di click indipendente dal pool, sommata all'uscita: non ruba una voce
// vera, quindi il conteggio si sente anche con tutti i tasti premuti.
void click(bool accent);

// --- stato ---
bool isSounding();
uint8_t activeVoices();

// --- monitoraggio dell'uscita (schermate VU e oscilloscopio) ---
// Misure prese dal task audio sul segnale finale, quello che esce davvero
// dall'I2S: volume, compensazione e clip compresi.

// Livello RMS gia' lisciato con ballistica da VU (salita svelta, discesa lenta),
// 0..1. Lisciarlo qui e non nel display e' obbligatorio: a 30 fps il core 1
// vedrebbe un campione ogni 1300 e il livello ballerebbe.
float rmsLevel();

// Picco assoluto raggiunto dall'ultima chiamata, 0..1. La lettura lo azzera,
// cosi' fra due refresh non si perde nessuna transiente.
float peakLevel();

// Copia l'ultima finestra catturata per l'oscilloscopio (SCOPE_SAMPLES campioni
// in scala -127..+127). Restituisce false se non ce n'e' una nuova pronta: il
// chiamante tiene a video quella di prima.
bool copyScope(int8_t *dst);

}  // namespace AudioEngine
