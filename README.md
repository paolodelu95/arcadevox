# ArcadeVox

Sintetizzatore DIY su **ESP32-S3**, mono o polifonico, su PCB dedicato: 20 tasti Cherry MX
hot-swap illuminati, 4 encoder, joystick e display tondo.

<img src="docs/arcadevox_boot.gif" width="240" align="right" alt="Schermata di avvio">

> **Versione 2.0** — scheda nuova. Il pannello riciclato da un controller per Euro Truck
> Simulator ha lasciato il posto a un PCB disegnato apposta: la tastiera è passata da 7
> pulsanti arcade a **13 tasti cromatici** più **7 tasti funzione**, gli encoder da 2 a 4,
> e ogni tasto ha il suo LED RGB. Il firmware di conseguenza è cambiato dappertutto: vedi
> [`docs/HARDWARE.md`](docs/HARDWARE.md) per il cablaggio completo.

![Blueprint del pannello](docs/pannello.svg)

## Cos'è

Un synth hardware completo che gira su due core: il motore audio ha il core 0 tutto per sé,
mentre input, sequencer, luci e display stanno sul core 1. Monofonico o polifonico a scelta,
con un inviluppo vero e un filtro risonante.

- **Oscillatore** a phase-accumulator, 6 forme d'onda: sine, square, saw, triangle, pulse, noise
- **Filtro risonante** a variabili di stato (2 poli): cutoff 80 Hz – 7,2 kHz e **risonanza**
  fino a un soffio dall'autoscillazione, una manopola per ciascuno
- **8 BIT** su un tasto: bitcrusher più decimazione, quattro gradini da 12 a 4 bit
- **Eco, drive, sub-oscillatore, detune, portamento e LFO** (vibrato / filtro / tremolo)
- **Inviluppo ADSR** reale, un encoder per parametro
- **Uscita I2S** 44.1 kHz / 16 bit verso un MAX98357
- **MONO / POLIFONICO** commutabile: 16 voci, ognuna con fase, inviluppo e filtro suoi
- **13 tasti nota** — un'ottava cromatica intera — con last-note-priority e memoria
  dell'ordine di pressione
- **Scale e tonica**: cromatica, maggiore, minore, pentatonica, blues, dorica, araba
- **Modalità accordo**: un tasto, una triade (quinta, maggiore, minore, sospeso, ottava)
- **Arpeggiator** in cinque modi (su, giù, su/giù, casuale, ordine di pressione), a tempo
  con il sequencer
- **Step-sequencer** a 16 step con scrittura passo-passo, record quantizzato in overdub,
  preconteggio e metronomo
- **20 LED RGB sotto i tasti**: la tastiera si disegna da sola, le funzioni attive si
  accendono, l'ordine della catena la scheda **se lo impara** da sola
- **Display GC9A01** tondo con 8 schermate cicliche — fra cui effetti, VU meter ad ago,
  oscilloscopio e un menu impostazioni per categorie
- **Memoria**: pattern, parametri e mappa dei LED sopravvivono allo spegnimento
- **Aggiornamento via WiFi** con QR da inquadrare col telefono

## Hardware

| Parte | Modello |
|---|---|
| MCU | ESP32-S3-DevKitC-1 **N8** (8 MB flash, niente PSRAM) |
| Tastiera | 20 Cherry MX hot-swap con SK6812 integrato, matrice 4×5 con diodo per tasto |
| Espansore | MCP23017 su I2C: 4 colonne + 5 righe + i 4 pulsanti degli encoder |
| Encoder | 4 × EC11 con pulsante d'albero |
| Audio | MAX98357 (DAC I2S + amplificatore) su altoparlante 4–8 Ω |
| Display | GC9A01, TFT tondo 240×240, SPI |
| Joystick | 4 microswitch digitali |

Il dettaglio dei connettori, dei diodi e delle scelte di scansione sta in
[`docs/HARDWARE.md`](docs/HARDWARE.md), ricavato dallo schematico e dalla netlist del
progetto EasyEDA.

## Pinout

| GPIO | Funzione | GPIO | Funzione |
|---|---|---|---|
| 4 / 5 | I2C SDA / SCL verso l'MCP23017 | 13 / 14 | Display SCLK / MOSI |
| 6 / 7 | Encoder 1 A / B | 15 / 16 / 17 | Display RST / DC / CS |
| 8 / 9 | Encoder 2 A / B | 18 / 21 / 1 | I tre segnali I2S (vedi sotto) |
| 10 / 11 | Encoder 3 A / B | 12 | Dato della catena di LED |
| 40 / 39 | Encoder 4 A / B | 2 / 41 / 42 / 47 | Joystick su / giù / sx / dx |
| — | I 20 tasti passano dall'espansore | 48 | LED RGB di bordo |

**I tre fili dell'audio non hanno un nome sullo schematico.** Il connettore porta 3V3, GND e
tre segnali numerati: quale sia il BCLK, quale l'LRCLK e quale il DIN lo decide il cavo. Il
firmware parte con l'ordine serigrafato sui moduli MAX98357 (LRC, BCK, DIN) e, se non è
quello, la voce **AUDIO → USCITA** del menu prova le altre cinque combinazioni a caldo,
senza ricompilare niente.

## I comandi

**13 tasti nota** in basso e al centro, disposti come un pezzo di pianoforte: i tasti neri
nella fila di mezzo, i bianchi in quella sotto. **7 tasti funzione** nella fila in alto: ognuno
fa una cosa premuto e un'altra tenuto premuto.

| Tasto | Pressione breve | Tenuto premuto |
|---|---|---|
| **FN1** | arpeggiator on/off | cambia modo (su, giù, su/giù, casuale, ordine) |
| **FN2** | **8 BIT** on/off | cambia profondità: 12, 8, 6, 4 bit |
| **FN3** | REC | STEP EDIT |
| **FN4** | play / stop | svuota il pattern |
| **FN5** | HOLD | ADSR edit |
| **FN6** | mono / polifonico | modalità accordo |
| **FN7** | schermata successiva | menu impostazioni |

I quattro encoder cambiano mestiere secondo dove ti trovi:

| | uso normale | ADSR edit | menu impostazioni | step edit |
|---|---|---|---|---|
| **ENC 1** | cutoff | attack | scorre le voci | scorre il cursore |
| **ENC 2** | **risonanza** | decay | cambia il valore | — |
| **ENC 3** | volume | sustain | — | — |
| **ENC 4** | parametro a scelta | release | — | BPM |

Il **click dell'albero** dei primi tre inserisce il passo fine; quello del quarto sceglie cosa
comanda l'encoder 4 fra BPM, eco (mix e tempo), velocità e profondità dell'LFO, drive, sub,
detune e glide. Il joystick fa ottava (su/giù) e forma d'onda (sinistra/destra).

## Mono e polifonico

**FN6** commuta fra monofonico e polifonico. Il motore ha **16 voci**, ognuna con fase,
inviluppo e filtro propri — una nota nuova non eredita lo stato di quella precedente. Gli
identificativi sono esattamente quanti servono (13 tasti, 1 per il sequencer, 2 per le note
aggiunte dagli accordi), quindi ogni voce è dedicata: niente allocazione dinamica, niente
*voice stealing*, comportamento sempre prevedibile. Se ne suonassero comunque più di dieci
insieme, il motore lascia fuori dal giro quelle più spente — code di rilascio — invece di
mancare un blocco e far sentire un buco.

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

Tieni premuto **FN3** per mezzo secondo. Compare un cursore bianco sulla griglia: da lì in
poi non c'è nessuna fretta, il tempo non scorre.

| Comando | Cosa fa |
|---|---|
| tasto nota | scrive la nota sotto il cursore, te la fa sentire, **avanza da solo** |
| joystick ↑ ↓ | ottava dello step che stai per scrivere |
| joystick ← → | sposta il cursore |
| encoder 1 | scorre il cursore velocemente |
| encoder 4 | BPM continuo, 40–240 |
| FN5 (breve) | svuota lo step e avanza |
| FN1 (breve) | scrive un **legato**: la nota precedente prosegue senza ripartire |
| FN4 (breve) | avvia o ferma il loop — puoi continuare a scrivere mentre suona |
| FN4 (lungo) | **svuota tutti i 16 step**: non si torna indietro |
| FN3 (lungo) | esce |

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

Long-press di **FN5** (>600 ms). Con quattro encoder la faccenda delle modalità da ricordare
finisce: **un parametro per manopola**, nello stesso ordine in cui sono scritti sullo schermo.

| Parametro | Comando |
|---|---|
| **A**ttack | encoder 1 |
| **D**ecay | encoder 2 |
| **S**ustain | encoder 3 |
| **R**elease | encoder 4 |

Al joystick, che qui resterebbe senza lavoro, va il bersaglio dell'LFO: sinistra e destra
scorrono fra spento, vibrato, filtro e tremolo. La legenda è scritta in fondo alla schermata:
`1=A  2=D  3=S  4=R`.

## 8 BIT

**FN2** e tutto quello che esce diventa a 8 bit. L'effetto agisce sul segnale finale — non su
una voce sola — e fa due cose insieme, che è quello che serve perché suoni davvero come un
chip e non come un file rovinato:

1. **decimazione**: il campione viene tenuto fermo per N giri, il che porta l'alias in banda
   ed è ciò che mette quel velo metallico;
2. **quantizzazione**: i livelli si riducono a 2ⁿ, e sulle code di rilascio si sente il
   gradino.

Tenendo premuto **FN2** si scorrono i quattro gradini: `12 BIT` (appena sporco), `8 BIT`
(quello classico), `6 BIT`, `4 BIT` (console tascabile). Cambiare gradino accende anche
l'effetto: nessuno gira quella manopola per sentire il silenzio.

## Luci sotto i tasti

Ogni tasto ha un SK6812 dentro, tutti in catena su un filo solo. Le note disegnano la
tastiera (bianchi e neri hanno tinte diverse) e si accendono quando suonano; la sequenza si
distingue perché è verde; i tasti funzione stanno sempre accesi appena appena, e si
illuminano quando la loro funzione è inserita.

**L'ordine della catena non è deducibile dallo schematico** oltre al primo LED, quindi la
scheda non lo indovina: **SETTINGS → LUCI → IMPARA LUCI** accende un LED alla volta e aspetta
che tu prema il tasto che si è illuminato. Venti pressioni, e la mappa finisce in NVS per
sempre. La luminosità si regola nella voce sopra, da spenta a otto.

## Sensibilità degli encoder

Gli encoder sono incrementali, e quanto muove uno scatto era cablato nel codice:
col volume a 1/50 di corsa per scatto servivano **due giri e mezzo** di manopola
per andare da zero a fondo scala. Ora si regola dalla schermata **SETTINGS**.

| Categoria | Voce | Cosa fa | Default |
|---|---|---|---|
| ENCODER | VOLUME | sensibilità dell'encoder 3 | 2.4 giri per l'intera corsa |
| | CUTOFF | sensibilità degli encoder 1 e 2 | 3.2 giri da 80 Hz a 7,2 kHz |
| | ADSR | sensibilità in ADSR edit e sull'encoder 4 | 2.4 giri |
| | PASSO FINE | divisore col click dell'encoder | 1/4 |
| TASTIERA | SCALA | cromatica, maggiore, minore, pentatonica, blues, dorica, araba | cromatica |
| | TONICA | da quale nota parte la scala | DO |
| LUCI | LUMINOSITÀ | da spente a 8 | 5 |
| | IMPARA LUCI | insegna alla scheda l'ordine della catena | — |
| AUDIO | USCITA | quale filo è BCK, LRC e DIN | LRC BCK DIN |
| RETE | MODALITÀ WIFI | accende la radio e mostra il QR | — |

Per modificarle **tieni premuto FN7** dalla schermata SETTINGS: si entra nel menu. Da dentro,
una **pressione breve** dello stesso tasto scende di una voce — e sulle voci d'azione la
esegue — mentre una **lunga** esce. L'encoder 2 cambia il valore della voce scelta, l'encoder
1 la scorre: lì gli encoder non fanno cutoff e volume, che restano fermi finché non esci. I
valori sono scritti in giri di manopola perché è la grandezza che senti sotto le dita, non
frazioni di corsa.

Le voci sono dieci e il vetro è tondo: se ne vedono cinque per volta, e la finestra segue il
cursore. Due frecce ai lati dicono che sopra o sotto c'è dell'altro.

Il *passo fine* adesso è raggiungibile davvero: sulla scheda nuova i pulsanti degli alberi
sono cablati, e i primi tre encoder lo commutano col click.

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

Dal telefono agganciato al QR **il portale si apre subito, senza login**: per stare su
quella rete hai già digitato la password WPA2, e chiedertela una seconda volta in una
finestra del browser non proteggerebbe niente — nel browser ridotto del captive portal
quella finestra è anzi il punto in cui ci si incastra. La richiesta di utente e password
resta solo per chi apre il portale **dall'indirizzo sulla rete di casa**, che è raggiungibile
da chiunque sia in LAN senza aver visto il display: lì l'utente è `arcade` e la password è
quella scritta accanto al QR. Si esce con **PLAY**, che riavvia il synth.

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
| [`docs/ArcadeVox_Libretto.pdf`](docs/ArcadeVox_Libretto.pdf) | **Libretto 2.0**, 16 pagine A4: blueprint, connettori pin per pin, montaggio, uso |
| [`docs/ArcadeVox_Libretto.html`](docs/ArcadeVox_Libretto.html) | La sorgente del libretto, da cui si rigenera il PDF |
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | Il cablaggio della scheda, ricavato dallo schematico e dalla netlist |
| [`docs/pannello.svg`](docs/pannello.svg) | Blueprint del pannello, da solo |
| [`PROGRESS.md`](PROGRESS.md) | Stato dei milestone e differenze rispetto al brief iniziale |
| [`CLAUDE_2.md`](CLAUDE_2.md) | Il brief di progetto originale (scheda 1.x) |

Il PDF è già pronto da stampare — 16 pagine, cioè quattro fogli esatti a libretto. Se ritocchi
l'HTML lo rigeneri così:

```bash
"/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge" --headless --disable-gpu \
  --no-pdf-header-footer --run-all-compositor-stages-before-draw --virtual-time-budget=6000 \
  --print-to-pdf=docs/ArcadeVox_Libretto.pdf "file://$PWD/docs/ArcadeVox_Libretto.html"
```

Va bene qualunque browser Chromium. Attenzione a non far crescere le sezioni: ognuna deve
stare in **269 mm** di altezza utile, o si porta dietro una pagina in più.

## Struttura

```
src/
  main.cpp            setup(), loop(), logica di priorità delle note
  pinout.h            tutta la scheda: matrice, encoder, connettori, LED
  version.h           versione del firmware e URL del manifest
  audio_engine.*      16 voci, filtro risonante, ADSR, 8 BIT, eco, LFO, task I2S sul core 0
  input_handler.*     matrice su MCP23017, 4 encoder, joystick, debounce
  keylight.*          catena di 20 SK6812: driver RMT, colori, apprendimento della mappa
  sequencer.*         16 step, step edit, record quantizzato, preconteggio
  display.*           GC9A01, 8 schermate cicliche + menu, ADSR, effetti, QR
  logo.h              wordmark della schermata di avvio (generato, non editare)
  storage.*           persistenza NVS con scrittura ritardata
  net_portal.*        access point, captive portal, OTA
  status_led.*        animazioni sul WS2812 di bordo

tools/
  make_logo.py        rigenera src/logo.h dal font Handel Gothic
  simdisplay/         renderizza le schermate sul computer e controlla il cerchio

firmware/
  manifest.json       la release pubblicata, che il synth va a cercare
  firmware.bin        l'immagine corrispondente
```
