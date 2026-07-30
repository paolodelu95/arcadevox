// sim_main.cpp — il banco di prova: mette il synth in uno stato e fotografa lo schermo.
//
// Due scelte che vale la pena spiegare, perche' non sono ovvie.
//
// 1) Qui dentro si include src/display.cpp, il file vero. Non una copia, non un
//    estratto: il *file*. E' l'unica cosa che rende onesto tutto il resto — se
//    domani qualcuno sposta una fillRect di due pixel, i PNG cambiano senza che
//    nessuno debba ricordarsi di aggiornare il simulatore. In cambio si ottiene
//    anche l'accesso al namespace anonimo di display.cpp, quindi si possono
//    chiamare direttamente chrome(), splash() e drawNetworkIdleScreen(), che
//    dall'esterno non esistono.
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
    inAdsrScreen = false;
    inSeqOverride = false;
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
    v.adsrEdit = false;
    v.attackMs = 10.0f;
    v.decayMs = 120.0f;
    v.sustain = 0.7f;
    v.releaseMs = 200.0f;
    v.seqMode = Sequencer::SEQ_IDLE;
    v.seqStep = 0;
    v.seqCursor = 0;
    v.seqEditing = false;
    v.countIn = 0;
    v.seqRev = 1;
    v.bpm = 120;
    v.hold = false;
    v.arp = false;
    v.poly = false;
    v.voices = 0;
    for (int i = 0; i < SETTING_COUNT; ++i) v.setIndex[i] = Settings::ENTRIES[i].byDefault;
    v.setCursor = 0;
    v.setEditing = false;
    v.clearedAgo = 0;
    return v;
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

// --- OSC --------------------------------------------------------------------
void sceneOsc(uint8_t wave, bool poly, uint8_t voices) {
    boot();
    gotoScreen(0);
    SynthView v = baseView();
    v.waveform = wave;
    v.poly = poly;
    v.voices = voices;
    tick(v);
}
void scene_osc_sine() { sceneOsc(WAVE_SINE, false, 1); }
void scene_osc_square() { sceneOsc(WAVE_SQUARE, true, 4); }
void scene_osc_saw() { sceneOsc(WAVE_SAW, true, 8); }
void scene_osc_triangle() { sceneOsc(WAVE_TRIANGLE, false, 0); }

void scene_osc_dopo_uso() {
    boot();
    gotoScreen(0);
    SynthView v = baseView();
    tick(v);  // disegno completo
    // Sette aggiornamenti che toccano tutte le fasce dinamiche della schermata:
    // l'icona dell'onda, il nome, le caselle delle voci, la riga mono/poli.
    const uint8_t waves[7] = {WAVE_SQUARE, WAVE_SAW,      WAVE_TRIANGLE, WAVE_SINE,
                              WAVE_SAW,    WAVE_TRIANGLE, WAVE_SQUARE};
    const uint8_t voices[7] = {1, 4, 8, 0, 3, 8, 2};
    for (int i = 0; i < 7; ++i) {
        v.waveform = waves[i];
        v.poly = (i % 2) == 0;
        v.voices = voices[i];
        tick(v);
    }
}

// --- OCTAVE -----------------------------------------------------------------
void sceneOctave(int8_t oct) {
    boot();
    gotoScreen(1);
    SynthView v = baseView();
    v.octave = oct;
    tick(v);
}
void scene_octave_min() { sceneOctave(-2); }
void scene_octave_zero() { sceneOctave(0); }
void scene_octave_max() { sceneOctave(2); }

void scene_octave_dopo_uso() {
    boot();
    gotoScreen(1);
    SynthView v = baseView();
    tick(v);
    const int8_t seq[8] = {1, 2, -2, -1, 0, 2, -2, 1};
    for (int i = 0; i < 8; ++i) {
        v.octave = seq[i];
        tick(v);
    }
}

// --- LEVELS -----------------------------------------------------------------
void sceneLevels(float cutoff, float vol) {
    boot();
    gotoScreen(2);
    SynthView v = baseView();
    v.cutoffHz = cutoff;
    v.volume = vol;
    tick(v);
}
void scene_levels_min() { sceneLevels(80.0f, 0.0f); }
void scene_levels_max() { sceneLevels(8000.0f, 1.0f); }
void scene_levels_medio() { sceneLevels(1234.0f, 0.55f); }

void scene_levels_dopo_uso() {
    boot();
    gotoScreen(2);
    SynthView v = baseView();
    tick(v);
    // Dal minimo al massimo e ritorno: il numero passa da 2 a 4 cifre e la
    // fascia di pulizia del valore deve reggere entrambi senza lasciare code.
    const float cut[7] = {80.0f, 8000.0f, 440.0f, 8000.0f, 95.0f, 3300.0f, 80.0f};
    const float vol[7] = {0.0f, 1.0f, 0.07f, 1.0f, 0.5f, 0.0f, 1.0f};
    for (int i = 0; i < 7; ++i) {
        v.cutoffHz = cut[i];
        v.volume = vol[i];
        tick(v);
    }
}

// --- SEQUENCER --------------------------------------------------------------
void scene_seq_vuoto() {
    boot();
    gotoScreen(3);
    Sim::seqClear();
    SynthView v = baseView();
    tick(v);
}

void scene_seq_pieno_play() {
    boot();
    gotoScreen(3);
    Sim::seqFill();
    SynthView v = baseView();
    v.seqMode = Sequencer::SEQ_PLAYING;
    v.seqStep = 6;
    v.bpm = 240;  // tre cifre
    tick(v);
}

void scene_seq_rec() {
    boot();
    gotoScreen(3);
    Sim::seqFill();
    SynthView v = baseView();
    v.seqMode = Sequencer::SEQ_RECORDING;
    v.seqStep = 12;
    v.bpm = 40;  // due cifre
    tick(v);
}

void scene_seq_stepedit() {
    boot();
    Sim::seqFill();
    Sim::seqSetStep(9, SEQ_TIE, 0);
    SynthView v = baseView();
    v.seqEditing = true;
    v.seqCursor = 4;  // una nota vera: la riga di dettaglio scrive "05  GAT +2"
    v.seqStep = 4;
    v.seqMode = Sequencer::SEQ_PLAYING;
    tick(v);
}

void scene_seq_stepedit_legato() {
    boot();
    Sim::seqFill();
    Sim::seqSetStep(9, SEQ_TIE, 0);
    SynthView v = baseView();
    v.seqEditing = true;
    v.seqCursor = 9;  // legato: la riga di dettaglio e' la piu' lunga
    v.seqStep = 2;
    v.seqMode = Sequencer::SEQ_PLAYING;
    tick(v);
}

void scene_seq_hold_arp() {
    boot();
    gotoScreen(3);
    Sim::seqFill();
    SynthView v = baseView();
    v.seqMode = Sequencer::SEQ_PLAYING;
    v.hold = true;
    v.arp = true;
    v.poly = true;
    v.bpm = 176;
    v.seqStep = 15;
    tick(v);
}

void scene_seq_countin() {
    boot();
    Sim::seqFill();
    SynthView v = baseView();
    v.countIn = 4;
    v.seqMode = Sequencer::SEQ_COUNTIN;
    tick(v);
}

void scene_seq_svuotato() {
    boot();
    gotoScreen(3);
    Sim::seqClear();
    SynthView v = baseView();
    tick(v);
    v.clearedAgo = 400;  // dentro il secondo e mezzo di conferma
    tick(v);
}

void scene_seq_dopo_uso() {
    boot();
    gotoScreen(3);
    Sim::seqFill();
    SynthView v = baseView();
    tick(v);
    // La testina che cammina, il tempo che cambia numero di cifre, le targhette
    // che si accendono e si spengono: tutto quello che ripulisce una fascia.
    const uint16_t bpms[8] = {120, 40, 240, 99, 100, 240, 88, 176};
    for (int i = 0; i < 8; ++i) {
        v.seqMode = (i < 4) ? Sequencer::SEQ_PLAYING : Sequencer::SEQ_IDLE;
        v.seqStep = (uint8_t)((i * 3) % SEQ_STEPS);
        v.bpm = bpms[i];
        v.hold = (i % 2) == 0;
        v.arp = (i % 3) == 0;
        v.poly = (i % 2) == 1;
        tick(v);
    }
}

// --- ADSR -------------------------------------------------------------------
void sceneAdsr(float a, float d, float s, float r) {
    boot();
    SynthView v = baseView();
    v.adsrEdit = true;
    v.attackMs = a;
    v.decayMs = d;
    v.sustain = s;
    v.releaseMs = r;
    tick(v);
}
void scene_adsr_min() { sceneAdsr(2.0f, 5.0f, 0.0f, 10.0f); }
void scene_adsr_max() { sceneAdsr(500.0f, 1000.0f, 1.0f, 2000.0f); }

void scene_adsr_dopo_uso() {
    boot();
    SynthView v = baseView();
    v.adsrEdit = true;
    tick(v);
    const float a[7] = {2.0f, 500.0f, 37.0f, 500.0f, 2.0f, 128.0f, 499.0f};
    const float d[7] = {5.0f, 1000.0f, 250.0f, 5.0f, 999.0f, 40.0f, 1000.0f};
    const float s[7] = {0.0f, 1.0f, 0.33f, 1.0f, 0.0f, 0.9f, 1.0f};
    const float r[7] = {10.0f, 2000.0f, 750.0f, 10.0f, 1999.0f, 55.0f, 2000.0f};
    for (int i = 0; i < 7; ++i) {
        v.attackMs = a[i];
        v.decayMs = d[i];
        v.sustain = s[i];
        v.releaseMs = r[i];
        tick(v);
    }
}

// --- SETTINGS ---------------------------------------------------------------
void sceneSettings(bool editing, uint8_t cursor) {
    boot();
    gotoScreen(SCREEN_SETTINGS);
    SynthView v = baseView();
    v.setEditing = editing;
    v.setCursor = cursor;
    tick(v);
}
void scene_settings_chiuso() { sceneSettings(false, 0); }
void scene_settings_riga0() { sceneSettings(true, 0); }
void scene_settings_riga1() { sceneSettings(true, 1); }
void scene_settings_riga2() { sceneSettings(true, 2); }
void scene_settings_riga3() { sceneSettings(true, 3); }
void scene_settings_riga4() { sceneSettings(true, 4); }

void scene_settings_valori_estremi() {
    boot();
    gotoScreen(SCREEN_SETTINGS);
    SynthView v = baseView();
    v.setEditing = true;
    v.setCursor = 3;
    // Etichette piu' lunghe di ogni voce: "5.0 giri" e "1/16", scritte a size 2
    // e allineate a destra, sono il caso in cui il valore rischia la cornice.
    v.setIndex[SETTING_VOL] = 0;
    v.setIndex[SETTING_CUTOFF] = 5;
    v.setIndex[SETTING_ADSR] = 0;
    v.setIndex[SETTING_FINE] = 3;
    tick(v);
}

void scene_settings_dopo_uso() {
    boot();
    gotoScreen(SCREEN_SETTINGS);
    SynthView v = baseView();
    tick(v);          // fuori dal menu
    v.setEditing = true;
    tick(v);          // entrata nel menu: ridisegno completo
    // Il cursore scende su ogni riga e i valori girano: e' lo scorrimento che si
    // vede al banco, comprese le intestazioni di categoria che non devono
    // sparire quando la riga sotto si ripulisce.
    for (int i = 0; i < SETTING_COUNT; ++i) {
        v.setCursor = (uint8_t)i;
        if (!Settings::isAction((uint8_t)i)) {
            v.setIndex[i] = (uint8_t)(Settings::ENTRIES[i].count - 1);
        }
        tick(v);
    }
    v.setCursor = 0;
    v.setIndex[SETTING_VOL] = 0;
    tick(v);
    v.setEditing = false;
    tick(v);
}

// --- VU ---------------------------------------------------------------------
void sceneVu(float rms, float peak) {
    boot();
    gotoScreen(SCREEN_VU);
    Sim::setLevels(rms, peak);
    SynthView v = baseView();
    tick(v);
    // L'ago si muove solo se il livello e' cambiato rispetto all'ultimo
    // fotogramma: un secondo giro lo porta nella posizione giusta anche quando
    // il primo l'ha trovato gia' a zero.
    tick(v);
}
// -40 dBFS e' fondo scala sinistro: l'ago sta a riposo.
void scene_vu_zero() { sceneVu(0.0f, 0.0f); }
// -20 dBFS: meta' corsa esatta.
void scene_vu_meta() { sceneVu(0.1f, 0.15f); }
// 0 dBFS con il picco in clip: ago a fondo, spia rossa accesa.
void scene_vu_fondo() { sceneVu(1.0f, 1.0f); }

void scene_vu_dopo_uso() {
    boot();
    gotoScreen(SCREEN_VU);
    Sim::setLevels(0.0f, 0.0f);
    SynthView v = baseView();
    tick(v);
    const float rms[8] = {0.02f, 0.3f, 0.9f, 0.05f, 0.5f, 1.0f, 0.01f, 0.25f};
    const float pk[8] = {0.1f, 0.5f, 1.0f, 0.2f, 0.7f, 1.0f, 0.05f, 0.4f};
    for (int i = 0; i < 8; ++i) {
        Sim::setLevels(rms[i], pk[i]);
        tick(v, 200);  // 200 ms: il picco fa in tempo a scendere fra un colpo e l'altro
    }
}

// --- SCOPE ------------------------------------------------------------------
void sceneScope(uint8_t wave, float amp) {
    boot();
    gotoScreen(SCREEN_SCOPE);
    Sim::setScope(wave, amp);
    SynthView v = baseView();
    v.waveform = wave;
    tick(v);
}
void scene_scope_silenzio() { sceneScope(WAVE_SINE, 0.0f); }
void scene_scope_pieno() { sceneScope(WAVE_SQUARE, 127.0f); }

void scene_scope_dopo_uso() {
    boot();
    gotoScreen(SCREEN_SCOPE);
    Sim::setScope(WAVE_SINE, 60.0f);
    SynthView v = baseView();
    tick(v);
    const uint8_t w[7] = {WAVE_SQUARE, WAVE_SAW, WAVE_TRIANGLE, WAVE_SINE,
                          WAVE_SAW,    WAVE_SQUARE, WAVE_TRIANGLE};
    const float a[7] = {127.0f, 20.0f, 100.0f, 4.0f, 127.0f, 60.0f, 15.0f};
    for (int i = 0; i < 7; ++i) {
        Sim::setScope(w[i], a[i]);
        v.waveform = w[i];
        tick(v);
    }
}

// --- NETWORK ----------------------------------------------------------------
void scene_net_idle() {
    boot();
    SynthView v = baseView();
    drawNetworkIdleScreen(v, true);
}

void scene_net_attesa() {
    boot();
    // Il QR di aggancio alla rete, con SSID e password di lunghezza vera.
    Sim::netSet(NetPortal::NET_AP, "WIFI:S:ArcadeVox-7C3A;T:WPA;P:arcade9F7C;;",
                "in attesa del telefono", "");
    Display::updateNetwork();
}

void scene_net_portale() {
    boot();
    Sim::netSet(NetPortal::NET_CONNECTED, "http://192.168.4.1/", "apri il portale", "");
    Display::updateNetwork();
}

void scene_net_in_rete() {
    boot();
    // Indirizzo lungo: "in rete: 192.168.100.237" e' la riga piu' larga che
    // questa schermata possa scrivere.
    Sim::netSet(NetPortal::NET_STA_OK, "http://192.168.100.237/", "collegato a internet",
                "192.168.100.237");
    Display::updateNetwork();
}

void scene_net_dopo_uso() {
    boot();
    Sim::netSet(NetPortal::NET_AP, "WIFI:S:ArcadeVox-7C3A;T:WPA;P:arcade9F7C;;",
                "in attesa del telefono", "");
    Display::updateNetwork();
    // La sequenza vera di una sessione: aggancio, portale, tentativo verso casa,
    // rientro riuscito. Il QR cambia una volta sola, tutto il resto e' riscrittura
    // della sola fascia di stato — che e' esattamente dove si nascondono i guai.
    Sim::netSet(NetPortal::NET_CONNECTED, "http://192.168.4.1/", "apri il portale", "");
    Display::updateNetwork();
    Sim::netSet(NetPortal::NET_STA_WAIT, "http://192.168.4.1/", "collegamento in corso", "");
    Display::updateNetwork();
    Sim::netSet(NetPortal::NET_STA_WAIT, "http://192.168.4.1/", "mi ricollego alla rete", "");
    Display::updateNetwork();
    Sim::netSet(NetPortal::NET_STA_FAIL, "http://192.168.4.1/", "rete non raggiunta", "");
    Display::updateNetwork();
    Sim::netSet(NetPortal::NET_STA_OK, "http://192.168.4.1/", "collegato a internet",
                "192.168.100.237");
    Display::updateNetwork();
    Sim::netSet(NetPortal::NET_STA_OK, "http://192.168.4.1/", "scarico dalla rete",
                "192.168.100.237");
    Display::updateNetwork();
}

void sceneOta(int pct) {
    boot();
    // La barra parte sempre da un disegno completo: drawOtaProgress ricostruisce
    // la cornice quando la percentuale torna indietro, e a freddo lastPct e' -1.
    Display::drawOtaProgress(0);
    if (pct > 0) {
        for (int p = 1; p <= pct; ++p) Display::drawOtaProgress(p);
    }
}
void scene_ota_0() { sceneOta(0); }
void scene_ota_50() { sceneOta(50); }
void scene_ota_100() { sceneOta(100); }

void scene_net_fallito() {
    boot();
    // Il messaggio piu' lungo che net_portal.cpp puo' mettere in stato di errore.
    Sim::netSet(NetPortal::NET_FAILED, "", "trasferimento interrotto", "");
    Display::updateNetwork();
}

// --- SPLASH -----------------------------------------------------------------
void scene_splash() {
    // Display::begin() vero, animazione compresa: le delay() non fermano nulla,
    // quindi quello che resta nel framebuffer e' l'ultimo fotogramma.
    Display::begin();
}

// ------------------------------------------------------------------ elenco
struct Scene {
    const char *file;
    void (*fn)();
};

const Scene SCENES[] = {
    {"01-osc-sine-mono-1voce", scene_osc_sine},
    {"02-osc-square-poli-4voci", scene_osc_square},
    {"03-osc-saw-poli-8voci", scene_osc_saw},
    {"04-osc-triangle-mono-0voci", scene_osc_triangle},
    {"05-osc-dopo-uso", scene_osc_dopo_uso},

    {"06-octave-min", scene_octave_min},
    {"07-octave-zero", scene_octave_zero},
    {"08-octave-max", scene_octave_max},
    {"09-octave-dopo-uso", scene_octave_dopo_uso},

    {"10-levels-min", scene_levels_min},
    {"11-levels-max", scene_levels_max},
    {"12-levels-medio", scene_levels_medio},
    {"13-levels-dopo-uso", scene_levels_dopo_uso},

    {"14-seq-vuoto", scene_seq_vuoto},
    {"15-seq-pieno-play-bpm3cifre", scene_seq_pieno_play},
    {"16-seq-rec-bpm2cifre", scene_seq_rec},
    {"17-seq-stepedit-nota", scene_seq_stepedit},
    {"18-seq-stepedit-legato", scene_seq_stepedit_legato},
    {"19-seq-hold-arp-poli", scene_seq_hold_arp},
    {"20-seq-countin", scene_seq_countin},
    {"21-seq-svuotato", scene_seq_svuotato},
    {"22-seq-dopo-uso", scene_seq_dopo_uso},

    {"23-adsr-min", scene_adsr_min},
    {"24-adsr-max", scene_adsr_max},
    {"25-adsr-dopo-uso", scene_adsr_dopo_uso},

    {"26-settings-chiuso", scene_settings_chiuso},
    {"27-settings-riga0-volume", scene_settings_riga0},
    {"28-settings-riga1-cutoff", scene_settings_riga1},
    {"29-settings-riga2-adsr", scene_settings_riga2},
    {"30-settings-riga3-passofine", scene_settings_riga3},
    {"31-settings-riga4-azione-wifi", scene_settings_riga4},
    {"32-settings-valori-estremi", scene_settings_valori_estremi},
    {"33-settings-dopo-uso", scene_settings_dopo_uso},

    {"34-vu-zero", scene_vu_zero},
    {"35-vu-meta", scene_vu_meta},
    {"36-vu-fondoscala-clip", scene_vu_fondo},
    {"37-vu-dopo-uso", scene_vu_dopo_uso},

    {"38-scope-silenzio", scene_scope_silenzio},
    {"39-scope-pieno", scene_scope_pieno},
    {"40-scope-dopo-uso", scene_scope_dopo_uso},

    {"41-network-idle", scene_net_idle},
    {"42-network-qr-rete", scene_net_attesa},
    {"43-network-qr-indirizzo", scene_net_portale},
    {"44-network-in-rete-ip-lungo", scene_net_in_rete},
    {"45-network-ota-0", scene_ota_0},
    {"46-network-ota-50", scene_ota_50},
    {"47-network-ota-100", scene_ota_100},
    {"48-network-fallito", scene_net_fallito},
    {"49-network-dopo-uso", scene_net_dopo_uso},

    {"50-splash", scene_splash},
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
