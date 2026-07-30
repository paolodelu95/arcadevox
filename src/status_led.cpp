// status_led.cpp — tre giochi di luce in loop sul WS2812 di bordo.
//
// Il LED viene pilotato con neopixelWrite() dell'Arduino core (RMT): una scrittura
// dura ~30 us, quindi si puo' chiamare tranquillamente dal loop del core 1 senza
// disturbare il task audio sul core 0.
//
// A luminosita' bassa il WS2812 ha pochi livelli utili (a 20% restano ~51 passi su
// 255), percio' le rampe passano per una curva quadratica: la dissolvenza resta
// morbida all'occhio invece di partire a scatti.

#include "status_led.h"

#include <math.h>

#include "pinout.h"

namespace {

constexpr uint32_t FRAME_MS = 25;       // ~40 fps
constexpr uint32_t PATTERN_MS = 7000;   // durata di ogni gioco di luce

float brightness = 0.20f;

uint32_t lastFrame = 0;
uint32_t patternStart = 0;
uint8_t pattern = 0;

enum { PAT_RAINBOW = 0, PAT_BREATH, PAT_PULSE, PAT_COUNT };

// Palette usata dal battito: gli stessi colori dei tasti del pannello.
struct Rgb {
    uint8_t r, g, b;
};
const Rgb PULSE_COLORS[4] = {
    {40, 170, 80},   // verde
    {220, 45, 30},   // rosso
    {240, 180, 5},   // giallo
    {30, 110, 235},  // blu
};

// Livello 0..1 -> byte, con curva quadratica e scala di luminosita'.
uint8_t level(float x, float channel) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    float v = x * x * channel * brightness;
    int out = (int)(v * 255.0f + 0.5f);
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    return (uint8_t)out;
}

// HSV -> RGB con h in gradi, s e v in 0..1. Restituisce canali 0..1.
void hsv(float h, float s, float v, float &r, float &g, float &b) {
    h = fmodf(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    if (h < 60) { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    r += m; g += m; b += m;
}

void writeRgb(float r, float g, float b, float amount) {
    neopixelWrite(PIN_RGB_LED, level(amount, r), level(amount, g), level(amount, b));
}

// 1) Arcobaleno: la tinta ruota lentamente a piena ampiezza.
void patternRainbow(uint32_t t) {
    float r, g, b;
    hsv((float)t * 0.055f, 1.0f, 1.0f, r, g, b);
    writeRgb(r, g, b, 1.0f);
}

// 2) Respiro: dissolvenza sinusoidale su una tinta che deriva dal ciano al viola.
void patternBreath(uint32_t t) {
    float phase = (float)t * (2.0f * (float)M_PI / 2800.0f);
    float amount = 0.5f - 0.5f * cosf(phase);  // 0 -> 1 -> 0
    float r, g, b;
    hsv(180.0f + 90.0f * sinf((float)t * 0.0004f), 0.85f, 1.0f, r, g, b);
    writeRgb(r, g, b, amount);
}

// 3) Battito: due lampi ravvicinati e poi pausa, cambiando colore ad ogni battito.
void patternPulse(uint32_t t) {
    constexpr uint32_t BEAT_MS = 1500;
    uint32_t phase = t % BEAT_MS;
    uint32_t beat = t / BEAT_MS;
    const Rgb &c = PULSE_COLORS[beat % 4];

    float amount = 0.0f;
    if (phase < 220) {
        amount = 1.0f - (float)phase / 220.0f;
    } else if (phase >= 300 && phase < 560) {
        amount = 0.65f * (1.0f - (float)(phase - 300) / 260.0f);
    }

    writeRgb(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, amount);
}

}  // namespace

namespace StatusLed {

void begin() {
    // Prima cosa: zittire il LED, che all'accensione puo' trovarsi in qualsiasi stato.
    neopixelWrite(PIN_RGB_LED, 0, 0, 0);
    lastFrame = millis();
    patternStart = lastFrame;
    pattern = PAT_RAINBOW;
}

void off() { neopixelWrite(PIN_RGB_LED, 0, 0, 0); }

void setBrightness(float b) {
    if (b < 0.0f) b = 0.0f;
    if (b > 1.0f) b = 1.0f;
    brightness = b;
}

void update(uint32_t now) {
    if (now - lastFrame < FRAME_MS) return;
    lastFrame = now;

    if (now - patternStart >= PATTERN_MS) {
        patternStart = now;
        pattern = (uint8_t)((pattern + 1) % PAT_COUNT);
    }

    const uint32_t t = now - patternStart;
    switch (pattern) {
        case PAT_BREATH: patternBreath(t); break;
        case PAT_PULSE:  patternPulse(t); break;
        case PAT_RAINBOW:
        default:         patternRainbow(t); break;
    }
}

}  // namespace StatusLed
