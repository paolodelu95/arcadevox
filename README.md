# ArcadeVox — SprigSynth

Sintetizzatore monofonico DIY su **ESP32-S3**, costruito attorno a un pannello di comandi
arcade riciclato da un vecchio controller per Euro Truck Simulator.

![Layout del pannello](docs/pannello.svg)

## Cos'è

Un synth hardware completo che gira su due core: il motore audio ha il core 0 tutto per sé,
mentre input, sequencer e display stanno sul core 1. Niente polifonia, niente fronzoli —
una nota alla volta, ma con un inviluppo vero e un filtro che si apre.

- **Oscillatore** a phase-accumulator, 4 forme d'onda: sine, square, saw, triangle
- **Filtro** passa-basso one-pole IIR, cutoff 80 Hz – 8 kHz con mappatura esponenziale
- **Inviluppo ADSR** reale, con state machine, tutto regolabile dal pannello
- **Uscita I2S** 44.1 kHz / 16 bit verso un MAX98357
- **8 tasti nota** con last-note-priority e memoria dell'ordine di pressione
- **Arpeggiator** sulle note tenute, in ordine di pressione, passo 150 ms
- **Step-sequencer** quantizzato a 16 step con registrazione in tempo reale e 7 preset BPM
- **Display GC9A01** tondo con 4 schermate cicliche più una dedicata all'edit dell'ADSR
- **LED RGB** di bordo con tre giochi di luce in loop

## Hardware

| Parte | Modello |
|---|---|
| MCU | ESP32-S3-WROOM-1 **N16R8** (16 MB flash, 8 MB PSRAM ottale) |
| Audio | MAX98357 (DAC I2S + amplificatore) su altoparlante 4–8 Ω |
| Display | GC9A01, TFT tondo 240x240, SPI |
| Pannello | 8 pulsanti arcade Ø22, joystick a 4 microswitch, 2 encoder, 3 bilancieri, 3 leve |

Tutti i contatti sono a 2 terminali verso GND e usano i pull-up interni: nessuna resistenza
esterna. I comandi sono tutti momentanei — ogni stato ON/OFF vive nel firmware.

## Pinout

| GPIO | Funzione | GPIO | Funzione |
|---|---|---|---|
| 6–13 | Note DO … DO' | 38 / 39 / 40 | I2S BCLK / LRCLK / DIN |
| 14–17 | Joystick su / giù / sx / dx | 42 / 47 | SPI SCLK / MOSI |
| 18 | Scorri schermate display | 3 / 45 / 46 | Display CS / DC / RST |
| 21 / 1 | REC / PLAY-STOP | 48 | LED RGB di bordo |
| 2 | HOLD (breve) · ADSR edit (>600 ms) | 4 / 5 | Encoder 1 (A/B) |
| 41 / 0 | Leva arpeggiator / preset BPM | 43 / 44 | Encoder 2 (A/B) |

Due trappole di questa scheda, entrambe già risolte nel codice:

- **GPIO 48 è il LED RGB saldato sulla DevKitC-1.** Usarlo per altro lo lascia acceso bianco
  fisso: per questo il CS del display sta sul GPIO 3.
- **GPIO 43/44 sono la UART0.** Occupandoli con l'encoder 2, flash e monitor vanno fatti
  dalla porta **USB** della scheda, non dalla porta **UART**.
- **GPIO 35/36/37 sono esposti sul connettore ma inutilizzabili**: sono le linee della PSRAM
  ottale interna al modulo.

## Compilare e caricare

```bash
pio run              # compila
pio run -t upload    # carica (dalla porta USB nativa)
pio device monitor   # seriale a 115200
```

Il `platformio.ini` è pinnato al core Arduino 2.0.x, perché il motore audio usa il driver
legacy `driver/i2s.h`; di conseguenza anche Arduino_GFX è fermo alla 1.4.9, l'ultima
compatibile con quel core.

## Documentazione

| File | Cosa contiene |
|---|---|
| [`docs/SprigSynth_Manuale.pdf`](docs/SprigSynth_Manuale.pdf) | Manuale completo A4: blueprint, funzioni, collegamenti, guida all'uso |
| [`docs/SprigSynth_Libretto_A5_stampa.pdf`](docs/SprigSynth_Libretto_A5_stampa.pdf) | Lo stesso manuale imposto per stampare un libretto A5 |
| [`docs/SprigSynth_Libretto_A5_lettura.pdf`](docs/SprigSynth_Libretto_A5_lettura.pdf) | Versione A5 in ordine sequenziale, per lo schermo |
| [`PROGRESS.md`](PROGRESS.md) | Stato dei milestone e differenze rispetto al brief iniziale |
| [`CLAUDE_2.md`](CLAUDE_2.md) | Il brief di progetto originale |

I PDF sono generati da `docs/*.html`: si rigenerano stampandoli da browser in PDF.

## Struttura

```
src/
  main.cpp            setup(), loop(), logica di priorità delle note
  pinout.h            tutti i GPIO, centralizzati
  audio_engine.*      oscillatore, filtro, ADSR, task I2S sul core 0
  input_handler.*     debounce, last-note-priority, encoder in quadratura
  sequencer.*         16 step quantizzati, preset BPM
  display.*           GC9A01, 4+1 schermate
  status_led.*        animazioni sul WS2812 di bordo
```
