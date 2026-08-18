// sim_main.cpp — il banco di prova: mette il synth in uno stato e fotografa lo schermo.
//
// Due scelte che vale la pena spiegare, perche' non sono ovvie.
//
// 1) Qui dentro si include src/display.cpp, il file vero. Non una copia, non un
//    estratto: il *file*. E' l'unica cosa che rende onesto tutto il resto — se
//    domani qualcuno sposta una fillRect di due pixel, i PNG cambiano senza che
//    nessuno debba ricordarsi di aggiornare il simulatore. In cambio si ottiene
//    anche l'accesso al namespace anonimo di display.cpp, quindi si possono
//    chiamare direttamente chrome() e splash(), che dall'esterno non esistono.
//
// 2) Ogni scena viene disegnata in un processo figlio suo. display.cpp e' pieno
//    di stato statico (l'ultima vista, l'ago del VU, la traccia precedente
//    dell'oscilloscopio, l'ultimo QR), e quello stato e' proprio cio' che si
//    vuole mettere alla prova: azzerarlo a mano vorrebbe dire scrivere una
//    seconda versione della logica di ridisegno, cioe' inventarsi la risposta.
//    Una fork() per scena e il problema non esiste: ogni PNG parte da un synth
//    appena acceso.
//
// Le scene "-dopo-uso" sono la ragione per cui questo strumento esiste. Il
// disegno completo lo sanno fare tutti; i difetti veri nascono al secondo
// aggiornamento, quando una clearBand troppo alta si mangia un pezzo di cornice
// che nessuno ridisegnera' piu' fino al prossimo cambio schermata.

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/display.cpp"  // il sorgente vero, mai adattato
#include "sim_fakes.h"

// ------------------------------------------------------------------ utilita'

namespace {

std::string gOutDir = ".";

// PPM binario a 8 bit per canale. La conversione da RGB565 replica i bit alti
// nei bassi (il modo classico): conserva esattamente la distinzione fra nero e
// non-nero, che e' l'unica cosa su cui check.py fa affidamento.
void savePpm(const std::string &path) {
    const uint16_t *fb = gfx->framebuffer();
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "impossibile scrivere %s\n", path.c_str());
        _exit(2);
    }
    fprintf(f, "P6\n240 240\n255\n");
    for (int i = 0; i < 240 * 240; ++i) {
        const uint16_t c = fb[i];
        const int r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
        const unsigned char px[3] = {(unsigned char)((r5 << 3) | (r5 >> 2)),
                                     (unsigned char)((g6 << 2) | (g6 >> 4)),
                                     (unsigned char)((b5 << 3) | (b5 >> 2))};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

// Accensione senza animazione di avvio: costruisce lo stesso pannello di
// Display::begin() ma salta splash(), che ha una scena sua.
void boot() {
    bus = new Arduino_ESP32SPI(PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_SCLK, PIN_TFT_MOSI,
                               GFX_NOT_DEFINED);
    gfx = new Arduino_GC9A01(bus, PIN_TFT_RST, 0, true);
    gfx->begin(40000000);
    gfx->fillScreen(BLACK);
    forceFull = true;
    prevValid = false;
    inSeqOverride = false;
    inLedLearn = false;
}

// Sceglie la schermata del ciclo senza passare da nextScreen(), che ci
// arriverebbe solo a forza di pressioni.
void gotoScreen(uint8_t which) {
    screen = which;
    forceFull = true;
    prevValid = false;
}

// Stato di partenza: il synth appena acceso, con i default di main.cpp.
SynthView baseView() {
    SynthView v{};
    v.waveform = WAVE_SINE;
    v.octave = 0;
    v.cutoffHz = 2000.0f;
    v.volume = 0.5f;
    v.attackMs = 10.0f;
    v.decayMs = 150.0f;
    v.sustain = 0.7f;
    v.releaseMs = 250.0f;
    v.seqMode = Sequencer::SEQ_IDLE;
    v.seqStep = 0;
    v.seqCursor = 0;
    v.seqEditing = false;
    v.seqNoteName = "PAUSA";
    v.countIn = 0;
    v.seqRev = 1;
    v.bpm = 120;
    v.hold = false;
    v.arp = false;
    v.arpMode = 0;
    v.arpName = "SU";
    v.poly = false;
    v.chordName = "SINGOLA";
    v.voices = 0;

    // Effetti: tutti a riposo, come all'accensione. Le stringhe non possono
    // restare nulle — il display le scrive senza chiedersi se esistono.
    v.resonance = 0.0f;
    v.crush = false;
    v.crushName = "8 BIT";
    v.delayMix = 0.0f;
    v.delayMs = 220.0f;
    v.lfoDepth = 0.0f;
    v.lfoRate = 5.0f;
    v.lfoTargetName = "SPENTO";
    v.drive = 0.0f;
    v.subLevel = 0.0f;
    v.detuneCents = 0.0f;
    v.glideMs = 0.0f;

    // L'elenco EFFETTI: i valori sono testo gia' formattato, come li compone
    // main.cpp. Le lunghezze vere contano — sono loro a decidere se la riga sta
    // dentro il cerchio.
    v.fxCursor = FX_ECO_MIX;
    static const char *const FX_DEMO[FX_ROW_COUNT] = {
        "8 BIT", "0 %",  "220 ms", "35 %", "SPENTO", "5.0 Hz", "0 %",
        "SU",    "0 %",  "0 ct",   "0 %",  "NO",     "0 %",    "300 ms"};
    for (int i = 0; i < FX_ROW_COUNT; ++i) {
        v.fxFrac[i] = 0.35f;
        snprintf(v.fxValue[i], sizeof(v.fxValue[i]), "%s", FX_DEMO[i]);
    }

    v.scaleName = "CROMAT.";
    v.rootName = "DO";
    v.expanderOk = true;
    v.ledLearn = false;
    v.ledLearnIndex = 0;
    v.holdFill = 0;
    v.flashLabel = nullptr;
    v.flashValue = nullptr;
    v.flashFrac = -1.0f;
    v.flashRev = 0;
    for (int i = 0; i < SETTING_COUNT; ++i) v.setIndex[i] = Settings::ENTRIES[i].byDefault;
    v.setCursor = 0;
    return v;
}

// Le quattro didascalie, che sul synth le riempie main.cpp. Senza, la corona
// resterebbe vuota e le scene non proverebbero la parte che regge tutto lo
// schema dei comandi.
void knobs(const char *a, const char *b, const char *c, const char *d, float fa = 0.35f,
           float fb = 0.5f, float fc = 0.5f, float fd = 0.7f) {
    Display::setKnob(0, a, "", fa, false);
    Display::setKnob(1, b, "", fb, false);
    Display::setKnob(2, c, "", fc, false);
    Display::setKnob(3, d, "", fd, false);
}

// Un aggiornamento come lo farebbe il loop: passa il tempo, poi si ridisegna.
void tick(const SynthView &v, uint32_t ms = 33) {
    simAdvanceMillis(ms);
    Display::update(v);
}

// ------------------------------------------------------------------- scene
//
// Ogni scena e' una funzione che lascia il framebuffer nello stato da salvare.
// Il nome del file e' il contratto con check.py: il prefisso dice la schermata,
// e i controlli sull'anello si saltano in base a quello.
//
// Le sette schermate dell'anello condividono ormai un telaio solo — corona di
// posizione, targhetta d'ottava, quattro archi con le loro didascalie — quindi
// ogni scena deve riempire anche quello: e' proprio il telaio la parte che una
// gomma sbagliata si porta via, ed e' la parte che non si ridisegna piu' fino al
// cambio di pagina.

// --- SUONA ------------------------------------------------------------------
void sceneSuona(uint8_t wave, bool poly, uint8_t voices, float cutoff, float res) {
    boot();
    gotoScreen(SCREEN_SUONA);
    Sim::setScope(wave, 0.8f);
    SynthView v = baseView();
    v.waveform = wave;
    v.poly = poly;
    v.voices = voices;
    v.cutoffHz = cutoff;
    v.resonance = res;
    knobs("ONDA", "TAGLIO", "VOL", "RISON.");
    tick(v);
}
void scene_suona_sine() { sceneSuona(WAVE_SINE, false, 1, 2000.0f, 0.0f); }
void scene_suona_square() { sceneSuona(WAVE_SQUARE, true, 4, 800.0f, 0.4f); }
void scene_suona_saw() { sceneSuona(WAVE_SAW, true, 8, 6000.0f, 0.9f); }
void scene_suona_noise() { sceneSuona(WAVE_NOISE, false, 0, 120.0f, 0.0f); }

void scene_suona_tutto_acceso() {
    boot();
    gotoScreen(SCREEN_SUONA);
    Sim::setScope(WAVE_SAW, 1.2f);  // oltre fondo scala: la traccia deve virare
    SynthView v = baseView();
    v.waveform = WAVE_SAW;
    v.arp = true;
    v.crush = true;
    v.hold = true;
    v.poly = true;
    v.voices = 12;
    v.octave = 2;
    knobs("ONDA", "TAGLIO", "VOL", "RISON.");
    tick(v);
}

void scene_suona_dopo_uso() {
    boot();
    gotoScreen(SCREEN_SUONA);
    Sim::setScope(WAVE_SINE, 0.6f);
    SynthView v = baseView();
    knobs("ONDA", "TAGLIO", "VOL", "RISON.");
    tick(v);
    // Dieci giri che toccano tutte le fasce dinamiche: la traccia, l'orizzonte
    // del filtro, la corona delle voci, la fila delle targhette e l'ottava.
    const uint8_t waves[10] = {WAVE_SQUARE, WAVE_SAW,   WAVE_TRIANGLE, WAVE_SINE, WAVE_PULSE,
                               WAVE_NOISE,  WAVE_SAW,   WAVE_SQUARE,   WAVE_SINE, WAVE_SAW};
    const int8_t octs[10] = {1, 2, -2, -1, 0, 2, -2, 1, 0, -1};
    for (int i = 0; i < 10; ++i) {
        v.waveform = waves[i];
        v.octave = octs[i];
        v.poly = (i % 2) == 0;
        v.voices = (uint8_t)(i + 3);
        v.cutoffHz = 200.0f + 700.0f * (float)i;
        v.resonance = 0.1f * (float)i;
        v.arp = (i % 3) == 0;
        v.crush = (i % 4) == 0;
        v.hold = (i % 5) == 0;
        Sim::setScope(waves[i], 0.4f + 0.08f * (float)i);
        tick(v);
    }
}

// --- TIMBRI -----------------------------------------------------------------
void sceneTimbri(uint8_t cursor, uint8_t loaded) {
    boot();
    gotoScreen(SCREEN_TIMBRI);
    SynthView v = baseView();
    v.timbroCursor = cursor;
    v.timbro = loaded;
    knobs("TIMBRO", "SCALA", "VOL", "ACCORDO");
    tick(v);
}
void scene_timbri_primo() { sceneTimbri(0, 0); }
// PAD SPAZIALE e CLAVICEMBALO sono i due nomi piu' lunghi: se un nome sborda dal
// vetro, sborda qui.
void scene_timbri_nome_lungo() { sceneTimbri(12, 1); }
void scene_timbri_ultimo() { sceneTimbri(PRESET_COUNT - 1, 3); }

void scene_timbri_dopo_uso() {
    boot();
    gotoScreen(SCREEN_TIMBRI);
    SynthView v = baseView();
    knobs("TIMBRO", "SCALA", "VOL", "ACCORDO");
    tick(v);
    for (int i = 0; i < (int)PRESET_COUNT; ++i) {
        v.timbroCursor = (uint8_t)i;
        v.timbro = (uint8_t)i;
        tick(v);
    }
}

// --- INVILUPPO --------------------------------------------------------------
void sceneInviluppo(float a, float d, float s, float r) {
    boot();
    gotoScreen(SCREEN_INVILUPPO);
    SynthView v = baseView();
    v.attackMs = a;
    v.decayMs = d;
    v.sustain = s;
    v.releaseMs = r;
    knobs("ATTAC", "DECAD", "SOST", "RILAS");
    tick(v);
}
void scene_inviluppo_min() { sceneInviluppo(2.0f, 5.0f, 0.0f, 10.0f); }
void scene_inviluppo_max() { sceneInviluppo(500.0f, 1000.0f, 1.0f, 2000.0f); }
void scene_inviluppo_medio() { sceneInviluppo(120.0f, 340.0f, 0.55f, 890.0f); }

void scene_inviluppo_dopo_uso() {
    boot();
    gotoScreen(SCREEN_INVILUPPO);
    SynthView v = baseView();
    knobs("ATTAC", "DECAD", "SOST", "RILAS");
    tick(v);
    for (int i = 0; i < 10; ++i) {
        v.attackMs = 2.0f + 50.0f * (float)i;
        v.decayMs = 5.0f + 100.0f * (float)i;
        v.sustain = 0.1f * (float)i;
        v.releaseMs = 10.0f + 200.0f * (float)i;
        tick(v);
    }
}

// --- EFFETTI ----------------------------------------------------------------
void sceneEffetti(uint8_t cursor) {
    boot();
    gotoScreen(SCREEN_EFFETTI);
    SynthView v = baseView();
    v.fxCursor = cursor;
    knobs("SCEGLI", FX_ROWS[cursor].label, "VOL", "-");
    tick(v);
}
void scene_effetti_prima() { sceneEffetti(FX_GRANA); }
// "PROFONDITA" e' l'etichetta piu' lunga dell'elenco, "VELOCITA'" la seconda.
void scene_effetti_etichetta_lunga() { sceneEffetti(FX_LFO_PROF); }
void scene_effetti_ultima() { sceneEffetti(FX_CHIUSURA); }

void scene_effetti_dopo_uso() {
    boot();
    gotoScreen(SCREEN_EFFETTI);
    SynthView v = baseView();
    tick(v);
    for (int i = 0; i < FX_ROW_COUNT; ++i) {
        v.fxCursor = (uint8_t)i;
        v.fxFrac[i] = (float)i / (float)(FX_ROW_COUNT - 1);
        knobs("SCEGLI", FX_ROWS[i].label, "VOL", "-");
        tick(v);
    }
}

// --- RITMO ------------------------------------------------------------------
void sceneRitmo(bool full, uint8_t mode, bool editing, uint16_t bpm, const char *nota) {
    boot();
    gotoScreen(SCREEN_RITMO);
    if (full) Sim::seqFill(); else Sim::seqClear();
    SynthView v = baseView();
    v.seqMode = mode;
    v.seqEditing = editing;
    v.seqNoteName = nota;
    v.bpm = bpm;
    v.seqStep = 5;
    v.seqCursor = 9;
    knobs("PASSO", "NOTA", "VOL", "TEMPO");
    tick(v);
}
void scene_ritmo_vuoto() { sceneRitmo(false, Sequencer::SEQ_IDLE, true, 120, "PAUSA"); }
void scene_ritmo_play() { sceneRitmo(true, Sequencer::SEQ_PLAYING, false, 240, "SOL#"); }
void scene_ritmo_rec() { sceneRitmo(true, Sequencer::SEQ_RECORDING, false, 40, "LEGATO"); }
void scene_ritmo_scrivi() { sceneRitmo(true, Sequencer::SEQ_IDLE, true, 137, "DO'"); }

void scene_ritmo_countin() {
    boot();
    gotoScreen(SCREEN_RITMO);
    Sim::seqFill();
    SynthView v = baseView();
    v.countIn = 3;
    v.seqMode = Sequencer::SEQ_COUNTIN;
    knobs("PASSO", "NOTA", "VOL", "TEMPO");
    tick(v);
}

void scene_ritmo_dopo_uso() {
    boot();
    gotoScreen(SCREEN_RITMO);
    Sim::seqFill();
    SynthView v = baseView();
    v.seqMode = Sequencer::SEQ_PLAYING;
    v.seqEditing = false;
    knobs("PASSO", "NOTA", "VOL", "TEMPO");
    tick(v);
    // La testina che gira per un giro intero piu' il tempo che cambia cifre: e'
    // il caso in cui la fascia del leggio e l'orbita si toccano piu' spesso.
    static const char *const NOMI[16] = {"DO",  "DO#", "RE",  "RE#", "MI",  "FA",  "FA#", "SOL",
                                         "SOL#", "LA", "LA#", "SI",  "DO'", "PAUSA", "LEGATO", "MI"};
    for (int i = 0; i < 16; ++i) {
        v.seqStep = (uint8_t)i;
        v.seqCursor = (uint8_t)((i * 7) % 16);
        v.seqNoteName = NOMI[i];
        v.bpm = (uint16_t)(40 + i * 13);
        tick(v);
    }
}

// --- LIVELLO ----------------------------------------------------------------
void sceneLivello(float rms, float peak) {
    boot();
    gotoScreen(SCREEN_LIVELLO);
    Sim::setLevels(rms, peak);
    SynthView v = baseView();
    knobs("-", "-", "VOL", "-");
    tick(v);
}
void scene_livello_zero() { sceneLivello(0.0f, 0.0f); }
void scene_livello_meta() { sceneLivello(0.1f, 0.2f); }
void scene_livello_clip() { sceneLivello(0.99f, 1.0f); }

void scene_livello_dopo_uso() {
    boot();
    gotoScreen(SCREEN_LIVELLO);
    SynthView v = baseView();
    knobs("-", "-", "VOL", "-");
    Sim::setLevels(0.0f, 0.0f);
    tick(v);
    const float lv[10] = {0.02f, 0.3f, 0.9f, 0.05f, 0.6f, 1.0f, 0.001f, 0.4f, 0.75f, 0.15f};
    for (int i = 0; i < 10; ++i) {
        Sim::setLevels(lv[i], lv[i]);
        tick(v);
    }
}

// --- MENU -------------------------------------------------------------------
void sceneMenu(uint8_t cursor) {
    boot();
    gotoScreen(SCREEN_MENU);
    SynthView v = baseView();
    v.setCursor = cursor;
    knobs("SCEGLI", Settings::isAction(cursor) ? "TIENI" : Settings::ENTRIES[cursor].label,
          "VOL", "-");
    tick(v);
}
void scene_menu_prima() { sceneMenu(SETTING_VOL); }
// "LRC BCK DIN" e' il valore piu' lungo del menu, "MODALITA' WIFI" l'etichetta.
void scene_menu_uscita_audio() { sceneMenu(SETTING_AUDIO); }
void scene_menu_azione_wifi() { sceneMenu(SETTING_NET); }
void scene_menu_azione_luci() { sceneMenu(SETTING_LEDLEARN); }

void scene_menu_conferma_a_meta() {
    boot();
    gotoScreen(SCREEN_MENU);
    SynthView v = baseView();
    v.setCursor = SETTING_NET;
    v.holdFill = 160;  // l'anello esterno che si sta caricando
    knobs("SCEGLI", "TIENI", "VOL", "-");
    tick(v);
}

void scene_menu_dopo_uso() {
    boot();
    gotoScreen(SCREEN_MENU);
    SynthView v = baseView();
    tick(v);
    for (int i = 0; i < SETTING_MENU_COUNT; ++i) {
        v.setCursor = (uint8_t)i;
        if (!Settings::isAction(i)) {
            v.setIndex[i] = (uint8_t)(Settings::valueCount(i) - 1);
        }
        knobs("SCEGLI", Settings::isAction(i) ? "TIENI" : Settings::ENTRIES[i].label, "VOL", "-");
        tick(v);
    }
}

// --- overlay ----------------------------------------------------------------
//
// La banda che vibrava. Qui conta soprattutto la scena "dopo-uso": la banda
// compare, scade, e cio' che resta a schermo deve essere una schermata intera —
// senza il buco della banda e senza i due monconi colorati ai fianchi che il
// vecchio riquadro, largo piu' di quanto la gomma riuscisse a cancellare,
// lasciava appesi fino al cambio di pagina.
void sceneOverlay(uint8_t scr, const char *label, const char *value, float frac) {
    boot();
    gotoScreen(scr);
    SynthView v = baseView();
    knobs("ONDA", "TAGLIO", "VOL", "RISON.");
    tick(v);
    v.flashLabel = label;
    v.flashValue = value;
    v.flashFrac = frac;
    v.flashRev = 1;
    tick(v);
}
void scene_overlay_corto() { sceneOverlay(SCREEN_SUONA, nullptr, "SILENZIO", -1.0f); }
void scene_overlay_lungo() { sceneOverlay(SCREEN_MENU, "USCITA AUDIO", "LRC BCK DIN", -1.0f); }
void scene_overlay_barra() { sceneOverlay(SCREEN_INVILUPPO, "SOSTEGNO", "COM'ERA", 0.62f); }

void scene_overlay_scaduto() {
    boot();
    gotoScreen(SCREEN_SUONA);
    Sim::setScope(WAVE_SAW, 0.7f);
    SynthView v = baseView();
    v.waveform = WAVE_SAW;
    knobs("ONDA", "TAGLIO", "VOL", "RISON.");
    tick(v);
    v.flashLabel = "TAGLIO";
    v.flashValue = "COM'ERA";
    v.flashRev = 1;
    tick(v);
    tick(v);
    // La banda scade: da qui in poi non deve restare niente di lei.
    v.flashLabel = nullptr;
    v.flashValue = nullptr;
    tick(v);
    tick(v);
}

// --- fuori dall'anello ------------------------------------------------------
void sceneLuci(uint8_t index) {
    boot();
    SynthView v = baseView();
    v.ledLearn = true;
    v.ledLearnIndex = index;
    tick(v);
}
void scene_luci_impara_prima() { sceneLuci(0); }
void scene_luci_impara_ultima() { sceneLuci(19); }

void scene_splash() {
    // Display::begin() vero, animazione compresa: le delay() non fermano nulla,
    // quindi quello che resta nel framebuffer e' l'ultimo fotogramma.
    Display::begin();
}

// --- rete e aggiornamento ---------------------------------------------------
// Pagine fuori dall'anello: nessun telaio radiale, si ridisegnano tutte ogni
// volta. Non sono cambiate, ma restano nel banco perche' il QR e gli indirizzi
// lunghi sono ancora il testo piu' largo che il firmware scriva.
void sceneNet(uint8_t stage, const char *qr, const char *msg, const char *ip) {
    boot();
    Sim::netSet(stage, qr, msg, ip);
    Display::updateNetwork();
}
void scene_net_scan() { sceneNet(NetPortal::NET_SCAN, "", "cerco le reti", ""); }
// La schermata su cui il telefono smette di servire: il synth si e' ricollegato
// da solo, ha letto il manifest e aspetta solo che qualcuno tenga premuto AVVIA.
void scene_net_aggiornamento() {
    boot();
    Sim::netSetUpdate(true, "2.9.0");
    Sim::netSet(NetPortal::NET_STA_OK, "http://192.168.178.123/", "aggiornamento pronto",
                "192.168.178.123");
    Display::updateNetwork();
}
void scene_net_attesa() {
    sceneNet(NetPortal::NET_AP, "WIFI:T:WPA;S:ArcadeVox-3C4A;P:arcade3C4A;;",
             "in attesa del telefono", "");
}
void scene_net_portale() {
    sceneNet(NetPortal::NET_CONNECTED, "http://192.168.4.1/", "apri il portale", "");
}
void scene_net_in_rete() {
    // SSID lungo come glielo consente lo standard: e' il caso in cui il nome
    // della rete, scritto per intero, uscirebbe dal vetro.
    Sim::netSetSsid("ReteDiCasaMoltoLungaWiFi5G");
    sceneNet(NetPortal::NET_STA_OK, "http://192.168.178.123/", "gia' aggiornato",
             "192.168.178.123");
}
void scene_net_fallito() {
    sceneNet(NetPortal::NET_FAILED, "", "manifest non raggiungibile", "");
}
void sceneOta(int pct) {
    boot();
    Display::drawOtaProgress(pct);
}
void scene_ota_0() { sceneOta(0); }
void scene_ota_50() { sceneOta(50); }
void scene_ota_100() { sceneOta(100); }

struct Scene {
    const char *file;
    void (*fn)();
};

const Scene SCENES[] = {
    {"01-suona-sine", scene_suona_sine},
    {"02-suona-square-poli", scene_suona_square},
    {"03-suona-saw-risonanza", scene_suona_saw},
    {"04-suona-noise", scene_suona_noise},
    {"05-suona-tutto-acceso", scene_suona_tutto_acceso},
    {"06-suona-dopo-uso", scene_suona_dopo_uso},

    {"07-timbri-primo", scene_timbri_primo},
    {"08-timbri-nome-lungo", scene_timbri_nome_lungo},
    {"09-timbri-ultimo", scene_timbri_ultimo},
    {"10-timbri-dopo-uso", scene_timbri_dopo_uso},

    {"11-inviluppo-min", scene_inviluppo_min},
    {"12-inviluppo-max", scene_inviluppo_max},
    {"13-inviluppo-medio", scene_inviluppo_medio},
    {"14-inviluppo-dopo-uso", scene_inviluppo_dopo_uso},

    {"15-effetti-prima", scene_effetti_prima},
    {"16-effetti-etichetta-lunga", scene_effetti_etichetta_lunga},
    {"17-effetti-ultima", scene_effetti_ultima},
    {"18-effetti-dopo-uso", scene_effetti_dopo_uso},

    {"19-ritmo-vuoto", scene_ritmo_vuoto},
    {"20-ritmo-play-bpm3cifre", scene_ritmo_play},
    {"21-ritmo-rec-bpm2cifre", scene_ritmo_rec},
    {"22-ritmo-scrivi", scene_ritmo_scrivi},
    {"23-ritmo-countin", scene_ritmo_countin},
    {"24-ritmo-dopo-uso", scene_ritmo_dopo_uso},

    {"25-livello-zero", scene_livello_zero},
    {"26-livello-meta", scene_livello_meta},
    {"27-livello-clip", scene_livello_clip},
    {"28-livello-dopo-uso", scene_livello_dopo_uso},

    {"29-menu-prima", scene_menu_prima},
    {"30-menu-uscita-audio", scene_menu_uscita_audio},
    {"31-menu-azione-wifi", scene_menu_azione_wifi},
    {"32-menu-azione-luci", scene_menu_azione_luci},
    {"33-menu-conferma-a-meta", scene_menu_conferma_a_meta},
    {"34-menu-dopo-uso", scene_menu_dopo_uso},

    {"35-overlay-corto", scene_overlay_corto},
    {"36-overlay-lungo", scene_overlay_lungo},
    {"37-overlay-barra", scene_overlay_barra},
    {"38-overlay-scaduto", scene_overlay_scaduto},

    {"39-luci-impara-primo", scene_luci_impara_prima},
    {"40-luci-impara-ultimo", scene_luci_impara_ultima},

    {"41-network-scan", scene_net_scan},
    {"41c-network-qr-aggiornamento", scene_net_aggiornamento},
    {"41b-network-qr-rete", scene_net_attesa},
    {"42-network-qr-indirizzo", scene_net_portale},
    {"43-network-in-rete-ip-lungo", scene_net_in_rete},
    {"44-network-ota-0", scene_ota_0},
    {"45-network-ota-50", scene_ota_50},
    {"46-network-ota-100", scene_ota_100},
    {"47-network-fallito", scene_net_fallito},

    {"48-splash", scene_splash},
};

constexpr int SCENE_COUNT = (int)(sizeof(SCENES) / sizeof(SCENES[0]));

}  // namespace



int main(int argc, char **argv) {
    if (argc > 1) gOutDir = argv[1];
    mkdir(gOutDir.c_str(), 0755);

    int failed = 0;
    for (int i = 0; i < SCENE_COUNT; ++i) {
        const std::string path = gOutDir + "/" + SCENES[i].file + ".ppm";
        const pid_t pid = fork();
        if (pid == 0) {
            SCENES[i].fn();
            savePpm(path);
            if (gfxSimClippedWrites > 0) {
                fprintf(stderr, "%s: %ld pixel scritti fuori dai 240x240\n", SCENES[i].file,
                        gfxSimClippedWrites);
            }
            _exit(0);
        } else if (pid < 0) {
            fprintf(stderr, "fork fallita per %s\n", SCENES[i].file);
            ++failed;
        } else {
            int st = 0;
            waitpid(pid, &st, 0);
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
                fprintf(stderr, "scena %s terminata male\n", SCENES[i].file);
                ++failed;
            } else {
                printf("%s\n", SCENES[i].file);
            }
        }
    }
    fprintf(stderr, "scene: %d, fallite: %d\n", SCENE_COUNT, failed);
    return failed ? 1 : 0;
}
