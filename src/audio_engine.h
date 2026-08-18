// audio_engine.h — oscillatori, filtro risonante, ADSR ed effetti, su task
// dedicato del core 0.
//
// Catena per voce:   osc + sub/detune -> filtro risonante (SVF) -> ADSR
// Catena finale:     somma -> drive -> delay -> bitcrush -> volume -> I2S
//
// Il motore ha un pool di voci ed e' polifonico per costruzione; la modalita'
// MONO non e' un motore diverso, e' semplicemente il chiamante che ne usa una
// sola (vedi main.cpp). Ogni voce ha fase, inviluppo e filtro suoi: una nota
// nuova non eredita lo stato di quella precedente.
#pragma once

#include <Arduino.h>

#define SAMPLE_RATE 44100

// 13 tasti + la voce del sequencer + due voci di scorta per la modalita'
// ACCORDO. Essendo esattamente quante servono, ogni identificativo ha la sua
// voce dedicata: niente allocazione, niente voice stealing, comportamento
// sempre prevedibile.
#define MAX_VOICES 16

#define VOICE_MONO 0   // in modalita' mono esiste una sola voce logica
#define VOICE_SEQ 13   // in poli la sequenza suona *sotto* le dita
#define VOICE_CHORD1 14  // le due note aggiunte dalla modalita' ACCORDO
#define VOICE_CHORD2 15

// Oltre questo numero di voci contemporanee il core 0 non ce la farebbe a
// chiudere il blocco in tempo: le eccedenti (sempre le piu' spente, cioe' code
// di rilascio) restano fuori dal giro interno per quel blocco.
#define MAX_LIVE_VOICES 10

// Campioni di una finestra dell'oscilloscopio: uno per colonna di pixel della
// traccia sul display.
#define SCOPE_SAMPLES 180

// Forme d'onda disponibili (ordine usato dal joystick sinistra/destra).
enum Waveform : uint8_t {
    WAVE_SINE = 0,
    WAVE_SQUARE,
    WAVE_SAW,
    WAVE_TRIANGLE,
    WAVE_PULSE,   // quadra stretta: nasale, tipica dei chip a 8 bit
    WAVE_NOISE,   // rumore bianco: percussioni e vento
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

// Bersaglio dell'LFO.
enum LfoTarget : uint8_t {
    LFO_OFF = 0,
    LFO_PITCH,   // vibrato
    LFO_CUTOFF,  // wah / wobble
    LFO_AMP,     // tremolo
    LFO_TARGET_COUNT
};

extern const char *const LFO_TARGET_NAMES[LFO_TARGET_COUNT];

namespace AudioEngine {

// Inizializza I2S e lancia il task audio pinnato sul core 0.
void begin();

// Ferma il task audio e libera l'I2S. Serve alla modalita' NETWORK: lo stack
// WiFi gira sullo stesso core 0 con priorita' molto piu' alta e farebbe
// sottoscorrere il DMA. Dopo questa chiamata il synth e' muto fino al reboot.
void shutdown();

// --- campioni (la schermata SUONI) ---
//
// Un canale a parte, che non passa dalle voci: un campione non ha un'altezza da
// intonare ne' un inviluppo da applicare, ha solo un inizio e una fine. Entra
// nella catena finale insieme alla somma delle voci, quindi eco e 8 BIT lo
// prendono come prendono tutto il resto — ed e' proprio la' che diventa
// divertente.
//
// Quattro slot: premere due tasti di fila non deve tagliare il suono di prima,
// che e' esattamente cio' che uno fa con dei suoni cosi'.
#define SAMPLE_SLOTS 4

// `data` sono byte a 8 bit senza segno (128 = silenzio) alla frequenza `rate`.
// Il puntatore deve restare valido: in pratica punta sempre in flash.
void playSample(const uint8_t *data, uint32_t len, uint32_t rate);
void stopSamples();
// Quanti stanno suonando adesso: serve al display e alle luci.
uint8_t samplesPlaying();
// Velocita' di lettura, 0.5..2.0. Sotto l'uno il suono si abbassa e si allunga,
// sopra si alza e si accorcia: e' la manopola che rende questi tredici suoni
// una cosa con cui si gioca invece di tredici pulsanti che fanno sempre uguale.
void setSampleSpeed(float mul);

// --- controllo delle voci (chiamati dal core 1) ---
// Gli eventi passano da una coda FreeRTOS: i due core non toccano mai lo stesso
// stato, e il task audio li consuma all'inizio di ogni blocco.
//
// Restituiscono false se la coda e' piena: il chiamante deve tenerne conto e
// non aggiornare la propria idea di cosa sta suonando, altrimenti un voiceOff
// perso lascerebbe una nota appesa per sempre.
// `velocity` 0..1 e' la forza della nota: scala il picco dell'inviluppo e, con
// l'inviluppo di filtro inserito, anche quanto si apre il filtro. Dalla
// tastiera arriva sempre 1 — i tasti sono interruttori, non sensori — ma dal
// MIDI arriva quella vera, ed e' li' che un pianoforte smette di sembrare un
// organo.
bool voiceOn(uint8_t id, float freq, float velocity = 1.0f);
bool voiceOff(uint8_t id);
bool voiceRetune(uint8_t id, float freq);
void allNotesOff();

// --- parametri timbrici ---
void setWaveform(uint8_t wave);
void setCutoff(float hz);       // 80..8000 Hz
void setResonance(float res);   // 0 = nessuna, 1 = sul punto di innescare
void setVolume(float vol);      // 0..1
void setDrive(float amount);    // 0 = pulito, 1 = saturo
void setSubLevel(float level);  // seconda voce un'ottava sotto, 0..1
void setDetune(float cents);    // scordatura della seconda voce, 0..50 cent
void setGlide(float ms);        // portamento fra due note, 0 = spento

// --- inviluppo di filtro ---
// Il filtro si apre di colpo all'attacco della nota e si richiude da solo. E'
// questo — non la forma d'onda — che distingue una corda pizzicata da una nota
// tenuta: senza, un preset "pianoforte" resta un organo con l'attacco veloce.
// `amount` 0 = filtro fermo sul cutoff, 1 = si apre di quattro ottave sopra.
void setFilterEnv(float amount, float decayMs);

// --- ADSR (comune a tutte le voci) ---
void setAttack(float ms);       // 2..500 ms
void setDecay(float ms);        // 5..1000 ms
void setSustain(float level);   // 0..1
void setRelease(float ms);      // 10..2000 ms

// --- 8 BIT (bitcrush + decimazione) ---
// L'effetto agisce sul segnale finale: "tutto a 8 bit", non una voce sola.
void setCrush(bool on);
void setCrushBits(uint8_t bits);      // 1..16
void setCrushDivider(uint8_t div);    // 1 = frequenza piena, N = fs/N

// --- delay ---
void setDelayTime(float ms);      // 20..400 ms
void setDelayFeedback(float fb);  // 0..0.9
void setDelayMix(float mix);      // 0 = spento

// --- LFO ---
void setLfoRate(float hz);        // 0.05..20 Hz
void setLfoDepth(float depth);    // 0..1
void setLfoTarget(uint8_t target);

// --- uscita I2S ---
// I tre fili del connettore audio non hanno un nome sullo schematico: questa
// funzione prova le sei combinazioni possibili senza ricompilare niente.
// `order` 0..5, vedi AUDIO_ORDER_NAMES.
#define AUDIO_ORDER_COUNT 6
extern const char *const AUDIO_ORDER_NAMES[AUDIO_ORDER_COUNT];
void setPinOrder(uint8_t order);
uint8_t pinOrder();

// --- metronomo ---
// Voce di click indipendente dal pool, sommata all'uscita: non ruba una voce
// vera, quindi il conteggio si sente anche con tutti i tasti premuti.
void click(bool accent);

// --- stato ---
bool isSounding();
uint8_t activeVoices();

// --- monitoraggio dell'uscita (schermate VU e oscilloscopio) ---
float rmsLevel();   // RMS gia' lisciato con ballistica da VU, 0..1
float peakLevel();  // picco dall'ultima chiamata (la lettura lo azzera)
bool copyScope(int8_t *dst);  // false se non c'e' una finestra nuova

}  // namespace AudioEngine
