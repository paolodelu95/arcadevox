// pinout.h — mappatura GPIO definitiva di ArcadeVox (ESP32-S3 N16R8)
//
// Tutti gli ingressi digitali (note, joystick, pulsanti, leve) usano il pull-up
// interno: i contatti sono a 2 terminali verso GND, nessuna resistenza esterna.
// Premuto = LOW.
#pragma once

// ------------------------------------------------------------ encoder rotativi
// Due encoder incrementali con detent (tipo EC11), letti in quadratura A/B con
// interrupt. Contatti verso GND (comune a massa), pull-up interno.
// Se un encoder risulta invertito, basta scambiare A e B qui sotto.
#define PIN_ENC1_A 4   // cutoff (normale) / attack (ADSR edit mode)
#define PIN_ENC1_B 5
#define PIN_ENC2_A 43  // volume (normale) / release (ADSR edit mode)
#define PIN_ENC2_B 44

// Click (pulsante dell'albero): NON collegati su questo pannello.
// La funzione "passo fine / passo grosso" e' gia' implementata: per abilitarla
// basta assegnare qui un GPIO libero (19 o 20) e cablare il pulsante a GND.
#define PIN_ENC1_SW -1
#define PIN_ENC2_SW -1

// ATTENZIONE — GPIO 43/44 sono i pin di UART0:
//  - la seriale di debug passa dall'USB nativo (USB CDC), non dalla UART;
//  - flash e monitor vanno fatti dalla porta "USB" della DevKitC-1, non da "UART";
//  - al boot la ROM stampa il suo log su GPIO 43 pilotandolo come uscita: se il
//    contatto dell'encoder e' chiuso proprio in quell'istante c'e' un breve
//    conflitto (innocuo, ma se preferisci evitarlo sposta ENC2 su 19/20 e riporta
//    la seriale su UART0 mettendo ARDUINO_USB_CDC_ON_BOOT=0).

// ------------------------------------------------------------------ note DO..SI
// Ordine fisico sul pannello tenuto in orizzontale (joystick in basso al centro),
// letto da sinistra a destra: blu, giallo, verde+rosso impilati, verde+rosso
// impilati, giallo, blu. Nelle coppie impilate il verde sta sopra.
// La scala sale quindi da sinistra verso destra, come su una tastiera.
//
// L'ottavo pulsante (blu di destra, GPIO 13) non e' piu' una nota: e' passato
// al selettore MONO/POLI, vedi sotto. La scala arriva quindi al SI, e il DO
// superiore si raggiunge con il joystick dell'ottava.
#define PIN_NOTE_DO   6
#define PIN_NOTE_RE   7
#define PIN_NOTE_MI   8
#define PIN_NOTE_FA   9
#define PIN_NOTE_SOL  10
#define PIN_NOTE_LA   11
#define PIN_NOTE_SI   12

#define NOTE_COUNT 7

// -------------------------------------------------------------------- joystick
// 4 microswitch digitali indipendenti (nessuna lettura analogica).
#define PIN_JOY_UP    14  // ottava +1   / decay +    (edit mode)
#define PIN_JOY_DOWN  15  // ottava -1   / decay -    (edit mode)
#define PIN_JOY_LEFT  16  // onda prec.  / sustain -  (edit mode)
#define PIN_JOY_RIGHT 17  // onda succ.  / sustain +  (edit mode)

// ------------------------------------------------------------ pulsanti funzione
#define PIN_BTN_DISPLAY 18  // scorre le schermate del display
#define PIN_BTN_POLY    13  // ex nota DO': commuta MONO / POLIFONICO
#define PIN_BTN_REC     21  // sequencer: REC
#define PIN_BTN_PLAY    1   // sequencer: PLAY/STOP
#define PIN_BTN_HOLD    2   // press breve = HOLD, long-press >600ms = ADSR EDIT MODE
#define PIN_LEVER_ARP   41  // leva: arpeggiator ON/OFF
#define PIN_LEVER_BPM   0   // leva: ciclo preset BPM  (STRAPPING PIN: non tenere
                            // premuto all'accensione o la board entra in download mode)

// ------------------------------------------------------------- audio I2S (MAX98357)
#define PIN_I2S_BCLK  38
#define PIN_I2S_LRCLK 39
#define PIN_I2S_DOUT  40

// --------------------------------------------------------------- display GC9A01 (SPI)
#define PIN_TFT_SCLK 42
#define PIN_TFT_MOSI 47
#define PIN_TFT_CS   3   // NON usare il 48: e' il LED RGB di bordo (vedi sotto)
#define PIN_TFT_DC   45  // strapping pin, ma qui e' un'uscita: nessun problema al boot
#define PIN_TFT_RST  46  // idem

// ------------------------------------------------------------ LED RGB di bordo
// Sulla ESP32-S3-DevKitC-1 il GPIO 48 e' cablato al WS2812 saldato sulla scheda
// (PIN_NEOPIXEL nel variant Arduino). Usandolo per altro, il LED riceve dati
// casuali e resta acceso fisso: per questo il CS del display sta sul GPIO 3.
#define PIN_RGB_LED 48

// GPIO 19, 20: unici pin ancora liberi, ma sono le linee dell'USB nativo usate
// per la seriale di debug: si liberano solo rinunciando all'USB CDC.
