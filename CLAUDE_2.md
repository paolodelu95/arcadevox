# SprigSynth — Brief di Progetto per Claude Code

## Contesto
Sto costruendo un sintetizzatore hardware DIY basato su ESP32-S3. Ho già progettato
l'architettura completa in una sessione precedente (pinout, firmware, logica delle
funzioni) e ora voglio che tu implementi il progetto da zero su questo repository,
seguendo esattamente le specifiche sotto. Non inventare funzioni diverse da quelle
elencate: l'obiettivo è ricreare fedelmente quanto già progettato.

## Hardware utilizzato
- **MCU**: ESP32-S3 N16R8 (16MB flash, 8MB PSRAM ottale)
- **Pannello di controllo fisico** (riciclato da un vecchio controller DIY per Euro Truck
  Simulator): 8 pulsanti arcade colorati (2 verdi, 2 gialli, 2 rossi, 2 blu), 1 joystick
  a 4 microswitch digitali (non analogico), 2 potenziometri lineari, 3 leve a scatto
  (pulsanti momentanei, non interruttori bistabili), 2 interruttori a bilanciere
  illuminati (anch'essi pulsanti momentanei)
- **Audio out**: modulo MAX98357 (DAC I2S + amplificatore integrato) verso altoparlante
  4-8Ω
- **Display**: GC9A01, TFT tondo 240x240, bus SPI, a colori
- **Nessuna matrice LED** (una MAX7219 era stata pianificata ma poi rimossa dal
  progetto — non implementarla)

## Pinout definitivo (ESP32-S3 N16R8)

| Funzione | GPIO | Tipo |
|---|---|---|
| Pot 1 (cutoff / attack in edit mode) | 4 | ADC1 |
| Pot 2 (volume / release in edit mode) | 5 | ADC1 |
| Nota DO | 6 | digitale, pull-up |
| Nota RE | 7 | digitale, pull-up |
| Nota MI | 8 | digitale, pull-up |
| Nota FA | 9 | digitale, pull-up |
| Nota SOL | 10 | digitale, pull-up |
| Nota LA | 11 | digitale, pull-up |
| Nota SI | 12 | digitale, pull-up |
| Nota DO' | 13 | digitale, pull-up |
| Joystick SU (ottava +1 / decay+ in edit mode) | 14 | digitale, pull-up |
| Joystick GIÙ (ottava -1 / decay- in edit mode) | 15 | digitale, pull-up |
| Joystick SINISTRA (onda precedente / sustain- in edit mode) | 16 | digitale, pull-up |
| Joystick DESTRA (onda successiva / sustain+ in edit mode) | 17 | digitale, pull-up |
| Scorri schermate display | 18 | digitale, pull-up |
| REC (sequencer) | 21 | digitale, pull-up |
| PLAY/STOP (sequencer) | 1 | digitale, pull-up |
| HOLD (press breve) / ADSR EDIT MODE (long-press >600ms) | 2 | digitale, pull-up |
| Leva Arpeggiator ON/OFF | 41 | digitale, pull-up |
| Leva BPM (ciclo preset) | 0 | digitale, pull-up (strapping pin) |
| I2S BCLK → MAX98357 | 38 | audio |
| I2S LRCLK/WS → MAX98357 | 39 | audio |
| I2S DIN → MAX98357 | 40 | audio |
| SPI SCLK → GC9A01 | 42 | SPI |
| SPI MOSI → GC9A01 | 47 | SPI |
| GC9A01 CS | 48 | SPI |
| GC9A01 DC | 45 | SPI (strapping pin) |
| GC9A01 RST | 46 | SPI (strapping pin) |
| Libero (riserva futura) | 3 | — |

Tutti i pin digitali (note, joystick, pulsanti, leve) usano `INPUT_PULLUP` interno via
software — nessuna resistenza esterna nel firmware, i contatti dei pulsanti sono a
2 terminali semplici verso GND.

## Funzioni da implementare

### 1. Motore audio (core 0, task FreeRTOS dedicato)
- Oscillatore monofonico con 4 forme d'onda: sine, square, saw, triangle
- Filtro passa-basso one-pole IIR, cutoff controllato da Pot 1 in modalità normale
  (mappatura esponenziale, ~80Hz–8000Hz)
- Inviluppo ADSR reale (non on/off istantaneo), con state machine
  IDLE → ATTACK → DECAY → SUSTAIN → RELEASE
- Uscita I2S a 44100Hz, 16-bit, verso MAX98357
- Volume generale su Pot 2 in modalità normale

### 2. Lettura input (core 1, loop principale)
- Scansione debounced di tutti i pulsanti (soglia debounce ~15ms)
- Note a 8 tasti con **last-note-priority**: se più tasti sono premuti insieme, suona
  l'ultimo premuto; mantenere anche un "press order" per servire l'arpeggiator
- Joystick a 4 direzioni, letto come 4 pulsanti digitali separati (non analogico)

### 3. Ottava e forma d'onda (joystick, modalità normale)
- Su/Giù: cambia ottava, range -2..+2, moltiplica la frequenza per 2^ottava
- Sinistra/Destra: cicla tra le 4 forme d'onda, rispettivamente indietro/avanti

### 4. HOLD e ADSR Edit Mode (pulsante HOLD, GPIO 2)
- Pressione breve (<600ms): toggle sustain/hold (la nota tenuta anche a tasti
  rilasciati)
- Pressione lunga (>600ms): entra/esce da "ADSR EDIT MODE". In questa modalità:
  - Pot 1 controlla Attack (mappatura esponenziale, ~2ms–500ms)
  - Pot 2 controlla Release (mappatura esponenziale, ~10ms–2000ms)
  - Joystick Su/Giù regola Decay in step di 10ms (range 5ms–1000ms)
  - Joystick Sinistra/Destra regola Sustain level in step del 5% (range 0–100%)
  - Cutoff e volume restano "congelati" all'ultimo valore mentre sei in questa
    modalità, per non far saltare il suono quando i pot cambiano funzione

### 5. Arpeggiator (leva GPIO 41)
- Toggle on/off
- Se attivo e più note sono tenute premute insieme, cicla automaticamente tra tutte
  le note tenute, in ordine di pressione, con step fisso ~150ms

### 6. Step-sequencer quantizzato (REC GPIO 21, PLAY/STOP GPIO 1)
- Griglia fissa a 16 step (stile drum machine, real-time step record)
- REC: ad ogni tick della griglia, registra quale nota è tenuta premuta in quel
  preciso istante (o silenzio se nessuna)
- Dopo 16 step la registrazione si ferma automaticamente e parte subito PLAY in loop
- REC può anche essere fermata manualmente prima dei 16 step (gli step rimanenti
  restano silenzio); in quel caso passa comunque a PLAY
- Durante PLAY, se l'utente suona dal vivo, la nota live ha sempre priorità sulla
  sequenza
- BPM regolabile su 7 preset: 60, 80, 100, 120, 140, 160, 180 — la leva su GPIO 0
  cicla tra questi ad ogni pressione; la durata dello step si ricalcola subito
  (sedicesimi di nota: `60000 / bpm / 4` ms)

### 7. Display GC9A01 (TFT tondo SPI, libreria "GFX Library for Arduino" di
   moononournation)
- 4 schermate cicliche tramite il pulsante "scorri display" (GPIO 18):
  1. Forma d'onda corrente (nome + icona disegnata a mano, non bitmap)
  2. Ottava corrente (numero grande, es. "+1", "-2")
  3. Barre orizzontali cutoff/volume in tempo reale
  4. Stato sequencer (REC/PLAY + step corrente/16) + stato HOLD + stato ARP + BPM
     corrente
- Quando ADSR EDIT MODE è attivo, il display mostra automaticamente una quinta
  schermata dedicata con i 4 valori A/D/S/R in tempo reale, bypassando il ciclo
  normale, e torna al ciclo normale quando si esce dalla modalità

## Note architetturali importanti
- Il synth è rigorosamente **monofonico**, una sola nota alla volta
- L'audio deve girare su un core FreeRTOS dedicato (core 0) per evitare glitch,
  mentre input/display/logica girano sul core 1
- Il bus SPI (SCLK GPIO 42, MOSI GPIO 47) è condiviso solo col display GC9A01 in
  questa versione (la matrice MAX7219 che ne condivideva l'uso è stata rimossa)
- Framework: Arduino su ESP32 (non ESP-IDF puro), compatibile con PlatformIO o
  Arduino IDE — se usi PlatformIO, configuralo per la board `esp32-s3-devkitc-1`
  con `board_build.psram = opi` per la PSRAM ottale della N16R8

## Librerie necessarie
- `driver/i2s.h` (nativa ESP32 Arduino core) per l'uscita audio
- `Arduino_GFX_Library` (moononournation) per il display GC9A01 via SPI

## Cosa NON includere
- Nessuna matrice LED MAX7219
- Nessuna polifonia
- Nessun controllo joystick analogico (il joystick fisico è a 4 microswitch digitali)
- Nessuna resistenza esterna di pull-up sui pulsanti (pull-up interno via software)

## Struttura file suggerita
```
/src
  main.cpp              // setup(), loop(), task audio
  audio_engine.h/.cpp    // oscillatore, filtro, ADSR
  input_handler.h/.cpp    // debounce, note, joystick, pulsanti funzione
  sequencer.h/.cpp        // step-sequencer quantizzato, BPM
  display.h/.cpp          // GC9A01, 4+1 schermate
  pinout.h                // tutte le #define dei GPIO, centralizzate
/platformio.ini (se usi PlatformIO)
```

## Primo step
Prima di scrivere codice, crea un `PROGRESS.md` con i milestone in ordine
(pinout/setup base → lettura input → motore audio → ADSR/edit mode → sequencer →
display), così possiamo tracciare l'avanzamento come già facciamo per Sprigling.
