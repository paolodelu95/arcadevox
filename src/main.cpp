// ArcadeVox — sintetizzatore mono/polifonico su ESP32-S3 N16R8.
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
#include "net_portal.h"
#include "pinout.h"
#include "sequencer.h"
#include "settings.h"
#include "status_led.h"
#include "storage.h"
#include "version.h"

// ---------------------------------------------------------------- costanti
// DO..SI in quarta ottava; l'ottava corrente moltiplica per 2^n. Il DO superiore
// non ha piu' un tasto suo (e' diventato il selettore MONO/POLI): si raggiunge
// con il joystick dell'ottava.
static const float NOTE_FREQ[NOTE_COUNT] = {
    261.63f,  // DO
    293.66f,  // RE
    329.63f,  // MI
    349.23f,  // FA
    392.00f,  // SOL
    440.00f,  // LA
    493.88f   // SI
};

static const uint32_t ARP_STEP_MS = 150;
static const uint32_t DISPLAY_INTERVAL_MS = 33;  // ~30 fps
// In modalita' rete non c'e' niente di veloce da mostrare, e ridisegnare un QR
// costa: bastano 4 giri al secondo.
static const uint32_t NETWORK_REFRESH_MS = 250;

static const int8_t OCTAVE_MIN = -2;
static const int8_t OCTAVE_MAX = 2;

// Moltiplicatori 2^oct per le ottave ammesse, indicizzati da oct+2: il
// sequencer li usa ad ogni step e powf() non serve.
static const float OCTAVE_MUL[5] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};

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

// Quanto muove uno scatto di encoder non e' piu' una costante: lo decide la
// schermata SETTINGS, perche' e' una questione di gusto e cambia da mano a mano.
// I valori esponenziali si muovono su una posizione normalizzata 0..1.
static uint8_t setIndex[SETTING_COUNT];

// Passo della voce `which`, diviso per il passo fine se il click e' inserito.
static float stepFor(uint8_t which, bool fine) {
    const float s = Settings::step(which, setIndex[which]);
    return fine ? (s / Settings::fineDivider(setIndex[SETTING_FINE])) : s;
}

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

static bool polyMode = false;

static int8_t latchedNote = -1;   // MONO: ultima nota suonata, tenuta viva da HOLD
static bool latchedChord[NOTE_COUNT] = {false};  // POLI: accordo tenuto da HOLD
static int lastTarget = -1;       // MONO: nota attualmente inviata al motore audio
static bool lastWasLive = false;  // MONO: sorgente dell'ultima nota, dal vivo o sequenza

// Specchio di cio' che il motore sta suonando, per mandargli solo i cambiamenti
// invece di ripetere lo stesso comando ad ogni giro di loop.
static bool voiceSounding[MAX_VOICES] = {false};
static float voiceFreq[MAX_VOICES] = {0.0f};

static uint8_t arpIndex = 0;
static uint32_t arpLastStep = 0;
static bool prevAnyHeld = false;

static uint32_t lastDisplayAt = 0;
// Istante dell'ultima cancellazione del pattern, per la conferma a schermo.
static uint32_t clearedAt = 0;
static uint8_t settingsCursor = 0;
// Dentro al menu impostazioni: cambia il significato di piu' di un comando.
static bool settingsEditing = false;

// --------------------------------------------------------------- utilities
static void applyOctave(int8_t oct) {
    if (oct < OCTAVE_MIN) oct = OCTAVE_MIN;
    if (oct > OCTAVE_MAX) oct = OCTAVE_MAX;
    octave = oct;
    octaveMul = OCTAVE_MUL[octave + 2];
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

// Frequenza di una nota della scala in una data ottava.
static inline float noteFreqAt(int note, int8_t oct) {
    if (oct < OCTAVE_MIN) oct = OCTAVE_MIN;
    if (oct > OCTAVE_MAX) oct = OCTAVE_MAX;
    return NOTE_FREQ[note] * OCTAVE_MUL[oct + 2];
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
    return s;
}

// Applica al motore audio tutti i parametri correnti (avvio e ricarica da NVS).
static void pushAllParams() {
    applyOctave(octave);
    AudioEngine::setWaveform(waveform);
    AudioEngine::setCutoff(cutoffHz);
    AudioEngine::setVolume(volume);
    AudioEngine::setAttack(attackMs);
    AudioEngine::setDecay(decayMs);
    AudioEngine::setSustain(sustainLevel);
    AudioEngine::setRelease(releaseMs);
}

// ------------------------------------------------------------------ setup
void setup() {
    Serial.begin(115200);

    for (int i = 0; i < SETTING_COUNT; ++i) setIndex[i] = Settings::ENTRIES[i].byDefault;

    StatusLed::begin();  // per primo: spegne il LED RGB prima di ogni altra cosa
    Input::begin();
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
        Sequencer::setBpm(saved.bpm);
        Serial.println(F("Stato ripristinato da NVS."));
    }

    // Posizioni iniziali degli encoder, ricavate dai valori correnti.
    cutoffPos = expMapInv(cutoffHz, CUTOFF_MIN_HZ, CUTOFF_RATIO);
    attackPos = expMapInv(attackMs, ATTACK_MIN_MS, ATTACK_RATIO);
    releasePos = expMapInv(releaseMs, RELEASE_MIN_MS, RELEASE_RATIO);

    pushAllParams();

    Display::begin();

    Serial.println(F("ArcadeVox pronto."));
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
        // Qui vanno bene entrambe: chi vuole uscire preme e basta, senza stare a
        // misurare quanto tiene giu' il tasto.
        if (Input::playShortPress() || Input::playLongPress()) {
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

    // ---------------------------------------------------- pulsanti funzione
    // Il pulsante "scorri display" ha tre significati, e quale valga dipende solo
    // da dove ti trovi: fuori dalle impostazioni scorre le schermate, dentro
    // sceglie la voce, e tenuto premuto entra o esce dal menu. Un tasto solo,
    // perche' sul pannello non ce ne sono di liberi.
    if (Input::displayLongPress()) {
        if (settingsEditing) {
            settingsEditing = false;
        } else if (Display::currentScreen() == SCREEN_SETTINGS) {
            settingsEditing = true;
            settingsCursor = 0;
        }
    }
    if (Input::displayShortPress()) {
        if (!settingsEditing) {
            Display::nextScreen();
        } else if (settingsCursor == SETTING_NET) {
            // Sull'ultima voce la pressione breve non scorre: esegue. E' la stessa
            // mano che scende lungo il menu e poi preme ancora una volta, senza
            // cambiare tasto ne' dita.
            //
            // Il cursore non torna a capo proprio per questo: scorrendo si arriva
            // qui e ci si ferma, cosi' non si accende la radio credendo di
            // tornare in cima. Per risalire c'e' l'encoder 1, o si esce e si
            // rientra.
            Storage::flush(snapshotState());  // niente va perso spegnendo l'audio
            NetPortal::begin();
            return;
        } else {
            settingsCursor = (uint8_t)(settingsCursor + 1);
        }
    }

    if (Input::recLongPress()) {
        // STEP EDIT e ADSR EDIT contendono gli stessi comandi: uno esclude l'altro.
        adsrEditMode = false;
        Sequencer::toggleEditing();
    }
    if (Input::recShortPress()) Sequencer::toggleRecord();
    if (Input::playShortPress()) Sequencer::togglePlay();

    if (Input::playLongPress()) {
        // Svuota tutti i 16 step. Non c'e' modo di tornare indietro, per questo
        // la soglia e' piu' alta degli altri long-press e il display lo conferma.
        Sequencer::clearAll();
        Storage::markDirty();
        clearedAt = now;
        Serial.println(F("Pattern svuotato."));
    }

    if (Input::holdLongPress()) {
        // Long-press (>600 ms): entra/esce dall'ADSR EDIT MODE.
        adsrEditMode = !adsrEditMode;
        if (adsrEditMode) Sequencer::setEditing(false);
    }

    // Stato delle modalita' *dopo* i pulsanti che possono cambiarle.
    const bool stepEdit = Sequencer::editing();
    const bool recording = (Sequencer::mode() == Sequencer::SEQ_RECORDING);

    if (Input::holdShortPress()) {
        if (recording) {
            // Durante il record HOLD e' il tasto di cancellazione: il latch non
            // deve scattare al rilascio.
        } else if (stepEdit) {
            Sequencer::writeAtCursor(SEQ_REST, 0);  // svuota lo step e avanza
            Storage::markDirty();
        } else {
            holdActive = !holdActive;
            if (holdActive) {
                // In poli l'accordo tenuto parte da quello che hai sotto le dita
                // in questo istante, e poi si arricchisce nota per nota.
                for (int n = 0; n < NOTE_COUNT; ++n) latchedChord[n] = Input::noteIsHeld(n);
            } else {
                latchedNote = -1;  // sganciando l'HOLD tutto si spegne
                for (int n = 0; n < NOTE_COUNT; ++n) latchedChord[n] = false;
            }
        }
    }

    if (Input::polyPressed()) {
        polyMode = !polyMode;
        // Le voci in corso appartengono all'altra modalita': si azzera tutto,
        // altrimenti resterebbero note appese senza nessuno che le rilasci.
        AudioEngine::allNotesOff();
        for (int i = 0; i < MAX_VOICES; ++i) voiceSounding[i] = false;
        for (int n = 0; n < NOTE_COUNT; ++n) latchedChord[n] = false;
        latchedNote = -1;
        lastTarget = -1;
        Storage::markDirty();
    }

    if (Input::arpPressed()) {
        if (stepEdit) {
            Sequencer::writeAtCursor(SEQ_TIE, 0);  // legato: tiene la nota precedente
            Storage::markDirty();
        } else {
            arpActive = !arpActive;
        }
    }
    if (Input::bpmPressed()) {
        Sequencer::cycleBpm();
        Storage::markDirty();
    }

    // --------------------------------------------------------- joystick
    if (!adsrEditMode) {
        // Su/giu' resta sempre l'ottava: dal vivo e' quella della tastiera, in
        // STEP EDIT e' il registro dello step che si sta per scrivere.
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
                waveform = (waveform + WAVE_COUNT - 1) % WAVE_COUNT;
                AudioEngine::setWaveform(waveform);
                Storage::markDirty();
            }
            if (Input::joyRight()) {
                waveform = (waveform + 1) % WAVE_COUNT;
                AudioEngine::setWaveform(waveform);
                Storage::markDirty();
            }
        }
    } else {
        // edit mode: decay (su/giu) e sustain (sx/dx)
        if (Input::joyUp()) {
            decayMs = min(decayMs + DECAY_STEP_MS, DECAY_MAX_MS);
            AudioEngine::setDecay(decayMs);
            Storage::markDirty();
        }
        if (Input::joyDown()) {
            decayMs = max(decayMs - DECAY_STEP_MS, DECAY_MIN_MS);
            AudioEngine::setDecay(decayMs);
            Storage::markDirty();
        }
        if (Input::joyRight()) {
            sustainLevel = min(sustainLevel + SUSTAIN_STEP, 1.0f);
            AudioEngine::setSustain(sustainLevel);
            Storage::markDirty();
        }
        if (Input::joyLeft()) {
            sustainLevel = max(sustainLevel - SUSTAIN_STEP, 0.0f);
            AudioEngine::setSustain(sustainLevel);
            Storage::markDirty();
        }
    }

    // -------------------------------------------------------- encoder
    // Essendo incrementali, nelle modalita' di edit cutoff e volume restano
    // congelati per costruzione: nessuno dei due encoder li tocca e all'uscita
    // non c'e' nessun salto da recuperare.
    if (Input::enc1Click()) enc1Fine = !enc1Fine;
    if (Input::enc2Click()) enc2Fine = !enc2Fine;

    const int enc1 = Input::enc1Delta();
    const int enc2 = Input::enc2Delta();

    if (settingsEditing) {
        // Nel menu gli encoder regolano se stessi. L'encoder 1 scorre le voci
        // come alternativa al pulsante, l'encoder 2 cambia il valore: cutoff e
        // volume restano fermi finche' non esci, ed e' quello che serve mentre
        // stai tarando proprio la loro sensibilita'.
        if (enc1 != 0) {
            int c = (int)settingsCursor + enc1;
            if (c < 0) c = 0;
            if (c > SETTING_COUNT - 1) c = SETTING_COUNT - 1;
            settingsCursor = (uint8_t)c;
        }
        const uint8_t which = settingsCursor;
        if (enc2 != 0 && !Settings::isAction(which)) {
            int idx = (int)setIndex[which] + enc2;
            if (idx < 0) idx = 0;
            if (idx > Settings::ENTRIES[which].count - 1) idx = Settings::ENTRIES[which].count - 1;
            setIndex[which] = (uint8_t)idx;
            Storage::markDirty();
        }
    } else if (adsrEditMode) {
        if (enc1 != 0) {
            attackPos = clamp01(attackPos + enc1 * stepFor(SETTING_ADSR, enc1Fine));
            attackMs = expMap(attackPos, ATTACK_MIN_MS, ATTACK_RATIO);
            AudioEngine::setAttack(attackMs);
            Storage::markDirty();
        }
        if (enc2 != 0) {
            releasePos = clamp01(releasePos + enc2 * stepFor(SETTING_ADSR, enc2Fine));
            releaseMs = expMap(releasePos, RELEASE_MIN_MS, RELEASE_RATIO);
            AudioEngine::setRelease(releaseMs);
            Storage::markDirty();
        }
    } else if (stepEdit) {
        // Scorrere 16 step con il joystick e' lento: l'encoder 1 fa da rotella.
        if (enc1 != 0) Sequencer::moveCursor(enc1);
        if (enc2 != 0) {
            Sequencer::nudgeBpm(enc2);  // tempo continuo, non solo i preset
            Storage::markDirty();
        }
    } else {
        if (enc1 != 0) {
            cutoffPos = clamp01(cutoffPos + enc1 * stepFor(SETTING_CUTOFF, enc1Fine));
            cutoffHz = expMap(cutoffPos, CUTOFF_MIN_HZ, CUTOFF_RATIO);
            AudioEngine::setCutoff(cutoffHz);
            Storage::markDirty();
        }
        if (enc2 != 0) {
            volume = clamp01(volume + enc2 * stepFor(SETTING_VOL, enc2Fine));
            AudioEngine::setVolume(volume);
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

    // -------------------------------- scrittura sul pattern (edit e record)
    // Si lavora sugli *attacchi* dei tasti, non sulla nota risultante dalla
    // priorita': rilasciando un tasto mentre un altro e' premuto non deve
    // comparire una nota che nessuno ha suonato.
    int pressed;
    while ((pressed = Input::consumeNoteOn()) >= 0) {
        if (stepEdit) {
            Sequencer::writeAtCursor(pressed, octave);
            Storage::markDirty();
        } else if (!arpActive) {
            Sequencer::noteEvent(now, pressed, octave);
            Storage::markDirty();
        }
        // Con l'arpeggiator attivo a finire nel pattern sono i suoi passi, non i
        // tasti tenuti: l'evento arriva qui sotto.
    }
    if (arpActive && arpRetrigger && liveNote >= 0 && !stepEdit) {
        Sequencer::noteEvent(now, liveNote, octave);
        Storage::markDirty();
    }

    // ------------------------------------------------------- sequencer
    // Tenendo HOLD durante il record si svuotano gli step che passano sotto la
    // testina: e' l'"erase" delle drum machine.
    Sequencer::update(now, recording && Input::holdIsDown());

    const bool seqTrigger = Sequencer::consumeTrigger();
    const int seqNote = Sequencer::outputNote();

    // ------------------------------------------------------------- voci
    // Le due modalita' non hanno percorsi separati: entrambe si limitano a
    // dichiarare *cosa deve suonare adesso*, e un unico blocco piu' sotto ne
    // ricava la sequenza minima di eventi da mandare al motore.
    bool wantVoice[MAX_VOICES] = {false};
    float wantFreq[MAX_VOICES] = {0.0f};
    bool wantRetrig[MAX_VOICES] = {false};

    if (!polyMode) {
        // MONO: una voce sola, la nota dal vivo ha priorita' sulla sequenza.
        const bool useLive = (liveNote >= 0);
        const int target = useLive ? liveNote : seqNote;
        if (target >= 0) {
            wantVoice[VOICE_MONO] = true;
            wantFreq[VOICE_MONO] =
                noteFreqAt(target, useLive ? octave : Sequencer::outputOctave());
            // Cambiando sorgente la nota riparte: il passaggio fra tastiera e
            // sequenza deve sentirsi come un attacco, non come una scivolata.
            wantRetrig[VOICE_MONO] = (target != lastTarget) || (useLive != lastWasLive) ||
                                     (useLive ? arpRetrigger : seqTrigger);
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
        // La sequenza non viene piu' zittita dalle dita: ci suoni sopra.
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
            if (voiceSounding[i] && AudioEngine::voiceOff((uint8_t)i)) {
                voiceSounding[i] = false;
            }
            continue;
        }
        if (!voiceSounding[i] || wantRetrig[i]) {
            if (AudioEngine::voiceOn((uint8_t)i, wantFreq[i])) {
                voiceSounding[i] = true;
                voiceFreq[i] = wantFreq[i];
            }
        } else if (fabsf(wantFreq[i] - voiceFreq[i]) > 0.01f) {
            // cambio ottava mentre la nota suona: reintono senza ritriggerare
            if (AudioEngine::voiceRetune((uint8_t)i, wantFreq[i])) {
                voiceFreq[i] = wantFreq[i];
            }
        }
    }

    // --------------------------------------------------------- salvataggio
    if (Storage::savePending(now)) Storage::flush(snapshotState());

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
        view.seqCursor = (uint8_t)Sequencer::cursor();
        view.seqEditing = stepEdit;
        view.countIn = (uint8_t)Sequencer::countInBeats();
        view.seqRev = Sequencer::revision();
        view.bpm = (uint16_t)Sequencer::bpm();
        view.hold = holdActive;
        view.arp = arpActive;
        view.poly = polyMode;
        view.voices = AudioEngine::activeVoices();
        for (int i = 0; i < SETTING_COUNT; ++i) view.setIndex[i] = setIndex[i];
        view.setCursor = settingsCursor;
        view.setEditing = settingsEditing;
        view.clearedAgo = (clearedAt == 0) ? 0 : (now - clearedAt);

        Display::update(view);
    }
}
