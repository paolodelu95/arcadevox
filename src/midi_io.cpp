// midi_io.cpp — interfaccia MIDI su TinyUSB.

#include "midi_io.h"

#if ARDUINO_USB_MODE
// Con il seriale/JTAG integrato (ARDUINO_USB_MODE=1) l'USB non e' programmabile:
// il MIDI non esiste e il modulo si riduce a un guscio vuoto, cosi' il firmware
// compila lo stesso e chi non vuole il MIDI non paga niente.
namespace MidiIn {
void begin() {}
bool connected() { return false; }
MidiEvent poll() { return MidiEvent{MIDI_NONE, 0, 0, 0, 0}; }
uint8_t activeNotes() { return 0; }
void noteCountSet(uint8_t) {}
void endpoints(uint8_t &in, uint8_t &out) { in = 0; out = 0; }
}  // namespace MidiIn
namespace MidiOut {
void noteOn(uint8_t, uint8_t) {}
void noteOff(uint8_t) {}
void cc(uint8_t, uint8_t) {}
void program(uint8_t) {}
void start() {}
void stop() {}
void clock() {}
bool connected() { return false; }
}  // namespace MidiOut

#else

#include <USB.h>
#include <esp32-hal-tinyusb.h>
#include <string.h>

extern "C" {
#include "tusb.h"
}

namespace {

bool started = false;
uint8_t liveNotes = 0;
// Quali endpoint ci ha dato l'allocatore. Zero vuol dire "finiti": sull'S3 le
// FIFO di trasmissione sono poche e la seriale ne prende gia' due.
uint8_t gEpIn = 0xFF, gEpOut = 0xFF;

// Descrittore dell'interfaccia. La chiama TinyUSB mentre monta la
// configurazione, una volta sola, prima che il dispositivo compaia al computer.
extern "C" uint16_t arcadevoxMidiDescriptor(uint8_t *dst, uint8_t *itf) {
    const uint8_t str = tinyusb_add_string_descriptor("ArcadeVox MIDI");
    const uint8_t epOut = tinyusb_get_free_out_endpoint();
    gEpOut = epOut;
    TU_VERIFY(epOut != 0);
    const uint8_t epIn = tinyusb_get_free_in_endpoint();
    gEpIn = epIn;
    TU_VERIFY(epIn != 0);

    const uint8_t descriptor[TUD_MIDI_DESC_LEN] = {
        TUD_MIDI_DESCRIPTOR(*itf, str, epOut, (uint8_t)(0x80 | epIn), 64)};
    // Il MIDI occupa **due** interfacce: quella di controllo audio e quella di
    // streaming. Contarne una sola sfasa i numeri di tutto quello che viene
    // registrato dopo, e il dispositivo non enumera.
    *itf += 2;
    memcpy(dst, descriptor, TUD_MIDI_DESC_LEN);
    return TUD_MIDI_DESC_LEN;
}

// L'interfaccia si registra **prima di setup()**, in un costruttore globale.
//
// Non e' un vezzo: l'oggetto `Serial` del core e' anche lui globale, e nel suo
// costruttore registra la CDC e fa partire TinyUSB. Quando setup() comincia,
// l'elenco delle interfacce e' gia' chiuso, e chiedere di aggiungerne una
// ottiene solo una riga di log:
//
//     tinyusb_enable_interface(): TinyUSB has already started!
//                                 Interface MIDI not enabled
//
// Registrandosi allo stesso stadio dei costruttori globali il MIDI entra
// nell'elenco insieme alla seriale, e il dispositivo compare al computer con
// tutte e due dentro. In che ordine fra loro non conta: la numerazione la fa
// TinyUSB con il contatore che gli passiamo.
struct MidiRegistrar {
    MidiRegistrar() {
        tinyusb_enable_interface(USB_INTERFACE_MIDI, TUD_MIDI_DESC_LEN,
                                 arcadevoxMidiDescriptor);
    }
};
MidiRegistrar registrar;

}  // namespace

namespace MidiIn {

void begin() {
    if (started) return;
    started = true;
    // L'interfaccia c'e' gia' (vedi MidiRegistrar): qui si controlla solo che
    // l'USB sia avviato. Se il core l'ha gia' fatto, begin() non fa niente.
    USB.begin();
}

bool connected() { return started && tud_ready(); }

MidiEvent poll() {
    MidiEvent e = {MIDI_NONE, 0, 0, 0, 0};
    if (!started) return e;

    uint8_t packet[4];
    while (tud_midi_available() && tud_midi_packet_read(packet)) {
        // packet[0] e' il "cable number" piu' il code index: il messaggio vero
        // sta nei tre byte dopo. I system-exclusive e i real-time hanno codici
        // che non ci interessano e si scartano leggendo il successivo.
        const uint8_t status = packet[1];
        const uint8_t type = status & 0xF0;
        const uint8_t ch = status & 0x0F;
        const uint8_t d1 = packet[2] & 0x7F;
        const uint8_t d2 = packet[3] & 0x7F;

        switch (type) {
            case 0x90:
                // Velocity zero e' un note-off travestito: mezzo mondo MIDI lo
                // manda cosi' invece dello 0x80, e trattarlo come un attacco
                // silenzioso lascerebbe la nota appesa per sempre.
                e.kind = (d2 > 0) ? MIDI_NOTE_ON : MIDI_NOTE_OFF;
                break;
            case 0x80:
                e.kind = MIDI_NOTE_OFF;
                break;
            case 0xB0:
                e.kind = (d1 == 120 || d1 == 123) ? MIDI_ALL_OFF : MIDI_CC;
                break;
            case 0xC0:
                e.kind = MIDI_PROGRAM;
                break;
            case 0xE0:
                e.kind = MIDI_BEND;
                e.bend = (int16_t)(((uint16_t)d2 << 7 | d1)) - 8192;
                break;
            default:
                continue;  // non ci riguarda: si passa al pacchetto dopo
        }
        e.channel = ch;
        e.data1 = d1;
        e.data2 = d2;
        return e;
    }
    return e;
}

uint8_t activeNotes() { return liveNotes; }
void noteCountSet(uint8_t n) { liveNotes = n; }
void endpoints(uint8_t &in, uint8_t &out) { in = gEpIn; out = gEpOut; }

}  // namespace MidiIn

namespace MidiOut {

namespace {
// Canale 1, che e' quello che un DAW guarda per primo.
constexpr uint8_t CH = 0;

inline void send(const uint8_t *data, uint32_t len) {
    // Il gancio e' tud_ready(), non tud_midi_mounted().
    //
    // Sulla scheda si vede una cosa che il nome non lascerebbe immaginare:
    // arrivano pacchetti MIDI dal computer — quindi l'interfaccia e' su e
    // funzionante — mentre tud_midi_mounted() continua a rispondere di no. Con
    // quel controllo davanti, il MIDI OUT non mandava mai niente e sembrava
    // rotto. tud_ready() dice cio' che serve davvero sapere: il dispositivo e'
    // configurato e il computer sta dall'altra parte.
    if (!started || !tud_ready()) return;
    tud_midi_stream_write(0, data, len);
}
}  // namespace

void noteOn(uint8_t note, uint8_t velocity) {
    const uint8_t m[3] = {(uint8_t)(0x90 | CH), (uint8_t)(note & 0x7F),
                          (uint8_t)(velocity & 0x7F)};
    send(m, 3);
}

void noteOff(uint8_t note) {
    const uint8_t m[3] = {(uint8_t)(0x80 | CH), (uint8_t)(note & 0x7F), 0};
    send(m, 3);
}

void cc(uint8_t number, uint8_t value) {
    const uint8_t m[3] = {(uint8_t)(0xB0 | CH), (uint8_t)(number & 0x7F),
                          (uint8_t)(value & 0x7F)};
    send(m, 3);
}

void program(uint8_t number) {
    const uint8_t m[2] = {(uint8_t)(0xC0 | CH), (uint8_t)(number & 0x7F)};
    send(m, 2);
}

// I messaggi di sistema in tempo reale non hanno canale e possono infilarsi
// ovunque, anche in mezzo a un altro messaggio: e' cosi' che sono fatti.
void start() { const uint8_t m = 0xFA; send(&m, 1); }
void stop() { const uint8_t m = 0xFC; send(&m, 1); }
void clock() { const uint8_t m = 0xF8; send(&m, 1); }

bool connected() { return started && tud_ready(); }

}  // namespace MidiOut

#endif  // ARDUINO_USB_MODE
