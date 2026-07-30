// sim_fakes.h — le manopole con cui il driver mette il synth in uno stato preciso.
//
// I moduli veri (motore audio, portale di rete, sequencer) qui non ci sono: sul
// Mac non c'e' ne' un I2S ne' una radio. Ci sono le loro *risposte*, che e' tutto
// cio' che il display guarda. Il criterio con cui sono scritte e' uno solo: le
// stringhe devono essere quelle vere, carattere per carattere, perche' e' la loro
// lunghezza a decidere se il testo sta dentro il cerchio o ne esce.
#pragma once

#include <Arduino.h>

#include "../../src/sequencer.h"

namespace Sim {

// --- motore audio -----------------------------------------------------------
// Livelli che il VU legge. peakLevel() sul chip e' distruttiva (azzera dopo la
// lettura): qui no, altrimenti una scena non sarebbe ripetibile.
void setLevels(float rms, float peak);

// Forma d'onda e ampiezza della finestra dell'oscilloscopio, in scala -127..127.
// `amp` a 0 e' silenzio vero, a 127 e' fondo scala.
void setScope(uint8_t waveform, float amp);
// copyScope() restituisce false quando non c'e' una finestra nuova: e' lo stato
// in cui il display tiene a video la traccia precedente, e va poter provare.
void setScopeFresh(bool fresh);

// --- sequencer --------------------------------------------------------------
void seqClear();                                     // tutti i 16 step a pausa
void seqFill();                                      // pattern pieno, note e ottave varie
void seqSetStep(int index, int8_t note, int8_t oct);  // note = SEQ_REST / SEQ_TIE ammessi

// --- portale di rete --------------------------------------------------------
// ssid e password hanno il formato che genera net_portal.cpp dal MAC:
// "ArcadeVox-%02X%02X" e "arcade%02X%02X". Lunghezze reali, non abbreviate.
void netSet(uint8_t stage, const char *qr, const char *msg, const char *staIp);

}  // namespace Sim
