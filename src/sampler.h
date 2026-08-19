// sampler.h — suonare il piano campionato e la batteria.
//
// Lo strato sottile fra i blob generati (instruments.h) e il player di campioni
// del motore audio. Non tiene stato di voci: i campioni sono one-shot, partono e
// finiscono da soli, e il rilascio del tasto non li ferma — su un pianoforte
// senza smorzatori e' esattamente cio' che succede quando tieni il pedale.
#pragma once

#include <Arduino.h>

#include "instruments.h"

namespace Sampler {

// Suona `midi` col piano, scegliendo la radice piu' vicina e rileggendola alla
// velocita' che la porta all'intonazione giusta.
//
// "Piu' vicina" si misura in semitoni con segno, senza casi particolari per le
// note fuori dall'arco delle radici: una nota due ottave sotto la radice piu'
// bassa da' uno scostamento di -24, cioe' un rapporto di 0,25 — una
// trasposizione d'ottava esatta, che e' l'unico spostamento grande che si
// perdona. Le note dentro l'arco non si spostano mai di piu' di un semitono e
// mezzo, che era il motivo di prendere sette radici invece di una.
void pianoNote(int midi);

// Colpisce il pezzo `index` della batteria (0..DRUM_COUNT-1). Fuori intervallo
// non fa niente: i tasti sono tredici e i pezzi otto, e il silenzio e' una
// risposta migliore di un colpo scelto a caso.
void drumHit(uint8_t index);

// Nome del pezzo, "" se l'indice non esiste. Serve al display e alle luci.
const char *drumName(uint8_t index);

}  // namespace Sampler
