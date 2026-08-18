// net_portal.cpp — access point, captive portal e aggiornamento firmware.
//
// Flusso pensato per essere fatto in piedi, con una mano sola:
//
//   1. il display mostra un QR che *e' la rete*: inquadrandolo lo smartphone si
//      aggancia all'AP senza digitare niente;
//   2. il DNS risponde qualunque cosa gli si chieda, cosi' iOS e Android aprono
//      il portale da soli (captive portal);
//   3. dal portale: o si carica il .bin dal telefono, o si danno al synth le
//      credenziali di casa e lui si aggiorna da internet.
//
// Appena un client si aggancia, il QR diventa quello dell'indirizzo del
// portale: serve da scorciatoia se la finestra del captive portal non compare.

#include "net_portal.h"

#include <DNSServer.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "audio_engine.h"
#include "display.h"
#include "status_led.h"
#include "storage.h"
#include "version.h"

namespace {

constexpr uint16_t DNS_PORT = 53;
constexpr const char *AUTH_USER = NET_AUTH_USER;

DNSServer dns;
WebServer server(80);

bool started = false;
NetPortal::Stage currentStage = NetPortal::NET_OFF;

char apSsid[24] = "ArcadeVox";
char apPass[16] = "arcadevox";
char qrText[80] = "";
char portal[32] = "http://192.168.4.1/";
char staIpText[20] = "";
char statusMsg[48] = "";
int otaProgress = 0;

uint32_t staDeadline = 0;
bool uploadRejected = false;

// --- l'aggiornamento che il synth ha trovato da solo ---
//
// Appena entra in rete va a leggere il manifest per conto suo. Non e' una
// comodita' in piu': e' cio' che rende il telefono facoltativo. La prima volta
// serve — bisogna pur dirgli qual e' la rete di casa — ma dalla seconda in poi
// si accende la radio, il synth si ricollega, e sul display c'e' gia' scritto se
// c'e' una versione nuova e quanto pesa premere un tasto per averla.
char foundVer[16] = "";
// Larghi quanto il manifest vero, non quanto sembrava ragionevole: le note della
// release in firmware/manifest.json passano i trecento caratteri, e un indirizzo
// tagliato a meta' fallirebbe come un generico "download fallito" senza che
// nessuno possa capire perche'.
char foundUrl[224] = "";
char foundNotes[352] = "";
bool updateReady = false;   // c'e' una versione piu' nuova, scaricabile
bool autoChecked = false;   // il controllo automatico e' andato a buon fine
uint32_t autoRetryAt = 0;   // ...oppure e' fallito, e si riprova da qui
uint8_t autoTries = 0;      // e non all'infinito: dopo tre si smette

// Quello che si sapeva della versione disponibile vale per la rete su cui lo si
// e' saputo: cambiando rete — o dimenticandola — non vale piu' niente, e lasciare
// a schermo un "tieni AVVIA per installare" senza piu' una strada per scaricare
// vorrebbe dire mandare qualcuno dritto in un aggiornamento fallito.
void forgetFoundUpdate() {
    updateReady = false;
    autoChecked = false;
    autoRetryAt = 0;
    // Anche il contatore dei tentativi: le credenziali nuove meritano tre
    // tentativi nuovi, altrimenti tre fallimenti su una rete sbagliata
    // spegnerebbero il controllo automatico per tutta l'accensione.
    autoTries = 0;
    foundVer[0] = foundUrl[0] = foundNotes[0] = '\0';
}

// Rientro automatico nella rete di casa, in corso. Corre in parallelo al
// portale e non passa dalla macchina a stati: se la rete non c'e' — sei fuori
// casa, o l'hai cambiata — il QR e la pagina devono restare utilizzabili.
bool staAuto = false;

void setStatus(const char *m) { strncpy(statusMsg, m, sizeof(statusMsg) - 1); }

// Nel formato WIFI: questi cinque caratteri delimitano i campi e vanno preceduti
// da una barra rovesciata. SSID e password attuali non ne contengono, ma sono
// generati da una snprintf poco piu' sotto: se un giorno cambia il formato del
// nome, un punto e virgola di troppo spezzerebbe il codice in silenzio.
void appendEscaped(char *dst, size_t cap, const char *src) {
    size_t n = strlen(dst);
    for (const char *p = src; *p && n + 2 < cap; ++p) {
        if (*p == '\\' || *p == ';' || *p == ',' || *p == ':' || *p == '"') dst[n++] = '\\';
        dst[n++] = *p;
    }
    dst[n] = '\0';
}

// Il QR racconta cose diverse a seconda del momento: prima come entrare nella
// rete, poi dove andare una volta dentro.
void setQrForJoin() {
    // Ordine dei campi S, T, P: e' quello che emettono la condivisione WiFi di
    // Android e Apple Configurator, ed e' di gran lunga il piu' collaudato dai
    // lettori. Lo standard non lo impone — con T davanti il codice si decodifica
    // ugualmente — ma qui non stiamo cercando di essere conformi, stiamo cercando
    // di farci riconoscere da un telefono qualsiasi.
    //
    // Niente campo H: la rete non e' nascosta e ometterlo vale "H:false", mentre
    // scriverlo costerebbe 8 byte su un codice di versione 3 che ne regge 53.
    qrText[0] = '\0';
    strncat(qrText, "WIFI:S:", sizeof(qrText) - 1);
    appendEscaped(qrText, sizeof(qrText), apSsid);
    strncat(qrText, ";T:WPA;P:", sizeof(qrText) - strlen(qrText) - 1);
    appendEscaped(qrText, sizeof(qrText), apPass);
    strncat(qrText, ";;", sizeof(qrText) - strlen(qrText) - 1);
}

void setQrForPortal() { strncpy(qrText, portal, sizeof(qrText) - 1); }

// --------------------------------------------------------------- scansione
//
// Setacciare i canali significa saltare da un canale all'altro, e mentre lo fa
// la radio non e' su quello dell'access point. Prima la scansione partiva quando
// si apriva la pagina, cioe' **dopo** che il telefono si era agganciato: proprio
// nel momento peggiore, perche' i tre secondi di salti li pagava il telefono che
// serviva. Il risultato era che l'elenco non compariva mai — restava scritto
// "scansione in corso, ricarica fra un istante" — e il nome della rete finiva per
// doverlo scrivere a mano ogni volta.
//
// Adesso si guarda l'etere **prima** di accendere l'access point: li' non c'e'
// ancora nessuno da disturbare, il display dice cosa sta succedendo, e quando il
// telefono arriva l'elenco e' gia' li'.
constexpr int SCAN_MAX = 24;

struct Seen {
    char name[33];
    int8_t rssi;
    bool locked;
};

Seen scanList[SCAN_MAX];
int scanCount = 0;
bool scanFresh = false;  // una scansione e' stata fatta almeno una volta

// I nomi delle reti li scrive chi ha configurato il router, non noi: "Bob's
// WiFi" dentro un value='...' chiude l'attributo a meta' e quello che arriva
// indietro e' "Bob". Non e' un problema di sicurezza — chi e' agganciato
// all'access point ha gia' la password — e' che la rete non si collega e non si
// capisce perche'.
String htmlEscape(const char *src) {
    String out;
    for (const char *p = src; *p; ++p) {
        switch (*p) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += *p; break;
        }
    }
    return out;
}

// Una rete con lo stesso nome puo' comparire piu' volte (due bande, o un
// ripetitore): nell'elenco ne basta una, la piu' forte.
int indexOfName(const char *name) {
    for (int i = 0; i < scanCount; ++i) {
        if (strcmp(scanList[i].name, name) == 0) return i;
    }
    return -1;
}

// Legge il risultato di WiFi.scanNetworks() nella nostra copia e lo butta: da
// qui in poi la pagina si disegna dalla cache, e la radio puo' tornare a fare
// solo l'access point.
void harvestScan(int found) {
    scanCount = 0;
    for (int i = 0; i < found && scanCount < SCAN_MAX; ++i) {
        const String name = WiFi.SSID(i);
        if (name.length() == 0 || name.length() > 32) continue;
        const int dup = indexOfName(name.c_str());
        if (dup >= 0) {
            // Stessa rete su due bande: si tiene il segnale migliore.
            if (WiFi.RSSI(i) > scanList[dup].rssi) scanList[dup].rssi = (int8_t)WiFi.RSSI(i);
            continue;
        }
        strncpy(scanList[scanCount].name, name.c_str(), sizeof(scanList[scanCount].name) - 1);
        scanList[scanCount].name[sizeof(scanList[scanCount].name) - 1] = '\0';
        scanList[scanCount].rssi = (int8_t)WiFi.RSSI(i);
        scanList[scanCount].locked = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        ++scanCount;
    }
    // Le piu' forti in cima: la tua rete di casa e' quasi sempre la prima, ed e'
    // esattamente quello che uno si aspetta di trovare senza scorrere.
    for (int i = 1; i < scanCount; ++i) {
        const Seen key = scanList[i];
        int j = i - 1;
        while (j >= 0 && scanList[j].rssi < key.rssi) {
            scanList[j + 1] = scanList[j];
            --j;
        }
        scanList[j + 1] = key;
    }
    WiFi.scanDelete();
    scanFresh = true;
}

// --------------------------------------------------------------- versioni
// "1.2.3" -> 0x010203, per confrontare le release con un solo intero.
uint32_t parseVersion(const char *s) {
    uint32_t a = 0, b = 0, c = 0;
    sscanf(s, "%u.%u.%u", &a, &b, &c);
    return ((a & 0xff) << 16) | ((b & 0xff) << 8) | (c & 0xff);
}

// Estrae il valore di "chiave":"valore" da un JSON piatto. Il manifest ha tre
// campi e nessun annidamento: tirarsi dietro un parser completo non si giustifica.
String jsonField(const String &src, const char *key) {
    String needle = String("\"") + key + "\"";
    int k = src.indexOf(needle);
    if (k < 0) return "";
    int colon = src.indexOf(':', k + needle.length());
    if (colon < 0) return "";
    int open = src.indexOf('"', colon);
    if (open < 0) return "";
    int close = src.indexOf('"', open + 1);
    if (close < 0) return "";
    return src.substring(open + 1, close);
}

// ------------------------------------------------------------------- HTML
const char PAGE_HEAD[] PROGMEM =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>ArcadeVox</title><style>"
    "body{background:#111;color:#eee;font:16px system-ui,sans-serif;margin:0;padding:20px;"
    "max-width:480px;margin:auto}"
    "h1{font-size:20px;color:#0ff;margin:.2em 0}h2{font-size:15px;color:#888;margin:1.6em 0 .4em;"
    "text-transform:uppercase;letter-spacing:.08em}"
    "form{background:#1c1c1c;border-radius:10px;padding:14px;margin:0 0 14px}"
    "input{width:100%;box-sizing:border-box;background:#000;color:#eee;border:1px solid #444;"
    "border-radius:6px;padding:10px;margin:5px 0 10px;font-size:16px}"
    "button{width:100%;background:#0aa;color:#000;border:0;border-radius:6px;padding:12px;"
    "font-size:16px;font-weight:600}"
    "small{color:#888;line-height:1.5}.v{color:#0f0}"
    "</style>";

String pageStatus() {
    String s = FPSTR(PAGE_HEAD);
    // Mentre il synth sta provando a entrare in rete la pagina si ricarica da
    // sola: l'esito arriva dopo qualche secondo e nessuno deve stare li' a
    // premere ricarica per sapere com'e' andata.
    if (currentStage == NetPortal::NET_STA_WAIT) s += "<meta http-equiv=refresh content=3>";
    s += "<h1>ArcadeVox</h1><small>firmware <span class=v>" FW_VERSION "</span>";
    if (staIpText[0]) {
        s += " &middot; in rete come <span class=v>";
        s += staIpText;
        s += "</span>";
    } else {
        s += " &middot; non collegato a internet";
    }
    s += "</small>";

    s += "<h2>Aggiorna dal telefono</h2><form method=POST action=/update "
         "enctype='multipart/form-data'>"
         "<small>Scegli il file <code>firmware.bin</code> gia' salvato sul "
         "telefono.</small>"
         "<input type=file name=fw accept='.bin' required>"
         "<button>Carica e riavvia</button></form>";

    s += "<h2>Collega alla rete di casa</h2><form method=POST action=/wifi>"
         "<small>Serve solo per gli aggiornamenti da internet. Le credenziali "
         "restano nel synth: alla prossima accensione della radio ci si "
         "ricollega da solo.</small>";
    // Sapere quale rete e' memorizzata evita di ridigitarla per il dubbio.
    String knownSsid, knownPass;
    const bool known = Storage::loadWifi(knownSsid, knownPass);
    if (known) {
        s += "<small>In memoria: <span class=v>";
        s += htmlEscape(knownSsid.c_str());
        s += "</span></small>";
    }
    if (scanCount > 0) {
        s += "<select name=ssid>";
        for (int i = 0; i < scanCount; ++i) {
            const String safe = htmlEscape(scanList[i].name);
            s += "<option value='";
            s += safe;
            s += "'";
            if (known && knownSsid == scanList[i].name) s += " selected";
            s += ">";
            s += safe;
            s += scanList[i].locked ? "  &#128274; " : "  ";
            // Le tacchette dicono la stessa cosa dei dBm senza chiedere a
            // nessuno di sapere cosa sia un dBm.
            const int8_t r = scanList[i].rssi;
            s += (r > -55) ? "||||" : (r > -67) ? "|||" : (r > -78) ? "||" : "|";
            s += "</option>";
        }
        s += "</select>";
        s += "<small>Non la vedi? Scrivila qui sotto, oppure ";
        s += "<a style='color:#0aa' href='/rescan'>cerca di nuovo</a>.</small>";
    } else {
        s += "<small>Nessuna rete trovata. ";
        s += "<a style='color:#0aa' href='/rescan'>Cerca di nuovo</a>, "
             "oppure scrivi il nome a mano.</small>";
    }
    // Resta un campo libero: le reti nascoste non compaiono in nessuna scansione,
    // e se e' pieno ha la precedenza sulla tendina.
    s += "<input name=ssid_manual placeholder='oppure scrivi il nome' value=''>";
    // Se la rete scelta e' quella gia' in memoria, lasciare vuota la password
    // vuol dire "usa quella di prima": e' la sola cosa che si e' costretti a
    // ridigitare, e non c'e' nessun motivo per cui debba succedere due volte.
    s += known ? "<input name=pass type=password placeholder='password (vuoto: quella salvata)'>"
               : "<input name=pass type=password placeholder=password>";
    s += "<button>Collega</button></form>";
    if (known) {
        s += "<form method=POST action=/forget>"
             "<button style='background:#622;color:#fdd'>Dimentica la rete</button></form>";
    }

    s += "<h2>Aggiorna da internet</h2><form method=GET action=/check>"
         "<small>Indirizzo del manifest delle release.</small>"
         "<input name=url value='";
    s += htmlEscape(Storage::loadManifestUrl(FW_MANIFEST_URL).c_str());
    s += "'><button>Cerca aggiornamenti</button></form>";

    s += "<small>Per uscire dalla modalita' rete spingi il joystick a sinistra: "
         "il synth si riavvia e torna suonabile.</small>";
    return s;
}

String pageMessage(const char *title, const String &body, bool ok) {
    String s = FPSTR(PAGE_HEAD);
    s += "<h1>";
    s += title;
    s += "</h1><p style='color:";
    s += ok ? "#0f0" : "#f55";
    s += "'>";
    s += body;
    s += "</p><p><a style='color:#0aa' href='/'>Torna indietro</a></p>";
    return s;
}

// Chi e' agganciato all'access point ha gia' dimostrato di conoscere la
// password WPA2 — che e' poi la stessa del portale: chiedergliela una seconda
// volta in una finestra di login non aggiunge nessuna barriera, e nel browser
// ridotto del captive portal quella finestra e' spesso un vicolo cieco (la si
// compila e ricompare, oppure non si apre affatto). La difesa che conta e'
// l'altra: da dentro casa, quando il synth e' anche sulla rete di famiglia, il
// portale risponde su un secondo indirizzo raggiungibile da chiunque sia in
// LAN senza aver visto il display. Li' la password serve davvero.
bool fromAccessPoint() {
    const IPAddress remote = server.client().remoteIP();
    const IPAddress apIp = WiFi.softAPIP();
    const IPAddress mask = WiFi.softAPSubnetMask();
    for (int i = 0; i < 4; ++i) {
        if ((remote[i] & mask[i]) != (apIp[i] & mask[i])) return false;
    }
    return true;
}

bool authOk() { return fromAccessPoint() || server.authenticate(AUTH_USER, apPass); }

bool requireAuth() {
    if (authOk()) return true;
    server.requestAuthentication(BASIC_AUTH, "ArcadeVox");
    return false;
}

// ----------------------------------------------------------------- rotte

void handleRoot() {
    // L'autenticazione va chiesta qui, sulla prima pagina. Chiedendola solo alle
    // rotte che scrivono — com'era prima — la finestra di login compariva a
    // sorpresa dopo aver gia' compilato il modulo del WiFi, e sembrava un errore
    // invece che l'ingresso. Il browser poi se la ricorda per tutta la sessione.
    if (!requireAuth()) return;
    server.send(200, "text/html", pageStatus());
}

// Le sonde con cui iOS e Android capiscono di essere dietro un captive portal:
// rispondendo con un redirect si guadagna l'apertura automatica della pagina.
// La risposta alle sonde con cui il telefono decide se la rete "ha internet".
//
// Android ne manda una in chiaro verso generate_204 e si aspetta un 204 vuoto:
// qualunque altra cosa vuol dire "c'e' un portale", e a quel punto apre la
// pagina da solo. Il 302 con l'indirizzo del synth e' la risposta canonica.
//
// Le tre righe di intestazione contro la cache non sono decorative. Il telefono
// ripete le sonde di continuo — al risveglio dello schermo, al cambio di rete,
// ogni pochi minuti — e se una risposta gli resta in cache si convince che il
// portale sia gia' stato superato e smette di proporlo. E' il caso in cui uno
// finisce a digitare l'indirizzo a mano senza capire perche'.
void handleCaptive() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    server.sendHeader("Location", portal, true);
    server.send(302, "text/html",
                "<!doctype html><meta http-equiv=refresh content='0;url=" + String(portal) +
                    "'><a href='" + String(portal) + "'>ArcadeVox</a>");
}

void handleUploadEnd() {
    if (uploadRejected) {
        server.send(401, "text/html", "non autorizzato");
        return;
    }
    server.sendHeader("Connection", "close");
    if (Update.hasError()) {
        currentStage = NetPortal::NET_FAILED;
        setStatus("caricamento fallito");
        server.send(200, "text/html",
                    pageMessage("Aggiornamento fallito",
                                "Il file non e' stato accettato. Il firmware "
                                "attuale e' rimasto intatto.",
                                false));
        return;
    }
    setStatus("riavvio...");
    server.send(200, "text/html",
                pageMessage("Fatto", "Il synth si sta riavviando con il nuovo firmware.", true));
    delay(600);
    ESP.restart();
}

void handleUploadData() {
    HTTPUpload &up = server.upload();

    if (up.status == UPLOAD_FILE_START) {
        // L'autenticazione va verificata qui: i dati arrivano prima che il
        // gestore di completamento venga chiamato.
        uploadRejected = !authOk();
        if (uploadRejected) return;

        otaProgress = 0;
        currentStage = NetPortal::NET_UPDATING;
        setStatus("ricezione firmware");
        // UPDATE_SIZE_UNKNOWN: la dimensione non e' nota in anticipo, il
        // bootloader scrive comunque nella partizione applicativa libera.
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            uploadRejected = true;
            currentStage = NetPortal::NET_FAILED;
            setStatus("spazio insufficiente");
        }
        return;
    }
    if (uploadRejected) return;

    if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
            Update.printError(Serial);
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (!Update.end(true)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        currentStage = NetPortal::NET_FAILED;
        setStatus("trasferimento interrotto");
    }
}

void handleRescan() {
    if (!requireAuth()) return;

    // Ordine invertito di proposito: prima si risponde, poi si cerca.
    //
    // Cercare le reti vuol dire saltare da un canale all'altro, e mentre lo fa la
    // radio non e' su quello dell'access point: il telefono resta scollegato per
    // qualche secondo. Se la risposta partisse dopo, se ne andrebbe proprio nel
    // buco — la pagina resterebbe a caricare e poi darebbe errore, mentre la
    // scansione era andata benissimo.
    //
    // Cosi' invece il telefono ha gia' in mano una pagina che si ricarica da sola
    // fra sei secondi: il buco lo passa guardando una scritta che glielo spiega, e
    // quando torna l'elenco e' pronto.
    server.send(200, "text/html",
                pageMessage("Sto cercando le reti",
                            "Ci vogliono cinque secondi, e durante la ricerca il "
                            "collegamento col synth si interrompe: e' normale. "
                            "Questa pagina si aggiorna da sola.<meta "
                            "http-equiv=refresh content='6;url=/'>",
                            true));
    server.client().stop();

    harvestScan(WiFi.scanNetworks(false, false));
}

// Cambiare casa, o rete, senza dover rientrare a mano ogni volta in una rete
// che non esiste piu'.
void handleForget() {
    if (!requireAuth()) return;
    Storage::clearWifi();
    forgetFoundUpdate();
    staAuto = false;
    WiFi.disconnect();
    staIpText[0] = '\0';
    server.send(200, "text/html",
                pageMessage("Rete dimenticata",
                            "Il synth non provera' piu' a ricollegarsi da solo.", true));
}

void handleWifi() {
    if (!requireAuth()) return;

    // Il campo scritto a mano ha la precedenza: se l'utente si e' preso la briga
    // di digitarlo, e' perche' nella tendina non c'era quello che cercava.
    String ssid = server.arg("ssid_manual");
    ssid.trim();
    if (ssid.length() == 0) ssid = server.arg("ssid");
    String pass = server.arg("pass");
    if (ssid.length() == 0) {
        server.send(200, "text/html",
                    pageMessage("Manca il nome", "Indica il nome della rete.", false));
        return;
    }

    // Password lasciata vuota su una rete che il synth conosce gia': vuol dire
    // "quella di prima". E' l'unica cosa che questo portale costringa a scrivere,
    // e non c'e' nessun motivo per cui debba succedere due volte.
    //
    // Su una rete *diversa*, pero', vuota vuol dire vuota, e salvarla cosi'
    // butterebbe via le credenziali buone in cambio di niente: meglio fermarsi e
    // chiederla, che e' anche cio' che l'utente si aspetta.
    if (pass.length() == 0) {
        String oldSsid, oldPass;
        const bool sameAsSaved = Storage::loadWifi(oldSsid, oldPass) && oldSsid == ssid;
        // Ci si ferma solo quando si *sa* che una password serve, cioe' quando
        // quella rete e' nell'elenco col lucchetto. Una rete aperta la password
        // non ce l'ha, e una rete nascosta scritta a mano non e' nell'elenco per
        // definizione: in tutti e due i casi rifiutare vorrebbe dire chiedere
        // qualcosa che non esiste, e lasciare senza uscita chi ha appena scritto
        // il nome giusto.
        const int idx = indexOfName(ssid.c_str());
        const bool knownLocked = (idx >= 0) && scanList[idx].locked;
        if (sameAsSaved) {
            pass = oldPass;
        } else if (knownLocked) {
            server.send(200, "text/html",
                        pageMessage("Manca la password",
                                    "Questa rete e' protetta e il synth non l'ha in "
                                    "memoria: la password serve. Quella salvata vale "
                                    "solo per la rete che c'e' gia' dentro.",
                                    false));
            return;
        }
    }

    Storage::saveWifi(ssid.c_str(), pass.c_str());
    // Rete nuova: quello che si sapeva della versione disponibile non vale piu',
    // e il controllo automatico va rifatto una volta dall'altra parte.
    forgetFoundUpdate();
    staAuto = false;  // da qui comanda la macchina a stati, non il rientro in sordina
    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), pass.c_str());
    currentStage = NetPortal::NET_STA_WAIT;
    staDeadline = millis() + 20000;
    setStatus("collegamento in corso");

    server.send(200, "text/html",
                pageMessage("Collegamento avviato",
                            "Il synth sta provando a entrare in rete: lo stato "
                            "compare sul suo display. Ricarica questa pagina "
                            "fra qualche secondo.",
                            true));
}

// Legge il manifest e dice se c'e' di meglio. Non scarica niente: si limita a
// guardare, e quello che trova finisce sul display.
bool lookForUpdate(const String &url, String &err, uint16_t timeoutMs = 10000) {
    updateReady = false;
    foundVer[0] = foundUrl[0] = foundNotes[0] = '\0';

    if (WiFi.status() != WL_CONNECTED) {
        err = "non collegato a internet";
        return false;
    }

    WiFiClientSecure client;
    // Nessuna verifica del certificato: il synth non ha un orologio affidabile
    // ne' un bundle di CA da tenere aggiornato. Il canale resta cifrato, ma un
    // uomo-nel-mezzo sulla rete di casa potrebbe servire un altro firmware.
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, url)) {
        err = "indirizzo non valido";
        return false;
    }
    http.setTimeout(timeoutMs);
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        err = String("il server ha risposto ") + code;
        return false;
    }
    const String body = http.getString();
    http.end();

    const String ver = jsonField(body, "version");
    const String bin = jsonField(body, "url");
    if (ver.length() == 0 || bin.length() == 0) {
        err = "manifest illeggibile";
        return false;
    }
    strncpy(foundVer, ver.c_str(), sizeof(foundVer) - 1);
    strncpy(foundUrl, bin.c_str(), sizeof(foundUrl) - 1);
    strncpy(foundNotes, jsonField(body, "notes").c_str(), sizeof(foundNotes) - 1);
    updateReady = parseVersion(foundVer) > parseVersion(FW_VERSION);
    return true;
}

// Scarica e installa quello che lookForUpdate() ha trovato. Al termine la
// scheda si riavvia da sola: non c'e' niente da confermare dopo.
void installFound() {
    if (!updateReady || foundUrl[0] == '\0') return;
    // Senza uplink non si scarica niente, e provarci porta dritti in NET_FAILED —
    // che e' uno stato terminale: da li' si esce solo riavviando. Meglio non
    // partire affatto.
    if (WiFi.status() != WL_CONNECTED) {
        setStatus("nessuna connessione");
        return;
    }
    currentStage = NetPortal::NET_UPDATING;
    otaProgress = 0;
    setStatus("scarico dalla rete");

    WiFiClientSecure dl;
    dl.setInsecure();
    httpUpdate.rebootOnUpdate(true);
    const t_httpUpdate_return res = httpUpdate.update(dl, foundUrl);
    if (res != HTTP_UPDATE_OK) {
        currentStage = NetPortal::NET_FAILED;
        setStatus("download fallito");
        // L'offerta si ritira: la schermata del fallimento non deve continuare a
        // proporre un tasto che non porta piu' da nessuna parte.
        updateReady = false;
        Serial.printf("httpUpdate: %d %s\n", (int)httpUpdate.getLastError(),
                      httpUpdate.getLastErrorString().c_str());
    }
}

void handleCheck() {
    if (!requireAuth()) return;

    String url = server.arg("url");
    if (url.length() == 0) url = Storage::loadManifestUrl(FW_MANIFEST_URL);
    Storage::saveManifestUrl(url.c_str());

    String err;
    if (!lookForUpdate(url, err)) {
        server.send(200, "text/html", pageMessage("Controllo non riuscito", err, false));
        return;
    }
    if (!updateReady) {
        server.send(200, "text/html",
                    pageMessage("Gia' aggiornato",
                                String("Installata la ") + FW_VERSION + ", disponibile la " +
                                    foundVer + ".",
                                true));
        return;
    }
    // Il controllo l'ha appena fatto una persona: quello automatico non ha piu'
    // niente da aggiungere, e lasciarlo partire piu' tardi vorrebbe dire che un
    // solo tentativo storto cancella un aggiornamento gia' confermato — perche'
    // lookForUpdate azzera l'esito all'ingresso.
    autoChecked = true;
    const String remoteVer = foundVer;
    const String notes = foundNotes;

    // Da qui in poi la risposta parte prima del download: il trasferimento dura
    // decine di secondi e la pagina andrebbe in timeout.
    server.send(200, "text/html",
                pageMessage("Aggiornamento in corso",
                            String("Sto scaricando la ") + remoteVer + ". " + notes +
                                "<br>Segui l'avanzamento sul display: al termine il synth "
                                "si riavvia da solo.",
                            true));
    server.client().stop();
    installFound();
}

}  // namespace

namespace NetPortal {

void begin() {
    if (started) return;
    started = true;

    // Prima di tutto: il core 0 deve restare libero per lo stack WiFi.
    AudioEngine::shutdown();
    StatusLed::off();  // le animazioni non gireranno piu': meglio spento che fermo

    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(apSsid, sizeof(apSsid), "ArcadeVox-%02X%02X", mac[4], mac[5]);
    // Password derivata dal MAC: sempre la stessa a ogni accensione, quindi si
    // puo' stampare sul display, ma non e' indovinabile da fuori.
    snprintf(apPass, sizeof(apPass), "arcade%02X%02X", mac[3], mac[4]);

    WiFi.persistent(false);

    // --- prima l'ascolto, poi la voce ---
    // La scansione si fa adesso, da sola sulla radio, con l'access point ancora
    // spento: e' l'unico momento in cui saltare da un canale all'altro non costa
    // niente a nessuno. Dura due o tre secondi, il display lo dice, e in cambio
    // quando il telefono arriva l'elenco delle reti e' gia' pronto — invece di
    // cominciare a formarsi proprio mentre il telefono ne avrebbe bisogno.
    WiFi.mode(WIFI_STA);
    currentStage = NET_SCAN;
    setStatus("cerco le reti");
    Display::updateNetwork();  // il loop non gira ancora: si disegna a mano
    harvestScan(WiFi.scanNetworks(false /* bloccante */, false /* niente nascoste */));
    Serial.printf("NETWORK: %d reti trovate\n", scanCount);

    WiFi.mode(WIFI_AP_STA);  // l'AP resta su anche dopo essere entrati in rete
    WiFi.softAP(apSsid, apPass);
    delay(100);

    snprintf(portal, sizeof(portal), "http://%s/", WiFi.softAPIP().toString().c_str());
    setQrForJoin();

    dns.setErrorReplyCode(DNSReplyCode::NoError);
    // Vita zero nelle risposte: il telefono non deve tenersi in cache l'esito di
    // una sonda vecchia. Android ne manda parecchie di fila, e una sola risposta
    // ricordata piu' del dovuto basta a fargli decidere che la rete va bene cosi'
    // — e a non aprire piu' il portale.
    dns.setTTL(0);
    dns.start(DNS_PORT, "*", WiFi.softAPIP());  // qualunque nome porta al portale

    server.on("/", HTTP_GET, handleRoot);
    server.on("/update", HTTP_POST, handleUploadEnd, handleUploadData);
    server.on("/wifi", HTTP_POST, handleWifi);
    server.on("/forget", HTTP_POST, handleForget);
    server.on("/rescan", HTTP_GET, handleRescan);
    server.on("/check", HTTP_GET, handleCheck);
    // Sonde di rilevazione del captive portal.
    server.on("/generate_204", HTTP_GET, handleCaptive);
    server.on("/gen_204", HTTP_GET, handleCaptive);
    server.on("/hotspot-detect.html", HTTP_GET, handleCaptive);
    server.on("/ncsi.txt", HTTP_GET, handleCaptive);
    server.onNotFound(handleCaptive);
    server.begin();

    Update.onProgress([](size_t done, size_t total) {
        otaProgress = total ? (int)((done * 100) / total) : 0;
        // Il trasferimento tiene occupato il loop: senza questo il display
        // resterebbe congelato per tutta la durata dell'aggiornamento.
        Display::drawOtaProgress(otaProgress);
    });

    // Le credenziali della rete di casa erano salvate in NVS da sempre, ma
    // nessuno le rileggeva: si finiva per ridigitarle sul telefono ad ogni
    // aggiornamento. Da qui in poi il synth ci riprova da solo.
    String savedSsid, savedPass;
    if (Storage::loadWifi(savedSsid, savedPass)) {
        WiFi.begin(savedSsid.c_str(), savedPass.c_str());
        staAuto = true;
        staDeadline = millis() + 20000;
        Serial.printf("NETWORK: riprovo la rete salvata '%s'\n", savedSsid.c_str());
    }

    currentStage = NET_AP;
    setStatus(staAuto ? "mi ricollego alla rete" : "in attesa del telefono");
    Serial.printf("NETWORK: %s / %s -> %s\n", apSsid, apPass, portal);
}

bool active() { return started; }

void update() {
    if (!started) return;

    dns.processNextRequest();
    server.handleClient();

    if (currentStage == NET_UPDATING || currentStage == NET_FAILED) return;

    if (staAuto) {
        if (WiFi.status() == WL_CONNECTED) {
            strncpy(staIpText, WiFi.localIP().toString().c_str(), sizeof(staIpText) - 1);
            staAuto = false;
            Serial.printf("NETWORK: in rete come %s\n", staIpText);
        } else if ((int32_t)(millis() - staDeadline) > 0) {
            staAuto = false;  // pazienza: resta il portale dall'access point
            Serial.println(F("NETWORK: rete salvata non raggiunta"));
        }
    }

    // Appena c'e' internet, il synth va a vedere da solo se esiste una versione
    // piu' nuova. Una volta sola per accensione: il manifest non cambia mentre
    // sei li' a guardarlo, e ogni lettura sono un paio di secondi di attesa.
    // Il controllo automatico serve al flusso *senza telefono*: la radio si
    // accende, il synth ritrova la rete di casa e dice da solo se c'e' una
    // versione nuova. Se invece un telefono e' agganciato all'access point non si
    // fa: leggere il manifest e' una richiesta bloccante da qualche secondo, e
    // quei secondi il portale li passerebbe muto — senza rispondere ne' al DNS ne'
    // alle richieste della pagina, che nel frattempo si sta ricaricando da sola.
    // Chi ha il telefono in mano ha gia' il suo pulsante "Cerca aggiornamenti".
    const bool phoneAttached = WiFi.softAPgetStationNum() > 0;
    if (!autoChecked && autoTries < 3 && !phoneAttached && WiFi.status() == WL_CONNECTED &&
        (autoRetryAt == 0 || (int32_t)(millis() - autoRetryAt) > 0)) {
        ++autoTries;
        String err;
        // Leggere il manifest e' bloccante: qualche secondo in cui il loop non
        // gira e il display resterebbe fermo sull'ultima cosa scritta. Si dice
        // prima cosa sta succedendo, e lo si disegna a mano — altrimenti quei
        // secondi si leggono come una scheda piantata. Sei e non dieci: e' il
        // tempo in cui il portale resta muto, e va tenuto corto.
        setStatus("cerco aggiornamenti");
        Display::updateNetwork();
        if (lookForUpdate(Storage::loadManifestUrl(FW_MANIFEST_URL), err, 6000)) {
            // Solo adesso si smette di riprovare: marcare il tentativo *prima*
            // voleva dire che un singolo momento storto — il DNS ancora freddo,
            // il router lento a dare la rotta — spegneva il controllo per tutta
            // l'accensione.
            autoChecked = true;
            if (updateReady) {
                Serial.printf("NETWORK: disponibile la %s\n", foundVer);
                setStatus("aggiornamento pronto");
            } else {
                setStatus("gia' aggiornato");
            }
        } else {
            // Tre tentativi e poi basta: un manifest irraggiungibile non
            // diventa raggiungibile a furia di riprovare, e ogni tentativo sono
            // sei secondi in cui il portale non risponde a nessuno.
            Serial.printf("NETWORK: controllo fallito (%s), tentativo %d di 3\n", err.c_str(),
                          (int)autoTries);
            setStatus(autoTries < 3 ? "riprovo fra poco" : "controllo non riuscito");
            autoRetryAt = millis() + 20000;
        }
    }

    // Attesa del collegamento alla rete di casa.
    if (currentStage == NET_STA_WAIT) {
        if (WiFi.status() == WL_CONNECTED) {
            strncpy(staIpText, WiFi.localIP().toString().c_str(), sizeof(staIpText) - 1);
            currentStage = NET_STA_OK;
            setStatus("collegato a internet");
        } else if ((int32_t)(millis() - staDeadline) > 0) {
            currentStage = NET_STA_FAIL;
            setStatus("rete non raggiunta");
        }
        return;
    }
    if (currentStage == NET_STA_OK || currentStage == NET_STA_FAIL) return;

    // Finche' siamo al primo passo, il QR segue lo stato: rete, poi indirizzo.
    const bool anyClient = WiFi.softAPgetStationNum() > 0;
    if (anyClient && currentStage == NET_AP) {
        currentStage = NET_CONNECTED;
        setQrForPortal();
        setStatus("apri il portale");
    } else if (!anyClient && currentStage == NET_CONNECTED) {
        currentStage = NET_AP;
        setQrForJoin();
        setStatus("in attesa del telefono");
    }
}

Stage stage() { return currentStage; }
const char *ssid() { return apSsid; }
const char *password() { return apPass; }
const char *qrPayload() { return qrText; }
const char *portalUrl() { return portal; }
const char *staIp() { return staIpText; }
const char *message() { return statusMsg; }
int progress() { return otaProgress; }

const char *updateVersion() { return updateReady ? foundVer : ""; }
bool updateAvailable() { return updateReady; }
void installUpdate() { installFound(); }

}  // namespace NetPortal
