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
- **Inviluppo ADSR** reale, una manopola per parametro, con il profilo disegnato a schermo
- **Inviluppo di filtro** regolabile: è l'ingrediente che distingue un pianoforte da un organo
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
- **Il pattern ha uno strumento suo**: registri il giro di batteria, cambi timbro e ci
  suoni sopra il synth mentre la batteria continua a girare. Il pattern adotta lo
  strumento con cui ci scrivi dentro, e se lo ricorda allo spegnimento
- **20 LED RGB sotto i tasti**: la tastiera si disegna da sola, le funzioni attive si
  accendono, l'ordine della catena la scheda **se lo impara** da sola
- **Collaudo delle luci all'accensione**: mentre il display disegna l'intro, il pannello
  fa passare rosso, verde e blu su tutti e venti i tasti — un LED morto o un canale morto
  si vedono subito — e poi racconta la stessa scena che sta comparendo sul vetro
- **Display GC9A01** tondo con un'interfaccia **radiale**: sette schermate, la ghiera che dice
  dove sei, e sotto ogni manopola scritto cosa fa in quel momento
- **Tredici suoni campionati** su una schermata sua: i tasti smettono di essere note e
  diventano tredici suoni, con la velocità di lettura su una manopola
- **MIDI IN** dalla porta USB nativa: il synth compare al computer come strumento,
  con dinamica vera, pitch bend, pedale di risonanza e i CC che tutti i DAW danno per scontati
- **Piano e batteria campionati**, in coda allo stesso elenco dei timbri: un pianoforte
  vero su sette radici e una batteria acustica di tredici pezzi, uno per tasto
- **15 timbri di fabbrica** — pianoforte, chitarra, organo, archi, campane, acido, arcade… —
  con una schermata tutta loro, e il ritratto di ognuno: forma d'onda e inviluppo
- **Memoria**: pattern, parametri e mappa dei LED sopravvivono allo spegnimento
- **Aggiornamento via WiFi** con QR da inquadrare col telefono

## Hardware

| Parte | Modello |
|---|---|
| MCU | ESP32-S3-DevKitC-1 **N16R8** (16 MB flash, 8 MB di PSRAM ottale) |
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

Una regola sola, e c'è scritta a schermo: **ogni tasto fa la parola che ha stampata sopra, e
ogni manopola fa quello che il display le scrive sotto.** Non c'è nient'altro da ricordare.

### I sette tasti funzione

Fila in alto, una funzione ciascuno. Niente seconde funzioni nascoste sotto la pressione
lunga: se un tasto fa una cosa, la fa e basta, sempre, in ogni schermata e in ogni modalità.

| Tasto | Cosa fa |
|---|---|
| **ARPEGGIO** | arpeggiator on/off |
| **8 BIT** | il degrado on/off |
| **REGISTRA** | REC — e ti porta sulla schermata RITMO |
| **AVVIA** | play / stop — e ti porta sulla schermata RITMO |
| **TIENI** | latch: le note continuano a suonare a tasti rilasciati |
| **VOCI** | mono / polifonico |
| **SILENZIO** | zittisce tutto, subito |

L'unica pressione lunga rimasta è su **AVVIA**, e non è una seconda funzione: è una richiesta
di conferma. Tenendolo premuto **si svuota il pattern**, e mentre tieni l'anello esterno del
display si riempie di rosso — molli prima e non è successo niente.

Le sei funzioni che prima erano nascoste non sono sparite, sono andate dove si vedono: il modo
dell'arpeggiator e la grana dell'8 BIT sono righe della schermata **EFFETTI**, il modo accordo
è la quarta manopola di **TIMBRI**, e lo step edit non esiste più come modalità perché su
**RITMO** il cursore c'è sempre.

### Le quattro manopole

Cambiano mestiere a seconda della schermata, ma non c'è niente da imparare: sotto ognuna, in
basso sul display, c'è **scritto cosa fa adesso**, con un arco che mostra a che punto della
corsa sei. Girando, il nome lascia il posto al valore per un secondo e poi torna.

| Schermata | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| **SUONA** | onda | taglio | volume | risonanza |
| **TIMBRI** | timbro | scala | volume | accordo |
| **SUONI** | scegli il suono | — | volume | velocità |
| **INVILUPPO** | attacco | decadimento | **sostegno** | rilascio |
| **EFFETTI** | scegli la riga | cambia il valore | volume | — |
| **RITMO** | passo | nota | volume | tempo |
| **MENU** | scegli la voce | cambia il valore | volume | — |

La terza è **sempre il volume**, tranne su INVILUPPO dove le lettere da regolare sono quattro
e la terza è la S. Dove una manopola non fa niente c'è scritto un trattino: è
un'informazione anche quella, e nessuno resta a chiedersi perché non succede nulla.

### Il click delle manopole

Due gesti, e li conosci già tutti e due:

- **tieni premuto il click e gira** → passo fine, finché lo tieni;
- **premi e lascia** (senza aver girato) → quel parametro **torna com'era** nel timbro
  caricato, con la conferma a schermo.

Il secondo è il comando più importante di tutto lo strumento e non è una scorciatoia da
esperti: è un'assicurazione. Una manopola che non conosci la giri volentieri solo se sai come
tornare indietro.

Nel **MENU** le tre righe rosse — IMPARA LUCI, AZZERA LUCI, MODALITÀ WIFI — non si premono, si
**tengono**: il click della seconda manopola per novecento millisecondi, con l'anello esterno
che si riempie. È lo stesso patto di AVVIA/svuota, applicato all'unico altro posto dove ci sono
gesti che non si tornano indietro.

### Il joystick

| Direzione | Cosa fa |
|---|---|
| ← → | schermata precedente / successiva |
| ↑ ↓ | ottava, da −2 a +2 |

Una regola sola, valida ovunque, e nessun tasto la duplica. Il joystick è anche il **torna
indietro** dello strumento: è lui che esce dall'apprendimento delle luci e dalla modalità WiFi.
E siccome è cablato direttamente sui GPIO, funziona anche quando l'espansore I2C non risponde
e la tastiera è muta.

L'ottava non fa comparire nessun riquadro: sta scritta in permanenza in alto a sinistra, su una
targhetta del colore dell'ottava — lo stesso che prendono le celle del sequencer.

### Le sette schermate

Si percorrono col joystick, nei due sensi. La ghiera del display è divisa in sette settori
colorati, uno per schermata: quello dove sei è acceso pieno, gli altri sono spenti. Dopo due
giri diventa una mappa — il viola sono i timbri, l'arancione i suoni, il lime il ritmo.

E se ti dimentichi dove sei, **ci torna da solo**: passati dieci secondi senza che nessuno
tocchi niente, il display rientra su SUONA. Uno strumento acceso in mezzo a una stanza viene
raccolto da qualcuno che non l'ha lasciato lì, e quello che deve trovare è la pagina su cui si
suona — non l'elenco delle impostazioni aperto dove l'ha piantato l'ultima persona. Tenere
premuto un tasto conta come attività, quindi nemmeno un accordo lungo lo fa scattare.

| Schermata | Cosa mostra |
|---|---|
| **SUONA** | la forma d'onda vera che esce, la curva del filtro dietro come orizzonte, e il livello in fondo |
| **TIMBRI** | i quindici timbri, il piano e la batteria campionati, col ritratto di quello scelto |
| **SUONI** | tredici suoni campionati, uno per tasto |
| **INVILUPPO** | il profilo A/D/S/R disegnato, che si deforma mentre giri |
| **EFFETTI** | quattordici righe: grana, eco, LFO, arpeggio, corpo, inviluppo di filtro |
| **RITMO** | la griglia a 16 passi, con la testina che orbita sul bordo |
| **MENU** | le impostazioni |

Sotto l'onda di **SUONA**, sull'arco basso, ventiquattro tacche dicono **quanto forte sta
uscendo**: verdi fin dove si sta larghi, rosse nell'ultimo quinto dove si rischia di tosare, e
una tacca bianca più lunga che resta appesa mezzo secondo sul picco prima di ricadere. Le
tacche della zona rossa si vedono, in rosso spento, anche quando il suono è basso: il confine
si guarda **prima** di arrivarci.

Prima era una schermata a sé, con un bel quadrante ad ago — e valeva molto meno, perché per
consultarla bisognava andarsene dalla pagina su cui si suona. Un livello si guarda *mentre* si
fa rumore, non al posto di farlo.

### Quando il display parla

La banda al centro compare solo per dire quello che **non è già sotto i tuoi occhi**: i tasti
funzione quando la loro targhetta non è a schermo, il ripristino di una manopola, gli allarmi.
Non compare mai per una manopola — quella ha già il suo arco e la sua didascalia — né per
l'ottava, che sta nel telaio. Dura novecento millisecondi e si disegna una volta sola.

## I tredici suoni

La schermata **SUONI**: ogni tasto nota fa partire un suono suo e nient'altro — nessuna voce
del motore si accende, quindi non resta niente appeso. La quarta manopola è la **velocità di
lettura**, da metà al doppio, ed è quella che li rende una cosa con cui si gioca invece di
tredici pulsanti che fanno sempre uguale. Passano dall'eco e dall'8 BIT come tutto il resto,
e i tredici LED prendono tredici tinte diverse, perché lì ogni tasto è per conto suo.

I tredici di serie sono **sintetizzati**, non registrati: una trombetta da stadio sono tre
lame scordate che calano mentre suonano, un boom è una sinusoide che scende sotto i quaranta
hertz con davanti un colpo di rumore, una vocale urlata sono tre risonanze su un treno di
impulsi glottali. Li genera [`tools/make_samples.py`](tools/make_samples.py) e costano 180 kB
di flash in tutto.

**Per metterci i tuoi**: butta i file audio in `tools/samples/` chiamandoli `01 nome.wav`,
`02 altro.mp3` e via — il numero decide il tasto — poi `python3 tools/make_samples.py` e
`pio run -t upload`. Vale qualunque formato, lo script converte, normalizza, taglia il
silenzio e sfuma le code. Quella cartella è esclusa da git apposta: vedi
[`tools/samples/README.md`](tools/samples/README.md).

## Mono e polifonico

Il tasto **VOCI** commuta fra monofonico e polifonico. Il motore ha **16 voci**, ognuna con fase,
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
| TIENI | tiene l'ultima nota | tiene l'**accordo**, e ogni tasto premuto dopo ci si aggiunge |
| arpeggiator | invariato | invariato: resta monofonico, è il suo senso |

L'ampiezza è compensata sull'**energia** delle voci e non sul loro numero, così una nota in
coda di rilascio non abbassa quelle che stanno ancora suonando. La modalità viene salvata e
si ritrova all'accensione successiva.

## Usare il sequencer

Sedici passi, un sedicesimo ciascuno, sulla schermata **RITMO**. Due modi di riempirli, che
convivono: si può scrivere anche mentre il loro giro sta suonando.

### Scrivere con calma

Non si "entra" più in niente: a giro fermo, su RITMO, **il cursore c'è già** e i tasti
scrivono. Lo dice la targhetta `SCRIVI` in cima alla schermata, e la si vede senza doverla
cercare — che è poi l'unico modo di non restare dentro una modalità senza accorgersene.

| Comando | Cosa fa |
|---|---|
| tasto nota | scrive la nota sotto il cursore, te la fa sentire, **avanza da solo** |
| manopola 1 | sposta il cursore |
| manopola 2 | scorre il contenuto del passo: pausa, le tredici note, legato |
| manopola 4 | tempo, 40–240 BPM |
| joystick ↑ ↓ | ottava del passo che stai per scrivere |
| click manopola 2 | svuota il passo sotto il cursore |
| **AVVIA** | fa partire o fermare il giro — puoi continuare a scrivere mentre suona |
| **AVVIA** tenuto | **svuota tutti i 16 passi**: non si torna indietro, e l'anello lo dice |

Il tasto che scrive e fa avanzare il cursore è lo *step input* di MPC e Roland MC: si digita
una melodia premendo un tasto dopo l'altro, come si scrive su una tastiera. La manopola 2
serve a chi non sa dove sta il MI — e a mettere pause e legati, che un tasto non ha.

### Registrare suonando

**REGISTRA**. Parte una battuta di preconteggio col metronomo (durante la quale il pattern che
c'è già suona), poi il giro va all'infinito e tutto quello che suoni ci finisce dentro.

- Le note si agganciano al sedicesimo **più vicino**: a 120 BPM hai ±62 ms di tolleranza.
- È un **overdub**: ogni passata aggiunge, niente viene cancellato. Non serve azzeccare tutto
  in una volta.
- **REGISTRA** di nuovo per uscire dalla registrazione lasciando il giro in play.
- Con l'arpeggiator acceso finiscono nel pattern i suoi passi, non i tasti che tieni.

REGISTRA e AVVIA ti portano da soli su RITMO: quello che vedi e quello che le manopole toccano
non possono più andare fuori sincrono.

La griglia mostra il contenuto: ogni cella porta l'iniziale della nota e un colore per
l'ottava, i legati una barretta. La cornice verde è la testina, quella bianca il cursore. Sul
bordo del display un pallino **orbita** sui sedici passi: il giro visto come giro.

## Inviluppo

Una schermata dell'anello come le altre, non più una modalità in cui si entra tenendo premuto
un tasto. **Un parametro per manopola**, nello stesso ordine in cui si leggono:

| Parametro | Manopola |
|---|---|
| **A**ttacco | 1 |
| **D**ecadimento | 2 |
| **S**ostegno | 3 |
| **R**ilascio | 4 |

Il **sostegno sulla terza** è l'unica eccezione alla regola "la terza è sempre il volume", ed è
una promessa mantenuta: la vecchia schermata scriveva `3=S` sotto le quattro righe mentre la
manopola faceva il volume, e il sustain era l'unico parametro del synth che nessun comando
poteva toccare — si poteva solo ereditarlo caricando un timbro.

Al centro il profilo dell'inviluppo è **disegnato**, e si deforma mentre giri. Ogni tratto ha
il colore della manopola che lo comanda, così si vede a colpo d'occhio quale delle quattro
muove quale pezzo della curva.

## 8 BIT

Premi **8 BIT** e tutto quello che esce diventa, appunto, a 8 bit. L'effetto agisce sul segnale finale — non su
una voce sola — e fa due cose insieme, che è quello che serve perché suoni davvero come un
chip e non come un file rovinato:

1. **decimazione**: il campione viene tenuto fermo per N giri, il che porta l'alias in banda
   ed è ciò che mette quel velo metallico;
2. **quantizzazione**: i livelli si riducono a 2ⁿ, e sulle code di rilascio si sente il
   gradino.

Il tasto **8 BIT** accende e spegne, e basta. I quattro gradini — `12 BIT` (appena sporco),
`8 BIT` (quello classico), `6 BIT`, `4 BIT` (console tascabile) — stanno sulla riga **GRANA**
della schermata EFFETTI, dove si vedono scritti invece di scorrere al buio sotto una pressione
lunga. Cambiare gradino accende anche l'effetto: nessuno gira quella manopola per sentire il
silenzio.

## Luci sotto i tasti

Ogni tasto ha un SK6812 dentro, tutti in catena su un filo solo. Le note disegnano la
tastiera (bianchi e neri hanno tinte diverse) e si accendono quando suonano; la sequenza si
distingue perché è verde; i tasti funzione stanno sempre accesi appena appena, e si
illuminano quando la loro funzione è inserita — ognuno del colore della sua targhetta a
schermo, così il tasto e quello che si legge sul display sono la stessa cosa.

Finché nessuno ha ancora suonato, i tredici tasti nota **respirano** insieme, piano: è un
invito che non ha una schermata da chiudere, non si ripete e non occupa un pixel di display, e
si spegne da solo alla prima nota. Il tasto **AVVIA** batte il tempo anche a pattern fermo: chi
non ha mai visto un sequencer capisce cos'è il BPM guardando un tasto lampeggiare, e girando la
manopola TEMPO lo vede cambiare prima ancora di premere qualcosa.

**L'ordine della catena non è deducibile dallo schematico** oltre al primo LED, quindi la
scheda non lo indovina: **MENU → LUCI → IMPARA LUCI** (tenendo il click della seconda manopola) accende un LED alla volta e aspetta
che tu prema il tasto che si è illuminato. Venti pressioni, e la mappa finisce in NVS per
sempre; si esce in qualunque momento col joystick a sinistra, e **AZZERA LUCI** rimette
l'ordine di fabbrica se la mappa è venuta storta. La luminosità si regola nella voce sopra, da
spenta a otto.

## Sensibilità degli encoder

Gli encoder sono incrementali, e quanto muove uno scatto era cablato nel codice:
col volume a 1/50 di corsa per scatto servivano **due giri e mezzo** di manopola
per andare da zero a fondo scala. Ora si regola dalla schermata **MENU**.

| Categoria | Voce | Cosa fa | Default |
|---|---|---|---|
| ENCODER | VOLUME | sensibilità della manopola del volume | 2.4 giri per l'intera corsa |
| | CUTOFF | sensibilità di taglio e risonanza | 3.2 giri da 80 Hz a 7,2 kHz |
| | ADSR | sensibilità dell'inviluppo e delle righe di EFFETTI | 2.4 giri |
| | PASSO FINE | divisore tenendo premuto il click | 1/4 |
| TASTIERA | SCALA | cromatica, maggiore, minore, pentatonica, blues, dorica, araba | cromatica |
| | TONICA | da quale nota parte la scala | DO |
| LUCI | LUMINOSITÀ | da spente a 8 | 5 |
| | IMPARA LUCI | insegna alla scheda l'ordine della catena | — |
| | AZZERA LUCI | rimette l'ordine di fabbrica | — |
| AUDIO | USCITA | quale filo è BCK, LRC e DIN | LRC BCK DIN |
| | MIDI OUT | cosa manda al computer | NOTE |
| RETE | MODALITÀ WIFI | accende la radio e mostra il QR | — |

Non si "entra" nel menu: guardarlo **è** esserci dentro. La manopola 1 scorre le voci, la 2
cambia il valore di quella scelta, e le tre voci rosse — che sono le uniche azioni che non si
tornano indietro — si eseguono **tenendo premuto il click della manopola 2** per novecento
millisecondi, con l'anello esterno che si riempie. I valori sono scritti in giri di manopola
perché è la grandezza che senti sotto le dita, non frazioni di corsa.

Il *passo fine* non è più uno stato che si inserisce e si dimentica: **si tiene premuto il
click e si gira**, come il tasto che rallenta il puntatore del mouse. Il divisore qui sopra
dice di quanto. Prima era un interruttore invisibile, senza conferma a schermo e per giunta
inerte su cinque schermate su nove: chi lasciava una manopola a 1/16 se la ritrovava, mesi
dopo, apparentemente rotta.

## MIDI IN

Il synth si collega al computer con la **porta USB nativa** e compare come strumento MIDI:
un dispositivo composito, porta seriale e MIDI sulla stessa presa. Non serve nessuna
libreria esterna — la classe MIDI è già dentro la TinyUSB del core (`CONFIG_TINYUSB_MIDI_ENABLED=y`),
basta registrare l'interfaccia prima che l'USB parta.

Le note in arrivo hanno **dinamica vera**: la velocity scala il picco dell'inviluppo *e*
quanto si apre il filtro, che è ciò che distingue un pianoforte suonato piano da uno
suonato forte. I tasti del pannello invece sono interruttori e mandano sempre forza piena.

| Messaggio | Cosa fa |
|---|---|
| Note on / off | suona, con dinamica; velocity 0 vale come note-off |
| CC 1 (modulazione) | profondità dell'LFO (e lo accende sul vibrato se era spento) |
| CC 7 | volume |
| CC 64 | pedale di risonanza: tiene le note rilasciate |
| CC 71 | **risonanza** del filtro |
| CC 74 | cutoff |
| CC 91 | mix dell'eco |
| CC 94 | detune |
| Program change | sceglie il **timbro** di fabbrica |
| Pitch bend | ±2 semitoni |
| CC 120 / 123 | spegne tutto |

I due versi si provano dalla console: il firmware scrive quando un host si collega, quali
endpoint USB ha ottenuto (zero significa che l'allocatore li aveva finiti) e quante note
stanno suonando per via del MIDI.

### MIDI OUT

Il verso opposto: quello che suoni finisce sul computer. Tasti, arpeggiator, sequencer e note
aggiunte dagli accordi partono **dallo stesso elenco che pilota il motore audio**, quindi non
c'è modo che le due cose vadano fuori sincrono. Escono anche i CC delle manopole (cutoff 74,
risonanza 71, volume 7), il cambio programma quando scegli un timbro, e — se lo abiliti — lo
start/stop e il clock a 24 impulsi per movimento, così un DAW può andare a tempo con il
sequencer.

Si regola da **MENU → AUDIO → MIDI OUT**: `SPENTO`, `NOTE`, `NOTE+CLOCK`. Il clock è
separato apposta — un DAW che riceve impulsi di sincronismo senza aspettarseli comincia a
seguire il tempo del synth, e chi non lo sapeva si ritrova il progetto che accelera da solo.

**Le note ricevute non tornano indietro.** Con un DAW che rimanda in eco quello che riceve si
innescherebbe un anello e ogni nota si moltiplicherebbe da sola: le voci che stanno suonando
perché arrivano dal cavo sono escluse dall'uscita.

Il clock nasce dal loop del core 1, che gira ogni millisecondo scarso: il tremolio è di
quell'ordine. Abbastanza per accompagnare, non per accordarci sopra un disco.

**Il tasto fisico ha sempre la precedenza.** Le voci sono sedici e la tastiera ne occupa una
per tasto: se una voce serve a un dito, la nota MIDI che ci stava sopra tace finché il dito
non si alza. L'alternativa — zittire le dita — renderebbe lo strumento inservibile proprio
quando un sequencer lo sta pilotando e ci vuoi suonare sopra. Quando le voci finiscono si
ruba la più vecchia.

## I timbri di fabbrica

Un synth sottrattivo non *riproduce* un pianoforte: quello lo fa un campionatore. Riproduce
il suo **comportamento** — attacco secco, suono che si spegne da solo mentre tieni il tasto,
timbro che si scurisce mentre la nota muore — e con quelle tre cose insieme l'orecchio dice
"pianoforte". È così che si chiamano i preset di qualunque synth analogico dagli anni Settanta.

L'ingrediente che lo rende possibile è l'**inviluppo di filtro**, aggiunto in questa versione:
il filtro si spalanca all'attacco e si richiude da solo. Senza, un "pianoforte" resta un
organo con l'attacco veloce.

Sono quindici: BASE, PIANOFORTE, CHITARRA, ORGANO, BASSO, ARCHI, FLAUTO, CAMPANE,
CLAVICEMBALO, VIBRAFONO, ACIDO, ARCADE, PAD SPAZIALE, TAMBURO, LASER. Si scelgono da
la schermata **TIMBRI**, e si sentono mentre giri la manopola: scorrere *è* caricare. Caricarne uno
sovrascrive i parametri correnti: da lì in poi si ritocca liberamente, un preset è un punto
di partenza e non una gabbia.

## Piano e batteria campionati

In coda ai quindici timbri ci sono **PIANO** e **BATTERIA**, sulla stessa manopola perché
per chi suona sono la stessa domanda — *con che suono* — e distinguerli in una schermata a
parte avrebbe separato ciò che l'orecchio non separa. Sotto però non sono preset: sono
campioni, e il motore sottrattivo tace.

Il **piano** è multi-campione: sette radici, una ogni tre semitoni. Una nota si suona
prendendo la radice più vicina e rileggendo il campione al rapporto che la porta
all'intonazione giusta. Dentro l'arco delle radici lo spostamento non supera mai il semitono
e mezzo — oltre, un pianoforte si sente tirato — e fuori si trasporta di ottave intere, cioè
con un rapporto esatto di 2, l'unico spostamento grande che un orecchio perdona.

La **batteria** ha tredici pezzi, uno per tasto, e non si intona: un rullante non ha
un'altezza da trasporre.

Un colpo campionato parte **fuori dalla catena delle voci**: non occupa una voce del motore,
quindi non toglie polifonia alle dita. È la ragione per cui il pattern e la tastiera possono
davvero suonare insieme invece di rubarsi il posto — vedi *Il pattern ha uno strumento suo*.

## Aggiornare il firmware via WiFi

> **Scorciatoia senza tastiera.** La modalità rete si raggiunge dal menu, cioè
> dalle manopole — che su una scheda con l'espansore non ancora funzionante non c'è. Per
> questo **tenendo premuto il tasto BOOT della DevKit per due secondi** si entra in modalità
> rete comunque: quel tasto sta sul modulo e non dipende da niente del PCB. Le credenziali
> del portale finiscono anche sulla seriale, quindi si può aggiornare pure con il display
> spento.

Vai col joystick fino a **MENU**, scorri con la manopola 1 fino a **MODALITÀ WIFI** e
**tieni premuto il click della manopola 2**: l'anello esterno si riempie in novecento
millisecondi e la radio si accende. Il cursore non torna a capo apposta — si scende fino
all'ultima voce e ci si ferma, così non si arriva sulla riga rossa credendo di tornare in cima.
Il synth **ammutolisce** — lo stack WiFi occupa lo stesso core del motore audio, quindi la
modalità è esclusiva — e sul display compare un QR. Si esce col **joystick a sinistra**, che
riavvia la scheda.

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
quella scritta accanto al QR. Si esce col **joystick a sinistra**, che riavvia il synth.

Il nome della rete di casa **si sceglie da un elenco**: il synth scandaglia le reti attorno e
te le propone in una tendina, con la potenza del segnale. Il campo libero sotto serve solo
alle reti nascoste, che in nessuna scansione compaiono.

Le credenziali della rete di casa **restano nel synth**: alla successiva accensione della radio
si ricollega da solo, senza ridigitarle, e il display mostra l'indirizzo che ha preso. Dal
portale si vede quale rete è in memoria e la si può dimenticare.

Ricollegandosi controlla anche se esiste un firmware più nuovo, e se c'è te lo scrive sul
display. Lo installi **tenendo premuto il click della seconda manopola** — la stessa che hai
tenuto premuta sulla voce RETE per arrivare qui: il gesto che apre la modalità rete e il gesto
che installa sono lo stesso, e l'anello esterno si riempie mentre tieni.

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

Dalla 2.1 l'USB è in modalità **OTG** (`ARDUINO_USB_MODE=0`), perché è l'unica delle due
periferiche USB dell'S3 che sappia fare MIDI. Conseguenza pratica: la porta "USB" cambia
identità quando il firmware parte. Se un caricamento non dovesse agganciarla ci sono due
strade che funzionano sempre — la porta **UART** della DevKitC-1, che ha un convertitore suo,
oppure tenere premuto **BOOT** mentre si preme **RESET** per entrare nel bootloader della ROM.

Il `platformio.ini` è pinnato al core Arduino 2.0.x, perché il motore audio usa il driver
legacy `driver/i2s.h`; di conseguenza anche Arduino_GFX è fermo alla 1.4.9, l'ultima
compatibile con quel core.

## Crediti dei campioni

Il **piano** viene dalla [University of Iowa Electronic Music Studios](https://theremin.music.uiowa.edu/MIS.html),
che pubblica le sue registrazioni dal 1997 dichiarandole utilizzabili *"for any projects,
without restrictions"*.

La **batteria** è la MuldjordKit di Lars Muldjord, registrata per DrumGizmo e rimessa
insieme dal progetto FreePats, sotto **Creative Commons Attribution 4.0**:

> Drum samples provided by DrumGizmo.org.

Quella riga non è una cortesia, è la condizione della licenza. È anche la regola che decide
da dove si prendono i campioni di questo progetto, ed è una sola: finiscono in
`firmware/firmware.bin`, che viene **ridistribuito via OTA**, quindi la licenza deve
permettere di ridistribuirli. È lo stesso motivo per cui `tools/samples/` resta fuori da git
— vedi [`tools/samples/README.md`](tools/samples/README.md).

## Documentazione

| File | Cosa contiene |
|---|---|
| [`docs/ArcadeVox_Uso.pdf`](docs/ArcadeVox_Uso.pdf) | **Libretto d'uso 2.9**, 16 pagine A4: suonare, timbri, piano e batteria, ritmo, MIDI, schermate, aggiornamenti |
| [`docs/ArcadeVox_Montaggio.pdf`](docs/ArcadeVox_Montaggio.pdf) | **Libretto di montaggio 2.8**, 9 pagine A4: blueprint, connettori pin per pin, tarature, collaudo e diagnostica |
| `docs/ArcadeVox_Uso.html` · `docs/ArcadeVox_Montaggio.html` | Le sorgenti, da cui si rigenerano i due PDF |
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | Il cablaggio della scheda, ricavato dallo schematico e dalla netlist |
| [`docs/pannello.svg`](docs/pannello.svg) | Blueprint del pannello, da solo |
| [`PROGRESS.md`](PROGRESS.md) | Stato dei milestone e differenze rispetto al brief iniziale |
| [`CLAUDE_2.md`](CLAUDE_2.md) | Il brief di progetto originale (scheda 1.x) |

I libretti sono **due** perché servono in due momenti diversi: quello di montaggio si tiene
aperto accanto alla scheda mentre si salda e si cabla, e finito il lavoro non serve più;
quello d'uso si tiene accanto allo strumento e non parla mai di connettori. Tenerli insieme
voleva dire che chi voleva imparare a suonare cominciava da quattro pagine di pinout.

Entrambi sono già pronti da stampare. Se ritocchi un HTML rigeneri il suo PDF così:

```bash
"/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge" --headless --disable-gpu \
  --no-pdf-header-footer --run-all-compositor-stages-before-draw --virtual-time-budget=6000 \
  --print-to-pdf=docs/ArcadeVox_Uso.pdf "file://$PWD/docs/ArcadeVox_Uso.html"
```

Su Windows l'eseguibile sta in `C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe`
e vuole `--headless=new`: con il vecchio `--headless` il file non viene scritto e il comando
esce lo stesso senza dire niente.

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
  sequencer.*         16 passi, scrittura col cursore, record quantizzato, preconteggio
  fx_rows.*           l'elenco della schermata EFFETTI, condiviso fra logica e display
  samples.h/.cpp      i tredici suoni (samples.cpp e' generato, non si edita)
  display.*           GC9A01, interfaccia radiale: 7 schermate, corone, overlay, QR
  logo.h              wordmark della schermata di avvio (generato, non editare)
  storage.*           persistenza NVS con scrittura ritardata
  net_portal.*        access point, captive portal, OTA
  status_led.*        animazioni sul WS2812 di bordo

tools/
  make_logo.py        rigenera src/logo.h dal font Handel Gothic
  make_samples.py     rigenera src/samples.cpp: sintetizza i tredici suoni, oppure
                      prende i tuoi file da tools/samples/
  samples/            i tuoi campioni (fuori da git: vedi il README li' dentro)
  simdisplay/         renderizza le schermate sul computer e controlla il cerchio

firmware/
  manifest.json       la release pubblicata, che il synth va a cercare
  firmware.bin        l'immagine corrispondente
```
