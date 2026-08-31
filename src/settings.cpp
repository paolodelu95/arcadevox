// settings.cpp — scale di sensibilita' degli encoder, scale musicali, luci.

#include "settings.h"

#include "audio_engine.h"  // AUDIO_ORDER_NAMES / AUDIO_ORDER_COUNT
#include "pinout.h"        // NOTE_COUNT
#include "presets.h"
#include "instruments.h"  // INSTRUMENT_EXTRA: piano e batteria in coda ai timbri

namespace {

// Scatti di detent per percorrere l'intera corsa del parametro. Una manopola
// EC11 ne fa 20 per giro, quindi l'etichetta mostra direttamente i giri: e' la
// grandezza che senti sotto le dita, molto piu' dei "1/64 di corsa per scatto"
// che c'erano nel codice.
//
// La scala e' scritta in ordine CRESCENTE di giri, e non e' un dettaglio di
// stile. Il menu somma l'indice al movimento dell'encoder, sempre, per ogni
// voce: se qui i numeri scendessero mentre l'indice sale, girando la manopola in
// senso orario il numero a video calerebbe, e la manopola sembrerebbe montata al
// contrario.
//
// Attenzione a chi ritocca questa tabella: gli indici finiscono in NVS. Se
// l'ordine cambia di nuovo, va alzato SCALE_REV in storage.h e aggiunta la
// conversione, altrimenti le schede gia' in giro si ritrovano la sensibilita'
// ribaltata al primo avvio.
const uint16_t DETENTS[6] = {12, 20, 32, 48, 64, 100};
const char *const TURNS[6] = {"0.6 giri", "1.0 giri", "1.6 giri",
                              "2.4 giri", "3.2 giri", "5.0 giri"};

const float FINE[4] = {2.0f, 4.0f, 8.0f, 16.0f};
const char *const FINE_LABELS[4] = {"1/2", "1/4", "1/8", "1/16"};

// ----------------------------------------------------------------- scale
// Con tredici tasti la scala cromatica ci sta tutta, ma proprio per questo ha
// senso poterne scegliere un'altra: su una pentatonica gli stessi tredici tasti
// diventano due ottave abbondanti, e non c'e' modo di sbagliare una nota.
const uint8_t SCALE_MAJOR[] = {0, 2, 4, 5, 7, 9, 11};
const uint8_t SCALE_MINOR[] = {0, 2, 3, 5, 7, 8, 10};
const uint8_t SCALE_PENTA[] = {0, 3, 5, 7, 10};
const uint8_t SCALE_BLUES[] = {0, 3, 5, 6, 7, 10};
const uint8_t SCALE_DORIAN[] = {0, 2, 3, 5, 7, 9, 10};
const uint8_t SCALE_ARABIC[] = {0, 1, 4, 5, 7, 8, 11};

struct Scale {
    const uint8_t *steps;
    uint8_t count;
};

// L'indice 0 (cromatica) non ha tabella: e' l'identita' e si tratta a parte.
const Scale SCALES[] = {
    {nullptr, 12},
    {SCALE_MAJOR, 7},
    {SCALE_MINOR, 7},
    {SCALE_PENTA, 5},
    {SCALE_BLUES, 6},
    {SCALE_DORIAN, 7},
    {SCALE_ARABIC, 7},
};
constexpr uint8_t SCALE_COUNT = sizeof(SCALES) / sizeof(SCALES[0]);

const char *const SCALE_LABELS[SCALE_COUNT] = {"CROMAT.", "MAGG.",  "MIN.",  "PENTAT.",
                                               "BLUES",   "DORICA", "ARABA"};

const char *const ROOT_LABELS[12] = {"DO",  "DO#", "RE",  "RE#", "MI",  "FA",
                                     "FA#", "SOL", "SOL#", "LA", "LA#", "SI"};

// Il clock si manda solo a chi lo vuole: un DAW che riceve impulsi di
// sincronismo senza aspettarseli comincia a seguire il tempo del synth, e chi
// non lo sapeva si ritrova il progetto che accelera da solo.
const char *const MIDIOUT_LABELS[3] = {"SPENTO", "NOTE", "NOTE+CLOCK"};

// Il verso dello schermo. "CAPOVOLTO" e non "180": chi lo cerca sta guardando un
// display montato al contrario, non un angolo da scegliere.
const char *const SCHERMO_LABELS[2] = {"NORMALE", "CAPOVOLTO"};

const char *const LED_LABELS[9] = {"SPENTE", "1", "2", "3", "4", "5", "6", "7", "MAX"};

}  // namespace

namespace Settings {

// I default riproducono il comportamento di sempre: chi aggiorna non trova le
// manopole cambiate sotto le dita finche' non decide lui.
const Entry ENTRIES[SETTING_COUNT] = {
    {"ENCODER", "VOLUME", 6, 3, TURNS},
    {nullptr, "CUTOFF", 6, 4, TURNS},
    {nullptr, "ADSR", 6, 3, TURNS},
    {nullptr, "PASSO FINE", 4, 1, FINE_LABELS},
    // I nomi dei timbri stanno in presets.cpp e non in una tabella qui: sono
    // gia' scritti accanto ai parametri che descrivono, ed e' li' che vanno
    // tenuti allineati. valueLabel() sa dove pescarli.
    {"TASTIERA", "SCALA", SCALE_COUNT, 0, SCALE_LABELS},
    {nullptr, "TONICA", 12, 0, ROOT_LABELS},
    {"LUCI", "LUMINOSITA'", 9, 5, LED_LABELS},
    {nullptr, "IMPARA LUCI", 0, 0, nullptr},
    // Da' finalmente un chiamante a Keylight::resetMap(), che esisteva da sempre
    // e non la chiamava nessuno: se la mappa veniva imparata storta, l'unico
    // rimedio da pannello era rifare tutta la procedura.
    {nullptr, "AZZERA LUCI", 0, 0, nullptr},
    {"SCHERMO", "VERSO", 2, 0, SCHERMO_LABELS},
    {"AUDIO", "USCITA", AUDIO_ORDER_COUNT, 0, AudioEngine::AUDIO_ORDER_NAMES},
    {nullptr, "MIDI OUT", 3, 1, MIDIOUT_LABELS},
    {"RETE", "MODALITA' WIFI", 0, 0, nullptr},
    // Fuori dal menu (sta oltre SETTING_MENU_COUNT): esiste per la memoria, per
    // il program change e per la schermata TIMBRI, che lo disegna da se'.
    {nullptr, "TIMBRO", 0xFF, 0, nullptr},
};

// Quante posizioni ha una voce. Il TIMBRO e' l'unica che non lo sa da sola: i
// preset sono definiti in presets.cpp e il loro numero cambia aggiungendone uno,
// senza dover ritoccare la tabella qui sopra.
uint8_t valueCount(uint8_t which) {
    if (which >= SETTING_COUNT) return 0;
    // I timbri sono i quindici preset piu' gli strumenti campionati, che stanno
    // sulla stessa manopola perche' per chi suona sono la stessa scelta.
    if (which == SETTING_TIMBRO) return PRESET_COUNT + INSTRUMENT_EXTRA;
    return ENTRIES[which].count;
}

uint8_t clampIndex(uint8_t which, uint8_t index) {
    const uint8_t n = valueCount(which);
    if (n == 0) return 0;
    return (index < n) ? index : ENTRIES[which].byDefault;
}

const char *valueLabel(uint8_t which, uint8_t index) {
    if (which == SETTING_TIMBRO) {
        const uint8_t i = clampIndex(which, index);
        if (i < PRESET_COUNT) return PRESETS[i].name;
        return SAMPLED_INSTRUMENTS[i - PRESET_COUNT].name;
    }
    if (which >= SETTING_COUNT || ENTRIES[which].count == 0) return "";
    return ENTRIES[which].valueLabels[clampIndex(which, index)];
}

float step(uint8_t which, uint8_t index) {
    return 1.0f / (float)DETENTS[clampIndex(which, index)];
}

float fineDivider(uint8_t index) { return FINE[clampIndex(SETTING_FINE, index)]; }

int scaleSemitone(uint8_t scaleIdx, int degree) {
    if (degree < 0) degree = 0;
    if (degree >= NOTE_COUNT) degree = NOTE_COUNT - 1;
    const Scale &s = SCALES[clampIndex(SETTING_SCALE, scaleIdx)];
    if (!s.steps) return degree;  // cromatica
    return 12 * (degree / s.count) + s.steps[degree % s.count];
}

bool scaleIsRoot(uint8_t scaleIdx, int degree) {
    const Scale &s = SCALES[clampIndex(SETTING_SCALE, scaleIdx)];
    const uint8_t n = s.steps ? s.count : 12;
    return (degree % n) == 0;
}

const char *rootName(uint8_t rootIdx) { return ROOT_LABELS[rootIdx % 12]; }

}  // namespace Settings
