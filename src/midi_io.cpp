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
}  // namespace MidiIn

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

// Descrittore dell'interfaccia. La chiama TinyUSB mentre monta la
// configurazione, una volta sola, prima che il dispositivo compaia al computer.
extern "C" uint16_t arcadevoxMidiDescriptor(uint8_t *dst, uint8_t *itf) {
    const uint8_t str = tinyusb_add_string_descriptor("ArcadeVox MIDI");
    const uint8_t epOut = tinyusb_get_free_out_endpoint();
    TU_VERIFY(epOut != 0);
    const uint8_t epIn = tinyusb_get_free_in_endpoint();
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

}  // namespace

namespace MidiIn {

void begin() {
    if (started) return;
    started = true;
    tinyusb_enable_interface(USB_INTERFACE_MIDI, TUD_MIDI_DESC_LEN, arcadevoxMidiDescriptor);
    // Da qui in poi la configurazione e' chiusa: USB.begin() chiama tinyusb_init
    // e il dispositivo compare al computer con dentro la seriale e il MIDI.
    USB.productName("ArcadeVox");
    USB.manufacturerName("ArcadeVox");
    USB.begin();
}

bool connected() { return started && tud_midi_mounted(); }

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

}  // namespace MidiIn

#endif  // ARDUINO_USB_MODE
