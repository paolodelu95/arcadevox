// net_portal.h — modalita' NETWORK: access point, captive portal e OTA.
//
// E' una modalita' *esclusiva*: lo stack WiFi di Arduino gira sul core 0, lo
// stesso del task audio, con priorita' molto piu' alta. Farli convivere
// significherebbe far sottoscorrere il DMA dell'I2S ad ogni pacchetto. Quindi
// all'ingresso il motore audio viene spento e il synth resta muto fino al
// riavvio: non e' un limite vero, non si suona mentre si aggiorna il firmware.
//
// Due strade per aggiornare, entrambe dallo smartphone:
//   1. carichi il .bin direttamente dal telefono (nessuna rete esterna serve);
//   2. scegli la tua rete da un elenco e scrivi la password, e lui si scarica il
//      firmware da solo. La rete resta in memoria: dalla seconda volta in poi si
//      ricollega senza che nessuno tocchi niente.
#pragma once

// Nome utente del portale. Sta qui e non dentro net_portal.cpp perche' il
// display lo scrive accanto al QR: la password si legge gia' a schermo, ma senza
// l'utente la finestra di login del browser resta un indovinello.
#define NET_AUTH_USER "arcade"

#include <Arduino.h>

namespace NetPortal {

enum Stage : uint8_t {
    NET_OFF = 0,
    NET_SCAN,        // setaccio l'etere: sto guardando che reti ci sono
    NET_AP,          // access point attivo, in attesa dello smartphone
    NET_CONNECTED,   // almeno un client agganciato all'AP
    NET_STA_WAIT,    // tentativo di collegamento alla rete di casa
    NET_STA_OK,      // collegato a internet
    NET_STA_FAIL,
    NET_UPDATING,    // trasferimento del firmware in corso
    NET_FAILED
};

// Accende la radio. Prima spegne il motore audio: da qui in poi si esce solo
// con un riavvio.
void begin();
bool active();
void update();  // da chiamare ad ogni giro finche' la modalita' e' attiva

Stage stage();
const char *ssid();
const char *password();
// Testo da incidere nel QR: la stringa "WIFI:..." per agganciare la rete finche'
// nessuno e' connesso, poi l'indirizzo del portale.
const char *qrPayload();
const char *portalUrl();
const char *staIp();      // "" se non collegati a una rete di casa
// A quale rete: con piu' reti in memoria, "sono in rete" non basta piu' —
// serve sapere su quale, perche' e' l'unico modo di accorgersi che si e'
// agganciato all'hotspot del telefono invece che al wifi del posto.
const char *staSsid();
const char *message();    // riga di stato per il display
int progress();           // 0..100 durante il trasferimento

// --- l'aggiornamento trovato da solo ---
// Appena il synth entra in rete legge il manifest per conto suo. E' cio' che
// rende il telefono facoltativo: la prima volta bisogna pur dirgli qual e' la
// rete di casa, ma dalla seconda in poi si accende la radio e sul display c'e'
// gia' scritto se esiste una versione nuova.
bool updateAvailable();
const char *updateVersion();  // "" se non ce n'e' una
void installUpdate();         // scarica e riavvia: dal synth, senza telefono

}  // namespace NetPortal
