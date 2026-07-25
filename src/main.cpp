// SprigSynth — sintetizzatore monofonico su ESP32-S3 N16R8.
//
//   core 0 : motore audio (task FreeRTOS dedicato, vedi audio_engine.cpp)
//   core 1 : questo loop — input, logica, sequencer, display
//
// Vedi CLAUDE_2.md per le specifiche e PROGRESS.md per lo stato dei milestone.

#include <Arduino.h>
#include <math.h>

#include "audio_engine.h"
#include "display.h"
#include "input_handler.h"
#include "pinout.h"
#include "sequencer.h"
#include "status_led.h"

// ---------------------------------------------------------------- costanti
// DO..DO' in quarta ottava; l'ottava corrente moltiplica per 2^n.
static const float NOTE_FREQ[NOTE_COUNT] = {
    261.63f,  // DO
    293.66f,  // RE
    329.63f,  // MI
    349.23f,  // FA
    392.00f,  // SOL
    440.00f,  // LA
    493.88f,  // SI
    523.25f   // DO'
};

static const uint32_t ARP_STEP_MS = 150;
static const uint32_t DISPLAY_INTERVAL_MS = 33;  // ~30 fps

static const int8_t OCTAVE_MIN = -2;
static const int8_t OCTAVE_MAX = 2;

// Range dei parametri (mappature esponenziali pilotate dagli encoder).
static const float CUTOFF_MIN_HZ = 80.0f;
static const float CUTOFF_RATIO = 100.0f;  // 80 Hz .. 8000 Hz
static const float ATTACK_MIN_MS = 2.0f;
static const float ATTACK_RATIO = 250.0f;  // 2 ms .. 500 ms
static const float RELEASE_MIN_MS = 10.0f;
static const float RELEASE_RATIO = 200.0f;  // 10 ms .. 2000 ms
static const float DECAY_MIN_MS = 5.0f;
static const float DECAY_MAX_MS = 1000.0f;
static const float DECAY_STEP_MS = 10.0f;
static const float SUSTAIN_STEP = 0.05f;

// Passo per scatto di encoder. I valori esponenziali si muovono su una posizione
// normalizzata 0..1: 1/64 di corsa per scatto = ~3 giri di manopola da 20 detent
// per l'intero range del cutoff.
static const float CUTOFF_STEP = 1.0f / 64.0f;
static const float CUTOFF_STEP_FINE = 1.0f / 256.0f;
static const float VOLUME_STEP = 0.02f;
static const float VOLUME_STEP_FINE = 0.005f;
static const float ADSR_STEP = 1.0f / 48.0f;
static const float ADSR_STEP_FINE = 1.0f / 192.0f;

// ------------------------------------------------------------------- stato
static uint8_t waveform = WAVE_SAW;
static int8_t octave = 0;
static float octaveMul = 1.0f;

static float cutoffHz = 4000.0f;
static float volume = 0.6f;

static float attackMs = 10.0f;
static float decayMs = 150.0f;
static float sustainLevel = 0.7f;
static float releaseMs = 250.0f;

// Posizioni normalizzate 0..1 dei parametri a mappatura esponenziale: gli encoder
// sono incrementali, quindi lo "stato" del controllo vive qui e non nella manopola.
static float cutoffPos = 0.0f;
static float attackPos = 0.0f;
static float releasePos = 0.0f;

// Passo fine attivabile dal click dell'encoder (se e quando verra' cablato).
static bool enc1Fine = false;
static bool enc2Fine = false;

static bool adsrEditMode = false;
static bool holdActive = false;
static bool arpActive = false;

static int8_t latchedNote = -1;   // ultima nota suonata, tenuta viva da HOLD
static int lastTarget = -1;       // nota attualmente inviata al motore audio
static float lastSentFreq = 0.0f;

static uint8_t arpIndex = 0;
static uint32_t arpLastStep = 0;
static bool prevAnyHeld = false;

static uint32_t lastDisplayAt = 0;

// --------------------------------------------------------------- utilities
static inline float noteFreq(int note) { return NOTE_FREQ[note] * octaveMul; }

static void applyOctave(int8_t oct) {
    if (oct < OCTAVE_MIN) oct = OCTAVE_MIN;
    if (oct > OCTAVE_MAX) oct = OCTAVE_MAX;
    octave = oct;
    octaveMul = powf(2.0f, (float)octave);
}

// Mappatura esponenziale di una posizione 0..1 su [min, min*ratio].
static inline float expMap(float p, float minVal, float ratio) {
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return minVal * powf(ratio, p);
}

// Inversa: da valore reale a posizione 0..1 (serve per inizializzare i default).
static inline float expMapInv(float value, float minVal, float ratio) {
    return logf(value / minVal) / logf(ratio);
}

static inline float clamp01(float v) { return (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ------------------------------------------------------------------ setup
void setup() {
    Serial.begin(115200);

    StatusLed::begin();  // per primo: spegne il LED RGB prima di ogni altra cosa
    Input::begin();
    Sequencer::begin();
    AudioEngine::begin();

    // Posizioni iniziali degli encoder, ricavate dai valori di default.
    cutoffPos = expMapInv(cutoffHz, CUTOFF_MIN_HZ, CUTOFF_RATIO);
    attackPos = expMapInv(attackMs, ATTACK_MIN_MS, ATTACK_RATIO);
    releasePos = expMapInv(releaseMs, RELEASE_MIN_MS, RELEASE_RATIO);

    applyOctave(0);
    AudioEngine::setWaveform(waveform);
    AudioEngine::setCutoff(cutoffHz);
    AudioEngine::setVolume(volume);
    AudioEngine::setAttack(attackMs);
    AudioEngine::setDecay(decayMs);
    AudioEngine::setSustain(sustainLevel);
    AudioEngine::setRelease(releaseMs);

    Display::begin();

    Serial.println(F("SprigSynth pronto."));
}

// ------------------------------------------------------------------- loop
void loop() {
    const uint32_t now = millis();

    Input::update();
    StatusLed::update(now);

    // ---------------------------------------------------- pulsanti funzione
    if (Input::displayPressed()) Display::nextScreen();

    if (Input::holdLongPress()) {
        // Long-press (>600 ms): entra/esce dall'ADSR EDIT MODE.
        adsrEditMode = !adsrEditMode;
    }
    if (Input::holdShortPress()) {
        holdActive = !holdActive;
        if (!holdActive) latchedNote = -1;  // sganciando l'HOLD la nota si spegne
    }

    if (Input::arpPressed()) arpActive = !arpActive;
    if (Input::bpmPressed()) Sequencer::cycleBpm();
    if (Input::recPressed()) Sequencer::toggleRecord();
    if (Input::playPressed()) Sequencer::togglePlay();

    // --------------------------------------------------------- joystick
    if (!adsrEditMode) {
        // modalita' normale: ottava (su/giu) e forma d'onda (sx/dx)
        if (Input::joyUp()) applyOctave(octave + 1);
        if (Input::joyDown()) applyOctave(octave - 1);
        if (Input::joyLeft()) {
            waveform = (waveform + WAVE_COUNT - 1) % WAVE_COUNT;
            AudioEngine::setWaveform(waveform);
        }
        if (Input::joyRight()) {
            waveform = (waveform + 1) % WAVE_COUNT;
            AudioEngine::setWaveform(waveform);
        }
    } else {
        // edit mode: decay (su/giu) e sustain (sx/dx)
        if (Input::joyUp()) {
            decayMs = min(decayMs + DECAY_STEP_MS, DECAY_MAX_MS);
            AudioEngine::setDecay(decayMs);
        }
        if (Input::joyDown()) {
            decayMs = max(decayMs - DECAY_STEP_MS, DECAY_MIN_MS);
            AudioEngine::setDecay(decayMs);
        }
        if (Input::joyRight()) {
            sustainLevel = min(sustainLevel + SUSTAIN_STEP, 1.0f);
            AudioEngine::setSustain(sustainLevel);
        }
        if (Input::joyLeft()) {
            sustainLevel = max(sustainLevel - SUSTAIN_STEP, 0.0f);
            AudioEngine::setSustain(sustainLevel);
        }
    }

    // -------------------------------------------------------- encoder
    // Essendo incrementali, in ADSR EDIT MODE cutoff e volume restano congelati
    // per costruzione: nessuno dei due encoder li tocca e all'uscita dalla
    // modalita' non c'e' nessun salto da recuperare.
    if (Input::enc1Click()) enc1Fine = !enc1Fine;
    if (Input::enc2Click()) enc2Fine = !enc2Fine;

    const int enc1 = Input::enc1Delta();
    const int enc2 = Input::enc2Delta();

    if (!adsrEditMode) {
        if (enc1 != 0) {
            cutoffPos = clamp01(cutoffPos + enc1 * (enc1Fine ? CUTOFF_STEP_FINE : CUTOFF_STEP));
            cutoffHz = expMap(cutoffPos, CUTOFF_MIN_HZ, CUTOFF_RATIO);
            AudioEngine::setCutoff(cutoffHz);
        }
        if (enc2 != 0) {
            volume = clamp01(volume + enc2 * (enc2Fine ? VOLUME_STEP_FINE : VOLUME_STEP));
            AudioEngine::setVolume(volume);
        }
    } else {
        if (enc1 != 0) {
            attackPos = clamp01(attackPos + enc1 * (enc1Fine ? ADSR_STEP_FINE : ADSR_STEP));
            attackMs = expMap(attackPos, ATTACK_MIN_MS, ATTACK_RATIO);
            AudioEngine::setAttack(attackMs);
        }
        if (enc2 != 0) {
            releasePos = clamp01(releasePos + enc2 * (enc2Fine ? ADSR_STEP_FINE : ADSR_STEP));
            releaseMs = expMap(releasePos, RELEASE_MIN_MS, RELEASE_RATIO);
            AudioEngine::setRelease(releaseMs);
        }
    }

    // ----------------------------------------- nota live (priorita' + arp)
    const int rawNote = Input::currentNote();
    const int heldCount = Input::heldCount();
    const bool anyHeld = heldCount > 0;
    if (rawNote >= 0) latchedNote = (int8_t)rawNote;

    bool arpRetrigger = false;
    int liveNote = -1;

    if (arpActive && anyHeld) {
        if (!prevAnyHeld) {
            // prima nota del pattern: parte subito
            arpIndex = 0;
            arpLastStep = now;
            arpRetrigger = true;
        } else if (now - arpLastStep >= ARP_STEP_MS) {
            arpLastStep = now;
            arpIndex = (uint8_t)((arpIndex + 1) % heldCount);
            arpRetrigger = true;
        }
        liveNote = Input::heldNoteByOrder(arpIndex % heldCount);
    } else if (anyHeld) {
        liveNote = rawNote;  // last-note-priority
    } else if (holdActive) {
        liveNote = latchedNote;  // nota tenuta anche a tasti rilasciati
    }
    prevAnyHeld = anyHeld;

    // ------------------------------------------------------- sequencer
    Sequencer::update(now, liveNote);
    const bool seqTick = Sequencer::consumeTick();
    const int seqNote = Sequencer::outputNote();

    // La nota live ha sempre priorita' sulla sequenza.
    const int target = (liveNote >= 0) ? liveNote : seqNote;
    const bool retrigger = arpRetrigger || (seqTick && liveNote < 0);

    if (target < 0) {
        if (lastTarget >= 0) AudioEngine::noteOff();
    } else {
        float f = noteFreq(target);
        if (target != lastTarget || retrigger) {
            AudioEngine::noteOn(f);
            lastSentFreq = f;
        } else if (fabsf(f - lastSentFreq) > 0.01f) {
            // cambio ottava mentre la nota suona: reintono senza ritriggerare
            AudioEngine::setFrequency(f);
            lastSentFreq = f;
        }
    }
    lastTarget = target;

    // --------------------------------------------------------- display
    if (now - lastDisplayAt >= DISPLAY_INTERVAL_MS) {
        lastDisplayAt = now;

        SynthView view;
        view.waveform = waveform;
        view.octave = octave;
        view.cutoffHz = cutoffHz;
        view.volume = volume;
        view.adsrEdit = adsrEditMode;
        view.attackMs = attackMs;
        view.decayMs = decayMs;
        view.sustain = sustainLevel;
        view.releaseMs = releaseMs;
        view.seqMode = (uint8_t)Sequencer::mode();
        view.seqStep = (uint8_t)Sequencer::currentStep();
        view.bpm = (uint16_t)Sequencer::bpm();
        view.hold = holdActive;
        view.arp = arpActive;

        Display::update(view);
    }
}
