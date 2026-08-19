// ArcadeVox — sintetizzatore mono/polifonico su ESP32-S3, scheda 2026-08.
//
//   core 0 : motore audio (task FreeRTOS dedicato, vedi audio_engine.cpp)
//   core 1 : questo loop — input, logica, sequencer, luci, display
//
// La scheda porta 13 tasti nota (un'ottava cromatica intera), 7 tasti funzione,
// 4 encoder con pulsante e un LED RGB sotto ogni tasto. Vedi pinout.h per il
// cablaggio e README.md per cosa fa ogni comando.
//
// Lo schema dei comandi ha una regola sola: ogni tasto fa la parola che ha
// stampata sopra, e ogni manopola fa quello che il display le scrive sotto. Da
// qui discende tutto il resto di questo file — niente pressioni lunghe con una
// seconda funzione nascosta (tranne la conferma su AVVIA, che non e' una
// funzione ma una domanda), niente modalita' in cui si entra senza accorgersene,
// e applicare uno scatto di manopola e dichiarare cosa fa quella manopola sono
// lo stesso pezzo di codice, cosi' non possono contraddirsi.

#include <Arduino.h>
#include <math.h>

#include "audio_engine.h"
#include "display.h"
#include "fx_rows.h"
#include "input_handler.h"
#include "keylight.h"
#include "midi_io.h"
#include "net_portal.h"
#include "pinout.h"
#include "presets.h"
#include "samples.h"
#include "sample_store.h"
#include "sampler.h"
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
// Inviluppo di filtro: quanto ci mette a richiudersi. E' il parametro che, dice
// presets.h, distingue un pianoforte da un organo, e finora non lo raggiungeva
// nessun comando: lo scrivevano solo i timbri di fabbrica.
static const float FILTENV_MIN_MS = 20.0f;
static const float FILTENV_RATIO = 100.0f;  // 20 ms .. 2000 ms

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

// ------------------------------------------------------------------- MIDI IN
//
// Le note che arrivano dal cavo non hanno un tasto a cui appoggiarsi: la
// tastiera occupa le voci 0..12 una per tasto, e una nota MIDI puo' essere
// qualunque delle 128. Serve quindi un assegnatore vero, con il furto della
// voce piu' vecchia quando finiscono — cosa che per i tasti fisici non serviva,
// perche' li' le voci erano esattamente quante le dita.
//
// Regola di precedenza: **il tasto fisico vince sempre**. Se una voce serve a
// un dito, la nota MIDI che ci stava sopra tace finche' il dito non si alza.
// L'alternativa (zittire le dita) renderebbe lo strumento inservibile mentre un
// sequencer esterno lo sta pilotando, che e' proprio quando ci vuoi suonare
// sopra.
static int8_t midiVoiceOfNote[128];
static int8_t midiNoteOfVoice[NOTE_COUNT];
static uint32_t midiVoiceAge[NOTE_COUNT];
static float midiVelOfVoice[NOTE_COUNT];
static bool midiRetrig[NOTE_COUNT];
static uint32_t midiAgeCounter = 0;
static float midiBend = 1.0f;   // moltiplicatore di frequenza del pitch bend
static bool midiSustain = false;  // pedale (CC 64): tiene le note rilasciate
static bool midiHeld[128];        // nota rilasciata ma trattenuta dal pedale
static uint8_t midiActive = 0;

static void midiReset() {
    for (int i = 0; i < 128; ++i) {
        midiVoiceOfNote[i] = -1;
        midiHeld[i] = false;
    }
    for (int v = 0; v < NOTE_COUNT; ++v) {
        midiNoteOfVoice[v] = -1;
        midiVoiceAge[v] = 0;
        midiVelOfVoice[v] = 1.0f;
        midiRetrig[v] = false;
    }
    midiActive = 0;
}

// Frequenza temperata di una nota MIDI: il 69 e' il LA 440, come da standard.
static inline float midiNoteFreq(uint8_t note) {
    return 440.0f * exp2f(((float)note - 69.0f) / 12.0f);
}

// Voce libera per una nota nuova. In MONO la voce 0 resta della tastiera: e'
// quella che il motore usa per la nota suonata a mano, e prestarla al MIDI
// vorrebbe dire vedersela sparire sotto le dita ad ogni nota in arrivo.
static int midiAllocate(int firstVoice) {
    int oldest = -1;
    for (int v = firstVoice; v < NOTE_COUNT; ++v) {
        if (midiNoteOfVoice[v] < 0 && !Input::noteIsHeld(v)) return v;
        if (midiNoteOfVoice[v] >= 0 &&
            (oldest < 0 || midiVoiceAge[v] < midiVoiceAge[oldest])) {
            oldest = v;
        }
    }
    return oldest;  // tutte occupate: si ruba la piu' vecchia
}

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
static float filtEnvAmount = 0.0f;
static float filtEnvMs = 300.0f;

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
// Riga selezionata nell'elenco EFFETTI. Si ricorda fra un'accensione e l'altra:
// chi sta lavorando sull'eco lo ritrova dove l'aveva lasciato.
static uint8_t fxCursor = FX_ECO_MIX;

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
static float filtEnvPos = 0.5f;

// --------------------------------------------------------- click e passo fine
//
// Il click dell'albero non commuta piu' un "passo fine" invisibile. Faceva la
// cosa peggiore che un comando possa fare: cambiava di nascosto il
// comportamento di una manopola, senza scriverlo da nessuna parte, e su cinque
// schermate su nove non faceva assolutamente niente perche' il parametro sotto
// quella manopola non consultava nemmeno il flag.
//
// Al suo posto due gesti che si spiegano da soli:
//   tienilo premuto e gira -> passo fine, finche' lo tieni (come il tasto che
//                             rallenta il puntatore: nessuno lo deve imparare);
//   premi e lascia          -> riporta indietro quel parametro.
//
// Il ripristino scatta al **rilascio** e solo se nel frattempo non hai girato,
// altrimenti ogni regolazione precisa finirebbe con un annullamento.
static bool encTurned[4] = {false, false, false, false};
// Le azioni che si caricano tenendo premuto: lo svuotamento del pattern e le
// tre righe rosse del menu. Il riempimento si vede a schermo mentre sale.
static uint32_t holdStartedAt = 0;
static uint8_t holdFill = 0;

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
// Ultima volta che qualcuno ha toccato un tasto, e se l'ha mai fatto. Servono
// tutti e due: appena acceso, millis() vale poche decine di millisecondi e
// qualunque confronto con una soglia di due minuti darebbe "no", cioe'
// esattamente il contrario di quello che serve — l'invito deve esserci **subito**,
// perche' il momento in cui uno si avvicina a uno strumento che non conosce e'
// il momento in cui lo accende.
static uint32_t lastTouchAt = 0;
static bool everTouched = false;

// Qualcuno sta usando lo strumento. Non basta guardare i tasti nota: chi sta
// scegliendo un timbro con la manopola, o chi lo sta pilotando da un sequencer
// esterno, sta usando il synth eccome, e vedersi partire il respiro dell'invito
// sotto le dita direbbe esattamente la cosa sbagliata.
static void touched(uint32_t now) {
    lastTouchAt = now;
    everTouched = true;
}
static uint8_t settingsCursor = 0;
// Riga selezionata nell'elenco dei timbri. Segue il preset caricato, perche' su
// questa schermata scorrere *e'* caricare.
static uint8_t timbroCursor = 0;

// Lo strumento che suona sotto i tasti. I quindici timbri sono tutti lo stesso
// motore sottrattivo con parametri diversi; il piano e la batteria no, sono
// campioni, e per questo hanno bisogno di essere una cosa a parte invece che il
// sedicesimo e il diciassettesimo preset.
//
// Stanno pero' **in coda allo stesso elenco**, sulla stessa manopola, perche'
// dal punto di vista di chi suona sono la stessa scelta: "con che suono".
// Distinguere qui cio' che l'orecchio non distingue avrebbe voluto dire una
// schermata in piu' per due voci.
enum : uint8_t { INSTR_SYNTH = 0, INSTR_PIANO, INSTR_BATTERIA };
static uint8_t instrument = INSTR_SYNTH;

// --- schermata SUONI ---
// L'ultimo suono partito, per la schermata, e la velocita' di lettura, che e' la
// manopola che rende questi tredici suoni una cosa con cui si gioca invece di
// tredici pulsanti che fanno sempre uguale.
static int8_t memeLast = -1;
static float memeSpeedPos = 1.0f / 3.0f;  // 0..1 sulla corsa 0,5x .. 2,0x
static float memeSpeed = 1.0f;

// L'overlay: cosa e' appena cambiato, sopra la schermata che stai guardando, per
// un paio di secondi. E' il perno del sistema — se ogni gesto si conferma da
// solo, nessun comando ha piu' bisogno che tu vada a cercare la schermata che lo
// mostra, ed e' proprio quel cercare che rendeva lo strumento difficile.
static const char *flashLabel = nullptr;
static const char *flashValue = nullptr;
static float flashFrac = -1.0f;
static uint32_t flashAt = 0;
// Cambia ad ogni messaggio nuovo. Il display lo usa per ridisegnare la banda una
// volta sola invece che ad ogni fotogramma — e' un contatore e non un confronto
// fra puntatori perche' i valori numerici si compongono sempre nello stesso
// buffer, quindi l'indirizzo non cambia mai nemmeno quando il testo cambia.
static uint16_t flashRev = 0;
// Novecento millisecondi e non duemila. Due secondi sono un'eternita' quando hai
// le mani sulle manopole, e la banda copre proprio la zona che vorresti guardare
// mentre agisci.
static const uint32_t FLASH_MS = 900;

// Maschera di bit sulle schermate: la banda tace su quelle in cui l'effetto
// del gesto e' gia' sotto gli occhi.
//
// E' la regola che l'utente ha chiesto per nome — "nei menu non mettermi la banda
// che mi fa vedere il cambio di voce, la vedo gia' perche' e' colorata, stessa
// cosa per le forme d'onda" — e vale la pena scriverla come regola generale
// invece che come due eccezioni: un annuncio che ripete cio' che gia' vedi non e'
// una conferma, e' qualcosa che ti copre la vista.
//
// Le cose che stanno nel telaio non hanno bisogno di dichiarare niente e infatti
// non chiamano mai queste funzioni: l'ottava ha la sua targhetta colorata in alto
// a sinistra su ogni schermata, e ogni manopola ha il suo arco e la sua
// didascalia in basso, sempre. Un valore permanentemente a video non si annuncia.
#define ON(s) (1u << (s))

// La schermata che si sta davvero guardando, che non e' sempre quella dell'anello:
// durante il preconteggio e la registrazione il sequencer scavalca il display, e
// un messaggio va zittito o mostrato in base a cio' che hai sotto gli occhi in
// quel momento, non a dove eri prima di premere REGISTRA.
static uint8_t visibleScreen = SCREEN_SUONA;

static void flashUnless(uint16_t visibleOn, const char *label, const char *value, float frac) {
    if (visibleOn & ON(visibleScreen)) return;
    flashLabel = label;
    flashValue = value;
    flashFrac = frac;
    flashAt = millis();
    ++flashRev;
}

// Un messaggio secco ("SILENZIO", "PATTERN VUOTO"): una riga sola, senza barra,
// che non ha un valore da mostrare.
static void toast(const char *text) { flashUnless(0, nullptr, text, -1.0f); }
static void toastUnless(uint16_t visibleOn, const char *text) {
    flashUnless(visibleOn, nullptr, text, -1.0f);
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

enum { MIDIOUT_OFF = 0, MIDIOUT_NOTES, MIDIOUT_NOTES_CLOCK };

static inline bool midiOutOn() { return setIndex[SETTING_MIDIOUT] != MIDIOUT_OFF; }

static void tweakCutoff(int delta, bool fine) {
    cutoffPos = clamp01(cutoffPos + delta * stepFor(SETTING_CUTOFF, fine));
    cutoffHz = expMap(cutoffPos, CUTOFF_MIN_HZ, CUTOFF_RATIO);
    AudioEngine::setCutoff(cutoffHz);
    Storage::markDirty();
}

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

static void pushAllParams();

// Carica un timbro di fabbrica: sovrascrive i parametri correnti e li manda al
// motore. Da qui in poi si e' liberi di ritoccare tutto — un preset e' un punto
// di partenza, non una gabbia.
static void loadPreset(uint8_t index) {
    if (index >= PRESET_COUNT) return;
    const Preset &p = PRESETS[index];

    waveform = p.wave;
    cutoffHz = p.cutoffHz;
    resonance = p.resonance;
    attackMs = p.attackMs;
    decayMs = p.decayMs;
    sustainLevel = p.sustain;
    releaseMs = p.releaseMs;
    filtEnvAmount = p.filtEnvAmount;
    filtEnvMs = p.filtEnvMs;
    subLevel = p.subLevel;
    detuneCents = p.detuneCents;
    driveAmt = p.drive;
    glideMs = p.glideMs;
    delayMix = p.delayMix;
    delayMs = p.delayMs;
    lfoRate = p.lfoRate;
    lfoDepth = p.lfoDepth;
    lfoTarget = p.lfoTarget;
    crushOn = p.crush;
    crushPreset = p.crushPreset;

    // Le posizioni normalizzate degli encoder vanno riallineate ai valori
    // appena caricati, o al primo scatto la manopola salterebbe dove stava
    // prima del preset.
    cutoffPos = expMapInv(cutoffHz, CUTOFF_MIN_HZ, CUTOFF_RATIO);
    attackPos = expMapInv(attackMs, ATTACK_MIN_MS, ATTACK_RATIO);
    decayPos = expMapInv(decayMs, DECAY_MIN_MS, DECAY_RATIO);
    releasePos = expMapInv(releaseMs, RELEASE_MIN_MS, RELEASE_RATIO);
    delayPos = expMapInv(delayMs, DELAY_MIN_MS, DELAY_RATIO);
    lfoRatePos = expMapInv(lfoRate, LFO_MIN_HZ, LFO_RATIO);
    glidePos = (glideMs > 0.0f) ? expMapInv(glideMs, GLIDE_MIN_MS, GLIDE_RATIO) : 0.0f;
    filtEnvPos = expMapInv(filtEnvMs, FILTENV_MIN_MS, FILTENV_RATIO);

    pushAllParams();
    Storage::markDirty();
}

// Applica la voce scelta nell'elenco dei timbri.
//
// I primi quindici sono preset del motore sottrattivo: cambiano i parametri e
// basta. Le ultime due sono strumenti campionati, e li' non c'e' nessun
// parametro da caricare — cambia chi produce il suono.
//
// Le voci che stavano suonando si spengono in entrambi i versi del passaggio:
// una nota del motore lasciata aperta mentre si passa al piano non la
// rilascerebbe piu' nessuno, perche' da quel momento i tasti non parlano piu'
// con il motore.
static void applyTimbro(uint8_t index) {
    const uint8_t before = instrument;
    instrument = (index < PRESET_COUNT)
                     ? INSTR_SYNTH
                     : ((index == PRESET_COUNT) ? INSTR_PIANO : INSTR_BATTERIA);

    if (instrument != before) {
        AudioEngine::allNotesOff();
        for (int i = 0; i < MAX_VOICES; ++i) voiceSounding[i] = false;
    }

    if (index < PRESET_COUNT) {
        loadPreset(index);
        if (midiOutOn()) MidiOut::program(index);
    }
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
    s.filtEnvAmount = filtEnvAmount;
    s.filtEnvMs = filtEnvMs;
    s.crushOn = crushOn ? 1 : 0;
    s.crushPreset = crushPreset;
    s.arpMode = arpMode;
    s.chordMode = chordMode;
    // Il campo non cambia posto ne' dimensione: cambia solo cosa vuol dire, da
    // "cosa comanda il quarto encoder" a "riga scelta nell'elenco EFFETTI". Il
    // blob resta identico, quindi le schede gia' in giro si rileggono senza
    // migrazioni, e i valori vecchi (0..8) cadono dentro il nuovo intervallo.
    s.enc4Assign = fxCursor;
    s.setScale = setIndex[SETTING_SCALE];
    s.setRoot = setIndex[SETTING_ROOT];
    s.setLed = setIndex[SETTING_LED];
    s.setAudio = setIndex[SETTING_AUDIO];
    s.setTimbro = setIndex[SETTING_TIMBRO];
    s.setMidiOut = setIndex[SETTING_MIDIOUT];
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
    AudioEngine::setFilterEnv(filtEnvAmount, filtEnvMs);
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

// Scorrimento di un elenco a scelte discrete, senza giro in tondo: arrivare in
// fondo e ritrovarsi all'inizio, su un elenco che non si vede tutto, disorienta.
//
// Si somma il delta intero e non un passo solo. encDelta() restituisce **gli
// scatti accumulati** dall'ultima lettura, e mentre il display ridisegna ne
// arrivano tranquillamente due o tre: prendendone uno per giro, una girata deciso
// sull'elenco dei timbri ne saltava la meta'.
static int nudgeIndex(int value, int delta, int count) {
    int v = value + delta;
    if (v < 0) v = 0;
    if (v > count - 1) v = count - 1;
    return v;
}

// ------------------------------------------------------- elenco EFFETTI
//
// Tre funzioni sole per quattordici righe: cosa vale, quanto e' piena la sua
// barretta, e cosa succede girando. Meglio tre switch che quattordici blocchi
// sparsi — cosi' aggiungere una riga vuol dire toccare tre punti vicini, e se
// uno se ne dimentica uno il compilatore glielo dice.

static const char *const LFO_SU_NAMES[LFO_TARGET_COUNT] = {"SPENTO", "ALTEZZA", "FILTRO",
                                                           "VOLUME"};

// Quanto e' piena la barretta della riga: 0..1. Sulle righe a scelta discreta
// (grana, bersaglio dell'LFO, modo dell'arpeggiator) e' la posizione nella lista,
// cosi' anche li' si vede a colpo d'occhio quanto manca alla fine.
static float fxFrac(uint8_t row) {
    switch (row) {
        case FX_GRANA: return (float)crushPreset / (float)(CRUSH_COUNT - 1);
        case FX_ECO_MIX: return delayMix;
        case FX_ECO_TEMPO: return delayPos;
        case FX_ECO_RITORNO: return delayFb;
        case FX_LFO_SU: return (float)lfoTarget / (float)(LFO_TARGET_COUNT - 1);
        case FX_LFO_VELOC: return lfoRatePos;
        case FX_LFO_PROF: return lfoDepth;
        case FX_MODO_ARP: return (float)arpMode / (float)(ARP_MODE_COUNT - 1);
        case FX_SUB: return subLevel;
        case FX_DETUNE: return detuneCents / 50.0f;
        case FX_DRIVE: return driveAmt;
        case FX_GLIDE: return glidePos;
        case FX_APERTURA: return filtEnvAmount;
        case FX_CHIUSURA: return filtEnvPos;
        default: return 0.0f;
    }
}

// Il valore scritto. Le unita' ci sono per un motivo: "220 ms" insegna cos'e'
// un tempo di eco, "55 %" no.
static void fxValueText(uint8_t row, char *out, size_t n) {
    switch (row) {
        case FX_GRANA: snprintf(out, n, "%s", CRUSH_PRESETS[crushPreset].label); break;
        case FX_ECO_MIX: snprintf(out, n, "%d %%", (int)(delayMix * 100.0f + 0.5f)); break;
        case FX_ECO_TEMPO: snprintf(out, n, "%d ms", (int)delayMs); break;
        case FX_ECO_RITORNO: snprintf(out, n, "%d %%", (int)(delayFb * 100.0f + 0.5f)); break;
        case FX_LFO_SU: snprintf(out, n, "%s", LFO_SU_NAMES[lfoTarget]); break;
        case FX_LFO_VELOC: snprintf(out, n, "%.1f Hz", lfoRate); break;
        case FX_LFO_PROF: snprintf(out, n, "%d %%", (int)(lfoDepth * 100.0f + 0.5f)); break;
        case FX_MODO_ARP: snprintf(out, n, "%s", ARP_NAMES[arpMode]); break;
        case FX_SUB: snprintf(out, n, "%d %%", (int)(subLevel * 100.0f + 0.5f)); break;
        case FX_DETUNE: snprintf(out, n, "%d ct", (int)(detuneCents + 0.5f)); break;
        case FX_DRIVE: snprintf(out, n, "%d %%", (int)(driveAmt * 100.0f + 0.5f)); break;
        case FX_GLIDE:
            if (glideMs <= 0.0f) snprintf(out, n, "NO");
            else snprintf(out, n, "%d ms", (int)glideMs);
            break;
        case FX_APERTURA: snprintf(out, n, "%d %%", (int)(filtEnvAmount * 100.0f + 0.5f)); break;
        case FX_CHIUSURA: snprintf(out, n, "%d ms", (int)filtEnvMs); break;
        default: snprintf(out, n, "-"); break;
    }
}

// Un giro di manopola sulla riga selezionata. Le righe a scelta discreta si
// muovono di una posizione per scatto e non scorrono in tondo: arrivare in fondo
// e ritrovarsi all'inizio, su un elenco che non si vede tutto, disorienta.
static void fxTurn(uint8_t row, int delta, bool fine) {
    const float s = stepFor(SETTING_ADSR, fine);
    switch (row) {
        case FX_GRANA: {
            crushPreset = (uint8_t)nudgeIndex(crushPreset, delta, CRUSH_COUNT);
            AudioEngine::setCrushBits(CRUSH_PRESETS[crushPreset].bits);
            AudioEngine::setCrushDivider(CRUSH_PRESETS[crushPreset].divider);
            // Cambiare la grana accende anche l'effetto: nessuno gira questa
            // riga per sentire il silenzio.
            crushOn = true;
            AudioEngine::setCrush(true);
            break;
        }
        case FX_ECO_MIX:
            delayMix = clamp01(delayMix + delta * s);
            AudioEngine::setDelayMix(delayMix);
            break;
        case FX_ECO_TEMPO:
            delayPos = clamp01(delayPos + delta * s);
            delayMs = expMap(delayPos, DELAY_MIN_MS, DELAY_RATIO);
            AudioEngine::setDelayTime(delayMs);
            break;
        case FX_ECO_RITORNO:
            // Il tetto e' 0,9 e non 1: a ritorno pieno l'eco non si spegne piu'
            // e cresce su se stesso finche' satura. Non e' un effetto, e' un
            // guasto, e non deve stare in fondo alla corsa di una manopola.
            delayFb = clamp01(delayFb + delta * s);
            if (delayFb > 0.9f) delayFb = 0.9f;
            AudioEngine::setDelayFeedback(delayFb);
            break;
        case FX_LFO_SU: {
            lfoTarget = (uint8_t)nudgeIndex(lfoTarget, delta, LFO_TARGET_COUNT);
            AudioEngine::setLfoTarget(lfoTarget);
            // Scegliere un bersaglio con la profondita' a zero non si sente:
            // il primo scatto apre anche un filo di profondita', altrimenti la
            // riga sembra rotta.
            if (lfoTarget != LFO_OFF && lfoDepth <= 0.0f) {
                lfoDepth = 0.25f;
                AudioEngine::setLfoDepth(lfoDepth);
            }
            break;
        }
        case FX_LFO_VELOC:
            lfoRatePos = clamp01(lfoRatePos + delta * s);
            lfoRate = expMap(lfoRatePos, LFO_MIN_HZ, LFO_RATIO);
            AudioEngine::setLfoRate(lfoRate);
            break;
        case FX_LFO_PROF:
            lfoDepth = clamp01(lfoDepth + delta * s);
            AudioEngine::setLfoDepth(lfoDepth);
            if (lfoDepth > 0.0f && lfoTarget == LFO_OFF) {
                lfoTarget = LFO_PITCH;
                AudioEngine::setLfoTarget(lfoTarget);
            }
            break;
        case FX_MODO_ARP:
            arpMode = (uint8_t)nudgeIndex(arpMode, delta, ARP_MODE_COUNT);
            arpDir = 1;
            break;
        case FX_SUB:
            subLevel = clamp01(subLevel + delta * s);
            AudioEngine::setSubLevel(subLevel);
            break;
        case FX_DETUNE:
            detuneCents += delta * s * 50.0f;
            if (detuneCents < 0.0f) detuneCents = 0.0f;
            if (detuneCents > 50.0f) detuneCents = 50.0f;
            AudioEngine::setDetune(detuneCents);
            break;
        case FX_DRIVE:
            driveAmt = clamp01(driveAmt + delta * s);
            AudioEngine::setDrive(driveAmt);
            break;
        case FX_GLIDE:
            glidePos = clamp01(glidePos + delta * s);
            glideMs = (glidePos <= 0.0f) ? 0.0f : expMap(glidePos, GLIDE_MIN_MS, GLIDE_RATIO);
            AudioEngine::setGlide(glideMs);
            break;
        case FX_APERTURA:
            filtEnvAmount = clamp01(filtEnvAmount + delta * s);
            AudioEngine::setFilterEnv(filtEnvAmount, filtEnvMs);
            break;
        case FX_CHIUSURA:
            filtEnvPos = clamp01(filtEnvPos + delta * s);
            filtEnvMs = expMap(filtEnvPos, FILTENV_MIN_MS, FILTENV_RATIO);
            AudioEngine::setFilterEnv(filtEnvAmount, filtEnvMs);
            break;
        default:
            break;
    }
}

// ------------------------------------------------------- corona dei comandi
//
// Qui sta il cuore del nuovo schema, ed e' una funzione sola per una ragione:
// finche' "cosa fa la manopola 2" era sparso fra uno switch degli scatti, una
// legenda scritta a mano su ogni schermata e un paio di eccezioni ricordate a
// memoria, le tre cose andavano fuori sincrono — e infatti la schermata ADSR
// prometteva "3=S" mentre sotto c'era il volume, e la HOME invitava a cambiare
// l'onda col joystick che ormai cambiava schermata.
//
// Adesso applicare lo scatto e dichiarare cosa fa quella manopola sono lo stesso
// pezzo di codice. Non possono piu' contraddirsi: se un giorno una manopola
// cambia mestiere, l'etichetta sotto il display cambia con lei perche' e' scritta
// nella riga accanto.
//
// L'unica regola che resta da ricordare e' che non c'e' niente da ricordare: la
// prima manopola e' il carattere del suono, la seconda il suo colore, la terza e'
// sempre il volume — tranne sull'inviluppo, dove le lettere sono quattro e la
// terza e' la S, che e' esattamente cio' che la schermata scrive.
static const char *const SEQ_NOTE_NAMES[NOTE_COUNT] = {
    "DO", "DO#", "RE", "RE#", "MI", "FA", "FA#", "SOL", "SOL#", "LA", "LA#", "SI", "DO'"};

// Il valore della manopola NOTA come posizione in un elenco unico e ordinato:
// pausa, le tredici note, legato. Una manopola sola al posto dei tre gesti
// nascosti con cui prima si scriveva un passo.
static int seqNoteIndex() {
    const Sequencer::Step &st = Sequencer::stepAt(Sequencer::cursor());
    if (st.note == SEQ_TIE) return NOTE_COUNT + 1;
    if (st.note < 0 || st.note >= NOTE_COUNT) return 0;
    return st.note + 1;
}

static const char *seqNoteName() {
    const int i = seqNoteIndex();
    if (i == 0) return "PAUSA";
    if (i == NOTE_COUNT + 1) return "LEGATO";
    return SEQ_NOTE_NAMES[i - 1];
}

// Scrive nello step sotto il cursore la posizione `i` dell'elenco qui sopra.
static void seqWriteIndex(int i) {
    if (i < 0) i = 0;
    if (i > NOTE_COUNT + 1) i = NOTE_COUNT + 1;
    if (i == 0) Sequencer::setAtCursor(SEQ_REST, 0);
    else if (i == NOTE_COUNT + 1) Sequencer::setAtCursor(SEQ_TIE, 0);
    else Sequencer::setAtCursor(i - 1, octave);
}

static void applyKnobs(uint8_t scr, const int enc[4], uint32_t now) {
    (void)now;
    // Il passo fine e' "tieni il click e gira": lo stesso gesto del tasto che
    // rallenta il puntatore del mouse, che nessuno ha mai dovuto imparare.
    bool fine[4];
    for (int e = 0; e < 4; ++e) fine[e] = Input::encIsDown(e);

    // Quattordici e non dieci: "LRC BCK DIN" e "NOTE+CLOCK" sono i due valori piu'
    // lunghi del menu, e tagliati a nove caratteri diventavano "LRC BCK D" e
    // "NOTE+CLOC" — cioe' proprio le due voci in cui leggere il valore per intero
    // e' l'unico modo di sapere se e' quello giusto.
    char buf[4][14];
    const char *label[4] = {"-", "-", "-", "-"};
    float frac[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int e = 0; e < 4; ++e) buf[e][0] = '\0';

    // --- manopola 3: il volume, il punto fermo dello strumento ---
    // Vale su sette schermate su otto. L'unica eccezione e' l'inviluppo, dove le
    // lettere da regolare sono quattro e la terza e' il SOSTEGNO: era l'unico
    // parametro del synth che nessun comando poteva toccare, mentre la schermata
    // prometteva "3=S" da sempre.
    if (scr != SCREEN_INVILUPPO) {
        if (enc[2] != 0) {
            volume = clamp01(volume + enc[2] * stepFor(SETTING_VOL, fine[2]));
            AudioEngine::setVolume(volume);
            Storage::markDirty();
        }
        label[2] = "VOL";
        frac[2] = volume;
        snprintf(buf[2], sizeof(buf[2]), "%d %%", (int)(volume * 100.0f + 0.5f));
    }

    switch (scr) {
        case SCREEN_SUONA:
            if (enc[0] != 0) {
                int w = ((int)waveform + enc[0]) % WAVE_COUNT;
                if (w < 0) w += WAVE_COUNT;
                waveform = (uint8_t)w;
                AudioEngine::setWaveform(waveform);
                Storage::markDirty();
            }
            if (enc[1] != 0) tweakCutoff(enc[1], fine[1]);
            if (enc[3] != 0) {
                resonance = clamp01(resonance + enc[3] * stepFor(SETTING_CUTOFF, fine[3]));
                AudioEngine::setResonance(resonance);
                Storage::markDirty();
            }
            label[0] = "ONDA";
            frac[0] = (float)waveform / (float)(WAVE_COUNT - 1);
            snprintf(buf[0], sizeof(buf[0]), "%s", WAVEFORM_NAMES[waveform]);
            label[1] = "TAGLIO";
            frac[1] = cutoffPos;
            snprintf(buf[1], sizeof(buf[1]), "%d Hz", (int)cutoffHz);
            label[3] = "RISON.";
            frac[3] = resonance;
            snprintf(buf[3], sizeof(buf[3]), "%d %%", (int)(resonance * 100.0f + 0.5f));
            break;

        case SCREEN_TIMBRI:
            // Scorrere carica: il timbro si sceglie suonando mentre giri, ed e'
            // il gesto piu' redditizio dello strumento per chi comincia.
            if (enc[0] != 0) {
                const int t =
                    nudgeIndex(timbroCursor, enc[0], PRESET_COUNT + INSTRUMENT_EXTRA);
                if (t != (int)timbroCursor) {
                    timbroCursor = (uint8_t)t;
                    setIndex[SETTING_TIMBRO] = timbroCursor;
                    applyTimbro(timbroCursor);
                    Storage::markDirty();
                }
            }
            if (enc[1] != 0) {
                setIndex[SETTING_SCALE] = (uint8_t)nudgeIndex(
                    setIndex[SETTING_SCALE], enc[1], Settings::valueCount(SETTING_SCALE));
                Storage::markDirty();
            }
            if (enc[3] != 0) {
                chordMode = (uint8_t)nudgeIndex(chordMode, enc[3], CHORD_COUNT);
                AudioEngine::voiceOff(VOICE_CHORD1);
                AudioEngine::voiceOff(VOICE_CHORD2);
                voiceSounding[VOICE_CHORD1] = false;
                voiceSounding[VOICE_CHORD2] = false;
                Storage::markDirty();
            }
            label[0] = "TIMBRO";
            // Le due voci campionate sono in fondo allo stesso elenco: l'arco
            // della manopola e il contatore devono contarle, altrimenti scelto
            // il PIANO l'arco resta oltre il fondo corsa e il contatore scrive
            // "16/15" — cioe' due modi diversi di dire che l'elenco e' finito
            // una voce prima di dove finisce davvero.
            frac[0] = (float)timbroCursor / (float)(PRESET_COUNT + INSTRUMENT_EXTRA - 1);
            snprintf(buf[0], sizeof(buf[0]), "%d/%d", timbroCursor + 1,
                     PRESET_COUNT + INSTRUMENT_EXTRA);
            label[1] = "SCALA";
            frac[1] = (float)setIndex[SETTING_SCALE] /
                      (float)(Settings::valueCount(SETTING_SCALE) - 1);
            snprintf(buf[1], sizeof(buf[1]), "%s",
                     Settings::valueLabel(SETTING_SCALE, setIndex[SETTING_SCALE]));
            label[3] = "ACCORDO";
            frac[3] = (float)chordMode / (float)(CHORD_COUNT - 1);
            snprintf(buf[3], sizeof(buf[3]), "%s", CHORDS[chordMode].label);
            break;

        case SCREEN_SUONI:
            // La prima manopola scorre i nomi senza far partire niente: serve a
            // sapere cosa c'e' prima di premere, e a ritrovare quello che
            // cercavi. La quarta e' la velocita', ed e' meta' del divertimento.
            if (enc[0] != 0) {
                memeLast = (int8_t)nudgeIndex((memeLast < 0) ? 0 : memeLast, enc[0], MEME_COUNT);
            }
            if (enc[3] != 0) {
                memeSpeedPos = clamp01(memeSpeedPos + enc[3] * stepFor(SETTING_ADSR, fine[3]));
                memeSpeed = 0.5f + memeSpeedPos * 1.5f;
                AudioEngine::setSampleSpeed(memeSpeed);
            }
            label[0] = "SUONO";
            frac[0] = (memeLast < 0) ? 0.0f : (float)memeLast / (float)(MEME_COUNT - 1);
            snprintf(buf[0], sizeof(buf[0]), "%s",
                     (memeLast < 0) ? "-" : MEME_SAMPLES[memeLast].name);
            label[3] = "VELOC.";
            frac[3] = memeSpeedPos;
            snprintf(buf[3], sizeof(buf[3]), "x%.2f", memeSpeed);
            break;

        case SCREEN_INVILUPPO:
            if (enc[0] != 0) {
                attackPos = clamp01(attackPos + enc[0] * stepFor(SETTING_ADSR, fine[0]));
                attackMs = expMap(attackPos, ATTACK_MIN_MS, ATTACK_RATIO);
                AudioEngine::setAttack(attackMs);
                Storage::markDirty();
            }
            if (enc[1] != 0) {
                decayPos = clamp01(decayPos + enc[1] * stepFor(SETTING_ADSR, fine[1]));
                decayMs = expMap(decayPos, DECAY_MIN_MS, DECAY_RATIO);
                AudioEngine::setDecay(decayMs);
                Storage::markDirty();
            }
            if (enc[2] != 0) {
                sustainLevel = clamp01(sustainLevel + enc[2] * stepFor(SETTING_ADSR, fine[2]));
                AudioEngine::setSustain(sustainLevel);
                Storage::markDirty();
            }
            if (enc[3] != 0) {
                releasePos = clamp01(releasePos + enc[3] * stepFor(SETTING_ADSR, fine[3]));
                releaseMs = expMap(releasePos, RELEASE_MIN_MS, RELEASE_RATIO);
                AudioEngine::setRelease(releaseMs);
                Storage::markDirty();
            }
            label[0] = "ATTAC";
            frac[0] = attackPos;
            snprintf(buf[0], sizeof(buf[0]), "%d ms", (int)attackMs);
            label[1] = "DECAD";
            frac[1] = decayPos;
            snprintf(buf[1], sizeof(buf[1]), "%d ms", (int)decayMs);
            label[2] = "SOST";
            frac[2] = sustainLevel;
            snprintf(buf[2], sizeof(buf[2]), "%d %%", (int)(sustainLevel * 100.0f + 0.5f));
            label[3] = "RILAS";
            frac[3] = releasePos;
            snprintf(buf[3], sizeof(buf[3]), "%d ms", (int)releaseMs);
            break;

        case SCREEN_EFFETTI:
            if (enc[0] != 0) {
                fxCursor = (uint8_t)nudgeIndex(fxCursor, enc[0], FX_ROW_COUNT);
                Storage::markDirty();
            }
            if (enc[1] != 0) {
                fxTurn(fxCursor, enc[1], fine[1]);
                Storage::markDirty();
            }
            label[0] = "SCEGLI";
            frac[0] = (float)fxCursor / (float)(FX_ROW_COUNT - 1);
            snprintf(buf[0], sizeof(buf[0]), "%d/%d", fxCursor + 1, FX_ROW_COUNT);
            // La seconda manopola non si chiama mai "CAMBIA": si chiama col nome
            // della riga su cui sei. Una parola generica non insegna niente.
            label[1] = FX_ROWS[fxCursor].label;
            frac[1] = fxFrac(fxCursor);
            fxValueText(fxCursor, buf[1], sizeof(buf[1]));
            break;

        case SCREEN_RITMO:
            if (enc[0] != 0) Sequencer::moveCursor(enc[0]);
            if (enc[1] != 0 && Sequencer::editing()) {
                seqWriteIndex(nudgeIndex(seqNoteIndex(), enc[1], NOTE_COUNT + 2));
                Storage::markDirty();
            }
            if (enc[3] != 0) {
                Sequencer::nudgeBpm(enc[3]);
                Storage::markDirty();
            }
            label[0] = "PASSO";
            frac[0] = (float)Sequencer::cursor() / (float)(SEQ_STEPS - 1);
            snprintf(buf[0], sizeof(buf[0]), "%d", Sequencer::cursor() + 1);
            label[1] = "NOTA";
            frac[1] = (float)seqNoteIndex() / (float)(NOTE_COUNT + 1);
            snprintf(buf[1], sizeof(buf[1]), "%s", seqNoteName());
            label[3] = "TEMPO";
            frac[3] = (float)(Sequencer::bpm() - 40) / 200.0f;
            snprintf(buf[3], sizeof(buf[3]), "%d", Sequencer::bpm());
            break;

        case SCREEN_MENU:
            if (enc[0] != 0) {
                settingsCursor =
                    (uint8_t)nudgeIndex(settingsCursor, enc[0], SETTING_MENU_COUNT);
            }
            if (enc[1] != 0 && !Settings::isAction(settingsCursor)) {
                const uint8_t which = settingsCursor;
                setIndex[which] = (uint8_t)nudgeIndex(setIndex[which], enc[1],
                                                     Settings::valueCount(which));
                // L'uscita audio va provata ad orecchio e le luci vanno viste:
                // hanno effetto mentre giri, non all'uscita dal menu.
                if (which == SETTING_AUDIO) AudioEngine::setPinOrder(setIndex[which]);
                Storage::markDirty();
            }
            label[0] = "SCEGLI";
            frac[0] = (float)settingsCursor / (float)(SETTING_MENU_COUNT - 1);
            snprintf(buf[0], sizeof(buf[0]), "%d/%d", settingsCursor + 1, SETTING_MENU_COUNT);
            if (Settings::isAction(settingsCursor)) {
                label[1] = "TIENI";
                frac[1] = (float)holdFill / 255.0f;
                snprintf(buf[1], sizeof(buf[1]), "AVVIA");
            } else {
                label[1] = Settings::ENTRIES[settingsCursor].label;
                frac[1] = (Settings::valueCount(settingsCursor) > 1)
                              ? (float)setIndex[settingsCursor] /
                                    (float)(Settings::valueCount(settingsCursor) - 1)
                              : 0.0f;
                snprintf(buf[1], sizeof(buf[1]), "%s",
                         Settings::valueLabel(settingsCursor, setIndex[settingsCursor]));
            }
            break;

        default:  // LIVELLO: si guarda, non si tocca. Tre trattini scritti sono
                  // informazione: dicono che qui non c'e' niente da girare, e
                  // nessuno resta a chiedersi perche' non succede niente.
            break;
    }

    for (int e = 0; e < 4; ++e) {
        // Il valore lampeggia al posto del nome solo se un valore ce l'ha: sulle
        // manopole mute di LIVELLO uno scatto non deve far comparire una
        // didascalia vuota al posto del trattino.
        Display::setKnob(e, label[e], buf[e], frac[e], enc[e] != 0 && buf[e][0] != '\0');
    }
}

// Il ripristino: il parametro che quella manopola comanda torna al valore del
// timbro caricato. Non e' una scorciatoia da esperti, e' un'assicurazione da
// principianti — una manopola sconosciuta si gira volentieri solo se si sa come
// tornare indietro — ed e' il motivo per cui il click non fa piu' il passo fine.
static void resetKnob(uint8_t scr, int e) {
    const Preset &p = PRESETS[Settings::clampIndex(SETTING_TIMBRO, setIndex[SETTING_TIMBRO])];
    const char *what = nullptr;

    if (scr != SCREEN_INVILUPPO && e == 2) {
        volume = 0.7f;
        AudioEngine::setVolume(volume);
        what = "VOLUME";
    } else if (scr == SCREEN_SUONA) {
        if (e == 0) {
            waveform = p.wave;
            AudioEngine::setWaveform(waveform);
            what = "ONDA";
        } else if (e == 1) {
            cutoffHz = p.cutoffHz;
            cutoffPos = expMapInv(cutoffHz, CUTOFF_MIN_HZ, CUTOFF_RATIO);
            AudioEngine::setCutoff(cutoffHz);
            what = "TAGLIO";
        } else if (e == 3) {
            resonance = p.resonance;
            AudioEngine::setResonance(resonance);
            what = "RISONANZA";
        }
    } else if (scr == SCREEN_INVILUPPO) {
        if (e == 0) {
            attackMs = p.attackMs;
            attackPos = expMapInv(attackMs, ATTACK_MIN_MS, ATTACK_RATIO);
            AudioEngine::setAttack(attackMs);
            what = "ATTACCO";
        } else if (e == 1) {
            decayMs = p.decayMs;
            decayPos = expMapInv(decayMs, DECAY_MIN_MS, DECAY_RATIO);
            AudioEngine::setDecay(decayMs);
            what = "DECADIM.";
        } else if (e == 2) {
            sustainLevel = p.sustain;
            AudioEngine::setSustain(sustainLevel);
            what = "SOSTEGNO";
        } else {
            releaseMs = p.releaseMs;
            releasePos = expMapInv(releaseMs, RELEASE_MIN_MS, RELEASE_RATIO);
            AudioEngine::setRelease(releaseMs);
            what = "RILASCIO";
        }
    } else if (scr == SCREEN_RITMO && e == 1 && Sequencer::editing()) {
        // Su un passo "com'era" vuol dire vuoto: e' l'unico valore di fabbrica
        // che una griglia possa avere. Ma solo a giro fermo: mentre il pattern
        // suona il cursore non e' nemmeno disegnato, e cancellare un passo che
        // non si vede sarebbe un incidente, non un comando.
        Sequencer::setAtCursor(SEQ_REST, 0);
        what = "PASSO";
    } else if (scr == SCREEN_RITMO && e == 3) {
        Sequencer::setBpm(120);
        what = "TEMPO";
    } else if (scr == SCREEN_MENU && e == 1 && !Settings::isAction(settingsCursor)) {
        setIndex[settingsCursor] = Settings::ENTRIES[settingsCursor].byDefault;
        if (settingsCursor == SETTING_AUDIO) AudioEngine::setPinOrder(setIndex[settingsCursor]);
        what = Settings::ENTRIES[settingsCursor].label;
    } else if (scr == SCREEN_SUONI && e == 3) {
        memeSpeedPos = 1.0f / 3.0f;
        memeSpeed = 1.0f;
        AudioEngine::setSampleSpeed(memeSpeed);
        what = "VELOCITA'";
    } else if (scr == SCREEN_TIMBRI && e == 0) {
        // Ricarica il timbro da capo, buttando via le modifiche fatte a mano: e'
        // la scialuppa di chi ha girato troppe manopole e vuole ricominciare.
        applyTimbro(timbroCursor);
        what = "TIMBRO";
    }

    if (!what) return;
    Storage::markDirty();
    // Questo overlay compare sempre, anche dove il valore e' gia' disegnato: cio'
    // che devi verificare non e' il numero nuovo, e' che lo strumento ha capito
    // che volevi tornare indietro. Un annullamento senza conferma non tranquillizza
    // nessuno.
    flashUnless(0, what, "COM'ERA", -1.0f);
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

// ------------------------------------------------------------------ MIDI OUT
//
// Quello che esce dal cavo si ricava dallo stesso elenco che pilota il motore:
// non c'e' un secondo posto dove decidere cosa sta suonando, quindi le due cose
// non possono andare fuori sincrono. Tasti, arpeggiator, sequencer e note
// aggiunte dagli accordi ci finiscono dentro senza codice dedicato.
static int8_t outNote[MAX_VOICES];
static uint8_t outCcCutoff = 0xFF, outCcRes = 0xFF, outCcVol = 0xFF;
static bool outTransport = false;
static uint32_t outClockAt = 0;
static uint32_t outClockCount = 0;

// I tasti non hanno sensori di forza: la dinamica in uscita e' fissa. Meglio un
// valore pieno ma non massimo — 100 lascia spazio a chi vuole ritoccarla nel DAW
// senza trovarsi gia' al tetto.
static const uint8_t OUT_VELOCITY = 100;



// Da frequenza a numero di nota MIDI. Tutte le voci nascono dalla scala
// temperata, quindi il giro non perde niente — ed e' l'unico modo di prendere
// con una formula sola tasti, sequencer e note aggiunte dagli accordi.
static inline int8_t freqToMidiNote(float hz) {
    if (hz < 8.0f) return -1;
    const int n = (int)lroundf(69.0f + 12.0f * log2f(hz / 440.0f));
    return (n < 0 || n > 127) ? (int8_t)-1 : (int8_t)n;
}

// Fa suonare il tasto `note` con lo strumento campionato scelto.
//
// Il piano passa dalla stessa noteFreqAt() del motore, quindi eredita scala,
// tonica e ottava senza duplicarne la logica: cambia il timbro sotto le dita,
// non che cosa suona un tasto. La batteria invece ignora tutto questo — un
// rullante non ha un'intonazione da trasporre — e mappa il tasto sul pezzo.
static void playSampledKey(int note, int8_t oct) {
    if (instrument == INSTR_BATTERIA) {
        Sampler::drumHit((uint8_t)note);
        return;
    }
    const int8_t midi = freqToMidiNote(noteFreqAt(note, oct));
    if (midi >= 0) Sampler::pianoNote(midi);
}

// Spegne tutto quello che stavamo mandando fuori: cambio di modalita', uscita,
// o MIDI OUT appena disinserito.
static void midiOutAllOff() {
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (outNote[i] >= 0) {
            MidiOut::noteOff((uint8_t)outNote[i]);
            outNote[i] = -1;
        }
    }
}

// Libera la voce che stava suonando una nota MIDI.
static void midiRelease(uint8_t note) {
    const int8_t v = midiVoiceOfNote[note];
    if (v >= 0) midiNoteOfVoice[v] = -1;
    midiVoiceOfNote[note] = -1;
    midiHeld[note] = false;
}

// Svuota la posta del cavo MIDI. Il tetto sul numero di messaggi per giro non e'
// pignoleria: un DAW che manda un glissando puo' riempire la coda piu' in fretta
// di quanto il loop la smaltisca, e restare qui dentro vorrebbe dire non
// ridisegnare piu' il display.
static void midiPump() {
    for (int guard = 0; guard < 48; ++guard) {
        const MidiEvent e = MidiIn::poll();
        if (e.kind == MIDI_NONE) break;

        switch (e.kind) {
            case MIDI_NOTE_ON: {
                // In MONO la voce 0 resta alla tastiera, vedi midiAllocate().
                const int v = midiAllocate(polyMode ? 0 : 1);
                if (v < 0) break;
                if (midiNoteOfVoice[v] >= 0) midiVoiceOfNote[midiNoteOfVoice[v]] = -1;
                midiVoiceOfNote[e.data1] = (int8_t)v;
                midiNoteOfVoice[v] = (int8_t)e.data1;
                midiVelOfVoice[v] = (float)e.data2 / 127.0f;
                midiVoiceAge[v] = ++midiAgeCounter;
                midiRetrig[v] = true;
                midiHeld[e.data1] = false;
                break;
            }
            case MIDI_NOTE_OFF:
                // Col pedale premuto la nota non si spegne: si mette in attesa.
                if (midiSustain) {
                    midiHeld[e.data1] = true;
                } else {
                    midiRelease(e.data1);
                }
                break;

            case MIDI_CC:
                switch (e.data1) {
                    case 1:  // rotella di modulazione -> profondita' dell'LFO
                        lfoDepth = (float)e.data2 / 127.0f;
                        AudioEngine::setLfoDepth(lfoDepth);
                        if (lfoDepth > 0.0f && lfoTarget == LFO_OFF) {
                            lfoTarget = LFO_PITCH;
                            AudioEngine::setLfoTarget(lfoTarget);
                        }
                        break;
                    case 7:  // volume di canale
                        volume = (float)e.data2 / 127.0f;
                        AudioEngine::setVolume(volume);
                        break;
                    case 64:  // pedale di risonanza
                        midiSustain = e.data2 >= 64;
                        if (!midiSustain) {
                            for (int n = 0; n < 128; ++n) {
                                if (midiHeld[n]) midiRelease((uint8_t)n);
                            }
                        }
                        break;
                    case 71:  // risonanza: e' il numero standard, e qui ce l'ha davvero
                        resonance = (float)e.data2 / 127.0f;
                        AudioEngine::setResonance(resonance);
                        break;
                    case 74:  // "brightness": il cutoff, per tutti i DAW del mondo
                        cutoffPos = (float)e.data2 / 127.0f;
                        cutoffHz = expMap(cutoffPos, CUTOFF_MIN_HZ, CUTOFF_RATIO);
                        AudioEngine::setCutoff(cutoffHz);
                        break;
                    case 91:  // riverbero -> qui e' l'eco, che e' quello che abbiamo
                        delayMix = (float)e.data2 / 127.0f;
                        AudioEngine::setDelayMix(delayMix);
                        break;
                    case 94:  // detune
                        detuneCents = (float)e.data2 / 127.0f * 50.0f;
                        AudioEngine::setDetune(detuneCents);
                        break;
                    default:
                        break;
                }
                Storage::markDirty();
                break;

            case MIDI_PROGRAM:
                Serial.print(F("MIDI: cambio programma "));
                Serial.println(e.data1);
                // Il cambio programma sceglie il timbro: e' il modo in cui un
                // sequencer esterno si aspetta di poterlo fare.
                setIndex[SETTING_TIMBRO] = (uint8_t)(e.data1 % PRESET_COUNT);
                applyTimbro(setIndex[SETTING_TIMBRO]);
                toast(PRESETS[setIndex[SETTING_TIMBRO]].name);
                break;

            case MIDI_BEND:
                // Due semitoni per parte, che e' il valore che tutti danno per
                // scontato quando nessuno dichiara il contrario.
                midiBend = exp2f(((float)e.bend / 8192.0f) * 2.0f / 12.0f);
                break;

            case MIDI_ALL_OFF:
                midiSustain = false;
                midiReset();
                break;

            default:
                break;
        }
    }

    uint8_t live = 0;
    for (int v = 0; v < NOTE_COUNT; ++v) {
        if (midiNoteOfVoice[v] >= 0) ++live;
    }
    midiActive = live;
    MidiIn::noteCountSet(live);

    // Diagnostica sulla seriale: due righe in croce, ma sono quelle che
    // rispondono alla domanda "il cavo funziona?" senza dover collegare un
    // altoparlante. Il conteggio si stampa solo quando cambia, altrimenti una
    // scala suonata da un DAW riempirebbe la console.
    static bool wasConnected = false;
    const bool isConnected = MidiIn::connected();
    if (isConnected != wasConnected) {
        wasConnected = isConnected;
        Serial.println(isConnected ? F("MIDI: host collegato.") : F("MIDI: host scollegato."));
    }
    // Il conteggio si stampa al massimo una volta al secondo: suonando una scala
    // da un DAW cambierebbe dieci volte al secondo, e la console diventerebbe
    // illeggibile proprio mentre serve.
    static uint8_t reported = 0xFF;
    static uint32_t reportedAt = 0;
    const uint32_t nowMs = millis();
    if (live != reported && (uint32_t)(nowMs - reportedAt) >= 1000) {
        reported = live;
        reportedAt = nowMs;
        Serial.print(F("MIDI: note attive "));
        Serial.println(live);
    }
}

// Tenendo premuto il tasto BOOT della DevKit si entra in modalita' rete.
//
// Serve perche' l'unica altra via e' il menu impostazioni, cioe' la tastiera: su
// una scheda dove l'espansore non risponde — un ponticello ancora da fare, un
// contatto sporco — l'aggiornamento via WiFi sarebbe irraggiungibile proprio
// quando serve. Il tasto BOOT sta sul modulo, non sul PCB, e non dipende da
// niente di tutto questo.
static bool bootHeldSince(uint32_t now) {
    static uint32_t downAt = 0;
    if (digitalRead(0) == LOW) {
        if (downAt == 0) downAt = now;
        return (now - downAt) >= 2000;
    }
    downAt = 0;
    return false;
}

// ------------------------------------------------------------------ setup
void setup() {
    Serial.begin(115200);

    for (int i = 0; i < SETTING_COUNT; ++i) setIndex[i] = Settings::ENTRIES[i].byDefault;

    StatusLed::begin();  // per primo: spegne il LED RGB prima di ogni altra cosa
    // Il MIDI va registrato prima che TinyUSB parta: dopo, l'elenco delle
    // interfacce e' chiuso e il synth comparirebbe come sola porta seriale.
    MidiIn::begin();
    midiReset();
    for (int i = 0; i < MAX_VOICES; ++i) outNote[i] = -1;
    pinMode(0, INPUT_PULLUP);  // tasto BOOT: via di fuga verso la modalita' rete
    Input::begin();
    Keylight::begin();
    Sequencer::begin();
    Storage::begin();
    AudioEngine::begin();
    // I suoni della schermata SUONI, se qualcuno ne ha caricati di propri nella
    // partizione dati. Va dopo AudioEngine::begin() solo per ordine di lettura:
    // non dipende dal motore, gli prepara solo cio' che dovra' suonare.
    SampleStore::begin();

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
        // Zero e' un valore legittimo — un'eco che non ritorna e' un'eco singola —
        // e adesso che la riga esiste nell'elenco EFFETTI qualcuno la mettera' li'
        // apposta. Non e' piu' il campo a dire se e' stato scritto, lo dice la
        // revisione del blob.
        if (saved.stateRev >= 2) delayFb = clamp01(saved.delayFb);
        delayMix = saved.delayMix;
        if (saved.lfoRate > 0.0f) lfoRate = saved.lfoRate;
        lfoDepth = saved.lfoDepth;
        lfoTarget = (saved.lfoTarget < LFO_TARGET_COUNT) ? saved.lfoTarget : LFO_OFF;
        // Un blob piu' vecchio non li ha e li rilegge a zero: un tempo di
        // richiusura nullo non e' un valore, e' un campo mancante.
        if (saved.filtEnvMs > 0.0f) {
            filtEnvAmount = clamp01(saved.filtEnvAmount);
            filtEnvMs = saved.filtEnvMs;
        }
        filtEnvPos = expMapInv(filtEnvMs, FILTENV_MIN_MS, FILTENV_RATIO);
        crushOn = saved.crushOn != 0;
        crushPreset = (saved.crushPreset < CRUSH_COUNT) ? saved.crushPreset : 1;
        arpMode = (saved.arpMode < ARP_MODE_COUNT) ? saved.arpMode : ARP_UP;
        chordMode = (saved.chordMode < CHORD_COUNT) ? saved.chordMode : 0;
        fxCursor = (saved.enc4Assign < FX_ROW_COUNT) ? saved.enc4Assign : FX_ECO_MIX;
        setIndex[SETTING_SCALE] = Settings::clampIndex(SETTING_SCALE, saved.setScale);
        setIndex[SETTING_ROOT] = Settings::clampIndex(SETTING_ROOT, saved.setRoot);
        setIndex[SETTING_LED] = Settings::clampIndex(SETTING_LED, saved.setLed);
        setIndex[SETTING_AUDIO] = Settings::clampIndex(SETTING_AUDIO, saved.setAudio);
        setIndex[SETTING_TIMBRO] = Settings::clampIndex(SETTING_TIMBRO, saved.setTimbro);
        setIndex[SETTING_MIDIOUT] = Settings::clampIndex(SETTING_MIDIOUT, saved.setMidiOut);
        // La schermata TIMBRI si apre gia' ferma sul timbro che stai sentendo,
        // non in cima all'elenco.
        timbroCursor = setIndex[SETTING_TIMBRO];

        // Blob scritto prima che il marcatore esistesse: i campi in coda sono
        // riempimento, non dati. Si torna ai valori di fabbrica solo per quelli.
        //
        // Il confronto e' con 2 e non con STORAGE_STATE_REV: la revisione che ha
        // introdotto TIMBRO e MIDI OUT e' quella, e legarlo al numero corrente
        // vorrebbe dire che *ogni* revisione futura, anche se aggiunge tutt'altro,
        // riazzera queste due voci su tutte le schede in giro.
        if (saved.stateRev < 2) {
            setIndex[SETTING_TIMBRO] = Settings::ENTRIES[SETTING_TIMBRO].byDefault;
            setIndex[SETTING_MIDIOUT] = Settings::ENTRIES[SETTING_MIDIOUT].byDefault;
            timbroCursor = setIndex[SETTING_TIMBRO];
            Storage::markDirty();
            Serial.println(F("Impostazioni nuove riportate ai valori di fabbrica."));
        }

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
    filtEnvPos = expMapInv(filtEnvMs, FILTENV_MIN_MS, FILTENV_RATIO);

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
        // Gli eventi di AVVIA si leggono **prima** di NetPortal::update(), e la
        // riga sotto e' la ragione per cui questo ordine conta.
        //
        // Sono eventi a fronte: se non li si consuma restano in coda per sempre.
        // E update() e' proprio la funzione che puo' fermarsi qualche secondo a
        // leggere il manifest — ed e' quella che, tornando, arma l'offerta di
        // aggiornamento. Leggendoli dopo, una pressione lunga fatta *prima* che
        // ci fosse qualcosa da installare, e gia' rilasciata da un pezzo, si
        // sarebbe applicata all'offerta appena comparsa: una riprogrammazione del
        // firmware partita da sola, senza nessun dito sul tasto.
        //
        // E' la stessa famiglia di difetto del joystick che scendeva soltanto: un
        // fronte letto due volte, o letto nel momento sbagliato, non e' mai
        // innocuo.
        const bool playHeldLong = Input::fnLongPress(FN_PLAY);
        Input::fnShortPress(FN_PLAY);  // il breve qui non vuol dire niente

        NetPortal::update();
        // Si esce col joystick a sinistra, che e' il "torna indietro" di tutto
        // lo strumento. Ed e' anche l'unica uscita che funziona sempre: il
        // joystick sta su GPIO diretti, quindi risponde anche quando l'espansore
        // I2C non risponde e la tastiera e' muta.
        if (Input::joyLeft()) {
            Serial.println(F("Uscita dalla modalita' NETWORK: riavvio."));
            ESP.restart();
        }

        // Se il synth ha gia' trovato una versione nuova da solo, il telefono non
        // serve piu' a niente: si tiene premuto AVVIA e si installa da qui. Stessa
        // conferma dello svuotamento del pattern — l'anello che si riempie mentre
        // tieni — perche' e' la stessa categoria di gesto: dopo, il firmware non
        // e' piu' quello di prima.
        if (NetPortal::updateAvailable()) {
            uint8_t fill = 0;
            if (Input::fnIsDown(FN_PLAY)) {
                const uint32_t held = Input::fnHeldMs(FN_PLAY);
                fill = (held >= FN_LONG_PRESS_SLOW_MS)
                           ? 255
                           : (uint8_t)(held * 255u / FN_LONG_PRESS_SLOW_MS);
            }
            Display::drawNetHold(fill);
            // Il dito dev'esserci ancora: la soglia scatta col tasto premuto, e
            // pretenderlo qui chiude anche l'ultima fessura per cui un fronte
            // vecchio possa far partire da solo una riprogrammazione.
            if (playHeldLong && Input::fnIsDown(FN_PLAY)) {
                Serial.println(F("NETWORK: installo l'aggiornamento dal synth."));
                NetPortal::installUpdate();
            }
        }
        if (now - lastDisplayAt >= NETWORK_REFRESH_MS) {
            lastDisplayAt = now;
            Display::updateNetwork();
        }
        return;
    }

    StatusLed::update(now);
    // Prima di qualunque cosa possa voler dire qualcosa: da qui in poi ogni
    // messaggio sa se il suo effetto e' gia' sotto gli occhi di chi guarda.
    visibleScreen = (Sequencer::countInBeats() > 0 ||
                     Sequencer::mode() == Sequencer::SEQ_RECORDING)
                        ? SCREEN_RITMO
                        : Display::currentScreen();
    midiPump();

    // Via di fuga: BOOT tenuto premuto due secondi accende la radio anche senza
    // tastiera. Vale solo fuori dalle modalita' di edit, dove il tasto non c'e'
    // comunque.
    if (bootHeldSince(now)) {
        Serial.println(F("BOOT tenuto premuto: entro in modalita' NETWORK."));
        Storage::flush(snapshotState());
        Keylight::allOff();
        NetPortal::begin();
        return;
    }

    // ------------------------------------------- apprendimento delle luci
    // Finche' dura, la tastiera non suona: ogni pressione serve a dire "questo
    // e' il tasto che si e' acceso". Si esce da soli dopo venti tasti, o con
    // FN7 se ci si e' persi.
    if (Keylight::learning()) {
        static const uint8_t NOTE_SLOT[NOTE_COUNT] = MATRIX_NOTE_SLOTS;
        static const uint8_t FN_SLOT[FN_COUNT] = MATRIX_FN_SLOTS;

        // Qui joystick ed encoder non comandano niente, ma vanno letti lo stesso.
        // Un evento a fronte non scade: aspetta. Chi urta una manopola mentre sta
        // imparando la mappa se la ritroverebbe tutta insieme al primo giro utile
        // — il volume che fa un balzo pari a tutti gli scatti messi da parte, la
        // schermata che cambia da sola. Leggere e buttare via e' l'unico modo di
        // dire davvero "in questo momento non contano".
        for (int e = 0; e < 4; ++e) {
            Input::encDelta(e);
            Input::encClick(e);
            Input::encRelease(e);
        }
        Input::joyUp();
        Input::joyDown();
        Input::joyRight();

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
            Input::fnLongPress(f);
        }
        // Si annulla col joystick e non con un tasto: qui tutti e venti i tasti
        // della matrice servono a farsi imparare, e uno che facesse anche altro
        // sarebbe ambiguo proprio nel momento in cui la mappa non c'e' ancora.
        if (Input::joyLeft()) {
            Keylight::cancelLearn();
            toast("MAPPA ANNULLATA");
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
    //
    // Sette parole, sette funzioni: quella stampata sopra il tasto, sempre, in
    // ogni schermata e in ogni modalita'. Le sei pressioni lunghe che c'erano
    // prima non sono sparite, sono andate dove si vedono — il modo
    // dell'arpeggiator e la grana dell'8 BIT sono righe dell'elenco EFFETTI, il
    // modo accordo e' la quarta manopola di TIMBRI, lo step edit e' semplicemente
    // quello che RITMO fa sempre — e l'unica superstite, quella che svuota il
    // pattern, non e' una seconda funzione: e' una richiesta di conferma.
    //
    // Sparisce anche la variante contestuale, che era la parte peggiore: TIENI
    // voleva dire tre cose diverse a seconda di cosa stesse facendo il sequencer,
    // e ARPEGGIO ne voleva dire due. Un tasto che cambia significato senza dirlo
    // e' esattamente il carico di memoria che questo schema toglie di mezzo.

    if (Input::fnShortPress(FN_ARP)) {
        arpActive = !arpActive;
        // Su SUONA e su RITMO la targhetta e' gia' accesa nella fila sotto il
        // titolo: annunciare con un riquadro cio' che si vede gia' non e' una
        // conferma, e' qualcosa che copre la vista.
        toastUnless(ON(SCREEN_SUONA), arpActive ? "ARPEGGIO ON" : "ARPEGGIO OFF");
        Storage::markDirty();
    }

    if (Input::fnShortPress(FN_CRUSH)) {
        crushOn = !crushOn;
        AudioEngine::setCrush(crushOn);
        toastUnless(ON(SCREEN_SUONA) | ON(SCREEN_EFFETTI),
                    crushOn ? CRUSH_PRESETS[crushPreset].label : "8 BIT OFF");
        Storage::markDirty();
    }

    // REGISTRA e AVVIA portano su RITMO. E' la riparazione di un disallineamento
    // vero: il display saltava alla griglia durante la registrazione, ma le
    // manopole restavano quelle della schermata da cui eri partito, quindi
    // guardavi il sequencer mentre la prima manopola muoveva il cutoff.
    if (Input::fnShortPress(FN_REC)) {
        Sequencer::toggleRecord();
        Display::goTo(SCREEN_RITMO);
    }
    if (Input::fnShortPress(FN_PLAY)) {
        Sequencer::togglePlay();
        Display::goTo(SCREEN_RITMO);
    }
    // I due gesti che non si tornano indietro — svuotare il pattern e le tre righe
    // rosse del menu — si caricano tutti e due sull'anello esterno, quindi il
    // riempimento lo decide **una riga sola**, alla fine, guardando quale dei due
    // e' in corso. Con due punti che ci scrivevano dentro, il secondo azzerava
    // quello che aveva scritto il primo e la conferma piu' importante — quella
    // dello svuotamento — non si vedeva mai.
    uint8_t confirmFill = 0;

    // Finche' l'attesa restava invisibile era un trabocchetto: tenevi premuto
    // senza sapere che stava succedendo qualcosa.
    if (Input::fnIsDown(FN_PLAY)) {
        const uint32_t held = Input::fnHeldMs(FN_PLAY);
        confirmFill = (held >= FN_LONG_PRESS_SLOW_MS)
                          ? 255
                          : (uint8_t)(held * 255u / FN_LONG_PRESS_SLOW_MS);
    }
    if (Input::fnLongPress(FN_PLAY)) {
        confirmFill = 0;
        Sequencer::clearAll();
        Storage::markDirty();
        toast("PATTERN VUOTO");
    }

    if (Input::fnShortPress(FN_HOLD)) {
        holdActive = !holdActive;
        if (holdActive) {
            for (int n = 0; n < NOTE_COUNT; ++n) latchedChord[n] = Input::noteIsHeld(n);
        } else {
            latchedNote = -1;
            for (int n = 0; n < NOTE_COUNT; ++n) latchedChord[n] = false;
        }
        toastUnless(ON(SCREEN_SUONA), holdActive ? "TIENI ON" : "TIENI OFF");
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
        toastUnless(ON(SCREEN_SUONA), polyMode ? "POLIFONICO" : "MONO");
        Storage::markDirty();
    }

    // Il panico, finalmente su un tasto suo e con la parola scritta sopra. Prima
    // stava sotto la pressione lunga di un altro tasto — cioe' invisibile — mentre
    // questo settimo tasto duplicava il joystick a destra. E' la cosa che serve
    // piu' spesso quando qualcosa va storto, e non deve chiedere di ricordarsi
    // niente ne' di aspettare seicento millisecondi.
    if (Input::fnShortPress(FN_SILENCE)) {
        AudioEngine::allNotesOff();
        // Anche i campioni: SILENZIO vuol dire silenzio, e un boom da un secondo
        // e mezzo partito per sbaglio e' esattamente il genere di cosa per cui
        // questo tasto esiste.
        AudioEngine::stopSamples();
        for (int i = 0; i < MAX_VOICES; ++i) voiceSounding[i] = false;
        for (int n = 0; n < NOTE_COUNT; ++n) latchedChord[n] = false;
        latchedNote = -1;
        lastTarget = -1;
        holdActive = false;
        midiOutAllOff();
        toast("SILENZIO");
    }

    // --------------------------------------------------------- joystick
    // Il joystick naviga e cambia registro, e basta: orizzontale la schermata,
    // verticale l'ottava. Una regola sola, valida ovunque, e adesso nessun tasto
    // la duplica piu'.
    if (Input::joyRight()) Display::nextScreen();
    if (Input::joyLeft()) Display::prevScreen();

    // Le due letture stanno in due variabili e non dentro l'if. Sembra pedanteria
    // e invece era il bug: joyUp() e' un evento a fronte, leggerla *e'* consumarla,
    // e chiamandola una volta nella condizione e una nel calcolo la seconda
    // trovava sempre il fronte gia' bruciato. Risultato, il ramo +1 era
    // irraggiungibile e l'ottava poteva soltanto scendere — le moltiplicazioni
    // x2.00 e x4.00 si potevano solo ereditare dalla memoria all'accensione.
    const bool joySu = Input::joyUp();
    const bool joyGiu = Input::joyDown();
    if (joySu != joyGiu) {
        applyOctave(octave + (joySu ? 1 : -1));
        // Nessun riquadro: l'ottava ha la sua targhetta colorata in alto a
        // sinistra, su ogni schermata. Un valore sempre a video non va annunciato.
        Storage::markDirty();
    }

    // -------------------------------------------------------- encoder
    //
    // Il click non commuta piu' un "passo fine" invisibile — che per giunta su
    // cinque schermate su nove non faceva assolutamente niente. Adesso fa due
    // cose che si spiegano da sole:
    //
    //   tienilo premuto e gira -> passo fine, finche' lo tieni;
    //   premi e lascia         -> riporta indietro quel parametro.
    //
    // Il ripristino scatta al rilascio e solo se nel frattempo non hai girato,
    // quindi i due gesti non si pestano i piedi per costruzione. Ed e' la cosa
    // piu' importante di tutto lo schema: un neofita gira una manopola che non
    // conosce solo se sa come tornare indietro.
    const int enc[4] = {Input::encDelta(0), Input::encDelta(1), Input::encDelta(2),
                        Input::encDelta(3)};
    if (enc[0] || enc[1] || enc[2] || enc[3]) touched(now);
    for (int e = 0; e < 4; ++e) {
        if (Input::encClick(e)) encTurned[e] = false;
        if (enc[e] != 0 && Input::encIsDown(e)) encTurned[e] = true;
    }

    // Le manopole seguono quello che hai davvero sotto gli occhi: quando il
    // sequencer scavalca il display, seguono lui.
    const bool recording = (Sequencer::mode() == Sequencer::SEQ_RECORDING);
    // Le manopole seguono la stessa schermata dei messaggi: quello che vedi,
    // quello che tocchi e quello che ti viene detto sono sempre la stessa pagina.
    const uint8_t uiScreen = visibleScreen;

    // Su RITMO il cursore c'e' sempre, e a giro fermo i tasti scrivono. Non si
    // "entra" piu' nello step edit: ci si e' dentro per il fatto di guardare la
    // griglia, che e' anche l'unico modo di non restarci dentro per sbaglio.
    const bool stepWrite =
        (uiScreen == SCREEN_RITMO) && (Sequencer::mode() == Sequencer::SEQ_IDLE);
    Sequencer::setEditing(stepWrite);

    // Le tre voci d'azione del menu non si premono: si tengono. Stesso patto di
    // SVUOTA — tenere vuol dire "lo sto facendo apposta" — e l'anello esterno si
    // riempie mentre tieni, cosi' mollare prima e' una scelta e non un incidente.
    const bool onMenuAction =
        (uiScreen == SCREEN_MENU) && Settings::isAction(settingsCursor);
    // Il latch e' quello che rende il gesto una pressione e non un ripetitore: una
    // volta scattato non riparte finche' non si molla. Senza, tenendo premuto si
    // rifarebbe la mappa dei LED ogni novecento millisecondi, con una scrittura in
    // NVS ogni volta.
    static bool holdFired = false;
    if (Input::encIsDown(1) && onMenuAction && !holdFired) {
        if (holdStartedAt == 0) holdStartedAt = now;
        const uint32_t held = now - holdStartedAt;
        confirmFill = (held >= FN_LONG_PRESS_SLOW_MS)
                          ? 255
                          : (uint8_t)(held * 255u / FN_LONG_PRESS_SLOW_MS);
        if (held >= FN_LONG_PRESS_SLOW_MS) {
            holdFired = true;
            holdStartedAt = 0;
            confirmFill = 0;
            encTurned[1] = true;  // il rilascio non deve valere come ripristino
            if (settingsCursor == SETTING_NET) {
                Storage::flush(snapshotState());  // niente va perso spegnendo l'audio
                Keylight::allOff();
                NetPortal::begin();
                return;
            }
            if (settingsCursor == SETTING_LEDLEARN) {
                AudioEngine::allNotesOff();
                for (int i = 0; i < MAX_VOICES; ++i) voiceSounding[i] = false;
                Keylight::startLearn();
                return;
            }
            if (settingsCursor == SETTING_LEDRESET) {
                Keylight::resetMap();
                Storage::saveLedMap(Keylight::map(), KEYLED_COUNT);
                toast("LUCI AZZERATE");
            }
        }
    } else {
        // Anche uscendo dalla riga d'azione col click ancora premuto: l'anello
        // rosso non deve restare appeso, e soprattutto il conto non deve
        // riprendere da dov'era se ci si torna sopra — un'azione distruttiva che
        // scatta all'istante perche' il timer era gia' carico e' esattamente
        // quello che la conferma esiste per impedire.
        holdStartedAt = 0;
        if (!Input::encIsDown(1)) holdFired = false;
    }
    holdFill = confirmFill;

    // Le didascalie si ricalcolano quando serve: se nessuna manopola si e' mossa e
    // non e' il momento di ridisegnare, non c'e' niente da dire e non c'e' niente
    // da applicare. Il loop gira a qualche kilohertz e quattro snprintf per giro
    // sarebbero lavoro buttato via.
    if (enc[0] || enc[1] || enc[2] || enc[3] ||
        (now - lastDisplayAt >= DISPLAY_INTERVAL_MS)) {
        applyKnobs(uiScreen, enc, now);
    }

    // Il ripristino, al rilascio e solo se non si e' girato.
    for (int e = 0; e < 4; ++e) {
        if (!Input::encRelease(e)) continue;
        if (encTurned[e]) continue;
        resetKnob(uiScreen, e);
    }

    // ------------------------------------------------ i tredici suoni
    // Sulla schermata SUONI i tasti smettono di essere note: ognuno fa partire il
    // suo campione, e basta. Non si accende nessuna voce del motore, quindi non
    // c'e' niente da rilasciare e non resta niente appeso — un campione ha un
    // inizio e una fine e se le gestisce da solo.
    //
    // La riga sta qui e non piu' in basso perche' la nota viva si decide subito
    // dopo: senza, premendo un tasto sarebbero partiti tutti e due, il suono e la
    // nota, e il latch avrebbe lasciato quest'ultima a suonare per sempre sotto.
    const bool memeMode = (uiScreen == SCREEN_SUONI);

    // ----------------------------------------- nota live (priorita' + arp)
    const int rawNote = Input::currentNote();
    const int heldCount = Input::heldCount();
    if (heldCount > 0) touched(now);
    const bool anyHeld = heldCount > 0;
    // Fra i suoni il tasto premuto non e' una nota, quindi non deve nemmeno
    // diventare "l'ultima nota tenuta": con TIENI inserito, uscendo dalla
    // schermata SUONI il synth avrebbe ripreso a suonare da solo un tasto che
    // nessuno aveva premuto come nota.
    if (rawNote >= 0 && !memeMode) latchedNote = (int8_t)rawNote;

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
    // Col piano o la batteria scelti fra i timbri, i tasti non pilotano piu' il
    // motore sottrattivo: fanno partire un campione. Vale la stessa soppressione
    // della schermata SUONI, e per gli stessi motivi.
    const bool sampledKeys = memeMode || (instrument != INSTR_SYNTH);
    if (sampledKeys) liveNote = -1;  // qui i tasti fanno i suoni, non le note
    prevAnyHeld = anyHeld;

    // -------------------------------- scrittura sul pattern (edit e record)
    int pressed;
    while ((pressed = Input::consumeNoteOn()) >= 0) {
        if (memeMode) {
            if (pressed < MEME_COUNT) {
                const MemeSample &m = MEME_SAMPLES[pressed];
                AudioEngine::playSample(m.data, m.len, MEME_RATE);
                memeLast = (int8_t)pressed;
            }
        } else if (instrument != INSTR_SYNTH) {
            // Il campione parte subito, e il tasto continua a scrivere sul
            // pattern come farebbe col synth: e' cosi' che una base di batteria
            // si registra suonandola invece di programmarla.
            playSampledKey(pressed, octave);
            if (stepWrite) {
                Sequencer::writeAtCursor(pressed, octave);
                Storage::markDirty();
            } else if (!arpActive) {
                Sequencer::noteEvent(now, pressed, octave);
                Storage::markDirty();
            }
        } else if (stepWrite) {
            // A giro fermo, sulla schermata RITMO, il tasto premuto suona **e**
            // scrive: il cursore avanza da solo, quindi una melodia si compone
            // premendo un tasto dopo l'altro e basta.
            Sequencer::writeAtCursor(pressed, octave);
            Storage::markDirty();
        } else if (!arpActive) {
            Sequencer::noteEvent(now, pressed, octave);
            Storage::markDirty();
        }
    }
    if (arpActive && arpRetrigger && liveNote >= 0 && !stepWrite) {
        Sequencer::noteEvent(now, liveNote, octave);
        Storage::markDirty();
    }

    // ------------------------------------------------------- sequencer
    Sequencer::update(now, false);

    const bool seqTrigger = Sequencer::consumeTrigger();
    const int seqNote = Sequencer::outputNote();

    // ------------------------------------------------------------- voci
    bool wantVoice[MAX_VOICES] = {false};
    float wantFreq[MAX_VOICES] = {0.0f};
    bool wantRetrig[MAX_VOICES] = {false};
    // I tasti non hanno sensori di forza: la dinamica e' piena, e solo le note
    // in arrivo dal MIDI portano la loro.
    float wantVel[MAX_VOICES];
    for (int i = 0; i < MAX_VOICES; ++i) wantVel[i] = 1.0f;

    if (sampledKeys) {
        // Quando i tasti fanno partire campioni, il motore sottrattivo non deve
        // suonarci sotto. Zittire la sola nota "viva" non bastava: in polifonico
        // ogni tasto ha una voce sua, che non passa da liveNote — quindi premendo
        // si sentivano insieme il campione e la nota, e con TIENI inserito
        // quest'ultima restava agganciata all'accordo e non si spegneva piu'.
        lastTarget = -1;
        lastWasLive = false;

        // La sequenza pero' **non** tace, se lo strumento e' campionato: e' il
        // punto di tutto: il pattern tiene la batteria mentre le mani fanno
        // altro. Solo sulla schermata SUONI resta muta, perche' li' i tredici
        // tasti sono effetti sonori e non c'e' niente da accompagnare.
        if (!memeMode && seqTrigger && seqNote >= 0) {
            playSampledKey(seqNote, Sequencer::outputOctave());
        }
    } else if (!polyMode) {
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

    // Le note che arrivano dal cavo occupano le voci rimaste. Il tasto fisico ha
    // la precedenza: se una voce serve a un dito, la nota MIDI che ci stava
    // sopra tace finche' il dito non si alza, invece di zittire chi sta suonando.
    for (int v = 0; v < NOTE_COUNT; ++v) {
        const int8_t n = midiNoteOfVoice[v];
        if (n < 0 || wantVoice[v]) continue;
        wantVoice[v] = true;
        wantFreq[v] = midiNoteFreq((uint8_t)n) * midiBend;
        wantVel[v] = midiVelOfVoice[v];
        wantRetrig[v] = midiRetrig[v];
    }
    for (int v = 0; v < NOTE_COUNT; ++v) midiRetrig[v] = false;

    // Lo specchio si aggiorna solo se l'evento e' stato davvero accodato: se la
    // coda fosse piena, il giro successivo ritenta invece di dare per scontato
    // un comando mai arrivato.
    for (int i = 0; i < MAX_VOICES; ++i) {
        if (!wantVoice[i]) {
            if (voiceSounding[i] && AudioEngine::voiceOff((uint8_t)i)) voiceSounding[i] = false;
            continue;
        }
        if (!voiceSounding[i] || wantRetrig[i]) {
            if (AudioEngine::voiceOn((uint8_t)i, wantFreq[i], wantVel[i])) {
                voiceSounding[i] = true;
                voiceFreq[i] = wantFreq[i];
            }
        } else if (fabsf(wantFreq[i] - voiceFreq[i]) > 0.01f) {
            // cambio ottava mentre la nota suona: reintono senza ritriggerare
            if (AudioEngine::voiceRetune((uint8_t)i, wantFreq[i])) voiceFreq[i] = wantFreq[i];
        }
    }

    // ------------------------------------------------------------ MIDI OUT
    {
        static int8_t outState = -1;
        const int8_t nowState = (!midiOutOn()) ? 0 : (MidiOut::connected() ? 2 : 1);
        if (nowState != outState) {
            outState = nowState;
            Serial.println(nowState == 0   ? F("MIDI OUT: disinserito nelle impostazioni.")
                           : nowState == 1 ? F("MIDI OUT: nessun host in ascolto.")
                                           : F("MIDI OUT: pronto."));
            uint8_t epIn = 0, epOut = 0;
            MidiIn::endpoints(epIn, epOut);
            Serial.print(F("MIDI: endpoint IN "));
            Serial.print(epIn);
            Serial.print(F(", OUT "));
            Serial.println(epOut);
        }
    }
    if (!midiOutOn() || !MidiOut::connected()) {
        midiOutAllOff();
    } else {
        for (int i = 0; i < MAX_VOICES; ++i) {
            // Le voci che stanno suonando *perche' arrivano dal MIDI* non
            // tornano indietro: con un DAW che rimanda in eco quello che riceve
            // si innescherebbe un anello.
            const bool fromMidi = (i < NOTE_COUNT) && (midiNoteOfVoice[i] >= 0);
            const int8_t want =
                (wantVoice[i] && !fromMidi) ? freqToMidiNote(wantFreq[i]) : (int8_t)-1;

            if (want == outNote[i]) {
                // Stessa nota che riparte (ribattuto, passo dell'arpeggiator):
                // va spenta e riaccesa, o dall'altra parte non si sente nulla.
                if (want >= 0 && wantRetrig[i]) {
                    MidiOut::noteOff((uint8_t)want);
                    MidiOut::noteOn((uint8_t)want, OUT_VELOCITY);
                }
                continue;
            }
            if (outNote[i] >= 0) MidiOut::noteOff((uint8_t)outNote[i]);
            if (want >= 0) MidiOut::noteOn((uint8_t)want, OUT_VELOCITY);
            outNote[i] = want;
        }

        // Le manopole diventano CC, ma solo quando il valore a 7 bit cambia
        // davvero: un encoder girato piano genera decine di variazioni che a
        // valle sono lo stesso numero.
        const uint8_t cc74 = (uint8_t)(clamp01(cutoffPos) * 127.0f + 0.5f);
        if (cc74 != outCcCutoff) {
            MidiOut::cc(74, cc74);
            outCcCutoff = cc74;
        }
        const uint8_t cc71 = (uint8_t)(resonance * 127.0f + 0.5f);
        if (cc71 != outCcRes) {
            MidiOut::cc(71, cc71);
            outCcRes = cc71;
        }
        const uint8_t cc7 = (uint8_t)(volume * 127.0f + 0.5f);
        if (cc7 != outCcVol) {
            MidiOut::cc(7, cc7);
            outCcVol = cc7;
        }

        // Trasporto e clock. Il clock lo mandiamo dal loop del core 1, che gira
        // ogni millisecondo scarso: il tremolio e' di quell'ordine, abbastanza
        // per accompagnare un DAW ma non per pretendere che ci accordi sopra un
        // disco.
        const bool running = (Sequencer::mode() == Sequencer::SEQ_PLAYING) || recording;
        if (running != outTransport) {
            outTransport = running;
            if (running) {
                MidiOut::start();
                outClockAt = now;
                outClockCount = 0;
            } else {
                MidiOut::stop();
            }
        }
        if (running && setIndex[SETTING_MIDIOUT] == MIDIOUT_NOTES_CLOCK) {
            // 24 impulsi per movimento, come da standard.
            const uint32_t period = (uint32_t)(60000.0f / (float)Sequencer::bpm() / 24.0f);
            const uint32_t due = (period < 1) ? 1 : period;
            while ((int32_t)(now - outClockAt) >= (int32_t)due && outClockCount < 64) {
                MidiOut::clock();
                outClockAt += due;
                ++outClockCount;
            }
            outClockCount = 0;
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
        if (polyMode) lv.fnActive |= 1u << FN_POLY;
        // L'invito: finche' nessuno ha suonato, i tredici tasti respirano. Si
        // spegne alla prima nota e torna dopo due minuti di silenzio assoluto —
        // che e' esattamente il momento in cui qualcun altro si e' avvicinato
        // allo strumento e non sa da dove cominciare.
        lv.invite = !everTouched || (now - lastTouchAt) > 120000;
        // Il battito del tempo su AVVIA, mezzo movimento acceso e mezzo spento.
        // Il resto prima della moltiplicazione, non dopo: now * bpm trabocca i
        // trentadue bit dopo cinque ore di accensione, e il metronomo comincerebbe
        // a zoppicare da solo proprio quando la scheda e' rimasta accesa tutto il
        // pomeriggio.
        // Mezzo movimento acceso e mezzo spento: il periodo del lampeggio e' il
        // movimento, quindi la mezza fase e' meta' di quello.
        const uint32_t halfBeatMs = 30000u / (uint32_t)Sequencer::bpm();
        lv.tempoPulse = (halfBeatMs > 0) && ((now / halfBeatMs) % 2 == 0);
        lv.memeMode = memeMode;
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
        view.attackMs = attackMs;
        view.decayMs = decayMs;
        view.sustain = sustainLevel;
        view.releaseMs = releaseMs;
        view.seqMode = (uint8_t)Sequencer::mode();
        view.seqStep = (uint8_t)Sequencer::currentStep();
        view.seqCursor = (uint8_t)Sequencer::cursor();
        view.seqEditing = stepWrite;
        view.seqNoteName = seqNoteName();
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
        view.fxCursor = fxCursor;
        for (int i = 0; i < FX_ROW_COUNT; ++i) {
            view.fxFrac[i] = fxFrac(i);
            fxValueText(i, view.fxValue[i], sizeof(view.fxValue[i]));
        }

        view.scaleName = Settings::valueLabel(SETTING_SCALE, setIndex[SETTING_SCALE]);
        view.rootName = Settings::rootName(setIndex[SETTING_ROOT]);
        view.expanderOk = Input::expanderOk();

        for (int i = 0; i < SETTING_COUNT; ++i) view.setIndex[i] = setIndex[i];
        view.setCursor = settingsCursor;
        view.holdFill = holdFill;
        view.timbro = setIndex[SETTING_TIMBRO];
        view.timbroCursor = timbroCursor;
        view.memeLast = memeLast;
        view.memePlaying = AudioEngine::samplesPlaying();
        view.ledLearn = false;
        view.ledLearnIndex = 0;

        // Novecento millisecondi: abbastanza per leggerlo con le mani occupate,
        // non tanto da restare sullo schermo mentre stai gia' facendo altro.
        view.flashRev = flashRev;
        if (now - flashAt < FLASH_MS && (flashLabel || flashValue)) {
            view.flashLabel = flashLabel;
            view.flashValue = flashValue;
            view.flashFrac = flashFrac;
        } else {
            view.flashLabel = nullptr;
            view.flashValue = nullptr;
            view.flashFrac = -1.0f;
        }

        Display::update(view);
    }
}
