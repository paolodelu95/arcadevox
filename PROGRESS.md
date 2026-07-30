# ArcadeVox — PROGRESS

Sintetizzatore monofonico ESP32-S3 N16R8 · pannello arcade riciclato · MAX98357 (I2S) · GC9A01 (SPI)

Legenda: `[ ]` da fare · `[~]` in corso · `[x]` fatto (codice scritto) · `[T]` testato su hardware

---

## M0 — Setup progetto e pinout
- [x] `platformio.ini` per `esp32-s3-devkitc-1`, PSRAM ottale (`board_build.psram = opi`)
- [x] `lib_deps`: `GFX Library for Arduino` (moononournation), `QRCode` (ricmoo)
- [x] Partizioni `default_16MB.csv`: contiene già `otadata` + `app0`/`app1` da 6,25 MB
      ciascuna, quindi l'OTA funziona **senza cambiare tabella e senza riflashare**
- [x] `src/pinout.h` — tutte le `#define` dei GPIO centralizzate
- [x] Scheletro file: `audio_engine`, `input_handler`, `sequencer`, `display`, `main`
- [ ] **[T]** Verifica boot + seriale + nessun conflitto sui pin strapping (0, 45, 46)

## M1 — Lettura input
- [x] Debounce generico (~15 ms) su tutti i pulsanti digitali, `INPUT_PULLUP` interno
- [x] 7 tasti nota (DO–SI) con **last-note-priority** + press-order (per l'arpeggiator)
- [x] L'ottavo pulsante (GPIO 13, ex DO') è il **selettore MONO / POLIFONICO**
- [x] Joystick 4 microswitch letti come 4 pulsanti separati (+ auto-repeat per l'edit mode)
- [x] Pulsanti funzione: scorri display, REC, PLAY/STOP, HOLD, leva ARP, leva BPM
- [x] HOLD: distinzione press breve (<600 ms) / long-press (>600 ms)
- [x] 2 encoder rotativi in quadratura, decoder full-step a interrupt (Enc1 4/5, Enc2 43/44)
- [ ] **[T]** Verifica su hardware di ogni contatto e del senso di rotazione degli encoder

## M2 — Motore audio (core 0)
- [x] Task FreeRTOS dedicato pinnato su core 0, priorità alta
- [x] Uscita I2S 44100 Hz / 16 bit verso MAX98357 (`driver/i2s.h`)
- [x] Oscillatore a phase-accumulator: sine, square, saw, triangle
- [x] Filtro passa-basso one-pole IIR, cutoff esponenziale ~80 Hz–8 kHz (Pot 1)
- [x] Volume generale (Pot 2), con smoothing anti-zipper
- [x] Mappatura note DO..SI + ottava `-2..+2` (`freq * 2^ottava`)
- [ ] **[T]** Verifica assenza di glitch/underrun con display e sequencer attivi

## M2b — Polifonia commutabile
Il brief escludeva la polifonia; l'ottavo pulsante è stato riassegnato per abilitarla.

- [x] **Pool di 8 voci**, ognuna con fase, inviluppo e filtro propri: una nota nuova non
      eredita lo stato di quella precedente
- [x] Identificativi di voce **dedicati** (7 tasti + 1 sequencer = esattamente MAX_VOICES):
      niente allocazione dinamica, niente voice stealing, comportamento sempre prevedibile
- [x] **Coda FreeRTOS** per gli eventi di nota fra i due core: un evento tocca più campi
      della stessa voce, quindi le `volatile` atomiche usate per i parametri non bastano.
      Il task audio la svuota a inizio blocco, latenza max ~2,9 ms
- [x] Incrementi d'inviluppo calcolati **una volta per blocco** invece che per campione per
      voce: sono comuni a tutte
- [x] Compensazione d'ampiezza sull'**energia** (somma degli inviluppi), non sul numero di
      voci: una nota in coda di rilascio non abbassa quelle che stanno ancora suonando
- [x] MONO e POLI non sono due percorsi separati: entrambi dichiarano *cosa deve suonare*, e
      un unico blocco in `main.cpp` ne ricava la sequenza minima di eventi
- [x] In POLI la sequenza suona **sotto** le dita invece di essere zittita, e HOLD tiene
      l'accordo, arricchendolo con ogni tasto premuto dopo
- [x] L'arpeggiator resta monofonico anche in POLI: è il suo senso
- [x] Modalità salvata in NVS; i pattern salvati da un firmware con 8 note vengono
      sanificati al caricamento (indici fuori scala → pausa)
- [ ] **[T]** Verifica del carico CPU con 8 voci attive e display in refresh
- [ ] **[T]** Verifica a orecchio della compensazione d'ampiezza sugli accordi

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
- [x] Griglia fissa a 16 step, durata `60000 / bpm / 4` ms (sedicesimi), ricalcolata subito
- [x] Priorità alla nota live durante il PLAY
- [x] 8 preset BPM (40…180) ciclati dalla leva GPIO 0
- [x] Durata step ancorata al tick precedente: nessuna deriva accumulata
- [ ] **[T]** Verifica tenuta del tempo e del loop

## M5b — Sequencer usabile (revisione)
La prima versione era ingiocabile: 16 step a 120 BPM sono **2 secondi in tutto**, e il
record campionava solo ciò che era premuto nell'*istante esatto* del tick. Rifatto sul
modello dei sequencer veri.

- [x] Modello dati per step: `{nota, ottava}`, con pausa e **legato** come valori speciali
- [x] Ottava memorizzata per singolo step, in modo **assoluto**: ciò che si scrive è ciò
      che si risente (vedi "Variazioni" in fondo per il perché non è relativa)
- [x] **STEP EDIT** (long-press REC): scrittura passo-passo a sequenza ferma *o in corso*,
      trasporto ed editor sono assi ortogonali
- [x] Step input alla MPC: il tasto nota scrive e **fa avanzare il cursore** da solo
- [x] Cursore: joystick Sx/Dx o encoder 1 (scorrimento veloce); encoder 2 → BPM continuo 40–240
- [x] HOLD breve in step edit → svuota lo step; leva ARP → scrive un legato
- [x] **Record quantizzato** al sedicesimo *più vicino*: tolleranza ±½ step (±62 ms a 120 BPM)
      contro gli ~0 ms di prima
- [x] Registrazione basata sugli **attacchi** dei tasti (coda note-on in `input_handler`),
      non sulla nota risultante dalla priorità
- [x] **Overdub in loop**: il pattern gira all'infinito, ogni passata aggiunge senza cancellare
- [x] **Count-in di 1 battuta** col metronomo, durante il quale il pattern esistente suona già
- [x] **Erase al volo**: HOLD tenuto durante il REC svuota gli step che passano sotto la testina
- [x] Registrazione dell'arpeggiator: i suoi passi entrano nel pattern come le note suonate
- [ ] **[T]** Verifica di step edit, quantizzazione, overdub ed erase su hardware

## M5c — Metronomo
- [x] Voce di click indipendente nel motore audio, sommata **dopo** filtro e inviluppo:
      non ruba la voce monofonica, si sente anche mentre una nota è in corso
- [x] Sinusoide a decadimento esponenziale ~25 ms, 2000 Hz sull'accento / 1400 Hz sui battiti
- [x] Nessun comando da imparare: attivo in count-in e record, muto in play
- [ ] **[T]** Verifica del livello del click rispetto al synth

## M6 — Display GC9A01
- [x] Init bus SPI + `Arduino_GC9A01` 240x240 IPS
- [x] Schermata 1: forma d'onda (nome + icona disegnata con primitive, non bitmap)
- [x] Schermata 2: ottava (numero grande con segno)
- [x] Schermata 3: barre orizzontali cutoff / volume in tempo reale
- [x] Schermata 4: sequencer — **griglia con il contenuto del pattern**, non solo la testina:
      cella colorata per ottava con l'iniziale della nota, barretta per i legati, cornice
      per testina e cursore. Più stato, BPM, HOLD, ARP
- [x] Schermata 5: NETWORK (anticamera dell'aggiornamento via WiFi)
- [x] Schermata ADSR dedicata, attivata automaticamente in edit mode (bypassa il ciclo)
- [x] Schermata di preconteggio a numeroni; step edit e record scavalcano il ciclo, così
      durante la scrittura si guarda sempre la griglia
- [x] Redraw parziale (solo i valori cambiati) per evitare flicker; la griglia si ridisegna
      per intero solo a pattern cambiato, altrimenti solo le celle toccate da testina e cursore
- [ ] **[T]** Verifica leggibilità dentro l'area circolare e frame rate

## M6c — Persistenza
- [x] `storage.*` su NVS (`Preferences`): pattern, BPM, forma d'onda, ottava, cutoff,
      volume, ADSR e credenziali WiFi in un unico blob
- [x] Scrittura ritardata di 3 s dall'ultima modifica: girando un encoder si generano
      centinaia di variazioni al secondo, sulla flash ne arriva una
- [ ] **[T]** Verifica che pattern e parametri tornino dopo lo spegnimento

## M8 — Modalità NETWORK: QR e aggiornamento OTA
- [x] Quinta schermata + long-press (1 s) del pulsante SCORRI DISPLAY per accendere la radio:
      due gesti deliberati, il WiFi non può partire per sbaglio
- [x] **Modalità esclusiva**: `AudioEngine::shutdown()` ferma il task e libera l'I2S prima di
      alzare la radio. Lo stack WiFi gira sul core 0, lo stesso dell'audio, con priorità 23
      contro 10: convivendo farebbe sottoscorrere il DMA. Si esce con PLAY → riavvio
- [x] SoftAP WPA2 `ArcadeVox-XXXX`, password derivata dal MAC (stabile fra i riavvii,
      quindi stampabile sul display, ma non indovinabile)
- [x] **QR sul display** (`ricmoo/QRCode`, versione 3, 4 px per modulo): prima il payload
      `WIFI:` che aggancia lo smartphone senza digitare nulla, poi — appena un client si
      connette — l'indirizzo del portale, come scorciatoia se il captive portal non compare
- [x] DNS con wildcard su 192.168.4.1 + sonde di rilevazione: iOS e Android aprono il portale da soli
- [x] `POST /update`: upload del `.bin` **dal telefono**, nessuna rete esterna necessaria
- [x] `POST /wifi`: credenziali di casa salvate in NVS, connessione in STA (l'AP resta su)
- [x] `GET /check`: manifest JSON, confronto di versione, `httpUpdate` da internet
- [x] Barra di avanzamento disegnata dal callback di `Update`, che tiene occupato il loop
- [x] HTTP basic auth su upload e credenziali, con la password mostrata sul display
- [ ] **[T]** Verifica scansione del QR, captive portal su iOS e Android, upload e reboot
- [ ] **[T]** Verifica che il motore audio riparta pulito dopo un ciclo NETWORK + riavvio

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

### Ottava per step: assoluta, non relativa
Il progetto iniziale prevedeva di memorizzare l'ottava di ogni step come *scostamento*
dall'ottava globale, così che il joystick Su/Giù trasponesse l'intero pattern. Non regge:
il joystick è **un solo controllo** che dovrebbe fare due mestieri incompatibili, cioè
scegliere l'ottava dello step che stai scrivendo *e* trasporre tutto quanto. Con un solo
riferimento lo scostamento risulta sempre zero, e la trasposizione rende falsa la nota
appena scritta rispetto a quella che hai appena sentito.

Scelta finale: l'ottava dello step è **assoluta**. Ciò che scrivi è ciò che risenti, adesso
e fra un'ora. Il joystick Su/Giù resta l'ottava della tastiera dal vivo e, in STEP EDIT,
sceglie il registro dello step successivo. Una trasposizione globale del pattern resta
possibile in futuro, ma va su un comando suo, non su quello dell'ottava.

### Pulsanti a doppia funzione
Il pannello è saturo: restano liberi solo GPIO 19 e 20, e solo rinunciando alla seriale USB.
Le funzioni nuove sono quindi entrate su pressioni lunghe, con la stessa meccanica già usata
da HOLD, estratta in un helper riusabile (`PressTracker` in `input_handler.cpp`):

| Pulsante | Breve | Lunga |
|---|---|---|
| HOLD | latch della nota (o svuota lo step in STEP EDIT) | ADSR EDIT (600 ms) |
| REC | avvia/ferma la registrazione | STEP EDIT (600 ms) |
| SCORRI DISPLAY | schermata successiva | modalità NETWORK (1 s, solo dalla schermata NETWORK) |

Conseguenza: gli short-press ora scattano al **rilascio** e non alla pressione. Impercettibile
su un tocco rapido, e per il REC del tutto irrilevante — la registrazione parte comunque
dopo una battuta di preconteggio.

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
- Nessuna matrice MAX7219, nessun joystick analogico: fuori specifica.
- **La polifonia invece è entrata** (vedi M2b), a costo dell'ottavo tasto nota: il DO acuto
  non ha più un pulsante suo e si raggiunge con il joystick dell'ottava. Scala DO–SI.
