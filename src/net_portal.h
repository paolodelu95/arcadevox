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
//   2. dai al synth le credenziali di casa e lui si scarica il firmware da solo.
#pragma once

// Nome utente del portale. Sta qui e non dentro net_portal.cpp perche' il
// display lo scrive accanto al QR: la password si legge gia' a schermo, ma senza
// l'utente la finestra di login del browser resta un indovinello.
#define NET_AUTH_USER "arcade"

#include <Arduino.h>

namespace NetPortal {

enum Stage : uint8_t {
    NET_OFF = 0,
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
const char *staIp();      // "" se non collegati alla rete di casa
const char *message();    // riga di stato per il display
int progress();           // 0..100 durante il trasferimento

}  // namespace NetPortal
