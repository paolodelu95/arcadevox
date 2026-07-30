// audio_engine.cpp — motore audio polifonico a 8 voci.
//
// Catena per voce:  osc(fase, onda) -> LPF one-pole -> ADSR
// Catena finale:    somma delle voci -> compensazione -> volume -> clip -> I2S
//
// Gira interamente in un task FreeRTOS pinnato sul core 0, cosi' che il refresh
// del display e la scansione degli input (core 1) non possano introdurre glitch.
//
// I parametri timbrici sono float/bool a 32 bit allineati: su ESP32 load/store
// sono atomici, quindi bastano le `volatile` senza lock fra i due core. Gli
// eventi di nota invece toccano piu' campi della stessa voce e non possono
// essere atomici: passano da una coda FreeRTOS che il task audio svuota
// all'inizio di ogni blocco. Nessuno dei due core scrive mai lo stato dell'altro.

#include "audio_engine.h"

#include <driver/i2s.h>
#include <math.h>
#include <string.h>

#include "pinout.h"

const char *const WAVEFORM_NAMES[WAVE_COUNT] = {"SINE", "SQUARE", "SAW", "TRIANGLE"};

namespace {

// ------------------------------------------------------------------ costanti
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr size_t RENDER_BLOCK = 128;  // campioni mono renderizzati per giro
constexpr int SINE_TABLE_BITS = 10;
constexpr int SINE_TABLE_SIZE = 1 << SINE_TABLE_BITS;  // 1024

float sineTable[SINE_TABLE_SIZE];

// --------------------------------------------------- parametri (scritti dal core 1)
volatile uint8_t pWave = WAVE_SINE;
volatile float pVolume = 0.6f;
volatile float pFilterAlpha = 0.5f;  // coefficiente del one-pole, gia' calcolato

volatile float pAttackMs = 10.0f;
volatile float pDecayMs = 120.0f;
volatile float pSustain = 0.7f;
volatile float pReleaseMs = 250.0f;

volatile uint8_t pActiveVoices = 0;

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
};

QueueHandle_t eventQueue = nullptr;

// --------------------------------------------------- stato delle voci (solo core 0)
// Ogni identificativo ha la sua voce: `voices[id]`, senza allocazione.
struct Voice {
    uint32_t phase;
    uint32_t phaseInc;
    float envLevel;
    float releaseInc;
    float lpState;
    EnvStage stage;
};

Voice voices[MAX_VOICES];

float volSmooth = 0.0f;
float alphaSmooth = 0.5f;
// Compensazione d'ampiezza: 8 voci a fondo scala clipperebbero. Si divide per la
// radice del numero di voci attive (somma incoerente ~ sqrt(n)), con smoothing
// perche' un gradino di guadagno all'attacco di una nota si sente come un
// risucchio sulle altre.
float gainSmooth = 1.0f;

// --------------------------------------------------------------- metronomo
// Sinusoide corta a decadimento esponenziale, sommata *dopo* filtro e inviluppo:
// il click resta netto e si sente anche mentre delle note sono in corso.
constexpr float CLICK_LEVEL = 0.22f;
constexpr float CLICK_DECAY = 0.99375f;  // ~25 ms fino all'inudibile
constexpr float CLICK_EPS = 0.001f;
constexpr float CLICK_HZ = 1400.0f;
constexpr float CLICK_HZ_ACCENT = 2000.0f;

uint32_t clickPhase = 0;
uint32_t clickInc = 0;
float clickEnv = 0.0f;

// ------------------------------------------------------------- monitoraggio
// Quello che il display mostra nelle schermate VU e SCOPE. Misurato qui perche'
// e' l'unico posto dove il segnale finale esiste campione per campione.
//
// Niente lock: livello e picco sono float a 32 bit allineati (load/store atomici
// su ESP32), mentre la finestra dell'oscilloscopio e' protetta da una macchina a
// tre stati in cui il buffer appartiene a un core solo alla volta.
volatile float pRms = 0.0f;
volatile float pPeak = 0.0f;

// Ballistica del VU: sale quasi subito, scende piano. Coefficienti per blocco
// (~2,9 ms): 0,35 in salita e 0,05 in discesa fanno ~8 ms e ~58 ms di costante.
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

// Senza aggancio l'onda scorrerebbe lateralmente ad ogni fotogramma: si parte a
// catturare solo su un attraversamento dello zero in salita. In silenzio quel
// fronte non arriva mai, quindi dopo 100 ms si cattura comunque e si vede la
// traccia piatta invece di una schermata congelata.
constexpr uint32_t SCOPE_TIMEOUT = SAMPLE_RATE / 10;

// Numero di campioni corrispondenti a una durata in ms (min 1, evita divisioni per 0).
inline float samplesFor(float ms) {
    float s = ms * (SAMPLE_RATE / 1000.0f);
    return (s < 1.0f) ? 1.0f : s;
}

inline uint32_t freqToPhaseInc(float freq) {
    // 2^32 / SAMPLE_RATE * freq
    return (uint32_t)(freq * (4294967296.0f / (float)SAMPLE_RATE));
}

// Un campione di oscillatore, -1..+1.
inline float oscSample(uint32_t ph, uint8_t wave) {
    switch (wave) {
        case WAVE_SQUARE:
            return (ph < 0x80000000UL) ? 1.0f : -1.0f;
        case WAVE_SAW:
            // rampa lineare -1..+1 sul periodo
            return (float)((int32_t)(ph - 0x80000000UL)) * (1.0f / 2147483648.0f);
        case WAVE_TRIANGLE: {
            float t = (float)ph * (1.0f / 4294967296.0f);  // 0..1
            return (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
        }
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

// Avanza l'inviluppo di una voce di un campione.
inline void envTick(Voice &v, const EnvRates &r) {
    switch (v.stage) {
        case ENV_ATTACK:
            v.envLevel += r.attack;
            if (v.envLevel >= 1.0f) {
                v.envLevel = 1.0f;
                v.stage = ENV_DECAY;
            }
            break;
        case ENV_DECAY:
            v.envLevel -= r.decay;
            if (v.envLevel <= r.sustain) {
                v.envLevel = r.sustain;
                v.stage = ENV_SUSTAIN;
            }
            break;
        case ENV_SUSTAIN:
            // segue in tempo reale eventuali modifiche del sustain in edit mode
            v.envLevel += (r.sustain - v.envLevel) * 0.0005f;
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

// Un campione dentro la finestra dell'oscilloscopio. Gira nel giro interno del
// render: deve restare qualche operazione in croce.
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

// Svuota la coda degli eventi arrivati dal core 1. Girando una volta per blocco,
// la latenza massima e' quella di un blocco: ~2,9 ms, sotto la soglia percepibile.
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
                v.phaseInc = freqToPhaseInc(ev.freq);
                v.stage = ENV_ATTACK;
                // La fase non si azzera: ripartire sempre da zero su onde a
                // spigolo produce un click ad ogni attacco.
                break;
            case EV_OFF:
                releaseVoice(v);
                break;
            case EV_RETUNE:
                v.phaseInc = freqToPhaseInc(ev.freq);
                break;
            default:
                break;
        }
    }
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
    const float alphaTarget = pFilterAlpha;

    EnvRates rates;
    rates.sustain = pSustain;
    rates.attack = 1.0f / samplesFor(pAttackMs);
    rates.decay = (1.0f - rates.sustain) / samplesFor(pDecayMs);

    // Solo le voci che stanno davvero suonando entrano nel giro interno.
    Voice *live[MAX_VOICES];
    uint8_t liveCount = 0;
    float energy = 0.0f;
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (voices[i].stage == ENV_IDLE) continue;
        live[liveCount++] = &voices[i];
        energy += voices[i].envLevel;
    }
    pActiveVoices = liveCount;

    // Compensazione sull'energia, non sul numero di voci: una nota in coda di
    // rilascio pesa quanto vale davvero, invece di abbassare le altre come se
    // stesse ancora suonando a piena ampiezza.
    const float gainTarget = (energy > 1.0f) ? (1.0f / sqrtf(energy)) : 1.0f;

    float blockPeak = 0.0f;
    float sumSq = 0.0f;

    for (size_t i = 0; i < frames; ++i) {
        // smoothing di volume, cutoff e compensazione: evita zipper noise
        // quando si gira una manopola o entra una voce nuova
        volSmooth += (volTarget - volSmooth) * 0.0008f;
        alphaSmooth += (alphaTarget - alphaSmooth) * 0.0008f;
        gainSmooth += (gainTarget - gainSmooth) * 0.0008f;

        float s = 0.0f;
        for (uint8_t n = 0; n < liveCount; ++n) {
            Voice &v = *live[n];
            envTick(v, rates);
            v.phase += v.phaseInc;
            const float osc = oscSample(v.phase, wave);
            v.lpState += alphaSmooth * (osc - v.lpState);  // passa-basso one-pole IIR
            s += v.lpState * v.envLevel;
        }
        s *= gainSmooth * volSmooth;

        if (clickEnv > CLICK_EPS) {
            clickPhase += clickInc;
            s += sineTable[clickPhase >> (32 - SINE_TABLE_BITS)] * clickEnv * CLICK_LEVEL *
                 volSmooth;
            clickEnv *= CLICK_DECAY;
        } else {
            clickEnv = 0.0f;
        }

        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;

        // Misure per il display: dopo il clip, cosi' quello che si vede e'
        // esattamente quello che si sente.
        const float mag = fabsf(s);
        if (mag > blockPeak) blockPeak = mag;
        sumSq += s * s;
        scopeTick(s);

        int16_t v16 = (int16_t)(s * 32000.0f);

        out[2 * i] = v16;      // L
        out[2 * i + 1] = v16;  // R (il MAX98357 e' mono: duplico per essere
                               //    indipendente dalla configurazione del pin SD)
    }

    // Chiusura delle misure del blocco.
    const float rms = sqrtf(sumSq / (float)frames);
    const float level = pRms;
    pRms = level + (rms - level) * ((rms > level) ? VU_RISE : VU_FALL);
    if (blockPeak > pPeak) pPeak = blockPeak;

    // Le voci spente lasciano il filtro carico: scaricarlo dolcemente evita che
    // al rientro la voce parta con un offset DC residuo.
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (voices[i].stage == ENV_IDLE) voices[i].lpState -= voices[i].lpState * 0.01f;
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

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_I2S_BCLK;
    pins.ws_io_num = PIN_I2S_LRCLK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    i2s_driver_install(I2S_PORT, &cfg, 0, nullptr);
    i2s_set_pin(I2S_PORT, &pins);
    i2s_zero_dma_buffer(I2S_PORT);
    driverInstalled = true;
}

// Accodare un evento non deve mai bloccare il core 1: timeout zero, e in caso di
// coda piena si riferisce il fallimento al chiamante invece di fermare il loop.
bool post(uint8_t type, uint8_t id, float freq) {
    if (!eventQueue) return false;
    NoteEvent ev = {type, id, freq};
    return xQueueSend(eventQueue, &ev, 0) == pdTRUE;
}

}  // namespace

namespace AudioEngine {

void begin() {
    for (int i = 0; i < SINE_TABLE_SIZE; ++i) {
        sineTable[i] = sinf(2.0f * (float)M_PI * (float)i / (float)SINE_TABLE_SIZE);
    }
    for (int i = 0; i < MAX_VOICES; ++i) {
        voices[i] = Voice{};
        voices[i].stage = ENV_IDLE;
    }
    volSmooth = pVolume;
    alphaSmooth = pFilterAlpha;
    gainSmooth = 1.0f;

    eventQueue = xQueueCreate(32, sizeof(NoteEvent));

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

bool voiceOn(uint8_t id, float freq) { return post(EV_ON, id, freq); }
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
    if (hz < 20.0f) hz = 20.0f;
    if (hz > SAMPLE_RATE * 0.45f) hz = SAMPLE_RATE * 0.45f;
    // coefficiente del passa-basso one-pole: a = 1 - e^(-2*pi*fc/fs)
    float a = 1.0f - expf(-2.0f * (float)M_PI * hz / (float)SAMPLE_RATE);
    if (a > 1.0f) a = 1.0f;
    pFilterAlpha = a;
}

void setVolume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    pVolume = vol;
}

void setAttack(float ms) { pAttackMs = ms; }
void setDecay(float ms) { pDecayMs = ms; }
void setSustain(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    pSustain = level;
}
void setRelease(float ms) { pReleaseMs = ms; }

bool isSounding() { return pActiveVoices > 0; }
uint8_t activeVoices() { return pActiveVoices; }

float rmsLevel() { return pRms; }

float peakLevel() {
    const float p = pPeak;
    // Azzerare dopo la lettura: se nel frattempo il task audio ha alzato il
    // picco si perde un valore, ma solo verso il basso e solo per un fotogramma.
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
