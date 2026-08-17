// midi_io.h — MIDI IN dalla porta USB nativa (OTG).
//
// L'ESP32-S3 ha due periferiche USB sugli stessi due piedini: il seriale/JTAG
// integrato, che sa fare solo la porta seriale, e l'OTG, che con TinyUSB puo'
// presentarsi come qualunque cosa. Passando a ARDUINO_USB_MODE=0 si usa la
// seconda, e il synth compare al computer come **strumento MIDI** oltre che come
// porta seriale: un dispositivo composito, una presa sola.
//
// La classe MIDI e' gia' compilata dentro la TinyUSB che il core si porta
// dietro (CONFIG_TINYUSB_MIDI_ENABLED=y): non serve nessuna libreria in piu',
// basta registrare l'interfaccia prima di avviare l'USB.
#pragma once

#include <Arduino.h>

// Messaggi che interessano al synth. Tutto il resto (system exclusive, real
// time, aftertouch) viene scartato appena letto: non avrebbe dove andare.
enum MidiKind : uint8_t {
    MIDI_NONE = 0,
    MIDI_NOTE_ON,
    MIDI_NOTE_OFF,
    MIDI_CC,
    MIDI_PROGRAM,
    MIDI_BEND,
    MIDI_ALL_OFF  // panico: CC 120/123, o cavo staccato
};

struct MidiEvent {
    uint8_t kind;
    uint8_t channel;  // 0..15
    uint8_t data1;    // nota, numero di CC, programma
    uint8_t data2;    // velocity o valore
    int16_t bend;     // solo per MIDI_BEND: -8192..+8191
};

namespace MidiIn {

// Registra l'interfaccia MIDI e avvia l'USB. Va chiamata **prima** che
// qualcuno usi la porta: dopo tinyusb_init non si aggiungono interfacce.
void begin();

// True quando il computer ha agganciato il dispositivo.
bool connected();

// Legge dalla porta e restituisce il prossimo messaggio, MIDI_NONE se non ce
// n'e'. Da chiamare a ripetizione in ogni giro di loop finche' non torna vuoto.
MidiEvent poll();

// Quante note stanno suonando per via del MIDI: serve al display per far vedere
// che il synth lo sta pilotando qualcun altro.
uint8_t activeNotes();
void noteCountSet(uint8_t n);

// Endpoint assegnati all'interfaccia. Zero significa che l'allocatore li aveva
// finiti, e allora il verso corrispondente non funziona: e' la prima cosa da
// guardare se il MIDI enumera ma non passa dati.
void endpoints(uint8_t &in, uint8_t &out);

}  // namespace MidiIn

// ============================================================================
// MIDI OUT
// ============================================================================
// Il verso opposto: quello che suoni finisce sul computer. Tasti, arpeggiator,
// sequencer e note aggiunte dagli accordi partono tutti dallo stesso punto —
// l'elenco di cosa deve suonare adesso, quello che pilota anche il motore — per
// cui non c'e' modo che le due cose vadano fuori sincrono.
//
// Le note che arrivano **dal** MIDI non vengono rimandate indietro: con un DAW
// che rimanda in eco quello che riceve si innescherebbe un anello, e ogni nota
// si moltiplicherebbe da sola.
namespace MidiOut {

void noteOn(uint8_t note, uint8_t velocity);
void noteOff(uint8_t note);
void cc(uint8_t number, uint8_t value);
void program(uint8_t number);

// --- trasporto ---
// Start/stop e il clock a 24 impulsi per movimento: e' quello che serve a un
// sequencer esterno per andare a tempo con questo.
void start();
void stop();
void clock();

// Il computer sta ascoltando? Se non c'e' nessuno collegato si evita perfino di
// comporre i messaggi.
bool connected();

}  // namespace MidiOut
