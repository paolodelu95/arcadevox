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
// Setacciare i canali richiede qualche secondo. Farlo in modo sincrono dentro la
// richiesta HTTP terrebbe fermo il server per tutto quel tempo e la pagina
// sembrerebbe piantata: si lancia in asincrono e la si raccoglie quando e'
// pronta, ricaricando.
void startScanIfIdle() {
    if (WiFi.scanComplete() == WIFI_SCAN_FAILED) WiFi.scanNetworks(true /* async */);
}

// Una rete con lo stesso nome puo' comparire piu' volte (due bande, o un
// ripetitore): nell'elenco ne basta una.
bool alreadyListed(int upTo, const String &name) {
    for (int i = 0; i < upTo; ++i) {
        if (WiFi.SSID(i) == name) return true;
    }
    return false;
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
        s += knownSsid;
        s += "</span></small>";
    }
    startScanIfIdle();
    const int found = WiFi.scanComplete();
    if (found > 0) {
        s += "<select name=ssid>";
        for (int i = 0; i < found; ++i) {
            const String name = WiFi.SSID(i);
            if (name.length() == 0 || alreadyListed(i, name)) continue;
            s += "<option value='";
            s += name;
            s += "'";
            if (known && name == knownSsid) s += " selected";
            s += ">";
            s += name;
            s += "  (";
            s += String(WiFi.RSSI(i));
            s += " dBm)</option>";
        }
        s += "</select>";
        s += "<small>Non la vedi? Scrivila qui sotto, oppure ";
        s += "<a style='color:#0aa' href='/rescan'>ripeti la scansione</a>.</small>";
    } else {
        s += "<small>Scansione delle reti in corso: ";
        s += "<a style='color:#0aa' href='/'>ricarica</a> fra un istante. ";
        s += "Nel frattempo puoi scrivere il nome a mano.</small>";
    }
    // Resta un campo libero: le reti nascoste non compaiono in nessuna scansione,
    // e se e' pieno ha la precedenza sulla tendina.
    s += "<input name=ssid_manual placeholder='oppure scrivi il nome' value=''>"
         "<input name=pass type=password placeholder=password>"
         "<button>Collega</button></form>";
    if (known) {
        s += "<form method=POST action=/forget>"
             "<button style='background:#622;color:#fdd'>Dimentica la rete</button></form>";
    }

    s += "<h2>Aggiorna da internet</h2><form method=GET action=/check>"
         "<small>Indirizzo del manifest delle release.</small>"
         "<input name=url value='";
    s += Storage::loadManifestUrl(FW_MANIFEST_URL);
    s += "'><button>Cerca aggiornamenti</button></form>";

    s += "<small>Per uscire dalla modalita' rete premi PLAY sul synth: si "
         "riavvia e torna suonabile.</small>";
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
void handleCaptive() {
    server.sendHeader("Location", portal, true);
    server.send(302, "text/plain", "");
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
    WiFi.scanDelete();      // butta il risultato vecchio...
    WiFi.scanNetworks(true);  // ...e ne chiede uno nuovo
    server.sendHeader("Location", "/");
    server.send(303);
}

// Cambiare casa, o rete, senza dover rientrare a mano ogni volta in una rete
// che non esiste piu'.
void handleForget() {
    if (!requireAuth()) return;
    Storage::clearWifi();
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

    Storage::saveWifi(ssid.c_str(), pass.c_str());
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

void handleCheck() {
    if (!requireAuth()) return;

    String url = server.arg("url");
    if (url.length() == 0) url = Storage::loadManifestUrl(FW_MANIFEST_URL);
    Storage::saveManifestUrl(url.c_str());

    if (WiFi.status() != WL_CONNECTED) {
        server.send(200, "text/html",
                    pageMessage("Nessuna connessione",
                                "Il synth non e' collegato a internet: dagli "
                                "prima le credenziali della rete di casa.",
                                false));
        return;
    }

    WiFiClientSecure client;
    // Nessuna verifica del certificato: il synth non ha un orologio affidabile
    // ne' un bundle di CA da tenere aggiornato. Il canale resta cifrato, ma un
    // uomo-nel-mezzo sulla rete di casa potrebbe servire un altro firmware.
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, url)) {
        server.send(200, "text/html", pageMessage("Indirizzo non valido", url, false));
        return;
    }
    http.setTimeout(10000);
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        server.send(200, "text/html",
                    pageMessage("Manifest non raggiungibile",
                                String("Il server ha risposto ") + code, false));
        return;
    }
    const String body = http.getString();
    http.end();

    const String remoteVer = jsonField(body, "version");
    const String binUrl = jsonField(body, "url");
    const String notes = jsonField(body, "notes");

    if (remoteVer.length() == 0 || binUrl.length() == 0) {
        server.send(200, "text/html",
                    pageMessage("Manifest illeggibile",
                                "Servono i campi \"version\" e \"url\".", false));
        return;
    }
    if (parseVersion(remoteVer.c_str()) <= parseVersion(FW_VERSION)) {
        server.send(200, "text/html",
                    pageMessage("Gia' aggiornato",
                                String("Installata la ") + FW_VERSION + ", disponibile la " +
                                    remoteVer + ".",
                                true));
        return;
    }

    // Da qui in poi la risposta parte prima del download: il trasferimento dura
    // decine di secondi e la pagina andrebbe in timeout.
    server.send(200, "text/html",
                pageMessage("Aggiornamento in corso",
                            String("Sto scaricando la ") + remoteVer + ". " + notes +
                                "<br>Segui l'avanzamento sul display: al termine il synth "
                                "si riavvia da solo.",
                            true));
    server.client().stop();

    currentStage = NetPortal::NET_UPDATING;
    otaProgress = 0;
    setStatus("scarico dalla rete");

    WiFiClientSecure dl;
    dl.setInsecure();
    httpUpdate.rebootOnUpdate(true);
    const t_httpUpdate_return res = httpUpdate.update(dl, binUrl);
    if (res != HTTP_UPDATE_OK) {
        currentStage = NetPortal::NET_FAILED;
        setStatus("download fallito");
        Serial.printf("httpUpdate: %d %s\n", (int)httpUpdate.getLastError(),
                      httpUpdate.getLastErrorString().c_str());
    }
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
    WiFi.mode(WIFI_AP_STA);  // l'AP resta su anche dopo essere entrati in rete
    WiFi.softAP(apSsid, apPass);
    delay(100);

    snprintf(portal, sizeof(portal), "http://%s/", WiFi.softAPIP().toString().c_str());
    setQrForJoin();

    dns.setErrorReplyCode(DNSReplyCode::NoError);
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

}  // namespace NetPortal
