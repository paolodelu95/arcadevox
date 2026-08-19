// instruments.h — il piano campionato e la batteria della schermata RITMO.
//
// Due strumenti veri accanto al motore sottrattivo, entrambi riletti dal player
// di campioni che gia' serviva la schermata SUONI.
//
// Il PIANO e' multi-campione: sette radici, una ogni tre semitoni. Una nota si
// suona prendendo la radice piu' vicina e rileggendola piu' in fretta o piu'
// piano — `playSample()` accetta gia' la frequenza per singola riproduzione,
// quindi intonare non costa niente al motore.
//
// Sette radici bastano perche' lo spostamento massimo resta un semitono e mezzo:
// oltre quello un pianoforte comincia a sentirsi tirato, e la voce si allarga o
// si stringe in modo riconoscibile. Fuori dalle ventuno note coperte dalle
// radici si trasporta di **ottave intere**, cioe' con un rapporto esatto di 2,
// che e' l'unico spostamento grande che un orecchio perdona.
//
// I campioni vengono dalla University of Iowa Electronic Music Studios, che li
// pubblica dal 1997 dichiarandoli utilizzabili "for any projects, without
// restrictions". E' la ragione per cui sono questi e non altri: finiscono in
// firmware/firmware.bin, che il progetto ridistribuisce via OTA, e ridistribuire
// registrazioni altrui e' esattamente cio' che tools/samples/README.md vieta.
//
// La BATTERIA invece e' sintetizzata dalle formule in tools/make_instruments.py.
// Non e' un ripiego: i suoni di batteria elettronica nascono cosi' — una cassa e'
// una sinusoide che scende mentre si spegne, un rullante e' rumore piu' tono — e
// sintetizzarli costa qualche riga invece di centinaia di kilobyte.
#pragma once

#include <Arduino.h>

// Una radice del piano: il blob, la sua lunghezza e la nota MIDI a cui e' stata
// registrata, che serve a calcolare di quanto intonare.
struct PianoRoot {
    const uint8_t *data;
    uint32_t len;
    uint8_t midi;
};

// Frequenza a cui sono stati generati tutti i blob di questo file.
#define INSTRUMENT_RATE 16000

// Quante voci gli strumenti campionati aggiungono in coda all'elenco dei timbri:
// PIANO e BATTERIA. Sta qui e non in main.cpp perche' il menu deve sapere
// quanto e' lungo l'elenco e come si chiamano le ultime due voci.
#define INSTRUMENT_EXTRA 2

extern const PianoRoot PIANO_ROOTS[];
extern const uint8_t PIANO_ROOT_COUNT;

// Un pezzo della batteria. Niente nota MIDI: non si intona, si colpisce.
struct DrumHit {
    const char *name;
    const char *hint;
    const uint8_t *data;
    uint32_t len;
};

extern const DrumHit DRUM_KIT[];
extern const uint8_t DRUM_COUNT;
