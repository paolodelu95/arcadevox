// pinout.h — mappatura definitiva della scheda ArcadeVox rev. 2026-08-17.
//
// Questa e' la scheda vera, non piu' il pannello riciclato: ESP32-S3-DevKitC-1
// **N8** (8 MB di flash, niente PSRAM) montato su un PCB con 20 tasti Cherry MX
// hot-swap illuminati, 4 encoder, joystick a 4 microswitch, display tondo e
// uscita audio I2S su connettore.
//
// Tutti i valori qui sotto sono ricavati dallo schematico e dalla netlist del
// progetto EasyEDA: dove lo schematico non dice abbastanza (l'ordine dei tre
// segnali audio) il commento lo segnala apertamente invece di far finta di
// niente.
#pragma once

// ============================================================================
// Tasti: matrice 4 colonne x 5 righe sull'espansore I2C
// ============================================================================
//
// I 20 tasti non sono cablati uno per GPIO: passano da un MCP23017 (U1) letto in
// I2C. Ogni tasto ha il suo diodo 1N4148, quindi il ghosting non esiste e si
// possono premere tutti i tasti che si vuole insieme.
//
// Verso dei diodi (dallo schematico): il catodo sta dal lato del tasto, l'anodo
// dal lato della riga. La corrente puo' quindi scorrere solo RIGA -> TASTO ->
// COLONNA, e la scansione e' obbligata:
//
//   colonne = uscite, una alla volta a livello basso;
//   righe   = ingressi con pull-up; tasto premuto = riga bassa.
//
// Invertire i ruoli non leggerebbe *niente*: i diodi bloccherebbero.
#define MCP_ADDR 0x20  // A0/A1/A2 a massa

// Porta B: 4 colonne in uscita + i 4 pulsanti degli encoder in ingresso.
#define MCP_COL0_BIT 0  // GPB0
#define MCP_COL1_BIT 1  // GPB1
#define MCP_COL2_BIT 2  // GPB2
#define MCP_COL3_BIT 3  // GPB3
#define MCP_ENC1_SW_BIT 4  // GPB4
#define MCP_ENC2_SW_BIT 5  // GPB5
#define MCP_ENC3_SW_BIT 6  // GPB6
#define MCP_ENC4_SW_BIT 7  // GPB7

// Porta A: 5 righe in ingresso (GPA0..GPA4). GPA5..GPA7 non sono cablate.
#define MATRIX_COLS 4
#define MATRIX_ROWS 5

// ------------------------------------------------------------------ posizioni
// Indice di un tasto nella matrice = riga * MATRIX_COLS + colonna.
// Corrispondenza presa dallo schematico (riga per riga, da sinistra a destra):
//
//   ROW0 : DO    DO#   RE    RE#
//   ROW1 : MI    FA    FA#   SOL
//   ROW2 : SOL#  LA    LA#   SI
//   ROW3 : DO'   FN1   FN2   FN3
//   ROW4 : FN4   FN5   FN6   FN7
//
// Sul pannello i tasti sono disposti come un pezzo di pianoforte: i tasti neri
// (le alterazioni) stanno nella fila di mezzo, i bianchi in basso, le sette
// funzioni in alto.
#define KEY_AT(row, col) ((row) * MATRIX_COLS + (col))

// Le 13 note vanno da DO a DO' incluso: un'ottava cromatica completa.
#define NOTE_COUNT 13
// Le 7 funzioni.
#define FN_COUNT 7

// Indice di matrice di ogni nota, in ordine di scala cromatica.
#define MATRIX_NOTE_SLOTS \
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }
// Indice di matrice di FN1..FN7.
//
// NOTA DI COLLAUDO, scheda 2026-08 — FN6 (slot 18, riga 4 colonna 2) e' guasto
// sull'esemplare montato a mano: non arriva al firmware, con nessuna funzione
// sopra. Il guasto e' su quel solo incrocio, e le due linee che ci passano sono
// state assolte una per una:
//
//   riga 4    sana  -> FN7 e' riga 4 colonna 3 e risponde
//   colonna 2 sana  -> FN2 e' riga 3 colonna 2 e risponde
//
// Resta quindi il pulsante, il suo diodo o una delle sue saldature. La prova che
// lo ha dimostrato e' stata scambiare qui le ultime due voci — POLI su slot 19,
// SILENZIO su slot 18 — e vedere POLI rispondere puntualmente su FN7 mentre FN6
// restava muto anche col panico sopra. Il firmware non c'entrava.
//
// Se serve rifarla, o tenere POLI raggiungibile mentre la scheda aspetta il
// saldatore, e' una riga sola:
//
//     { 13, 14, 15, 16, 17, 19, 18 }
//
// Qui sotto resta la tabella vera, quella che descrive il pannello com'e'
// disegnato: una scheda sana deve trovare il progetto giusto, non la
// medicazione di un esemplare.
#define MATRIX_FN_SLOTS \
    { 13, 14, 15, 16, 17, 18, 19 }

// ============================================================================
// Encoder rotativi (4) — direttamente sui GPIO
// ============================================================================
//
// Contatti A/B verso il comune C a massa. ENC1..ENC3 hanno il pull-up esterno da
// 10k (R2..R7); ENC4 no: sullo schematico R8/R9 sono finite *in serie* sulle sue
// linee invece che verso i 3,3 V. Funziona lo stesso — il pull-up interno da
// ~45k vince sui 10k in serie e il livello basso resta valido — ma ha meno
// margine di rumore degli altri tre. Se un giorno ENC4 dovesse fare scatti
// fantasma, e' li' che si guarda, non nel firmware.
//
// Il pull-up interno resta acceso su tutti e quattro: dove c'e' gia' quello
// esterno non fa danno, dove manca fa il lavoro.
#define PIN_ENC1_A 6
#define PIN_ENC1_B 7
#define PIN_ENC2_A 8
#define PIN_ENC2_B 9
#define PIN_ENC3_A 10
#define PIN_ENC3_B 11
#define PIN_ENC4_A 40
#define PIN_ENC4_B 39

// I pulsanti degli alberi non sono sui GPIO: passano dall'espansore, vedi
// MCP_ENC*_SW_BIT qui sopra. Finalmente ci sono davvero, e il "passo fine" non
// e' piu' una funzione scritta e mai raggiungibile.

// ============================================================================
// Joystick a 4 microswitch
// ============================================================================
// Connettore J_JOY: 4 direzioni verso GND, pull-up interno. Premuto = LOW.
#define PIN_JOY_UP 2
#define PIN_JOY_DOWN 41
#define PIN_JOY_LEFT 42
#define PIN_JOY_RIGHT 47

// ============================================================================
// I2C verso l'espansore MCP23017
// ============================================================================
#define PIN_I2C_SDA 4
#define PIN_I2C_SCL 5
// 400 kHz: una scansione completa della matrice sono 5 righe x 2 transazioni,
// circa 300 us. A 100 kHz sarebbe piu' di un millisecondo per giro di loop.
#define I2C_FREQ_HZ 400000

// ============================================================================
// LED RGB sotto i tasti (SK6812 dentro ogni Cherry MX hot-swap)
// ============================================================================
// Catena unica di 20 LED alimentata dai 5 V del connettore J_PWR; il dato passa
// da GPIO12 attraverso R1 (330 ohm).
#define PIN_KEYLED_DATA 12
#define KEYLED_COUNT 20

// ============================================================================
// Display GC9A01 (SPI) — connettore J_DISPLAY a 7 poli
// ============================================================================
// Ordine dei poli sul connettore: 1=3V3  2=GND  3..7 = i cinque segnali, nello
// stesso ordine in cui stanno stampati sui moduli GC9A01 in commercio
// (SCL, SDA, RES, DC, CS). Il BLK del modulo va ai 3,3 V: la retroilluminazione
// e' sempre accesa e non occupa un filo.
#define PIN_TFT_SCLK 13  // J_DISPLAY.3
#define PIN_TFT_MOSI 14  // J_DISPLAY.4
#define PIN_TFT_RST 15   // J_DISPLAY.5
#define PIN_TFT_DC 16    // J_DISPLAY.6
#define PIN_TFT_CS 17    // J_DISPLAY.7

// ============================================================================
// Audio I2S (MAX98357) — connettore J_AUDIO a 5 poli
// ============================================================================
// 1=3V3  2=GND  3=GPIO18  4=GPIO21  5=GPIO1.
//
// ATTENZIONE — lo schematico numera i tre segnali e basta: quale sia il BCLK,
// quale il LRCLK e quale il DIN lo decide il filo che si infila nel connettore,
// e li' nessun disegno puo' aiutare. Qui sotto c'e' l'ordine piu' probabile
// (quello serigrafato sui moduli MAX98357: LRC, BCLK, DIN); se il synth dovesse
// restare muto o suonare a una velocita' sbagliata non serve ricompilare: la
// voce "USCITA AUDIO" nel menu impostazioni prova le altre combinazioni a caldo
// e salva quella giusta.
#define PIN_AUDIO_A 18  // J_AUDIO.3
#define PIN_AUDIO_B 21  // J_AUDIO.4
#define PIN_AUDIO_C 1   // J_AUDIO.5

// ============================================================================
// LED RGB di bordo della DevKitC-1
// ============================================================================
// Sulla ESP32-S3-DevKitC-1 il WS2812 di bordo sta sul GPIO 48, qui libero.
#define PIN_RGB_LED 48

// ============================================================================
// GPIO ancora liberi
// ============================================================================
// 3, 46 (non cablati sul PCB), 19/20 (USB nativo: si liberano solo rinunciando
// alla seriale via USB CDC), 35/36/37/38 (usati dalla flash/PSRAM su alcune
// varianti del modulo: non toccarli), 0/45 (strapping).
