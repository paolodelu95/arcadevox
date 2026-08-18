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
// Le due chiavi della versione a rete singola. Restano lette per una volta sola,
// il tempo di travasare quello che c'era dentro nell'elenco nuovo: chi aggiorna
// non deve ridigitare la rete di casa.
const char *KEY_SSID = "ssid";
const char *KEY_PASS = "pass";
const char *KEY_WIFIS = "wifis";
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
    // La 2.1.0 ha aggiunto in coda i due parametri dell'inviluppo di filtro.
    // Sono float, quindi la struttura cresce di otto byte pieni: nessuna
    // lunghezza vecchia puo' coincidere con quella nuova, e un blob lungo fin
    // qui e' senza ambiguita' una 2.0.
    constexpr size_t PRE_V21_BYTES = offsetof(SynthState, filtEnvAmount);
    const size_t stored = prefs.getBytesLength(KEY_STATE);
    if (stored != sizeof(tmp) && stored != PRE_V21_BYTES && stored != PRE_V2_BYTES &&
        stored != PRE_SCALE_BYTES && stored != LEGACY_BYTES) {
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

// ------------------------------------------------------------- reti conosciute
//
// Un blocco solo in NVS invece di dieci chiavi separate: le stringhe hanno una
// lunghezza massima nota (32 caratteri lo SSID, 63 la password WPA2, piu' il
// terminatore) e cosi' scrivere l'elenco e' una scrittura sola, come per il resto
// dello stato.
namespace {

struct WifiSlot {
    char ssid[33];
    char pass[65];
};

struct WifiBook {
    WifiSlot slot[WIFI_SLOTS];
};

WifiBook book;
bool bookLoaded = false;

void bookRead() {
    if (bookLoaded) return;
    memset(&book, 0, sizeof(book));
    // Il guardiano prima di dichiarare l'elenco letto: marcarlo come caricato
    // mentre la NVS non e' ancora pronta vorrebbe dire tenersi un elenco vuoto per
    // sempre — e la prima rete salvata dopo lo riscriverebbe sopra a quelle vere.
    if (!ready) return;
    bookLoaded = true;

    if (prefs.getBytesLength(KEY_WIFIS) == sizeof(book)) {
        prefs.getBytes(KEY_WIFIS, &book, sizeof(book));
        return;
    }
    // Nessun elenco: c'e' forse la rete singola delle versioni precedenti. La si
    // travasa nella prima casella e non se ne parla piu'. Le due chiavi vecchie
    // non si cancellano — costano venti byte e sono l'unica rete di casa di chi
    // dovesse tornare a un firmware di prima.
    const String s0 = prefs.getString(KEY_SSID, "");
    if (s0.length() > 0) {
        strncpy(book.slot[0].ssid, s0.c_str(), sizeof(book.slot[0].ssid) - 1);
        const String p0 = prefs.getString(KEY_PASS, "");
        strncpy(book.slot[0].pass, p0.c_str(), sizeof(book.slot[0].pass) - 1);
    }
}

void bookWrite() {
    if (ready) prefs.putBytes(KEY_WIFIS, &book, sizeof(book));
}

int bookFind(const char *ssid) {
    for (int i = 0; i < WIFI_SLOTS; ++i) {
        if (strcmp(book.slot[i].ssid, ssid) == 0 && book.slot[i].ssid[0]) return i;
    }
    return -1;
}

}  // namespace

void wifiRemember(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return;
    bookRead();

    int at = bookFind(ssid);
    if (at < 0) {
        // Casella libera, altrimenti si butta l'ultima: e' quella usata piu'
        // tempo fa, ed e' l'unica scelta che non chiede a nessuno di decidere.
        at = WIFI_SLOTS - 1;
        for (int i = 0; i < WIFI_SLOTS; ++i) {
            if (!book.slot[i].ssid[0]) {
                at = i;
                break;
            }
        }
    }
    WifiSlot entry;
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.ssid, ssid, sizeof(entry.ssid) - 1);
    strncpy(entry.pass, pass ? pass : "", sizeof(entry.pass) - 1);

    // In cima: l'ordine e' quello di ultimo uso.
    for (int i = at; i > 0; --i) book.slot[i] = book.slot[i - 1];
    book.slot[0] = entry;
    bookWrite();
}

uint8_t wifiCount() {
    bookRead();
    uint8_t n = 0;
    for (int i = 0; i < WIFI_SLOTS; ++i) {
        if (book.slot[i].ssid[0]) ++n;
    }
    return n;
}

bool wifiAt(uint8_t i, String &ssid, String &pass) {
    bookRead();
    if (i >= WIFI_SLOTS || !book.slot[i].ssid[0]) return false;
    ssid = book.slot[i].ssid;
    pass = book.slot[i].pass;
    return true;
}

bool wifiPasswordFor(const char *ssid, String &pass) {
    bookRead();
    const int at = bookFind(ssid);
    if (at < 0) return false;
    pass = book.slot[at].pass;
    return true;
}

void wifiForget(const char *ssid) {
    bookRead();
    const int at = bookFind(ssid);
    if (at < 0) return;
    for (int i = at; i < WIFI_SLOTS - 1; ++i) book.slot[i] = book.slot[i + 1];
    memset(&book.slot[WIFI_SLOTS - 1], 0, sizeof(WifiSlot));
    bookWrite();
}

void wifiForgetAll() {
    bookRead();
    memset(&book, 0, sizeof(book));
    bookWrite();
    if (ready) {
        prefs.remove(KEY_SSID);
        prefs.remove(KEY_PASS);
    }
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
