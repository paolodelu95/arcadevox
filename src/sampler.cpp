// sampler.cpp — vedi sampler.h.

#include "sampler.h"

#include <math.h>

#include "audio_engine.h"

namespace Sampler {

void pianoNote(int midi) {
    if (PIANO_ROOT_COUNT == 0) return;

    // Radice piu' vicina in semitoni. Con sette radici il ciclo costa niente, e
    // scriverlo come ricerca invece che come formula lo tiene giusto anche se un
    // domani le radici diventano nove o cambiano passo.
    const PianoRoot *best = &PIANO_ROOTS[0];
    int bestDelta = midi - (int)PIANO_ROOTS[0].midi;
    for (uint8_t i = 1; i < PIANO_ROOT_COUNT; ++i) {
        const int d = midi - (int)PIANO_ROOTS[i].midi;
        if (abs(d) < abs(bestDelta)) {
            bestDelta = d;
            best = &PIANO_ROOTS[i];
        }
    }

    // Un semitono e' la radice dodicesima di due: dodici semitoni raddoppiano la
    // frequenza, ed e' lo stesso rapporto con cui si rilegge il campione.
    const float ratio = powf(2.0f, (float)bestDelta / 12.0f);

    uint32_t rate = (uint32_t)((float)INSTRUMENT_RATE * ratio + 0.5f);
    if (rate < 1) rate = 1;

    // `false`: la manopola VELOCITA della schermata SUONI non deve toccare il
    // piano. Li' scordare e' il gioco, qui sarebbe un pianoforte fuori accordo.
    AudioEngine::playSample(best->data, best->len, rate, false);
}

void drumHit(uint8_t index) {
    if (index >= DRUM_COUNT) return;
    AudioEngine::playSample(DRUM_KIT[index].data, DRUM_KIT[index].len, INSTRUMENT_RATE, false);
}

const char *drumName(uint8_t index) {
    return (index < DRUM_COUNT) ? DRUM_KIT[index].name : "";
}

}  // namespace Sampler
