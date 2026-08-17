// storage.cpp — NVS via Preferences, con scrittura ritardata.

#include "storage.h"

#include <Preferences.h>
#include <stddef.h>  // offsetof, per la migrazione del blob

#include "sequencer.h"

namespace {

constexpr uint32_t STATE_MAGIC = 0x53505247;  // "SPRG": marca la versione del blob

// Namespace e magic restano quelli di prima nonostante il cambio di nome del
// progetto: sono chiavi dentro la NVS, non testo che qualcuno legga. Ribattezzarli
// renderebbe irraggiungibile quello che c'e' gia' scritto sulle schede in giro —
// parametri, pattern e credenziali WiFi — che ripartirebbero dai valori di
// fabbrica al primo avvio dopo l'aggiornamento.
const char *NAMESPACE = "sprig";
const char *KEY_STATE = "state";
const char *KEY_PATTERN = "patt";
const char *KEY_SSID = "ssid";
const char *KEY_PASS = "pass";
const char *KEY_MANIFEST = "manifest";
const char *KEY_LEDMAP = "ledmap";

Preferences prefs;
bool ready = false;

bool dirty = false;
uint32_t dirtyAt = 0;

void writeAll(const Storage::SynthState &s) {
    if (!ready) return;
    Storage::SynthState copy = s;
    copy.magic = STATE_MAGIC;
    copy.stateRev = STORAGE_STATE_REV;
    prefs.putBytes(KEY_STATE, &copy, sizeof(copy));
    prefs.putBytes(KEY_PATTERN, Sequencer::patternData(), Sequencer::patternSize());
    dirty = false;
}

}  // namespace

namespace Storage {

void begin() { ready = prefs.begin(NAMESPACE, false /* read-write */); }

bool load(SynthState &s) {
    if (!ready) return false;

    SynthState tmp = {};

    // La 1.3.0 ha aggiunto in coda alla struttura la sensibilita' degli encoder.
    // Un blob piu' corto e' quindi una versione precedente, non un errore:
    // si rilegge per quello che e' e la coda resta a zero, che i chiamanti
    // riportano ai valori di fabbrica. Senza questo, aggiornando si perderebbero
    // onda, ottava, cutoff, volume e ADSR di chi il synth lo stava gia' usando.
    constexpr size_t LEGACY_BYTES = offsetof(SynthState, stepVol);
    // La 1.12.0 ha aggiunto in coda l'orientamento della scala di sensibilita'.
    // Stessa logica di prima, un anello piu' avanti: un blob lungo fin qui e'
    // stato scritto fra la 1.3.0 e la 1.11.0, quando l'indice cresceva verso i
    // giri piu' bassi. Si rilegge, il campo nuovo resta a zero, e chi lo usa sa
    // che quegli indici vanno rovesciati prima di crederci.
    constexpr size_t PRE_SCALE_BYTES = offsetof(SynthState, scaleRev);
    // La 2.0.0 e' il salto piu' grosso: scheda nuova, tredici note, quattro
    // encoder, luci ed effetti. Tutto il resto della struttura e' cresciuto in
    // coda, quindi un blob della 1.x si rilegge per intero e si perde solo
    // quello che nella 1.x non esisteva.
    constexpr size_t PRE_V2_BYTES = offsetof(SynthState, resonance);
    const size_t stored = prefs.getBytesLength(KEY_STATE);
    if (stored != sizeof(tmp) && stored != PRE_V2_BYTES && stored != PRE_SCALE_BYTES &&
        stored != LEGACY_BYTES) {
        return false;
    }
    if (prefs.getBytes(KEY_STATE, &tmp, stored) != stored) return false;
    if (tmp.magic != STATE_MAGIC) return false;  // formato vecchio: si riparte dai default

    // In un blob pre-1.3.0 la coda della sensibilita' non e' mai stata scritta:
    // quei byte sono zeri, non indici da convertire. Dichiararli gia' nella
    // scala nuova evita di rovesciare un dato che non esiste; i chiamanti li
    // riportano comunque ai valori di fabbrica.
    if (stored == LEGACY_BYTES) tmp.scaleRev = STORAGE_SCALE_REV;

    // Il pattern e' facoltativo: se manca, i parametri si caricano comunque.
    if (prefs.getBytes(KEY_PATTERN, Sequencer::patternData(), Sequencer::patternSize()) ==
        Sequencer::patternSize()) {
        // Un pattern salvato da un firmware con piu' note (il DO' esisteva
        // ancora) puo' contenere indici fuori scala: diventano pause invece di
        // leggere fuori dalla tabella delle frequenze.
        Sequencer::sanitizePattern();
        Sequencer::patternChanged();
    }

    s = tmp;
    return true;
}

void markDirty() {
    dirty = true;
    dirtyAt = millis();
}

bool savePending(uint32_t now) {
    if (!dirty || !ready) return false;
    return (now - dirtyAt) >= STORAGE_SAVE_DELAY_MS;
}

void flush(const SynthState &s) { writeAll(s); }

void saveWifi(const char *ssid, const char *pass) {
    if (!ready) return;
    prefs.putString(KEY_SSID, ssid);
    prefs.putString(KEY_PASS, pass);
}

bool loadWifi(String &ssid, String &pass) {
    if (!ready) return false;
    ssid = prefs.getString(KEY_SSID, "");
    pass = prefs.getString(KEY_PASS, "");
    return ssid.length() > 0;
}

void clearWifi() {
    if (!ready) return;
    prefs.remove(KEY_SSID);
    prefs.remove(KEY_PASS);
}

void saveLedMap(const uint8_t *map, size_t len) {
    if (ready && map && len) prefs.putBytes(KEY_LEDMAP, map, len);
}

bool loadLedMap(uint8_t *map, size_t len) {
    if (!ready || !map || !len) return false;
    if (prefs.getBytesLength(KEY_LEDMAP) != len) return false;
    return prefs.getBytes(KEY_LEDMAP, map, len) == len;
}

void saveManifestUrl(const char *url) {
    if (ready) prefs.putString(KEY_MANIFEST, url);
}

String loadManifestUrl(const char *fallback) {
    if (!ready) return String(fallback);
    String u = prefs.getString(KEY_MANIFEST, "");
    return (u.length() > 0) ? u : String(fallback);
}

}  // namespace Storage
