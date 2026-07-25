# SprigSynth — PROGRESS

Sintetizzatore monofonico ESP32-S3 N16R8 · pannello arcade riciclato · MAX98357 (I2S) · GC9A01 (SPI)

Legenda: `[ ]` da fare · `[~]` in corso · `[x]` fatto (codice scritto) · `[T]` testato su hardware

---

## M0 — Setup progetto e pinout
- [x] `platformio.ini` per `esp32-s3-devkitc-1`, PSRAM ottale (`board_build.psram = opi`)
- [x] `lib_deps`: `GFX Library for Arduino` (moononournation)
- [x] `src/pinout.h` — tutte le `#define` dei GPIO centralizzate
- [x] Scheletro file: `audio_engine`, `input_handler`, `sequencer`, `display`, `main`
- [ ] **[T]** Verifica boot + seriale + nessun conflitto sui pin strapping (0, 45, 46)

## M1 — Lettura input
- [x] Debounce generico (~15 ms) su tutti i pulsanti digitali, `INPUT_PULLUP` interno
- [x] 8 tasti nota con **last-note-priority** + press-order (per l'arpeggiator)
- [x] Joystick 4 microswitch letti come 4 pulsanti separati (+ auto-repeat per l'edit mode)
- [x] Pulsanti funzione: scorri display, REC, PLAY/STOP, HOLD, leva ARP, leva BPM
- [x] HOLD: distinzione press breve (<600 ms) / long-press (>600 ms)
- [x] 2 encoder rotativi in quadratura, decoder full-step a interrupt (Enc1 4/5, Enc2 43/44)
- [ ] **[T]** Verifica su hardware di ogni contatto e del senso di rotazione degli encoder

## M2 — Motore audio (core 0)
- [x] Task FreeRTOS dedicato pinnato su core 0, priorità alta
- [x] Uscita I2S 44100 Hz / 16 bit verso MAX98357 (`driver/i2s.h`)
- [x] Oscillatore monofonico a phase-accumulator: sine, square, saw, triangle
- [x] Filtro passa-basso one-pole IIR, cutoff esponenziale ~80 Hz–8 kHz (Pot 1)
- [x] Volume generale (Pot 2), con smoothing anti-zipper
- [x] Mappatura note DO..DO' + ottava `-2..+2` (`freq * 2^ottava`)
- [ ] **[T]** Verifica assenza di glitch/underrun con display e sequencer attivi

## M3 — ADSR e Edit Mode
- [x] Inviluppo ADSR reale, state machine IDLE → ATTACK → DECAY → SUSTAIN → RELEASE
- [x] Long-press HOLD (>600 ms) entra/esce da ADSR EDIT MODE
- [x] Edit mode: Enc 1 → Attack (2–500 ms), Enc 2 → Release (10–2000 ms)
- [x] Edit mode: joystick Su/Giù → Decay ±10 ms (5–1000 ms)
- [x] Edit mode: joystick Sx/Dx → Sustain ±5% (0–100%)
- [x] Cutoff e volume congelati per tutta la durata dell'edit mode (automatico con
      gli encoder: essendo incrementali, nessun salto nemmeno all'uscita)
- [x] Press breve HOLD: toggle sustain/hold della nota
- [ ] **[T]** Verifica a orecchio delle curve e dei range

## M4 — Arpeggiator
- [x] Leva GPIO 41: toggle on/off
- [x] Ciclo automatico sulle note tenute, in ordine di pressione, step fisso ~150 ms
- [ ] **[T]** Verifica con 2/3/4 note tenute insieme

## M5 — Step-sequencer quantizzato
- [x] Griglia fissa a 16 step, real-time step record
- [x] REC: registra ad ogni tick la nota tenuta in quell'istante (o silenzio)
- [x] Auto-stop dopo 16 step → PLAY in loop immediato
- [x] Stop manuale anticipato → step restanti a silenzio → PLAY
- [x] Priorità alla nota live durante il PLAY
- [x] 7 preset BPM (60/80/100/120/140/160/180) ciclati dalla leva GPIO 0
- [x] Durata step ricalcolata subito: `60000 / bpm / 4` ms (sedicesimi)
- [ ] **[T]** Verifica tenuta del tempo e del loop

## M6 — Display GC9A01
- [x] Init bus SPI + `Arduino_GC9A01` 240x240 IPS
- [x] Schermata 1: forma d'onda (nome + icona disegnata con primitive, non bitmap)
- [x] Schermata 2: ottava (numero grande con segno)
- [x] Schermata 3: barre orizzontali cutoff / volume in tempo reale
- [x] Schermata 4: stato sequencer (REC/PLAY + step/16), HOLD, ARP, BPM
- [x] Schermata ADSR dedicata, attivata automaticamente in edit mode (bypassa il ciclo)
- [x] Redraw parziale (solo i valori cambiati) per evitare flicker
- [ ] **[T]** Verifica leggibilità dentro l'area circolare e frame rate

## M6b — LED RGB di bordo
- [x] CS del display spostato da GPIO 48 a GPIO 3: il 48 è il WS2812 saldato sulla
      DevKitC-1 e il chip select lo teneva acceso bianco fisso
- [x] `status_led.cpp`: tre giochi di luce in loop (arcobaleno / respiro / battito),
      7 s ciascuno, luminosità 20% con curva quadratica per fade morbidi
- [ ] **[T]** Verifica del colore e della luminosità sulla scheda reale

## M7 — Rifinitura
- [ ] Taratura fine dei range di cutoff/ADSR a orecchio
- [ ] Eventuale glide/portamento (fuori specifica, solo se desiderato)
- [ ] Taratura del passo per scatto degli encoder (quanti giri per l'intera corsa)

---

## Variazioni rispetto a CLAUDE_2.md
Il brief parla di **2 potenziometri lineari su GPIO 4/5 (ADC1)**, ma l'hardware reale
monta **2 encoder rotativi incrementali**. Conseguenze:

- **Pinout**: Enc1 = GPIO 4 (A) + 5 (B), Enc2 = GPIO 43 (A) + 44 (B). GPIO 43/44 erano
  la UART0, quindi la seriale passa dall'USB nativo (flash e monitor dalla porta **USB**,
  non dalla porta **UART** della DevKitC-1). GPIO 3 è poi finito al CS del display
  (vedi M6b), quindi restano liberi solo 19 e 20.
- **Click degli encoder**: non cablati (scelta di progetto). La funzione "passo fine /
  passo grosso" è già scritta e inerte: per attivarla basta assegnare un GPIO libero a
  `PIN_ENC1_SW` / `PIN_ENC2_SW` in `pinout.h`, senza toccare altro.
- **Freeze in ADSR edit mode**: non serve più alcuna logica di soft-pickup. Gli encoder
  sono relativi, quindi cutoff e volume sono congelati per costruzione durante l'edit
  mode e non fanno alcun salto quando si esce.
- **Corsa dei controlli**: i parametri esponenziali si muovono su una posizione interna
  0..1. Cutoff: 1/64 di corsa per scatto (~3 giri di manopola da 20 detent per l'intero
  range). Volume: 2% per scatto. Attack/Release: 1/48 di corsa per scatto.
- **Pull-up**: gli encoder usano i pull-up interni come tutto il resto del pannello. Se
  i cavi verso il pannello sono lunghi e la lettura risulta rumorosa, servono pull-up
  esterni da 10k su A e B (unico punto in cui il vincolo "nessuna resistenza esterna"
  potrebbe non reggere).

---

## Note operative
- GPIO 0 (leva BPM) è uno **strapping pin**: se la leva è premuta all'accensione la board
  entra in download mode. Non tenerla premuta al boot.
- GPIO 45 (DC) e GPIO 46 (RST) sono anch'essi strapping pin, ma essendo **uscite** verso il
  display non creano problemi al boot.
- GPIO 3 ora porta il CS del display (liberato dal 48, che è il LED RGB). Restano liberi
  solo GPIO 19 e 20, e solo rinunciando alla seriale USB CDC.
- Al boot la ROM stampa il suo log su GPIO 43 pilotandolo come uscita: se in quell'istante
  il contatto dell'encoder 2 è chiuso c'è un breve conflitto elettrico, innocuo ma
  evitabile spostando Enc2 su GPIO 19/20 e riportando la seriale su UART0.
- Nessuna matrice MAX7219, nessuna polifonia, nessun joystick analogico: fuori specifica.
