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

}  // namespace MidiIn
