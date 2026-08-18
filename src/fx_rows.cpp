// fx_rows.cpp — la tabella dei nomi dell'elenco EFFETTI.
//
// Sta in un file suo e non nell'header per la stessa ragione per cui ci sta la
// tabella delle impostazioni: e' un dato, non una dichiarazione, e una copia sola
// in flash basta per tutti quelli che la leggono — main.cpp per applicare gli
// scatti, display.cpp per disegnarli.
#include "fx_rows.h"

// Le categorie seguono il percorso del suono al contrario, e non e' un capriccio:
// le due che si cercano piu' spesso — la grana dell'8 BIT e l'eco — stanno in
// cima, dove si arriva senza girare. Le due dell'inviluppo di filtro, che sono le
// piu' specialistiche, stanno in fondo.
const FxRowInfo FX_ROWS[FX_ROW_COUNT] = {
    {"8 BIT", "GRANA"},
    {"ECO", "MIX"},
    {nullptr, "TEMPO"},
    {nullptr, "RITORNO"},
    {"LFO", "SU"},
    {nullptr, "VELOCITA'"},
    {nullptr, "PROFONDITA"},
    {"ARPEGGIO", "MODO"},
    {"CORPO", "SUB"},
    {nullptr, "DETUNE"},
    {nullptr, "DRIVE"},
    {nullptr, "GLIDE"},
    {"FILTRO", "APERTURA"},
    {nullptr, "CHIUSURA"},
};
