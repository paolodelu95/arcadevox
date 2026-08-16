// ArcadeVox — sintetizzatore mono/polifonico su ESP32-S3, scheda 2026-08.
//
//   core 0 : motore audio (task FreeRTOS dedicato, vedi audio_engine.cpp)
//   core 1 : questo loop — input, logica, sequencer, luci, display
//
// La scheda nuova porta 13 tasti nota (un'ottava cromatica intera), 7 tasti
// funzione, 4 encoder con pulsante e un LED RGB sotto ogni tasto. Vedi
// pinout.h per il cablaggio e README.md per cosa fa ogni comando.

#include <Arduino.h>
#include <math.h>

#include "audio_engine.h"
#include "display.h"
#include "input_handler.h"
#include "keylight.h"
#include "net_portal.h"
#include "pinout.h"
#include "sequencer.h"
#include "settings.h"
#include "status_led.h"
#include "storage.h"
#include "version.h"

// ---------------------------------------------------------------- costanti
// DO centrale: da qui salgono i semitoni della scala scelta e scende o sale
// l'ottava. Con tredici tasti la tastiera copre un'ottava cromatica esatta,
// dal DO al DO successivo compreso.
static const float BASE_FREQ = 261.63f;

static const uint32_t DISPLAY_INTERVAL_MS = 33;  // ~30 fps
static const uint32_t LIGHT_INTERVAL_MS = 33;
// In modalita' rete non c'e' niente di veloce da mostrare, e ridisegnare un QR
// costa: bastano 4 giri al secondo.
static const uint32_t NETWORK_REFRESH_MS = 250;

static const int8_t OCTAVE_MIN = -2;
static const int8_t OCTAVE_MAX = 2;
static const float OCTAVE_MUL[5] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};

// Range dei parametri (mappature esponenziali pilotate dagli encoder).
static const float CUTOFF_MIN_HZ = 80.0f;
static const float CUTOFF_RATIO = 90.0f;  // 80 Hz .. 7200 Hz (il limite del filtro)
static const float ATTACK_MIN_MS = 2.0f;
static const float ATTACK_RATIO = 250.0f;  // 2 ms .. 500 ms
static const float RELEASE_MIN_MS = 10.0f;
static const float RELEASE_RATIO = 200.0f;  // 10 ms .. 2000 ms
static const float DECAY_MIN_MS = 5.0f;
static const float DECAY_RATIO = 200.0f;  // 5 ms .. 1000 ms
static const float DELAY_MIN_MS = 20.0f;
static const float DELAY_RATIO = 19.5f;  // 20 ms .. 390 ms
static const float LFO_MIN_HZ = 0.1f;
static const float LFO_RATIO = 200.0f;  // 0.1 Hz .. 20 Hz
static const float GLIDE_MIN_MS = 5.0f;
static const float GLIDE_RATIO = 100.0f;  // 5 ms .. 500 ms

// ------------------------------------------------------------- 8 BIT
// Quattro gradini di degrado, dal "vecchio campionatore" al "console tascabile".
// La decimazione conta piu' dei bit: e' lei a mettere quel velo metallico che si
// riconosce a orecchio.
struct CrushPreset {
    uint8_t bits;
    uint8_t divider;
    const char *label;
};

static const CrushPreset CRUSH_PRESETS[] = {
    {12, 1, "12 BIT"},
    {8, 2, "8 BIT"},
    {6, 3, "6 BIT"},
    {4, 6, "4 BIT"},
};
static const uint8_t CRUSH_COUNT = sizeof(CRUSH_PRESETS) / sizeof(CRUSH_PRESETS[0]);

// ------------------------------------------------------------- arpeggiator
enum ArpMode : uint8_t {
    ARP_UP = 0,
    ARP_DOWN,
    ARP_UPDOWN,
    ARP_RANDOM,
    ARP_ORDER,  // nell'ordine in cui hai premuto i tasti
    ARP_MODE_COUNT
};
static const char *const ARP_NAMES[ARP_MODE_COUNT] = {"SU", "GIU'", "SU/GIU'", "CASUALE",
                                                      "ORDINE"};

// ------------------------------------------------------------- accordi
// Intervalli aggiunti alla nota suonata, in semitoni. Zero = nessuna aggiunta.
struct ChordMode {
    int8_t a;
    int8_t b;
    const char *label;
};

static const ChordMode CHORDS[] = {
    {0, 0, "SINGOLA"}, {7, 0, "QUINTA"},  {4, 7, "MAGGIORE"},
    {3, 7, "MINORE"},  {5, 7, "SOSPESO"}, {12, 0, "OTTAVA"},
};
static const uint8_t CHORD_COUNT = sizeof(CHORDS) / sizeof(CHORDS[0]);

// --------------------------------------------------- quarto encoder
enum Enc4Target : uint8_t {
    E4_BPM = 0,
    E4_DELAY_MIX,
    E4_DELAY_TIME,
    E4_LFO_RATE,
    E4_LFO_DEPTH,
    E4_DRIVE,
    E4_SUB,
    E4_DETUNE,
    E4_GLIDE,
    E4_COUNT
};
static const char *const E4_NAMES[E4_COUNT] = {"BPM",   "ECO MIX", "ECO TEMPO", "LFO VEL",
                                               "LFO PROF", "DRIVE", "SUB",    "DETUNE",
                                               "GLIDE"};

// Quanto muove uno scatto di encoder non e' una costante: lo decide la schermata
// SETTINGS, perche' e' una questione di gusto e cambia da mano a mano.
static uint8_t setIndex[SETTING_COUNT];

// Passo della voce `which`, diviso per il passo fine se il click e' inserito.
static float stepFor(uint8_t which, bool fine) {
    const float s = Settings::step(which, setIndex[which]);
    return fine ? (s / Settings::fineDivider(setIndex[SETTING_FINE])) : s;
}

// ------------------------------------------------------------------- stato
static uint8_t waveform = WAVE_SAW;
static int8_t octave = 0;

static float cutoffHz = 4000.0f;
static float resonance = 0.0f;
static float volume = 0.6f;

static float attackMs = 10.0f;
static float decayMs = 150.0f;
static float sustainLevel = 0.7f;
static float releaseMs = 250.0f;

static float driveAmt = 0.0f;
static float subLevel = 0.0f;
static float detuneCents = 0.0f;
static float glideMs = 0.0f;

static float delayMs = 220.0f;
static float delayFb = 0.35f;
static float delayMix = 0.0f;

static float lfoRate = 5.0f;
static float lfoDepth = 0.0f;
static uint8_t lfoTarget = LFO_OFF;

static bool crushOn = false;
static uint8_t crushPreset = 1;  // "8 BIT": e' quello che la gente cerca

static uint8_t arpMode = ARP_UP;
static uint8_t chordMode = 0;
static uint8_t enc4Target = E4_BPM;

// Posizioni normalizzate 0..1 dei parametri a mappatura esponenziale: gli
// encoder sono incrementali, quindi lo "stato" del controllo vive qui e non
// nella manopola.
static float cutoffPos = 0.0f;
static float attackPos = 0.0f;
static float decayPos = 0.0f;
static float releasePos = 0.0f;
static float delayPos = 0.5f;
static float lfoRatePos = 0.5f;
static float glidePos = 0.0f;

// Passo fine, uno per encoder (il click dell'albero lo commuta).
static bool encFine[4] = {false, false, false, false};

static bool adsrEditMode = false;
static bool holdActive = false;
static bool arpActive = false;
static bool polyMode = false;

static int8_t latchedNote = -1;
static bool latchedChord[NOTE_COUNT] = {false};
static int lastTarget = -1;
static bool lastWasLive = false;

// Specchio di cio' che il motore sta suonando, per mandargli solo i cambiamenti
// invece di ripetere lo stesso comando ad ogni giro di loop.
static bool voiceSounding[MAX_VOICES] = {false};
static float voiceFreq[MAX_VOICES] = {0.0f};

static uint8_t arpIndex = 0;
static int8_t arpDir = 1;
static uint32_t arpLastStep = 0;
static bool prevAnyHeld = false;

static uint32_t lastDisplayAt = 0;
static uint32_t lastLightAt = 0;
static uint32_t clearedAt = 0;
static uint8_t settingsCursor = 0;
static bool settingsEditing = false;

// Messaggio breve in sovrimpressione ("8 BIT ON", "ARP SU/GIU'"...): dice cosa
// e' appena cambiato senza costringere a cercare la schermata giusta.
static const char *toastText = nullptr;
static uint32_t toastAt = 0;

static void toast(const char *text) {
    toastText = text;
    toastAt = millis();
}

// --------------------------------------------------------------- utilities
static void applyOctave(int8_t oct) {
    if (oct < OCTAVE_MIN) oct = OCTAVE_MIN;
    if (oct > OCTAVE_MAX) oct = OCTAVE_MAX;
    octave = oct;
}

static inline float expMap(float p, float minVal, float ratio) {
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return minVal * powf(ratio, p);
}

static inline float expMapInv(float value, float minVal, float ratio) {
    if (value <= minVal) return 0.0f;
    return logf(value / minVal) / logf(ratio);
}

static inline float clamp01(float v) { return (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Frequenza del tasto `note` (0..12) nell'ottava `oct`, passando per la scala e
// la tonica scelte nel menu.
static float noteFreqAt(int note, int8_t oct) {
    if (oct < OCTAVE_MIN) oct = OCTAVE_MIN;
    if (oct > OCTAVE_MAX) oct = OCTAVE_MAX;
    const int semi = Settings::scaleSemitone(setIndex[SETTING_SCALE], note) +
                     (int)Settings::clampIndex(SETTING_ROOT, setIndex[SETTING_ROOT]);
    return BASE_FREQ * OCTAVE_MUL[oct + 2] * exp2f((float)semi / 12.0f);
}

// Come sopra, ma trasposta di `semitones`: serve agli accordi.
static float noteFreqShifted(int note, int8_t oct, int semitones) {
    return noteFreqAt(note, oct) * exp2f((float)semitones / 12.0f);
}

// Fotografia dei parametri da mandare in NVS.
static Storage::SynthState snapshotState() {
    Storage::SynthState s = {};
    s.waveform = waveform;
    s.octave = octave;
    s.bpm = (uint16_t)Sequencer::bpm();
    s.poly = polyMode;
    s.cutoffHz = cutoffHz;
    s.volume = volume;
    s.attackMs = attackMs;
    s.decayMs = decayMs;
    s.sustain = sustainLevel;
    s.releaseMs = releaseMs;
    s.stepVol = setIndex[SETTING_VOL];
    s.stepCutoff = setIndex[SETTING_CUTOFF];
    s.stepAdsr = setIndex[SETTING_ADSR];
    s.stepFine = setIndex[SETTING_FINE];
    s.scaleRev = STORAGE_SCALE_REV;

    s.resonance = resonance;
    s.drive = driveAmt;
    s.subLevel = subLevel;
    s.detuneCents = detuneCents;
    s.glideMs = glideMs;
    s.delayMs = delayMs;
    s.delayFb = delayFb;
    s.delayMix = delayMix;
    s.lfoRate = lfoRate;
    s.lfoDepth = lfoDepth;
    s.lfoTarget = lfoTarget;
    s.crushOn = crushOn ? 1 : 0;
    s.crushPreset = crushPreset;
    s.arpMode = arpMode;
    s.chordMode = chordMode;
    s.enc4Assign = enc4Target;
    s.setScale = setIndex[SETTING_SCALE];
    s.setRoot = setIndex[SETTING_ROOT];
    s.setLed = setIndex[SETTING_LED];
    s.setAudio = setIndex[SETTING_AUDIO];
    return s;
}

// Applica al motore audio tutti i parametri correnti (avvio e ricarica da NVS).
static void pushAllParams() {
    applyOctave(octave);
    AudioEngine::setWaveform(waveform);
    AudioEngine::setCutoff(cutoffHz);
    AudioEngine::setResonance(resonance);
    AudioEngine::setVolume(volume);
    AudioEngine::setAttack(attackMs);
    AudioEngine::setDecay(decayMs);
    AudioEngine::setSustain(sustainLevel);
    AudioEngine::setRelease(releaseMs);
    AudioEngine::setDrive(driveAmt);
    AudioEngine::setSubLevel(subLevel);
    AudioEngine::setDetune(detuneCents);
    AudioEngine::setGlide(glideMs);
    AudioEngine::setDelayTime(delayMs);
    AudioEngine::setDelayFeedback(delayFb);
    AudioEngine::setDelayMix(delayMix);
    AudioEngine::setLfoRate(lfoRate);
    AudioEngine::setLfoDepth(lfoDepth);
    AudioEngine::setLfoTarget(lfoTarget);
    AudioEngine::setCrush(crushOn);
    AudioEngine::setCrushBits(CRUSH_PRESETS[crushPreset].bits);
    AudioEngine::setCrushDivider(CRUSH_PRESETS[crushPreset].divider);
    AudioEngine::setPinOrder(setIndex[SETTING_AUDIO]);
}

// Un giro di encoder sul parametro comandato dal quarto encoder.
static void turnEnc4(int delta, bool fine) {
    const float s = stepFor(SETTING_ADSR, fine);
    switch (enc4Target) {
        case E4_BPM:
            Sequencer::nudgeBpm(delta);
            break;
        case E4_DELAY_MIX:
            delayMix = clamp01(delayMix + delta * s);
            AudioEngine::setDelayMix(delayMix);
            break;
        case E4_DELAY_TIME:
            delayPos = clamp01(delayPos + delta * s);
            delayMs = expMap(delayPos, DELAY_MIN_MS, DELAY_RATIO);
            AudioEngine::setDelayTime(delayMs);
            break;
        case E4_LFO_RATE:
            lfoRatePos = clamp01(lfoRatePos + delta * s);
            lfoRate = expMap(lfoRatePos, LFO_MIN_HZ, LFO_RATIO);
            AudioEngine::setLfoRate(lfoRate);
            break;
        case E4_LFO_DEPTH:
            lfoDepth = clamp01(lfoDepth + delta * s);
            AudioEngine::setLfoDepth(lfoDepth);
            // Muovere la profondita' con l'LFO spento non produrrebbe niente:
            // il primo scatto accende anche il bersaglio, che e' quello che
            // uno si aspetta girando una manopola.
            if (lfoDepth > 0.0f && lfoTarget == LFO_OFF) {
                lfoTarget = LFO_PITCH;
                AudioEngine::setLfoTarget(lfoTarget);
            }
            break;
        case E4_DRIVE:
            driveAmt = clamp01(driveAmt + delta * s);
            AudioEngine::setDrive(driveAmt);
            break;
        case E4_SUB:
            subLevel = clamp01(subLevel + delta * s);
            AudioEngine::setSubLevel(subLevel);
            break;
        case E4_DETUNE:
            detuneCents += delta * s * 50.0f;
            if (detuneCents < 0.0f) detuneCents = 0.0f;
            if (detuneCents > 50.0f) detuneCents = 50.0f;
            AudioEngine::setDetune(detuneCents);
            break;
        case E4_GLIDE:
            glidePos = clamp01(glidePos + delta * s);
            glideMs = (glidePos <= 0.0f) ? 0.0f : expMap(glidePos, GLIDE_MIN_MS, GLIDE_RATIO);
            AudioEngine::setGlide(glideMs);
            break;
        default:
            break;
    }
}

// Note tenute in ordine di *altezza*, non di pressione. "SU" deve salire anche
// se i tasti li hai premuti alla rinfusa: l'ordine di pressione serve solo al
// modo ORDINE, che e' li' apposta per chi lo vuole.
static int arpSorted[NOTE_COUNT];

static int arpBuildSorted() {
    int n = 0;
    for (int i = 0; i < NOTE_COUNT; ++i) {
        if (Input::noteIsHeld(i)) arpSorted[n++] = i;
    }
    return n;
}

// Nota all'indice `i` della sequenza dell'arpeggiator, nel modo corrente.
static int arpNoteAt(int i, int count) {
    if (count <= 0) return -1;
    if (i < 0 || i >= count) i = 0;
    if (arpMode == ARP_ORDER) return Input::heldNoteByOrder(i);
    return arpSorted[i];
}

// Prossimo passo dell'arpeggiator, secondo il modo scelto.
static int arpAdvance(int heldCount) {
    if (heldCount <= 0) return -1;
    switch (arpMode) {
        case ARP_DOWN:
            arpIndex = (uint8_t)((arpIndex == 0) ? heldCount - 1 : arpIndex - 1);
            break;
        case ARP_UPDOWN:
            if (heldCount == 1) {
                arpIndex = 0;
            } else {
                int next = (int)arpIndex + arpDir;
                if (next >= heldCount) {
                    arpDir = -1;
                    next = heldCount - 2;
                } else if (next < 0) {
                    arpDir = 1;
                    next = 1;
                }
                arpIndex = (uint8_t)next;
            }
            break;
        case ARP_RANDOM:
            arpIndex = (uint8_t)random(heldCount);
            break;
        case ARP_ORDER:
        case ARP_UP:
        default:
            arpIndex = (uint8_t)((arpIndex + 1) % heldCount);
            break;
    }
    if (arpIndex >= heldCount) arpIndex = 0;
    return arpNoteAt(arpIndex, heldCount);
}

// Passo dell'arpeggiator: un sedicesimo del tempo corrente, cosi' sta in riga
// con il sequencer invece di andare per conto suo.
static uint32_t arpStepMs() {
    const int d = Sequencer::stepDurationMs();
    return (d < 30) ? 30 : (uint32_t)d;
}

// ------------------------------------------------------------------ setup
void setup() {
    Serial.begin(115200);

    for (int i = 0; i < SETTING_COUNT; ++i) setIndex[i] = Settings::ENTRIES[i].byDefault;

    StatusLed::begin();  // per primo: spegne il LED RGB prima di ogni altra cosa
    Input::begin();
    Keylight::begin();
    Sequencer::begin();
    Storage::begin();
    AudioEngine::begin();

    // Pattern e parametri dell'ultima accensione, se ci sono.
    Storage::SynthState saved;
    if (Storage::load(saved)) {
        waveform = (saved.waveform < WAVE_COUNT) ? saved.waveform : WAVE_SAW;
        octave = saved.octave;
        cutoffHz = saved.cutoffHz;
        volume = saved.volume;
        attackMs = saved.attackMs;
        decayMs = saved.decayMs;
        sustainLevel = saved.sustain;
        releaseMs = saved.releaseMs;
        polyMode = saved.poly;
        setIndex[SETTING_VOL] = Settings::clampIndex(SETTING_VOL, saved.stepVol);
        setIndex[SETTING_CUTOFF] = Settings::clampIndex(SETTING_CUTOFF, saved.stepCutoff);
        setIndex[SETTING_ADSR] = Settings::clampIndex(SETTING_ADSR, saved.stepAdsr);
        setIndex[SETTING_FINE] = Settings::clampIndex(SETTING_FINE, saved.stepFine);

        // Fino alla 1.11.0 la scala dei giri era scritta al contrario, e con lei
        // gli indici finiti in NVS. Rovesciarli qui vuol dire che chi aggiorna
        // ritrova la sensibilita' che aveva scelto, non la sua immagine
        // speculare.
        if (saved.scaleRev != STORAGE_SCALE_REV) {
            const uint8_t rovesciare[3] = {SETTING_VOL, SETTING_CUTOFF, SETTING_ADSR};
            for (uint8_t i = 0; i < 3; ++i) {
                const uint8_t which = rovesciare[i];
                setIndex[which] = Settings::ENTRIES[which].count - 1 - setIndex[which];
            }
            Storage::markDirty();
            Serial.println(F("Sensibilita' convertita alla scala crescente."));
        }

        // I campi della 2.0.0 restano a zero se il blob e' della 1.x: zero e'
        // gia' il valore giusto per quasi tutti (effetti spenti), e per i pochi
        // in cui non lo sarebbe si tiene il default.
        resonance = saved.resonance;
        driveAmt = saved.drive;
        subLevel = saved.subLevel;
        detuneCents = saved.detuneCents;
        glideMs = saved.glideMs;
        if (saved.delayMs > 0.0f) delayMs = saved.delayMs;
        if (saved.delayFb > 0.0f) delayFb = saved.delayFb;
        delayMix = saved.delayMix;
        if (saved.lfoRate > 0.0f) lfoRate = saved.lfoRate;
        lfoDepth = saved.lfoDepth;
        lfoTarget = (saved.lfoTarget < LFO_TARGET_COUNT) ? saved.lfoTarget : LFO_OFF;
        crushOn = saved.crushOn != 0;
        crushPreset = (saved.crushPreset < CRUSH_COUNT) ? saved.crushPreset : 1;
        arpMode = (saved.arpMode < ARP_MODE_COUNT) ? saved.arpMode : ARP_UP;
        chordMode = (saved.chordMode < CHORD_COUNT) ? saved.chordMode : 0;
        enc4Target = (saved.enc4Assign < E4_COUNT) ? saved.enc4Assign : E4_BPM;
        setIndex[SETTING_SCALE] = Settings::clampIndex(SETTING_SCALE, saved.setScale);
        setIndex[SETTING_ROOT] = Settings::clampIndex(SETTING_ROOT, saved.setRoot);
        setIndex[SETTING_LED] = Settings::clampIndex(SETTING_LED, saved.setLed);
        setIndex[SETTING_AUDIO] = Settings::clampIndex(SETTING_AUDIO, saved.setAudio);

        Sequencer::setBpm(saved.bpm);
        Serial.println(F("Stato ripristinato da NVS."));
    }

    uint8_t ledMap[KEYLED_COUNT];
    if (Storage::loadLedMap(ledMap, sizeof(ledMap))) Keylight::setMap(ledMap);

    // Posizioni iniziali degli encoder, ricavate dai valori correnti.
    cutoffPos = expMapInv(cutoffHz, CUTOFF_MIN_HZ, CUTOFF_RATIO);
    attackPos = expMapInv(attackMs, ATTACK_MIN_MS, ATTACK_RATIO);
    decayPos = expMapInv(decayMs, DECAY_MIN_MS, DECAY_RATIO);
    releasePos = expMapInv(releaseMs, RELEASE_MIN_MS, RELEASE_RATIO);
    delayPos = expMapInv(delayMs, DELAY_MIN_MS, DELAY_RATIO);
    lfoRatePos = expMapInv(lfoRate, LFO_MIN_HZ, LFO_RATIO);
    glidePos = (glideMs > 0.0f) ? expMapInv(glideMs, GLIDE_MIN_MS, GLIDE_RATIO) : 0.0f;

    pushAllParams();

    Display::begin();

    Serial.print(F("ArcadeVox "));
    Serial.print(F(FW_VERSION));
    Serial.println(F(" pronto."));
    if (!Input::expanderOk()) {
        Serial.println(F("ATTENZIONE: l'espansore MCP23017 non risponde sul bus I2C."));
    }
}

// ------------------------------------------------------------------- loop
void loop() {
    const uint32_t now = millis();

    Input::update();

    // ------------------------------------------------------ modalita' rete
    // Esclusiva: il motore audio e' spento e il core 0 e' tutto dello stack
    // WiFi. Si esce solo riavviando.
    if (NetPortal::active()) {
        NetPortal::update();
        if (Input::fnShortPress(FN_PLAY) || Input::fnLongPress(FN_PLAY)) {
            Serial.println(F("Uscita dalla modalita' NETWORK: riavvio."));
            ESP.restart();
        }
        if (now - lastDisplayAt >= NETWORK_REFRESH_MS) {
            lastDisplayAt = now;
            Display::updateNetwork();
        }
        return;
    }

    StatusLed::update(now);

    // ------------------------------------------- apprendimento delle luci
    // Finche' dura, la tastiera non suona: ogni pressione serve a dire "questo
    // e' il tasto che si e' acceso". Si esce da soli dopo venti tasti, o con
    // FN7 se ci si e' persi.
    if (Keylight::learning()) {
        static const uint8_t NOTE_SLOT[NOTE_COUNT] = MATRIX_NOTE_SLOTS;
        static const uint8_t FN_SLOT[FN_COUNT] = MATRIX_FN_SLOTS;
        int pressed;
        bool done = false;
        while ((pressed = Input::consumeNoteOn()) >= 0) {
            Keylight::learnAssign(NOTE_SLOT[pressed]);
            done = true;
        }
        for (int f = 0; f < FN_COUNT; ++f) {
            if (Input::fnShortPress(f)) {
                Keylight::learnAssign(FN_SLOT[f]);
                done = true;
            }
            if (Input::fnLongPress(f) && f == FN_SCREEN) {
                Keylight::cancelLearn();
                toast("MAPPA ANNULLATA");
            }
        }
        if (done && !Keylight::learning()) {
            Storage::saveLedMap(Keylight::map(), KEYLED_COUNT);
            toast("LUCI IMPARATE");
        }
        if (now - lastLightAt >= LIGHT_INTERVAL_MS) {
            lastLightAt = now;
            LightView lv = {};
            lv.brightness = setIndex[SETTING_LED];
            Keylight::update(now, lv);
        }
        if (now - lastDisplayAt >= DISPLAY_INTERVAL_MS) {
            lastDisplayAt = now;
            SynthView view = {};
            view.ledLearn = true;
            view.ledLearnIndex = Keylight::learnIndex();
            Display::update(view);
        }
        return;
    }

    // ---------------------------------------------------- tasti funzione
    // FN7: schermate fuori dal menu, voci dentro; tenuto premuto entra ed esce
    // dal menu impostazioni.
    if (Input::fnLongPress(FN_SCREEN)) {
        if (settingsEditing) {
            settingsEditing = false;
        } else if (Display::currentScreen() == SCREEN_SETTINGS) {
            settingsEditing = true;
            settingsCursor = 0;
        }
    }
    if (Input::fnShortPress(FN_SCREEN)) {
        if (!settingsEditing) {
            Display::nextScreen();
        } else if (Settings::isAction(settingsCursor)) {
            // Sulle voci d'azione la pressione breve non scorre: esegue.
            if (settingsCursor == SETTING_NET) {
                Storage::flush(snapshotState());  // niente va perso spegnendo l'audio
                Keylight::allOff();
                NetPortal::begin();
                return;
            }
            if (settingsCursor == SETTING_LEDLEARN) {
                settingsEditing = false;
                AudioEngine::allNotesOff();
                for (int i = 0; i < MAX_VOICES; ++i) voiceSounding[i] = false;
                Keylight::startLearn();
                return;
            }
        } else {
            settingsCursor = (uint8_t)((settingsCursor + 1) % SETTING_COUNT);
        }
    }

    if (Input::fnLongPress(FN_REC)) {
        // STEP EDIT e ADSR EDIT contendono gli stessi comandi: uno esclude l'altro.
        adsrEditMode = false;
        Sequencer::toggleEditing();
    }
    if (Input::fnShortPress(FN_REC)) Sequencer::toggleRecord();
    if (Input::fnShortPress(FN_PLAY)) Sequencer::togglePlay();

    if (Input::fnLongPress(FN_PLAY)) {
        Sequencer::clearAll();
        Storage::markDirty();
        clearedAt = now;
        toast("PATTERN VUOTO");
    }

    if (Input::fnLongPress(FN_HOLD)) {
        adsrEditMode = !adsrEditMode;
        if (adsrEditMode) Sequencer::setEditing(false);
        toast(adsrEditMode ? "ADSR EDIT" : "ADSR OK");
    }

    // --- 8 BIT: il tasto che il synth aspettava ---
    if (Input::fnShortPress(FN_CRUSH)) {
        crushOn = !crushOn;
        AudioEngine::setCrush(crushOn);
        toast(crushOn ? CRUSH_PRESETS[crushPreset].label : "8 BIT OFF");
        Storage::markDirty();
    }
    if (Input::fnLongPress(FN_CRUSH)) {
        crushPreset = (uint8_t)((crushPreset + 1) % CRUSH_COUNT);
        AudioEngine::setCrushBits(CRUSH_PRESETS[crushPreset].bits);
        AudioEngine::setCrushDivider(CRUSH_PRESETS[crushPreset].divider);
        // Cambiare profondita' accende anche l'effetto: nessuno gira quella
        // manopola per sentire il silenzio.
        crushOn = true;
        AudioEngine::setCrush(true);
        toast(CRUSH_PRESETS[crushPreset].label);
        Storage::markDirty();
    }

    // Stato delle modalita' *dopo* i pulsanti che possono cambiarle.
    const bool stepEdit = Sequencer::editing();
    const bool recording = (Sequencer::mode() == Sequencer::SEQ_RECORDING);

    if (Input::fnShortPress(FN_HOLD)) {
        if (recording) {
            // Durante il record HOLD e' il tasto di cancellazione: il latch non
            // deve scattare al rilascio.
        } else if (stepEdit) {
            Sequencer::writeAtCursor(SEQ_REST, 0);  // svuota lo step e avanza
            Storage::markDirty();
        } else {
            holdActive = !holdActive;
            if (holdActive) {
                for (int n = 0; n < NOTE_COUNT; ++n) latchedChord[n] = Input::noteIsHeld(n);
            } else {
                latchedNote = -1;
                for (int n = 0; n < NOTE_COUNT; ++n) latchedChord[n] = false;
            }
            toast(holdActive ? "HOLD ON" : "HOLD OFF");
        }
    }

    if (Input::fnShortPress(FN_POLY)) {
        polyMode = !polyMode;
        // Le voci in corso appartengono all'altra modalita': si azzera tutto,
        // altrimenti resterebbero note appese senza nessuno che le rilasci.
        AudioEngine::allNotesOff();
        for (int i = 0; i < MAX_VOICES; ++i) voiceSounding[i] = false;
        for (int n = 0; n < NOTE_COUNT; ++n) latchedChord[n] = false;
        latchedNote = -1;
        lastTarget = -1;
        toast(polyMode ? "POLIFONICO" : "MONO");
        Storage::markDirty();
    }
    if (Input::fnLongPress(FN_POLY)) {
        chordMode = (uint8_t)((chordMode + 1) % CHORD_COUNT);
        AudioEngine::voiceOff(VOICE_CHORD1);
        AudioEngine::voiceOff(VOICE_CHORD2);
        voiceSounding[VOICE_CHORD1] = false;
        voiceSounding[VOICE_CHORD2] = false;
        toast(CHORDS[chordMode].label);
        Storage::markDirty();
    }

    if (Input::fnShortPress(FN_ARP)) {
        if (stepEdit) {
            Sequencer::writeAtCursor(SEQ_TIE, 0);  // legato: tiene la nota precedente
            Storage::markDirty();
        } else {
            arpActive = !arpActive;
            toast(arpActive ? "ARP ON" : "ARP OFF");
        }
    }
    if (Input::fnLongPress(FN_ARP)) {
        arpMode = (uint8_t)((arpMode + 1) % ARP_MODE_COUNT);
        arpDir = 1;
        arpActive = true;
        toast(ARP_NAMES[arpMode]);
        Storage::markDirty();
    }

    // --------------------------------------------------------- joystick
    if (!adsrEditMode) {
        if (Input::joyUp()) {
            applyOctave(octave + 1);
            Storage::markDirty();
        }
        if (Input::joyDown()) {
            applyOctave(octave - 1);
            Storage::markDirty();
        }
        if (stepEdit) {
            if (Input::joyLeft()) Sequencer::moveCursor(-1);
            if (Input::joyRight()) Sequencer::moveCursor(1);
        } else {
            if (Input::joyLeft()) {
                waveform = (uint8_t)((waveform + WAVE_COUNT - 1) % WAVE_COUNT);
                AudioEngine::setWaveform(waveform);
                Storage::markDirty();
            }
            if (Input::joyRight()) {
                waveform = (uint8_t)((waveform + 1) % WAVE_COUNT);
                AudioEngine::setWaveform(waveform);
                Storage::markDirty();
            }
        }
    } else {
        // In ADSR EDIT i quattro parametri stanno sui quattro encoder: al
        // joystick resta il bersaglio dell'LFO, che e' l'altra cosa che si
        // regola guardando la stessa schermata.
        if (Input::joyRight()) {
            lfoTarget = (uint8_t)((lfoTarget + 1) % LFO_TARGET_COUNT);
            AudioEngine::setLfoTarget(lfoTarget);
            toast(LFO_TARGET_NAMES[lfoTarget]);
            Storage::markDirty();
        }
        if (Input::joyLeft()) {
            lfoTarget = (uint8_t)((lfoTarget + LFO_TARGET_COUNT - 1) % LFO_TARGET_COUNT);
            AudioEngine::setLfoTarget(lfoTarget);
            toast(LFO_TARGET_NAMES[lfoTarget]);
            Storage::markDirty();
        }
        if (Input::joyUp()) {
            applyOctave(octave + 1);
            Storage::markDirty();
        }
        if (Input::joyDown()) {
            applyOctave(octave - 1);
            Storage::markDirty();
        }
    }

    // -------------------------------------------------------- encoder
    // I primi tre commutano il passo fine col click; il quarto, che ha un
    // parametro diverso ogni volta, col click cambia proprio parametro.
    for (int e = 0; e < 3; ++e) {
        if (Input::encClick(e)) {
            encFine[e] = !encFine[e];
            toast(encFine[e] ? "PASSO FINE" : "PASSO NORMALE");
        }
    }
    if (Input::encClick(3)) {
        enc4Target = (uint8_t)((enc4Target + 1) % E4_COUNT);
        toast(E4_NAMES[enc4Target]);
        Storage::markDirty();
    }

    const int enc[4] = {Input::encDelta(0), Input::encDelta(1), Input::encDelta(2),
                        Input::encDelta(3)};

    if (settingsEditing) {
        // Nel menu gli encoder regolano se stessi: il primo scorre le voci, il
        // secondo cambia il valore. Cutoff e volume restano fermi finche' non
        // esci, ed e' quello che serve mentre stai tarando la loro sensibilita'.
        if (enc[0] != 0) {
            int c = (int)settingsCursor + enc[0];
            if (c < 0) c = 0;
            if (c > SETTING_COUNT - 1) c = SETTING_COUNT - 1;
            settingsCursor = (uint8_t)c;
        }
        const uint8_t which = settingsCursor;
        if (enc[1] != 0 && !Settings::isAction(which)) {
            int idx = (int)setIndex[which] + enc[1];
            if (idx < 0) idx = 0;
            if (idx > Settings::ENTRIES[which].count - 1) idx = Settings::ENTRIES[which].count - 1;
            setIndex[which] = (uint8_t)idx;
            // Due voci hanno effetto immediato: l'uscita audio, che va provata
            // ad orecchio, e le luci, che vanno viste.
            if (which == SETTING_AUDIO) AudioEngine::setPinOrder(setIndex[which]);
            Storage::markDirty();
        }
    } else if (adsrEditMode) {
        // Quattro encoder, quattro parametri: finalmente uno per manopola.
        if (enc[0] != 0) {
            attackPos = clamp01(attackPos + enc[0] * stepFor(SETTING_ADSR, encFine[0]));
            attackMs = expMap(attackPos, ATTACK_MIN_MS, ATTACK_RATIO);
            AudioEngine::setAttack(attackMs);
            Storage::markDirty();
        }
        if (enc[1] != 0) {
            decayPos = clamp01(decayPos + enc[1] * stepFor(SETTING_ADSR, encFine[1]));
            decayMs = expMap(decayPos, DECAY_MIN_MS, DECAY_RATIO);
            AudioEngine::setDecay(decayMs);
            Storage::markDirty();
        }
        if (enc[2] != 0) {
            sustainLevel = clamp01(sustainLevel + enc[2] * stepFor(SETTING_ADSR, encFine[2]));
            AudioEngine::setSustain(sustainLevel);
            Storage::markDirty();
        }
        if (enc[3] != 0) {
            releasePos = clamp01(releasePos + enc[3] * stepFor(SETTING_ADSR, encFine[3]));
            releaseMs = expMap(releasePos, RELEASE_MIN_MS, RELEASE_RATIO);
            AudioEngine::setRelease(releaseMs);
            Storage::markDirty();
        }
    } else {
        if (enc[0] != 0) {
            if (stepEdit) {
                // Scorrere 16 step col joystick e' lento: qui il primo encoder
                // fa da rotella.
                Sequencer::moveCursor(enc[0]);
            } else {
                cutoffPos = clamp01(cutoffPos + enc[0] * stepFor(SETTING_CUTOFF, encFine[0]));
                cutoffHz = expMap(cutoffPos, CUTOFF_MIN_HZ, CUTOFF_RATIO);
                AudioEngine::setCutoff(cutoffHz);
                Storage::markDirty();
            }
        }
        if (enc[1] != 0) {
            resonance = clamp01(resonance + enc[1] * stepFor(SETTING_CUTOFF, encFine[1]));
            AudioEngine::setResonance(resonance);
            Storage::markDirty();
        }
        if (enc[2] != 0) {
            volume = clamp01(volume + enc[2] * stepFor(SETTING_VOL, encFine[2]));
            AudioEngine::setVolume(volume);
            Storage::markDirty();
        }
        if (enc[3] != 0) {
            turnEnc4(enc[3], encFine[3]);
            Storage::markDirty();
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
        const int sortedCount = arpBuildSorted();
        if (!prevAnyHeld) {
            arpIndex = (arpMode == ARP_DOWN) ? (uint8_t)(sortedCount - 1) : 0;
            arpDir = 1;
            arpLastStep = now;
            arpRetrigger = true;
            liveNote = arpNoteAt(arpIndex, sortedCount);
        } else if (now - arpLastStep >= arpStepMs()) {
            arpLastStep = now;
            liveNote = arpAdvance(sortedCount);
            arpRetrigger = true;
        } else {
            liveNote = arpNoteAt(arpIndex % sortedCount, sortedCount);
        }
        if (liveNote < 0) liveNote = arpNoteAt(0, sortedCount);
    } else if (anyHeld) {
        liveNote = rawNote;  // last-note-priority
    } else if (holdActive) {
        liveNote = latchedNote;  // nota tenuta anche a tasti rilasciati
    }
    prevAnyHeld = anyHeld;

    // -------------------------------- scrittura sul pattern (edit e record)
    int pressed;
    while ((pressed = Input::consumeNoteOn()) >= 0) {
        if (stepEdit) {
            Sequencer::writeAtCursor(pressed, octave);
            Storage::markDirty();
        } else if (!arpActive) {
            Sequencer::noteEvent(now, pressed, octave);
            Storage::markDirty();
        }
    }
    if (arpActive && arpRetrigger && liveNote >= 0 && !stepEdit) {
        Sequencer::noteEvent(now, liveNote, octave);
        Storage::markDirty();
    }

    // ------------------------------------------------------- sequencer
    Sequencer::update(now, recording && Input::fnIsDown(FN_HOLD));

    const bool seqTrigger = Sequencer::consumeTrigger();
    const int seqNote = Sequencer::outputNote();

    // ------------------------------------------------------------- voci
    bool wantVoice[MAX_VOICES] = {false};
    float wantFreq[MAX_VOICES] = {0.0f};
    bool wantRetrig[MAX_VOICES] = {false};

    if (!polyMode) {
        // MONO: una voce sola, la nota dal vivo ha priorita' sulla sequenza.
        const bool useLive = (liveNote >= 0);
        const int target = useLive ? liveNote : seqNote;
        if (target >= 0) {
            const int8_t oct = useLive ? octave : Sequencer::outputOctave();
            const bool retrig = (target != lastTarget) || (useLive != lastWasLive) ||
                                (useLive ? arpRetrigger : seqTrigger);
            wantVoice[VOICE_MONO] = true;
            wantFreq[VOICE_MONO] = noteFreqAt(target, oct);
            // Cambiando sorgente la nota riparte: il passaggio fra tastiera e
            // sequenza deve sentirsi come un attacco, non come una scivolata.
            wantRetrig[VOICE_MONO] = retrig;

            // Modalita' accordo: le due note aggiunte seguono la principale.
            const ChordMode &ch = CHORDS[chordMode];
            if (ch.a != 0) {
                wantVoice[VOICE_CHORD1] = true;
                wantFreq[VOICE_CHORD1] = noteFreqShifted(target, oct, ch.a);
                wantRetrig[VOICE_CHORD1] = retrig;
            }
            if (ch.b != 0) {
                wantVoice[VOICE_CHORD2] = true;
                wantFreq[VOICE_CHORD2] = noteFreqShifted(target, oct, ch.b);
                wantRetrig[VOICE_CHORD2] = retrig;
            }
        }
        lastTarget = target;
        lastWasLive = useLive;
    } else {
        // POLI: ogni tasto ha la sua voce e suonano tutti insieme.
        const bool arpRunning = arpActive && anyHeld;
        for (int n = 0; n < NOTE_COUNT; ++n) {
            const bool held = Input::noteIsHeld(n);
            // Con HOLD inserito il tasto premuto entra nell'accordo tenuto e ci
            // resta: si costruisce un accordo una nota alla volta.
            if (holdActive && held) latchedChord[n] = true;

            bool sound = held || (holdActive && latchedChord[n]);
            if (arpRunning) {
                // L'arpeggiator resta monofonico anche in poli: e' il suo senso.
                sound = (n == liveNote);
                wantRetrig[n] = sound && arpRetrigger;
            }
            wantVoice[n] = sound;
            wantFreq[n] = noteFreqAt(n, octave);
        }
        // La sequenza non viene zittita dalle dita: ci suoni sopra.
        if (seqNote >= 0) {
            wantVoice[VOICE_SEQ] = true;
            wantFreq[VOICE_SEQ] = noteFreqAt(seqNote, Sequencer::outputOctave());
            wantRetrig[VOICE_SEQ] = seqTrigger;
        }
        lastTarget = -1;
        lastWasLive = false;
    }

    // Lo specchio si aggiorna solo se l'evento e' stato davvero accodato: se la
    // coda fosse piena, il giro successivo ritenta invece di dare per scontato
    // un comando mai arrivato.
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (!wantVoice[i]) {
            if (voiceSounding[i] && AudioEngine::voiceOff((uint8_t)i)) voiceSounding[i] = false;
            continue;
        }
        if (!voiceSounding[i] || wantRetrig[i]) {
            if (AudioEngine::voiceOn((uint8_t)i, wantFreq[i])) {
                voiceSounding[i] = true;
                voiceFreq[i] = wantFreq[i];
            }
        } else if (fabsf(wantFreq[i] - voiceFreq[i]) > 0.01f) {
            // cambio ottava mentre la nota suona: reintono senza ritriggerare
            if (AudioEngine::voiceRetune((uint8_t)i, wantFreq[i])) voiceFreq[i] = wantFreq[i];
        }
    }

    // --------------------------------------------------------- salvataggio
    if (Storage::savePending(now)) Storage::flush(snapshotState());

    // --------------------------------------------------------- luci
    if (now - lastLightAt >= LIGHT_INTERVAL_MS) {
        lastLightAt = now;
        LightView lv = {};
        for (int n = 0; n < NOTE_COUNT; ++n) {
            if (Input::noteIsHeld(n)) lv.noteHeld |= (uint16_t)(1u << n);
        }
        if (!polyMode) {
            if (liveNote >= 0) lv.noteSound |= (uint16_t)(1u << liveNote);
        } else {
            for (int n = 0; n < NOTE_COUNT; ++n) {
                if (voiceSounding[n]) lv.noteSound |= (uint16_t)(1u << n);
            }
        }
        lv.seqNote = (int8_t)seqNote;
        lv.seqRunning = (Sequencer::mode() == Sequencer::SEQ_PLAYING) || recording;
        if (arpActive) lv.fnActive |= 1u << FN_ARP;
        if (crushOn) lv.fnActive |= 1u << FN_CRUSH;
        if (recording) lv.fnPending |= 1u << FN_REC;
        if (Sequencer::mode() == Sequencer::SEQ_PLAYING) lv.fnActive |= 1u << FN_PLAY;
        if (Sequencer::mode() == Sequencer::SEQ_COUNTIN) lv.fnPending |= 1u << FN_PLAY;
        if (holdActive) lv.fnActive |= 1u << FN_HOLD;
        if (adsrEditMode) lv.fnPending |= 1u << FN_HOLD;
        if (polyMode) lv.fnActive |= 1u << FN_POLY;
        if (settingsEditing) lv.fnPending |= 1u << FN_SCREEN;
        lv.crush = crushOn;
        lv.brightness = setIndex[SETTING_LED];
        lv.scaleRoot = (setIndex[SETTING_SCALE] == 0) ? -1 : (int8_t)setIndex[SETTING_ROOT];
        for (int n = 0; n < NOTE_COUNT; ++n) {
            if (Settings::scaleIsRoot(setIndex[SETTING_SCALE], n)) {
                lv.scaleMask |= (uint16_t)(1u << n);
            }
        }
        Keylight::update(now, lv);
    }

    // --------------------------------------------------------- display
    if (now - lastDisplayAt >= DISPLAY_INTERVAL_MS) {
        lastDisplayAt = now;

        SynthView view;
        view.waveform = waveform;
        view.octave = octave;
        view.cutoffHz = cutoffHz;
        view.resonance = resonance;
        view.volume = volume;
        view.adsrEdit = adsrEditMode;
        view.attackMs = attackMs;
        view.decayMs = decayMs;
        view.sustain = sustainLevel;
        view.releaseMs = releaseMs;
        view.seqMode = (uint8_t)Sequencer::mode();
        view.seqStep = (uint8_t)Sequencer::currentStep();
        view.seqCursor = (uint8_t)Sequencer::cursor();
        view.seqEditing = stepEdit;
        view.countIn = (uint8_t)Sequencer::countInBeats();
        view.seqRev = Sequencer::revision();
        view.bpm = (uint16_t)Sequencer::bpm();
        view.hold = holdActive;
        view.arp = arpActive;
        view.arpMode = arpMode;
        view.arpName = ARP_NAMES[arpMode];
        view.poly = polyMode;
        view.chordName = CHORDS[chordMode].label;
        view.voices = AudioEngine::activeVoices();

        view.crush = crushOn;
        view.crushName = CRUSH_PRESETS[crushPreset].label;
        view.delayMix = delayMix;
        view.delayMs = delayMs;
        view.lfoDepth = lfoDepth;
        view.lfoRate = lfoRate;
        view.lfoTargetName = LFO_TARGET_NAMES[lfoTarget];
        view.drive = driveAmt;
        view.subLevel = subLevel;
        view.detuneCents = detuneCents;
        view.glideMs = glideMs;
        view.enc4Name = E4_NAMES[enc4Target];
        view.enc4Index = enc4Target;

        view.scaleName = Settings::valueLabel(SETTING_SCALE, setIndex[SETTING_SCALE]);
        view.rootName = Settings::rootName(setIndex[SETTING_ROOT]);
        view.expanderOk = Input::expanderOk();

        for (int i = 0; i < SETTING_COUNT; ++i) view.setIndex[i] = setIndex[i];
        view.setCursor = settingsCursor;
        view.setEditing = settingsEditing;
        view.clearedAgo = (clearedAt == 0) ? 0 : (now - clearedAt);
        view.ledLearn = false;
        view.ledLearnIndex = 0;

        // Il messaggio in sovrimpressione dura un secondo e mezzo: abbastanza
        // per leggerlo, non tanto da coprire la schermata mentre suoni.
        view.toast = (toastText && (now - toastAt) < 1500) ? toastText : nullptr;

        Display::update(view);
    }
}
