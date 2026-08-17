// audio_engine.cpp — motore audio polifonico a 16 voci con filtro risonante,
// effetti e uscita a 8 bit.
//
// Catena per voce:  osc (+ sub, + detune) -> SVF risonante -> ADSR
// Catena finale:    somma -> compensazione -> drive -> delay -> 8 BIT ->
//                   volume -> clip -> I2S
//
// Gira interamente in un task FreeRTOS pinnato sul core 0, cosi' che il refresh
// del display e la scansione degli input (core 1) non possano introdurre glitch.
//
// I parametri sono float/bool a 32 bit allineati: su ESP32 load/store sono
// atomici, quindi bastano le `volatile` senza lock fra i due core. Gli eventi di
// nota invece toccano piu' campi della stessa voce e non possono essere
// atomici: passano da una coda FreeRTOS che il task audio svuota all'inizio di
// ogni blocco. Nessuno dei due core scrive mai lo stato dell'altro.

#include "audio_engine.h"

#include <driver/i2s.h>
#include <math.h>
#include <string.h>

#include "pinout.h"

const char *const WAVEFORM_NAMES[WAVE_COUNT] = {"SINE",  "SQUARE", "SAW",
                                                "TRIANG", "PULSE",  "NOISE"};

const char *const LFO_TARGET_NAMES[LFO_TARGET_COUNT] = {"SPENTO", "VIBRATO", "FILTRO",
                                                        "TREMOLO"};

namespace {

// ------------------------------------------------------------------ costanti
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr size_t RENDER_BLOCK = 128;  // campioni mono renderizzati per giro
constexpr int SINE_TABLE_BITS = 10;
constexpr int SINE_TABLE_SIZE = 1 << SINE_TABLE_BITS;  // 1024

float sineTable[SINE_TABLE_SIZE];

// Delay: 400 ms bastano per un eco lungo senza mangiarsi la RAM. In int16 sono
// 35 kB; in float sarebbero 70, e la differenza si sentirebbe altrove.
constexpr int DELAY_SAMPLES = (SAMPLE_RATE * 400) / 1000;
int16_t delayBuf[DELAY_SAMPLES];
int delayWrite = 0;

// --------------------------------------------------- parametri (scritti dal core 1)
volatile uint8_t pWave = WAVE_SAW;
volatile float pVolume = 0.6f;
volatile float pCutoffHz = 4000.0f;
volatile float pResonance = 0.0f;
volatile float pDrive = 0.0f;
volatile float pSubLevel = 0.0f;
volatile float pDetuneCents = 0.0f;
volatile float pGlideMs = 0.0f;
volatile float pFiltEnvAmount = 0.0f;
volatile float pFiltEnvMs = 300.0f;

volatile float pAttackMs = 10.0f;
volatile float pDecayMs = 120.0f;
volatile float pSustain = 0.7f;
volatile float pReleaseMs = 250.0f;

volatile bool pCrushOn = false;
volatile uint8_t pCrushBits = 8;
volatile uint8_t pCrushDiv = 2;

volatile float pDelayMs = 220.0f;
volatile float pDelayFb = 0.35f;
volatile float pDelayMix = 0.0f;

volatile float pLfoRate = 5.0f;
volatile float pLfoDepth = 0.0f;
volatile uint8_t pLfoTarget = LFO_OFF;

volatile uint8_t pActiveVoices = 0;
volatile uint8_t pPinOrder = 0;

// Metronomo: richiesta di click, consumata a inizio blocco.
volatile bool pClick = false;
volatile bool pClickAccent = false;

// Spegnimento ordinato del task (modalita' NETWORK).
volatile bool pStopRequest = false;
volatile bool pStopped = false;
bool driverInstalled = false;

// ------------------------------------------------------- eventi di nota
enum : uint8_t { EV_ON = 0, EV_OFF, EV_RETUNE, EV_ALL_OFF };

struct NoteEvent {
    uint8_t type;
    uint8_t id;
    float freq;
    float velocity;
};

QueueHandle_t eventQueue = nullptr;

// --------------------------------------------------- stato delle voci (solo core 0)
// Ogni identificativo ha la sua voce: `voices[id]`, senza allocazione.
struct Voice {
    uint32_t phase;
    uint32_t phaseInc;      // valore in uso (insegue il target col portamento)
    uint32_t phaseIncTarget;
    uint32_t subPhase;
    uint32_t detunePhase;
    float envLevel;
    float envPeak;   // dove arriva l'attacco: e' la dinamica della nota
    float releaseInc;
    float filtEnv;   // 1 all'attacco, scende da solo: apre e richiude il filtro
    float svfF;      // coefficiente del filtro, ricalcolato una volta per blocco
    float svfLow;    // stato del filtro risonante
    float svfBand;
    uint32_t noiseState;
    EnvStage stage;
};

Voice voices[MAX_VOICES];

float volSmooth = 0.0f;
float gainSmooth = 1.0f;
float cutSmooth = 4000.0f;
float driveSmooth = 0.0f;
float mixSmooth = 0.0f;

float lfoPhase = 0.0f;

// --------------------------------------------------------------- metronomo
constexpr float CLICK_LEVEL = 0.22f;
constexpr float CLICK_DECAY = 0.99375f;  // ~25 ms fino all'inudibile
constexpr float CLICK_EPS = 0.001f;
constexpr float CLICK_HZ = 1400.0f;
constexpr float CLICK_HZ_ACCENT = 2000.0f;

uint32_t clickPhase = 0;
uint32_t clickInc = 0;
float clickEnv = 0.0f;

// ------------------------------------------------------------- monitoraggio
volatile float pRms = 0.0f;
volatile float pPeak = 0.0f;

constexpr float VU_RISE = 0.35f;
constexpr float VU_FALL = 0.05f;

enum : uint8_t {
    SCOPE_ARMED = 0,  // in attesa dell'aggancio; il buffer e' del core 0
    SCOPE_FILLING,    // cattura in corso; il buffer e' del core 0
    SCOPE_READY       // finestra completa; il buffer e' del core 1
};
volatile uint8_t scopeState = SCOPE_ARMED;
int8_t scopeBuf[SCOPE_SAMPLES];
int scopeIdx = 0;
float scopePrev = 0.0f;
uint32_t scopeWait = 0;

constexpr uint32_t SCOPE_TIMEOUT = SAMPLE_RATE / 10;

// ------------------------------------------------------------------- utility
inline float samplesFor(float ms) {
    const float s = ms * (SAMPLE_RATE / 1000.0f);
    return (s < 1.0f) ? 1.0f : s;
}

inline uint32_t freqToPhaseInc(float freq) {
    return (uint32_t)(freq * (4294967296.0f / (float)SAMPLE_RATE));
}

inline uint32_t nextNoise(uint32_t &s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// Un campione di oscillatore, -1..+1.
inline float oscSample(uint32_t ph, uint8_t wave, uint32_t &noise) {
    switch (wave) {
        case WAVE_SQUARE:
            return (ph < 0x80000000UL) ? 1.0f : -1.0f;
        case WAVE_SAW:
            return (float)((int32_t)(ph - 0x80000000UL)) * (1.0f / 2147483648.0f);
        case WAVE_TRIANGLE: {
            const float t = (float)ph * (1.0f / 4294967296.0f);
            return (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
        }
        case WAVE_PULSE:
            // Duty 25%: la stessa quadra ma nasale, il suono dei chip a 8 bit.
            return (ph < 0x40000000UL) ? 1.0f : -1.0f;
        case WAVE_NOISE:
            return (float)((int32_t)(nextNoise(noise) >> 1)) * (1.0f / 1073741824.0f) - 1.0f;
        case WAVE_SINE:
        default:
            return sineTable[ph >> (32 - SINE_TABLE_BITS)];
    }
}

// Incrementi d'inviluppo, ricalcolati una volta per blocco invece che per ogni
// campione di ogni voce: sono uguali per tutte.
struct EnvRates {
    float attack;
    float decay;
    float sustain;
};

inline void envTick(Voice &v, const EnvRates &r) {
    switch (v.stage) {
        case ENV_ATTACK:
            // Il tempo di salita non dipende dalla dinamica: una nota piano ci
            // mette quanto una forte ad arrivare al *suo* massimo, che e' come
            // si comportano gli inviluppi veri.
            v.envLevel += r.attack * v.envPeak;
            if (v.envLevel >= v.envPeak) {
                v.envLevel = v.envPeak;
                v.stage = ENV_DECAY;
            }
            break;
        case ENV_DECAY: {
            const float sus = r.sustain * v.envPeak;
            v.envLevel -= r.decay * v.envPeak;
            if (v.envLevel <= sus) {
                v.envLevel = sus;
                v.stage = ENV_SUSTAIN;
            }
            break;
        }
        case ENV_SUSTAIN:
            // segue in tempo reale eventuali modifiche del sustain in edit mode
            v.envLevel += (r.sustain * v.envPeak - v.envLevel) * 0.0005f;
            break;
        case ENV_RELEASE:
            v.envLevel -= v.releaseInc;
            if (v.envLevel <= 0.0f) {
                v.envLevel = 0.0f;
                v.stage = ENV_IDLE;
            }
            break;
        case ENV_IDLE:
        default:
            v.envLevel = 0.0f;
            break;
    }
}

inline void scopeTick(float s) {
    switch (scopeState) {
        case SCOPE_ARMED:
            if ((scopePrev < 0.0f && s >= 0.0f) || ++scopeWait >= SCOPE_TIMEOUT) {
                scopeWait = 0;
                scopeIdx = 0;
                scopeState = SCOPE_FILLING;
            }
            break;
        case SCOPE_FILLING:
            scopeBuf[scopeIdx++] = (int8_t)(s * 127.0f);
            if (scopeIdx >= SCOPE_SAMPLES) scopeState = SCOPE_READY;
            break;
        default:
            break;  // SCOPE_READY: il buffer e' del core 1 finche' non lo consuma
    }
    scopePrev = s;
}

void releaseVoice(Voice &v) {
    if (v.stage == ENV_IDLE || v.stage == ENV_RELEASE) return;
    v.releaseInc = (v.envLevel > 0.0f ? v.envLevel : 1.0f) / samplesFor(pReleaseMs);
    v.stage = ENV_RELEASE;
}

void drainEvents() {
    NoteEvent ev;
    while (eventQueue && xQueueReceive(eventQueue, &ev, 0) == pdTRUE) {
        if (ev.type == EV_ALL_OFF) {
            for (int i = 0; i < MAX_VOICES; ++i) releaseVoice(voices[i]);
            continue;
        }
        if (ev.id >= MAX_VOICES) continue;
        Voice &v = voices[ev.id];
        switch (ev.type) {
            case EV_ON:
                v.envPeak = (ev.velocity > 0.02f) ? ev.velocity : 0.02f;
                // Il filtro riparte spalancato ad ogni attacco, anche se la
                // voce stava gia' suonando: e' cio' che rende il ribattuto di
                // un pianoforte diverso da una nota tenuta.
                v.filtEnv = 1.0f;
                v.phaseIncTarget = freqToPhaseInc(ev.freq);
                // Il portamento parte da dove stava suonando: se la voce era
                // spenta non c'e' niente da cui scivolare e si attacca netta.
                if (v.stage == ENV_IDLE || pGlideMs <= 0.0f || v.phaseInc == 0) {
                    v.phaseInc = v.phaseIncTarget;
                }
                v.stage = ENV_ATTACK;
                // La fase non si azzera: ripartire sempre da zero su onde a
                // spigolo produce un click ad ogni attacco.
                break;
            case EV_OFF:
                releaseVoice(v);
                break;
            case EV_RETUNE:
                v.phaseIncTarget = freqToPhaseInc(ev.freq);
                if (pGlideMs <= 0.0f) v.phaseInc = v.phaseIncTarget;
                break;
            default:
                break;
        }
    }
}

// Saturazione morbida: niente tanh, che costerebbe piu' di tutto il resto.
// x - x^3/3 e' la stessa curva nei primi termini e satura da sola oltre |1.5|.
inline float softClip(float x) {
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    return x - (x * x * x) * (1.0f / 6.75f);
}

void renderBlock(int16_t *out, size_t frames) {
    drainEvents();

    if (pClick) {
        pClick = false;
        clickPhase = 0;
        clickInc = freqToPhaseInc(pClickAccent ? CLICK_HZ_ACCENT : CLICK_HZ);
        clickEnv = 1.0f;
    }

    const uint8_t wave = pWave;
    const float volTarget = pVolume;

    EnvRates rates;
    rates.sustain = pSustain;
    rates.attack = 1.0f / samplesFor(pAttackMs);
    rates.decay = (1.0f - rates.sustain) / samplesFor(pDecayMs);

    // ------------------------------------------------------------------ LFO
    // Un campione per blocco (~2,9 ms, 345 Hz di aggiornamento): per vibrato e
    // wobble e' abbondante, e costa una sinusoide invece di 128.
    const uint8_t lfoTarget = pLfoTarget;
    const float lfoDepth = pLfoDepth;
    float lfo = 0.0f;
    if (lfoTarget != LFO_OFF && lfoDepth > 0.0f) {
        lfoPhase += pLfoRate * ((float)RENDER_BLOCK / (float)SAMPLE_RATE);
        if (lfoPhase >= 1.0f) lfoPhase -= (float)(int)lfoPhase;
        lfo = sineTable[(int)(lfoPhase * SINE_TABLE_SIZE) & (SINE_TABLE_SIZE - 1)];
    }

    // --------------------------------------------------------------- filtro
    float cutTarget = pCutoffHz;
    if (lfoTarget == LFO_CUTOFF) {
        // Modulazione esponenziale: +/- 2 ottave a fondo corsa, che e' il modo
        // in cui il filtro si sente "spazzare" invece di ondeggiare appena.
        cutTarget *= powf(4.0f, lfo * lfoDepth);
    }
    if (cutTarget < 40.0f) cutTarget = 40.0f;
    // Limite di stabilita' del filtro a variabili di stato: oltre fs/6 il
    // coefficiente esce dalla zona in cui la ricorsione converge.
    const float cutMax = (float)SAMPLE_RATE / 6.0f;
    if (cutTarget > cutMax) cutTarget = cutMax;
    cutSmooth += (cutTarget - cutSmooth) * 0.25f;

    const float svfFbase = 2.0f * sinf((float)M_PI * cutSmooth / (float)SAMPLE_RATE);
    // Inviluppo di filtro: un colpo per blocco, non per campione. A 345
    // aggiornamenti al secondo la richiusura resta liscia all'orecchio e costa
    // una sinf per voce invece di una per campione — che a sedici voci sarebbe
    // il triplo di tutto il resto del motore messo insieme.
    const float filtAmount = pFiltEnvAmount;
    const float filtDecay =
        expf(-(float)RENDER_BLOCK / samplesFor(pFiltEnvMs));
    const float res = pResonance;
    // q e' lo smorzamento: 2 = nessuna risonanza, verso 0 il filtro si mette a
    // fischiare da solo. Non si arriva mai a zero, o resterebbe acceso anche a
    // tasti rilasciati.
    const float svfQ = 2.0f - 1.94f * res;
    // Con la risonanza alta il picco guadagna parecchio: si abbassa l'ingresso
    // del filtro nella stessa misura, altrimenti basta una nota per clippare.
    const float svfIn = 1.0f - 0.55f * res;

    const float subLevel = pSubLevel;
    const float detCents = pDetuneCents;
    const bool useSub = subLevel > 0.001f;
    const bool useDet = detCents > 0.01f;
    const float detRatio = useDet ? powf(2.0f, detCents / 1200.0f) : 1.0f;
    const float voiceNorm = 1.0f / (1.0f + subLevel + (useDet ? 0.7f : 0.0f));

    // Portamento: coefficiente per blocco, ricavato dal tempo impostato.
    const float glideMs = pGlideMs;
    const float glideCoef =
        (glideMs > 1.0f) ? (1.0f - expf(-(float)RENDER_BLOCK / samplesFor(glideMs))) : 1.0f;

    // Solo le voci che stanno davvero suonando entrano nel giro interno, e mai
    // piu' di MAX_LIVE_VOICES: oltre quel numero il blocco non si chiuderebbe
    // in tempo e si sentirebbe un buco, che e' molto peggio di una coda di
    // rilascio troncata.
    Voice *live[MAX_VOICES];
    uint8_t liveCount = 0;
    float energy = 0.0f;
    for (int i = 0; i < MAX_VOICES; ++i) {
        Voice &v = voices[i];
        if (v.stage == ENV_IDLE) continue;
        energy += v.envLevel;
        if (liveCount < MAX_LIVE_VOICES) {
            live[liveCount++] = &v;
        } else {
            // Sostituisce la piu' spenta fra quelle gia' scelte, se questa e'
            // piu' viva: chi si sente resta, chi sta svanendo esce.
            uint8_t weakest = 0;
            for (uint8_t k = 1; k < liveCount; ++k) {
                if (live[k]->envLevel < live[weakest]->envLevel) weakest = k;
            }
            if (v.envLevel > live[weakest]->envLevel) live[weakest] = &v;
        }
    }
    pActiveVoices = liveCount;

    // Portamento e vibrato: uno per voce, una volta per blocco.
    const float vibRatio =
        (lfoTarget == LFO_PITCH) ? powf(2.0f, (lfo * lfoDepth * 100.0f) / 1200.0f) : 1.0f;
    for (uint8_t n = 0; n < liveCount; ++n) {
        Voice &v = *live[n];
        if (v.phaseInc != v.phaseIncTarget) {
            const float cur = (float)v.phaseInc;
            const float tgt = (float)v.phaseIncTarget;
            const float next = cur + (tgt - cur) * glideCoef;
            v.phaseInc = (uint32_t)next;
            if (fabsf(tgt - next) < 16.0f) v.phaseInc = v.phaseIncTarget;
        }

        if (filtAmount <= 0.0f) {
            v.filtEnv = 0.0f;
            v.svfF = svfFbase;
        } else {
            v.filtEnv *= filtDecay;
            if (v.filtEnv < 0.002f) v.filtEnv = 0.0f;
            // Quattro ottave a fondo corsa, scalate anche dalla dinamica: una
            // nota piano apre meno di una forte, che e' quello che fa un
            // martelletto vero su una corda.
            float fc = cutSmooth * exp2f(filtAmount * 4.0f * v.filtEnv * v.envPeak);
            if (fc > cutMax) fc = cutMax;
            if (fc < 40.0f) fc = 40.0f;
            v.svfF = 2.0f * sinf((float)M_PI * fc / (float)SAMPLE_RATE);
        }
    }

    // Compensazione sull'energia, non sul numero di voci: una nota in coda di
    // rilascio pesa quanto vale davvero, invece di abbassare le altre come se
    // stesse ancora suonando a piena ampiezza.
    const float gainTarget = (energy > 1.0f) ? (1.0f / sqrtf(energy)) : 1.0f;

    // --------------------------------------------------------------- delay
    const float delayMix = pDelayMix;
    const bool useDelay = delayMix > 0.001f;
    int delaySamples = (int)(pDelayMs * (SAMPLE_RATE / 1000.0f));
    if (delaySamples < 64) delaySamples = 64;
    if (delaySamples > DELAY_SAMPLES - 1) delaySamples = DELAY_SAMPLES - 1;
    const float delayFb = pDelayFb;

    // -------------------------------------------------------------- 8 BIT
    const bool crush = pCrushOn;
    const uint8_t crushBits = pCrushBits;
    const int crushDiv = (pCrushDiv < 1) ? 1 : pCrushDiv;
    const float crushSteps = (float)(1u << (crushBits - 1));
    static float crushHold = 0.0f;
    static int crushPhase = 0;

    const float tremolo = (lfoTarget == LFO_AMP) ? (1.0f - lfoDepth * 0.5f * (1.0f + lfo)) : 1.0f;

    float blockPeak = 0.0f;
    float sumSq = 0.0f;

    for (size_t i = 0; i < frames; ++i) {
        // smoothing di volume, compensazione, drive e mix: evita lo zipper
        // noise quando si gira una manopola o entra una voce nuova
        volSmooth += (volTarget - volSmooth) * 0.0008f;
        gainSmooth += (gainTarget - gainSmooth) * 0.0008f;
        driveSmooth += (pDrive - driveSmooth) * 0.0008f;
        mixSmooth += (delayMix - mixSmooth) * 0.0008f;

        float s = 0.0f;
        for (uint8_t n = 0; n < liveCount; ++n) {
            Voice &v = *live[n];
            envTick(v, rates);

            const uint32_t inc =
                (vibRatio == 1.0f) ? v.phaseInc : (uint32_t)((float)v.phaseInc * vibRatio);
            v.phase += inc;
            float osc = oscSample(v.phase, wave, v.noiseState);
            if (useSub) {
                v.subPhase += inc >> 1;  // esattamente un'ottava sotto
                osc += subLevel * oscSample(v.subPhase, wave, v.noiseState);
            }
            if (useDet) {
                v.detunePhase += (uint32_t)((float)inc * detRatio);
                osc += 0.7f * oscSample(v.detunePhase, wave, v.noiseState);
            }
            osc *= voiceNorm;

            // Filtro a variabili di stato (Chamberlin): passa-basso risonante,
            // due sole ricorsioni per campione.
            const float high = osc * svfIn - v.svfLow - svfQ * v.svfBand;
            v.svfBand += v.svfF * high;
            v.svfLow += v.svfF * v.svfBand;
            // Il filtro con Q alto puo' divergere su transienti brutali: il
            // limite tiene la voce dentro invece di lasciarla esplodere.
            if (v.svfLow > 4.0f) v.svfLow = 4.0f;
            if (v.svfLow < -4.0f) v.svfLow = -4.0f;
            if (v.svfBand > 4.0f) v.svfBand = 4.0f;
            if (v.svfBand < -4.0f) v.svfBand = -4.0f;

            s += v.svfLow * v.envLevel;
        }
        s *= gainSmooth;

        if (driveSmooth > 0.001f) s = softClip(s * (1.0f + driveSmooth * 5.0f));

        if (useDelay) {
            int readIdx = delayWrite - delaySamples;
            if (readIdx < 0) readIdx += DELAY_SAMPLES;
            const float echo = (float)delayBuf[readIdx] * (1.0f / 32768.0f);
            float into = s + echo * delayFb;
            if (into > 1.0f) into = 1.0f;
            if (into < -1.0f) into = -1.0f;
            delayBuf[delayWrite] = (int16_t)(into * 32767.0f);
            delayWrite = (delayWrite + 1 >= DELAY_SAMPLES) ? 0 : delayWrite + 1;
            s += echo * mixSmooth;
        } else if (mixSmooth > 0.0005f) {
            // Il delay si e' appena spento: la coda va lasciata sfumare invece
            // di tagliarla di netto.
            int readIdx = delayWrite - delaySamples;
            if (readIdx < 0) readIdx += DELAY_SAMPLES;
            const float echo = (float)delayBuf[readIdx] * (1.0f / 32768.0f);
            delayBuf[delayWrite] = (int16_t)(echo * delayFb * 32767.0f);
            delayWrite = (delayWrite + 1 >= DELAY_SAMPLES) ? 0 : delayWrite + 1;
            s += echo * mixSmooth;
        }

        if (clickEnv > CLICK_EPS) {
            clickPhase += clickInc;
            s += sineTable[clickPhase >> (32 - SINE_TABLE_BITS)] * clickEnv * CLICK_LEVEL;
            clickEnv *= CLICK_DECAY;
        } else {
            clickEnv = 0.0f;
        }

        // ------------------------------------------------------------ 8 BIT
        // Prima la decimazione (tiene fermo il campione per N giri: e' quella
        // che fa il rumore metallico), poi la quantizzazione dei livelli.
        if (crush) {
            if (--crushPhase <= 0) {
                crushPhase = crushDiv;
                crushHold = s;
            }
            s = crushHold;
            s = floorf(s * crushSteps + 0.5f) / crushSteps;
        }

        s *= volSmooth * tremolo;

        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;

        // Misure per il display: dopo il clip, cosi' quello che si vede e'
        // esattamente quello che si sente.
        const float mag = fabsf(s);
        if (mag > blockPeak) blockPeak = mag;
        sumSq += s * s;
        scopeTick(s);

        const int16_t v16 = (int16_t)(s * 32000.0f);
        out[2 * i] = v16;      // L
        out[2 * i + 1] = v16;  // R (il MAX98357 e' mono: duplico per essere
                               //    indipendente dalla configurazione del pin SD)
    }

    const float rms = sqrtf(sumSq / (float)frames);
    const float level = pRms;
    pRms = level + (rms - level) * ((rms > level) ? VU_RISE : VU_FALL);
    if (blockPeak > pPeak) pPeak = blockPeak;

    // Le voci spente lasciano il filtro carico: scaricarlo dolcemente evita che
    // al rientro la voce parta con un offset residuo.
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (voices[i].stage == ENV_IDLE) {
            voices[i].svfLow -= voices[i].svfLow * 0.01f;
            voices[i].svfBand -= voices[i].svfBand * 0.01f;
        }
    }
}

void audioTask(void *) {
    static int16_t buffer[RENDER_BLOCK * 2];
    size_t written = 0;
    for (;;) {
        if (pStopRequest) {
            // Uscita ordinata: buffer DMA azzerato (niente ronzio residuo
            // sull'ampli), poi il task si elimina da solo.
            i2s_zero_dma_buffer(I2S_PORT);
            pStopped = true;
            vTaskDelete(nullptr);
        }
        renderBlock(buffer, RENDER_BLOCK);
        i2s_write(I2S_PORT, buffer, sizeof(buffer), &written, portMAX_DELAY);
    }
}

// Le sei assegnazioni possibili dei tre fili del connettore audio, nell'ordine
// in cui compaiono nel menu. La prima e' quella serigrafata sui moduli
// MAX98357 piu' diffusi.
struct AudioPins {
    uint8_t bclk;
    uint8_t lrclk;
    uint8_t dout;
};

const AudioPins AUDIO_PINSETS[AUDIO_ORDER_COUNT] = {
    {PIN_AUDIO_B, PIN_AUDIO_A, PIN_AUDIO_C},  // LRC BCLK DIN
    {PIN_AUDIO_A, PIN_AUDIO_B, PIN_AUDIO_C},  // BCLK LRC DIN
    {PIN_AUDIO_B, PIN_AUDIO_C, PIN_AUDIO_A},  // DIN BCLK LRC
    {PIN_AUDIO_C, PIN_AUDIO_B, PIN_AUDIO_A},  // DIN LRC BCLK
    {PIN_AUDIO_A, PIN_AUDIO_C, PIN_AUDIO_B},  // BCLK DIN LRC
    {PIN_AUDIO_C, PIN_AUDIO_A, PIN_AUDIO_B},  // LRC DIN BCLK
};

void applyPins(uint8_t order) {
    if (order >= AUDIO_ORDER_COUNT) order = 0;
    const AudioPins &p = AUDIO_PINSETS[order];
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = p.bclk;
    pins.ws_io_num = p.lrclk;
    pins.data_out_num = p.dout;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    i2s_set_pin(I2S_PORT, &pins);
}

void i2sInit() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 128;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.fixed_mclk = 0;

    i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);
    applyPins(pPinOrder);
    i2s_zero_dma_buffer(I2S_PORT);
    driverInstalled = true;
}

// Accodare un evento non deve mai bloccare il core 1: timeout zero, e in caso di
// coda piena si riferisce il fallimento al chiamante invece di fermare il loop.
bool post(uint8_t type, uint8_t id, float freq, float velocity = 1.0f) {
    if (!eventQueue) return false;
    NoteEvent ev = {type, id, freq, velocity};
    return xQueueSend(eventQueue, &ev, 0) == pdTRUE;
}

}  // namespace

namespace AudioEngine {

const char *const AUDIO_ORDER_NAMES[AUDIO_ORDER_COUNT] = {
    "LRC BCK DIN", "BCK LRC DIN", "DIN BCK LRC",
    "DIN LRC BCK", "BCK DIN LRC", "LRC DIN BCK",
};

void begin() {
    for (int i = 0; i < SINE_TABLE_SIZE; ++i) {
        sineTable[i] = sinf(2.0f * (float)M_PI * (float)i / (float)SINE_TABLE_SIZE);
    }
    for (int i = 0; i < MAX_VOICES; ++i) {
        voices[i] = Voice{};
        voices[i].stage = ENV_IDLE;
        voices[i].envPeak = 1.0f;
        voices[i].noiseState = 0x12345678u + (uint32_t)i * 2654435761u;
    }
    memset(delayBuf, 0, sizeof(delayBuf));
    volSmooth = pVolume;
    gainSmooth = 1.0f;
    cutSmooth = pCutoffHz;

    eventQueue = xQueueCreate(48, sizeof(NoteEvent));

    i2sInit();

    // Core 0 dedicato all'audio, core 1 a input/display/logica.
    xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 10, nullptr, 0);
}

void shutdown() {
    if (!driverInstalled) return;

    allNotesOff();
    pStopRequest = true;
    // Il task ricontrolla la richiesta ad ogni blocco (~3 ms): l'attesa e'
    // brevissima, ma con un tetto per non bloccare il core 1 in caso di guai.
    for (int i = 0; i < 100 && !pStopped; ++i) delay(5);

    i2s_driver_uninstall(I2S_PORT);
    driverInstalled = false;
}

bool voiceOn(uint8_t id, float freq, float velocity) {
    return post(EV_ON, id, freq, velocity);
}
bool voiceOff(uint8_t id) { return post(EV_OFF, id, 0.0f); }
bool voiceRetune(uint8_t id, float freq) { return post(EV_RETUNE, id, freq); }
void allNotesOff() { post(EV_ALL_OFF, 0, 0.0f); }

void click(bool accent) {
    pClickAccent = accent;
    pClick = true;
}

void setWaveform(uint8_t wave) {
    if (wave < WAVE_COUNT) pWave = wave;
}

void setCutoff(float hz) {
    if (hz < 40.0f) hz = 40.0f;
    if (hz > SAMPLE_RATE / 6.0f) hz = SAMPLE_RATE / 6.0f;
    pCutoffHz = hz;
}

void setResonance(float res) {
    if (res < 0.0f) res = 0.0f;
    if (res > 1.0f) res = 1.0f;
    pResonance = res;
}

void setVolume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    pVolume = vol;
}

void setDrive(float amount) {
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    pDrive = amount;
}

void setSubLevel(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    pSubLevel = level;
}

void setDetune(float cents) {
    if (cents < 0.0f) cents = 0.0f;
    if (cents > 50.0f) cents = 50.0f;
    pDetuneCents = cents;
}

void setGlide(float ms) {
    if (ms < 0.0f) ms = 0.0f;
    if (ms > 2000.0f) ms = 2000.0f;
    pGlideMs = ms;
}

void setFilterEnv(float amount, float decayMs) {
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    if (decayMs < 20.0f) decayMs = 20.0f;
    if (decayMs > 4000.0f) decayMs = 4000.0f;
    pFiltEnvAmount = amount;
    pFiltEnvMs = decayMs;
}

void setAttack(float ms) { pAttackMs = ms; }
void setDecay(float ms) { pDecayMs = ms; }
void setSustain(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    pSustain = level;
}
void setRelease(float ms) { pReleaseMs = ms; }

void setCrush(bool on) { pCrushOn = on; }

void setCrushBits(uint8_t bits) {
    if (bits < 1) bits = 1;
    if (bits > 16) bits = 16;
    pCrushBits = bits;
}

void setCrushDivider(uint8_t div) {
    if (div < 1) div = 1;
    if (div > 16) div = 16;
    pCrushDiv = div;
}

void setDelayTime(float ms) {
    if (ms < 20.0f) ms = 20.0f;
    if (ms > 395.0f) ms = 395.0f;
    pDelayMs = ms;
}

void setDelayFeedback(float fb) {
    if (fb < 0.0f) fb = 0.0f;
    if (fb > 0.9f) fb = 0.9f;  // oltre, l'eco cresce invece di spegnersi
    pDelayFb = fb;
}

void setDelayMix(float mix) {
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;
    pDelayMix = mix;
}

void setLfoRate(float hz) {
    if (hz < 0.05f) hz = 0.05f;
    if (hz > 20.0f) hz = 20.0f;
    pLfoRate = hz;
}

void setLfoDepth(float depth) {
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;
    pLfoDepth = depth;
}

void setLfoTarget(uint8_t target) {
    if (target < LFO_TARGET_COUNT) pLfoTarget = target;
}

void setPinOrder(uint8_t order) {
    if (order >= AUDIO_ORDER_COUNT) order = 0;
    pPinOrder = order;
    // Cambiare i pin a caldo e' legittimo: il driver resta installato, cambia
    // solo la matrice di uscita. Il buffer DMA si azzera per non sparare un
    // frammento di onda sui fili appena riassegnati.
    if (driverInstalled) {
        i2s_zero_dma_buffer(I2S_PORT);
        applyPins(order);
    }
}

uint8_t pinOrder() { return pPinOrder; }

bool isSounding() { return pActiveVoices > 0; }
uint8_t activeVoices() { return pActiveVoices; }

float rmsLevel() { return pRms; }

float peakLevel() {
    const float p = pPeak;
    pPeak = 0.0f;
    return p;
}

bool copyScope(int8_t *dst) {
    if (scopeState != SCOPE_READY) return false;
    memcpy(dst, scopeBuf, sizeof(scopeBuf));
    scopeState = SCOPE_ARMED;  // riarmata: la prossima finestra si riaggancia
    return true;
}

}  // namespace AudioEngine
