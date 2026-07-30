// storage.cpp — NVS via Preferences, con scrittura ritardata.

#include "storage.h"

#include <Preferences.h>

#include "sequencer.h"

namespace {

constexpr uint32_t STATE_MAGIC = 0x53505247;  // "SPRG": marca la versione del blob
const char *NAMESPACE = "sprig";
const char *KEY_STATE = "state";
const char *KEY_PATTERN = "patt";
const char *KEY_SSID = "ssid";
const char *KEY_PASS = "pass";
const char *KEY_MANIFEST = "manifest";

Preferences prefs;
bool ready = false;

bool dirty = false;
uint32_t dirtyAt = 0;

void writeAll(const Storage::SynthState &s) {
    if (!ready) return;
    Storage::SynthState copy = s;
    copy.magic = STATE_MAGIC;
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
    if (prefs.getBytes(KEY_STATE, &tmp, sizeof(tmp)) != sizeof(tmp)) return false;
    if (tmp.magic != STATE_MAGIC) return false;  // formato vecchio: si riparte dai default

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

void saveManifestUrl(const char *url) {
    if (ready) prefs.putString(KEY_MANIFEST, url);
}

String loadManifestUrl(const char *fallback) {
    if (!ready) return String(fallback);
    String u = prefs.getString(KEY_MANIFEST, "");
    return (u.length() > 0) ? u : String(fallback);
}

}  // namespace Storage
