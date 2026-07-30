# ArcadeVox

Sintetizzatore DIY su **ESP32-S3**, mono o polifonico, costruito attorno a un pannello di comandi
arcade riciclato da un vecchio controller per Euro Truck Simulator.

<img src="docs/arcadevox_boot.gif" width="240" align="right" alt="Schermata di avvio">

![Layout del pannello](docs/pannello.svg)

## Cos'è

Un synth hardware completo che gira su due core: il motore audio ha il core 0 tutto per sé,
mentre input, sequencer e display stanno sul core 1. Monofonico o polifonico a scelta, con
un inviluppo vero e un filtro che si apre.

- **Oscillatore** a phase-accumulator, 4 forme d'onda: sine, square, saw, triangle
- **Filtro** passa-basso one-pole IIR, cutoff 80 Hz – 8 kHz con mappatura esponenziale
- **Inviluppo ADSR** reale, con state machine, tutto regolabile dal pannello
- **Uscita I2S** 44.1 kHz / 16 bit verso un MAX98357
- **MONO / POLIFONICO** commutabile dal pannello: 8 voci, ognuna con fase, inviluppo e
  filtro suoi
- **7 tasti nota** (DO–SI) con last-note-priority e memoria dell'ordine di pressione
- **Arpeggiator** sulle note tenute, in ordine di pressione, passo 150 ms
- **Step-sequencer** a 16 step con scrittura passo-passo, record quantizzato in overdub,
  preconteggio e metronomo
- **Display GC9A01** tondo con 7 schermate cicliche — fra cui VU meter ad ago,
  oscilloscopio dell'uscita e un menu impostazioni per categorie — più quelle di
  edit ADSR e preconteggio
- **Sensibilità degli encoder regolabile** dal pannello, in giri di manopola
- **Memoria**: pattern e parametri sopravvivono allo spegnimento
- **Aggiornamento via WiFi** con QR da inquadrare col telefono
- **LED RGB** di bordo con tre giochi di luce in loop

## Hardware

| Parte | Modello |
|---|---|
| MCU | ESP32-S3-WROOM-1 **N16R8** (16 MB flash, 8 MB PSRAM ottale) |
| Audio | MAX98357 (DAC I2S + amplificatore) su altoparlante 4–8 Ω |
| Display | GC9A01, TFT tondo 240x240, SPI |
| Pannello | 8 pulsanti arcade Ø22 (7 note + selettore MONO/POLI), joystick a 4 microswitch, 2 encoder, 3 bilancieri, 3 leve |

Tutti i contatti sono a 2 terminali verso GND e usano i pull-up interni: nessuna resistenza
esterna. I comandi sono tutti momentanei — ogni stato ON/OFF vive nel firmware.

## Pinout

| GPIO | Funzione | GPIO | Funzione |
|---|---|---|---|
| 6–12 | Note DO … SI | 38 / 39 / 40 | I2S BCLK / LRCLK / DIN |
| 13 | Selettore MONO / POLIFONICO | 42 / 47 | SPI SCLK / MOSI |
| 14–17 | Joystick su / giù / sx / dx | 3 / 45 / 46 | Display CS / DC / RST |
| 18 | Scorri schermate · NETWORK (>1 s) | 48 | LED RGB di bordo |
| 21 / 1 | REC · STEP EDIT (>600 ms) / PLAY-STOP · svuota pattern (>800 ms) | | |
| 2 | HOLD (breve) · ADSR edit (>600 ms) | 4 / 5 | Encoder 1 (A/B) |
| 41 / 0 | Leva arpeggiator / preset BPM | 43 / 44 | Encoder 2 (A/B) |

Due trappole di questa scheda, entrambe già risolte nel codice:

- **GPIO 48 è il LED RGB saldato sulla DevKitC-1.** Usarlo per altro lo lascia acceso bianco
  fisso: per questo il CS del display sta sul GPIO 3.
- **GPIO 43/44 sono la UART0.** Occupandoli con l'encoder 2, flash e monitor vanno fatti
  dalla porta **USB** della scheda, non dalla porta **UART**.
- **GPIO 35/36/37 sono esposti sul connettore ma inutilizzabili**: sono le linee della PSRAM
  ottale interna al modulo.

## Mono e polifonico

L'ultimo pulsante blu di destra (GPIO 13), che prima suonava il DO acuto, **commuta fra
monofonico e polifonico**. La scala sulla tastiera arriva quindi al SI: il DO superiore si
raggiunge con il joystick dell'ottava.

Il motore ha **8 voci**, ognuna con fase, inviluppo e filtro propri — una nota nuova non
eredita lo stato di quella precedente. Gli identificativi sono esattamente quanti servono
(7 tasti + 1 per il sequencer), quindi ogni voce è dedicata: niente allocazione dinamica,
niente *voice stealing*, comportamento sempre prevedibile.

| | MONO | POLIFONICO |
|---|---|---|
| tasti | una nota alla volta, last-note-priority | tutti i tasti suonano insieme |
| sequenza | le dita hanno la **precedenza**, la sequenza tace | la sequenza suona **sotto** le dita: ci suoni sopra |
| HOLD | tiene l'ultima nota | tiene l'**accordo**, e ogni tasto premuto dopo ci si aggiunge |
| arpeggiator | invariato | invariato: resta monofonico, è il suo senso |

L'ampiezza è compensata sull'**energia** delle voci e non sul loro numero, così una nota in
coda di rilascio non abbassa quelle che stanno ancora suonando. La modalità viene salvata e
si ritrova all'accensione successiva.

## Usare il sequencer

Sedici step, un sedicesimo ciascuno. Due modi di riempirli, che convivono: si può editare
anche mentre il loop gira.

### STEP EDIT — scrivere con calma

Tieni premuto **REC** per mezzo secondo. Compare un cursore bianco sulla griglia: da lì in
poi non c'è nessuna fretta, il tempo non scorre.

| Comando | Cosa fa |
|---|---|
| tasto nota | scrive la nota sotto il cursore, te la fa sentire, **avanza da solo** |
| joystick ↑ ↓ | ottava dello step che stai per scrivere |
| joystick ← → | sposta il cursore |
| encoder 1 | scorre il cursore velocemente |
| encoder 2 | BPM continuo, 40–240 |
| HOLD (breve) | svuota lo step e avanza |
| leva ARP | scrive un **legato**: la nota precedente prosegue senza ripartire |
| PLAY (breve) | avvia o ferma il loop — puoi continuare a scrivere mentre suona |
| PLAY (lungo) | **svuota tutti i 16 step**: non si torna indietro |
| REC (lungo) | esce |

Il tasto che scrive e fa avanzare il cursore è il *step input* di MPC e Roland MC: si digita
una melodia premendo un tasto dopo l'altro, come si scrive su una tastiera.

### REC — registrare suonando

**REC** breve. Parte una battuta di preconteggio col metronomo (durante la quale il pattern
che c'è già suona), poi il loop gira all'infinito e tutto quello che suoni ci finisce dentro.

- Le note si agganciano al sedicesimo **più vicino**: a 120 BPM hai ±62 ms di tolleranza.
- È un **overdub**: ogni passata aggiunge, niente viene cancellato. Non serve azzeccare tutto
  in una volta.
- Tieni premuto **HOLD** mentre gira per **svuotare** gli step che passano sotto la testina.
- **REC** di nuovo per uscire dalla registrazione lasciando il loop in play.
- Con l'arpeggiator acceso finiscono nel pattern i suoi passi, non i tasti che tieni.

La griglia mostra il contenuto: ogni cella porta l'iniziale della nota e un colore per
l'ottava, i legati una barretta. La cornice verde è la testina, quella bianca il cursore.

## ADSR edit

Long-press di **HOLD** (>600 ms). I quattro parametri hanno quattro comandi diretti, senza
modalità da ricordare:

| Parametro | Comando | Perché lì |
|---|---|---|
| **A**ttack | encoder 1 | mappatura esponenziale, vuole una corsa continua |
| **R**elease | encoder 2 | idem |
| **D**ecay | joystick ↑ ↓ | passo fisso da 10 ms, l'auto-repeat basta |
| **S**ustain | joystick ← → | passo fisso del 5% |

La legenda è scritta in fondo alla schermata: `A R = ENCODER   D S = JOY`.

## Sensibilità degli encoder

Gli encoder sono incrementali, e quanto muove uno scatto era cablato nel codice:
col volume a 1/50 di corsa per scatto servivano **due giri e mezzo** di manopola
per andare da zero a fondo scala. Ora si regola dalla schermata **SETTINGS**.

| Voce | Cosa muove | Default |
|---|---|---|
| VOLUME | encoder 2 in uso normale | 2.4 giri per l'intera corsa |
| CUTOFF | encoder 1 in uso normale | 3.2 giri da 80 Hz a 8 kHz |
| ADSR | attack e release in ADSR edit | 2.4 giri |
| PASSO FINE | divisore col click dell'encoder | 1/4 |

Per modificarle **tieni premuto il pulsante *scorri display*** dalla schermata SETTINGS:
si entra nel menu. Da dentro, una **pressione breve** dello stesso pulsante scende di una voce
— e sull'ultima, *modalità WiFi*, la esegue mostrando il QR — mentre una **lunga** esce. L'encoder 2 cambia il valore della voce scelta (l'encoder 1 la scorre, se
preferisci): lì i due encoder non fanno cutoff e volume, che restano fermi finché non esci. I valori sono scritti in giri di manopola perché è la grandezza
che senti sotto le dita, non frazioni di corsa.

I default riproducono il comportamento precedente: aggiornando non trovi le
manopole cambiate sotto le dita finché non decidi tu. Il *passo fine* resterà
inattivo finché i click degli encoder non saranno cablati (vedi `pinout.h`).

## Aggiornare il firmware via WiFi

Scorri fino a **SETTINGS**, tieni premuto il pulsante *scorri display* per entrare nel menu,
poi scendi con pressioni brevi fino a **MODALITÀ WIFI**: lì una pressione breve mostra il QR.
Il cursore non torna a capo apposta — si scende fino all'ultima voce e ci si ferma, così non
si accende la radio credendo di tornare in cima. Per risalire c'è l'encoder 1. Il synth **ammutolisce** — lo stack WiFi occupa lo stesso core del motore audio,
quindi la modalità è esclusiva — e sul display compare un QR.

1. Inquadra il QR con la fotocamera del telefono: è la rete stessa, ti ci agganci senza
   digitare niente.
2. Il portale si apre da solo (captive portal). Se non succede, il QR nel frattempo è
   diventato l'indirizzo da aprire: reinquadralo.
3. Da lì puoi **caricare un `firmware.bin`** preso dal telefono, oppure dare al synth le
   credenziali del WiFi di casa e fargli **cercare gli aggiornamenti da internet**.

Il browser chiede utente e password appena apri il portale: sono `arcade` e la password
scritta sul display, che è la stessa dell'access point. Le trovi entrambe accanto al QR.
Si esce con **PLAY**, che riavvia il synth.

Il nome della rete di casa **si sceglie da un elenco**: il synth scandaglia le reti attorno e
te le propone in una tendina, con la potenza del segnale. Il campo libero sotto serve solo
alle reti nascoste, che in nessuna scansione compaiono.

Le credenziali della rete di casa **restano nel synth**: alla successiva accensione della radio
si ricollega da solo, senza ridigitarle, e il display mostra l'indirizzo che ha preso. Dal
portale si vede quale rete è in memoria e la si può dimenticare.

Due cose da sapere:

- Il **primo** firmware che contiene questa funzione va caricato **via USB**. Da lì in poi
  gli aggiornamenti passano dall'OTA.
- L'aggiornamento scrive nella partizione applicativa **opposta**: il firmware precedente
  resta intatto e recuperabile via USB. Non c'è rollback automatico.

Per l'aggiornamento da internet serve un manifest JSON raggiungibile via HTTPS:

```json
{"version": "1.2.0", "url": "https://.../firmware.bin", "notes": "cosa cambia"}
```

L'indirizzo di default sta in [`src/version.h`](src/version.h) e si può cambiare dal portale.
Il certificato non viene verificato (`setInsecure()`): il synth non ha un orologio affidabile
né un bundle di CA da tenere aggiornato.

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
| [`docs/ArcadeVox_Manuale.html`](docs/ArcadeVox_Manuale.html) | Manuale completo A4: blueprint, funzioni, collegamenti, guida all'uso |
| [`docs/ArcadeVox_Libretto_A5.html`](docs/ArcadeVox_Libretto_A5.html) | Lo stesso manuale imposto per stampare un libretto A5 |
| [`PROGRESS.md`](PROGRESS.md) | Stato dei milestone e differenze rispetto al brief iniziale |
| [`CLAUDE_2.md`](CLAUDE_2.md) | Il brief di progetto originale |

Gli HTML sono la sorgente: i PDF si generano stampandoli da browser.

> **I tre `docs/SprigSynth_*.pdf` sono scaduti.** Vengono da prima dell'ultima revisione
> degli HTML e da prima del cambio di nome, quindi dentro riportano ancora *SprigSynth*.
> Vanno rigenerati dal browser e rinominati; finché non succede, fa fede l'HTML.

## Struttura

```
src/
  main.cpp            setup(), loop(), logica di priorità delle note
  pinout.h            tutti i GPIO, centralizzati
  version.h           versione del firmware e URL del manifest
  audio_engine.*      pool di 8 voci, filtro, ADSR, metronomo, task I2S sul core 0
  input_handler.*     debounce, last-note-priority, encoder, coda degli attacchi
  sequencer.*         16 step, step edit, record quantizzato, preconteggio
  display.*           GC9A01, 7 schermate cicliche + menu impostazioni, ADSR, QR
  logo.h              wordmark della schermata di avvio (generato, non editare)
  storage.*           persistenza NVS con scrittura ritardata
  net_portal.*        access point, captive portal, OTA
  status_led.*        animazioni sul WS2812 di bordo

tools/
  make_logo.py        rigenera src/logo.h dal font Handel Gothic

firmware/
  manifest.json       la release pubblicata, che il synth va a cercare
  firmware.bin        l'immagine corrispondente
```
