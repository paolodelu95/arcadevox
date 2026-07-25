// audio_engine.cpp — motore audio monofonico.
//
// Catena per campione:  osc(phase, wave) -> LPF one-pole -> ADSR -> volume -> I2S
//
// Gira interamente in un task FreeRTOS pinnato sul core 0, cosi' che il refresh
// del display e la scansione degli input (core 1) non possano introdurre glitch.
// I parametri sono float/bool a 32 bit allineati: su ESP32 load/store sono atomici,
// quindi bastano le `volatile` senza lock fra i due core.

#include "audio_engine.h"

#include <driver/i2s.h>
#include <math.h>

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
volatile float pFreq = 440.0f;
volatile uint8_t pWave = WAVE_SINE;
volatile float pVolume = 0.6f;
volatile float pFilterAlpha = 0.5f;  // coefficiente del one-pole, gia' calcolato

volatile float pAttackMs = 10.0f;
volatile float pDecayMs = 120.0f;
volatile float pSustain = 0.7f;
volatile float pReleaseMs = 250.0f;

volatile bool pGate = false;       // nota tenuta
volatile bool pRetrigger = false;  // richiesta di ripartire dall'ATTACK
volatile uint8_t pStage = ENV_IDLE;

// ------------------------------------------------- stato interno (solo core 0)
uint32_t phase = 0;
uint32_t phaseInc = 0;
float lastFreq = -1.0f;

float envLevel = 0.0f;
float releaseInc = 0.0f;
EnvStage stage = ENV_IDLE;

float lpState = 0.0f;
float volSmooth = 0.0f;
float alphaSmooth = 0.5f;

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

// Avanza l'inviluppo di un campione.
inline void envTick() {
    switch (stage) {
        case ENV_ATTACK:
            envLevel += 1.0f / samplesFor(pAttackMs);
            if (envLevel >= 1.0f) {
                envLevel = 1.0f;
                stage = ENV_DECAY;
            }
            break;
        case ENV_DECAY: {
            float sus = pSustain;
            envLevel -= (1.0f - sus) / samplesFor(pDecayMs);
            if (envLevel <= sus) {
                envLevel = sus;
                stage = ENV_SUSTAIN;
            }
            break;
        }
        case ENV_SUSTAIN:
            // segue in tempo reale eventuali modifiche del sustain in edit mode
            envLevel += (pSustain - envLevel) * 0.0005f;
            break;
        case ENV_RELEASE:
            envLevel -= releaseInc;
            if (envLevel <= 0.0f) {
                envLevel = 0.0f;
                stage = ENV_IDLE;
            }
            break;
        case ENV_IDLE:
        default:
            envLevel = 0.0f;
            break;
    }
}

void renderBlock(int16_t *out, size_t frames) {
    // Eventi di nota, campionati una volta per blocco.
    if (pRetrigger) {
        pRetrigger = false;
        stage = ENV_ATTACK;
    }
    if (!pGate && stage != ENV_RELEASE && stage != ENV_IDLE) {
        releaseInc = (envLevel > 0.0f ? envLevel : 1.0f) / samplesFor(pReleaseMs);
        stage = ENV_RELEASE;
    }

    float freq = pFreq;
    if (freq != lastFreq) {
        lastFreq = freq;
        phaseInc = freqToPhaseInc(freq);
    }
    const uint8_t wave = pWave;
    const float volTarget = pVolume;
    const float alphaTarget = pFilterAlpha;

    for (size_t i = 0; i < frames; ++i) {
        envTick();

        // smoothing di volume e cutoff: evita zipper noise quando si gira un pot
        volSmooth += (volTarget - volSmooth) * 0.0008f;
        alphaSmooth += (alphaTarget - alphaSmooth) * 0.0008f;

        float s = 0.0f;
        if (stage != ENV_IDLE) {
            phase += phaseInc;
            s = oscSample(phase, wave);
            lpState += alphaSmooth * (s - lpState);  // passa-basso one-pole IIR
            s = lpState * envLevel * volSmooth;
        } else {
            // silenzio: scarico dolcemente il filtro per non lasciare offset DC
            lpState -= lpState * 0.01f;
        }

        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t v = (int16_t)(s * 32000.0f);

        out[2 * i] = v;      // L
        out[2 * i + 1] = v;  // R (il MAX98357 e' mono: duplico per essere
                             //    indipendente dalla configurazione del pin SD)
    }

    pStage = (uint8_t)stage;
}

void audioTask(void *) {
    static int16_t buffer[RENDER_BLOCK * 2];
    size_t written = 0;
    for (;;) {
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
}

}  // namespace

namespace AudioEngine {

void begin() {
    for (int i = 0; i < SINE_TABLE_SIZE; ++i) {
        sineTable[i] = sinf(2.0f * (float)M_PI * (float)i / (float)SINE_TABLE_SIZE);
    }
    volSmooth = pVolume;
    alphaSmooth = pFilterAlpha;

    i2sInit();

    // Core 0 dedicato all'audio, core 1 a input/display/logica.
    xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 10, nullptr, 0);
}

void noteOn(float freq) {
    pFreq = freq;
    pGate = true;
    pRetrigger = true;
}

void setFrequency(float freq) { pFreq = freq; }

void noteOff() { pGate = false; }

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

EnvStage envStage() { return (EnvStage)pStage; }
bool isSounding() { return pStage != ENV_IDLE; }

}  // namespace AudioEngine
