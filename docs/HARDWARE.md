# ArcadeVox — la scheda (rev. 2026-08-17)

Cablaggio completo della scheda nuova, ricavato dallo schematico e dalla netlist del progetto
EasyEDA. Questo file è la fonte di verità del firmware: `src/pinout.h` non fa che riscriverne
il contenuto in forma di `#define`.

## In breve

| | |
|---|---|
| MCU | ESP32-S3-DevKitC-1 **N16R8** — 16 MB di flash, 8 MB di PSRAM ottale |
| Tasti | 20 × Cherry MX hot-swap con SK6812 integrato (`CHERRY_MX-HOTSWAP-RGB_SK6812`) |
| Matrice | 4 colonne × 5 righe su MCP23017, un 1N4148W per tasto |
| Encoder | 4 × EC11B152442D, pulsante d'albero compreso |
| Connettori | J_PWR (2p), J_AUDIO (5p), J_DISPLAY (7p), J_JOY (5p) |

> **La variante del modulo non è indifferente.** Il firmware configura la PSRAM in modo
> **ottale** (`board_build.arduino.memory_type = qio_opi`), che è ciò che la R8 vuole: su una
> **N8**, priva di PSRAM, quella riga **impedisce l'avvio**. Con la tabella `default_16MB.csv`
> le due partizioni applicative passano da 3,2 a 6,2 MB e quella dei suoni da 1,5 a 3,4 MB.
> Chi monta una N8 deve rimettere `qio_qspi` e una tabella da 8 MB.
| Luci | catena di 20 SK6812 a 5 V, dato via R1 = 330 Ω |

## GPIO dell'ESP32

| GPIO | Va a | Note |
|---|---|---|
| 1 | J_AUDIO pin 5 | terzo segnale I2S |
| 2 | J_JOY pin 1 | joystick SU |
| 4 | MCP23017 pin 13 | **I²C SDA** |
| 5 | MCP23017 pin 12 | **I²C SCL** |
| 6 / 7 | ENC1 A / B | pull-up esterni R2, R3 (10 kΩ verso 3V3) |
| 8 / 9 | ENC2 A / B | pull-up esterni R4, R5 |
| 10 / 11 | ENC3 A / B | pull-up esterni R6, R7 |
| 12 | R1 → catena SK6812 | dato dei LED sotto i tasti |
| 13 | J_DISPLAY pin 3 | SPI SCLK |
| 14 | J_DISPLAY pin 4 | SPI MOSI |
| 15 | J_DISPLAY pin 5 | display RST |
| 16 | J_DISPLAY pin 6 | display DC |
| 17 | J_DISPLAY pin 7 | display CS |
| 18 | J_AUDIO pin 3 | primo segnale I2S |
| 21 | J_AUDIO pin 4 | secondo segnale I2S |
| 39 | ENC4 B | **niente pull-up verso 3V3**, vedi sotto |
| 40 | ENC4 A | idem |
| 41 | J_JOY pin 2 | joystick GIÙ |
| 42 | J_JOY pin 3 | joystick SINISTRA |
| 47 | J_JOY pin 4 | joystick DESTRA |
| 48 | — | WS2812 di bordo della DevKitC-1 |

Liberi e non cablati: **3**, **46**. GPIO 19/20 sono l'USB nativo, 43/42 sono TX/RX del
connettore ma restano scollegati.

## MCP23017 (U1)

Indirizzo **0x20** (A0/A1/A2 a massa), `RESET#` legato a 3V3.

| Piedino | Porta | Funzione |
|---|---|---|
| 1–4 | GPB0–GPB3 | **COL0–COL3**, uscite |
| 5–8 | GPB4–GPB7 | pulsanti di ENC1–ENC4, ingressi con pull-up |
| 21–25 | GPA0–GPA4 | **ROW0–ROW4**, ingressi con pull-up |
| 26–28 | GPA5–GPA7 | non cablati |

### Verso della scansione — non è arbitrario

Ogni tasto ha il suo 1N4148W, e sullo schematico il **catodo sta dal lato del tasto**, l'anodo
dal lato della riga. La corrente può quindi scorrere solo

```
ROW (anodo) → diodo → tasto → COL
```

per cui l'unica scansione possibile è: **colonne in uscita, una alla volta a livello basso;
righe in ingresso con pull-up; tasto premuto = riga bassa.** Invertendo i ruoli i diodi
bloccherebbero e non si leggerebbe niente.

## Matrice dei tasti

| | COL0 | COL1 | COL2 | COL3 |
|---|---|---|---|---|
| **ROW0** | SW_DO | SW_DOs | SW_RE | SW_REs |
| **ROW1** | SW_MI | SW_FA | SW_FAs | SW_SOL |
| **ROW2** | SW_SOLs | SW_LA | SW_LAs | SW_SI |
| **ROW3** | SW_DO2 | SW_FN1 | SW_FN2 | SW_FN3 |
| **ROW4** | SW_FN4 | SW_FN5 | SW_FN6 | SW_FN7 |

Nel firmware l'indice di matrice è `riga * 4 + colonna`; le 13 note occupano gli indici 0–12 e
le 7 funzioni 13–19 (vedi `MATRIX_NOTE_SLOTS` e `MATRIX_FN_SLOTS` in `pinout.h`).

Sul pannello la disposizione fisica è quella di un pianoforte: naturali in basso, alterazioni
nella fila di mezzo (con il salto fra RE# e FA#, dove sul PCB c'è il logo), funzioni in alto.

## Connettori

### J_PWR — 2 poli
| Pin | Segnale |
|---|---|
| 1 | +5 V (`5V_LED`, alimenta anche il pin 5V della DevKitC-1) |
| 2 | GND |

Venti SK6812 a piena luminosità chiedono da soli ~1,2 A: alimentatore da 5 V / 2 A.

### J_AUDIO — 5 poli, verso il MAX98357
| Pin | Segnale | Filo consigliato |
|---|---|---|
| 1 | 3V3 | VIN |
| 2 | GND | GND |
| 3 | GPIO18 | LRC |
| 4 | GPIO21 | BCLK |
| 5 | GPIO1 | DIN |

**Lo schematico non dice quale dei tre segnali sia quale.** Il connettore porta 3V3, GND e tre
segnali numerati; il nome glielo dà il cavo. Il firmware parte con l'ordine qui sopra (quello
serigrafato sui moduli MAX98357 più comuni) e permette di provare le altre cinque permutazioni
a caldo dalla voce **AUDIO → USCITA** del menu impostazioni, senza ricompilare
(`AudioEngine::setPinOrder`).

### J_DISPLAY — 7 poli, verso il GC9A01
| Pin | Segnale | Filo |
|---|---|---|
| 1 | 3V3 | VCC |
| 2 | GND | GND |
| 3 | GPIO13 | SCL / SCK |
| 4 | GPIO14 | SDA / MOSI |
| 5 | GPIO15 | RES |
| 6 | GPIO16 | DC |
| 7 | GPIO17 | CS |

Il `BLK` del modulo va ponticellato al VCC: il connettore ha sette poli e la retroilluminazione
resta sempre accesa.

### J_JOY — 5 poli, verso i microswitch
| Pin | Segnale | Direzione |
|---|---|---|
| 1 | GPIO2 | SU |
| 2 | GPIO41 | GIÙ |
| 3 | GPIO42 | SINISTRA |
| 4 | GPIO47 | DESTRA |
| 5 | GND | comune |

Quattro contatti indipendenti verso massa, letti col pull-up interno: nessuna lettura
analogica.

## Tre cose da sapere sullo schematico

0. **L'I2C non ha resistenze di pull-up.** Le reti SDA e SCL hanno due soli membri ciascuna —
   il piedino dell'ESP32 e quello dell'MCP23017 — e sul 3,3 V non c'è nessun resistore che le
   tiri su. Il bus vive quindi sui pull-up interni dell'ESP32, che stanno sui 45 kΩ: deboli
   per i 400 kHz, specie con qualche centimetro di pista e uno zoccolo DIP di mezzo.

   Il firmware non dà la colpa a nessuno: se a 400 kHz l'espansore non risponde riprova
   subito a 100 kHz, e poi continua ad alternare le due velocità una volta al secondo. Sulla
   seriale scrive a quale ha funzionato. **Se scrive 100 kHz, la cura vera sono due
   resistenze da 4,7 kΩ fra SDA e 3,3 V e fra SCL e 3,3 V**: si possono aggiungere sul
   retro, fra i piedini 12/13 e il 9 dell'MCP23017.



1. **ENC4 non ha i pull-up.** Su ENC1–ENC3 le resistenze da 10 kΩ (R2–R7) vanno da A e B verso
   3,3 V, come si deve. Su ENC4, invece, R8 e R9 sono finite **in serie** sulle due linee, e
   non c'è nessun ramo verso 3,3 V. Funziona lo stesso — il pull-up interno da ~45 kΩ vince
   sui 10 kΩ in serie e il livello basso resta valido — ma ENC4 ha meno margine di rumore
   degli altri tre. Se un giorno facesse scatti fantasma, è lì che si guarda.

2. **La catena dei LED è nota solo in testa.** Il primo LED della catena è quello del DO (è lì
   che entra la `LEDDIN` dal GPIO12); da lì in poi il collegamento fra DOUT e DIN passa da
   etichette di rete e non è ricostruibile dal disegno. Per questo il firmware non lo indovina:
   la procedura **SETTINGS → LUCI → IMPARA LUCI** accende un LED alla volta, aspetta la
   pressione del tasto corrispondente e salva la mappa in NVS.

## Distinta (dal BOM del progetto)

| Q.tà | Componente | Sigla |
|---|---|---|
| 1 | ESP32-S3-DEVKITC-1-**N16R8** | U0 |
| 1 | MCP23017-E/SP (DIP-28) | U1 |
| 20 | Cherry MX hot-swap con SK6812 | SW_* |
| 20 | 1N4148W (SOD-123) | D1–D20 |
| 4 | EC11B152442D-STEC11B03 | ENC1–ENC4 |
| 8 | 10 kΩ (1206) | R2–R9 |
| 1 | 330 Ω (1206) | R1 |
| 21 | 33 nF (0805) | C1, C3–C22 |
| 4 | 100 µF | C2, C222, C223, C224 |
| 1 | HX 2.54-2P WT | J_PWR |
| 1 | HX 2.54-5P WTDK | J_AUDIO |
| 1 | HX 2.54-7P WT | J_DISPLAY |
| 1 | HX 2.54-5P WT | J_JOY |
