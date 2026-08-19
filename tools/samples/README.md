# I tuoi suoni

Qui dentro ci metti i **tuoi** file audio e diventano i tredici suoni della
schermata SUONI. Vale qualunque formato che il computer sappia leggere: `.wav`,
`.mp3`, `.m4a`, `.aiff`, `.ogg`.

```sh
python3 tools/make_sample_image.py   # legge questa cartella -> tools/suoni.bin
sh tools/upload_sounds.sh            # e lo scrive nella partizione dei suoni
```

I tuoi suoni **non entrano nel firmware**: vanno in una partizione di dati tutta
loro, a 0x670000. Sono due caricamenti separati, e da questo discendono le due
cose che contano:

- `firmware/firmware.bin` non contiene le tue registrazioni, quindi pubblicarlo
  non le pubblica — non e' piu' una cosa da ricordarsi, e' una cosa che non puo'
  succedere;
- un **aggiornamento OTA non te li cancella**: riscrive solo la partizione
  dell'applicazione e lascia stare la loro.

La vecchia strada — compilarli dentro con `make_samples.py` — c'e' ancora, ma
serve solo a rigenerare i tredici *sintetizzati* di riserva. Se ci passi i tuoi
file, `src/samples.cpp` si sporca di registrazioni e il gancio pre-commit ti
blocca il commit: e' voluto, ma ormai e' la strada lunga.

## Oppure: i tredici veri, scaricati da soli

Se quello che vuoi sono i tredici suoni "meme" originali al posto di quelli
sintetizzati, c'e' uno script che se li prende da internet e li mette qui
dentro con i nomi giusti:

```sh
sh tools/fetch_memes.sh              # scarica i tredici in questa cartella
python3 tools/make_sample_image.py   # li impacchetta in tools/suoni.bin
sh tools/upload_sounds.sh            # e li scrive nella partizione
```

Vale tutto quello che c'e' scritto qui sotto: i file restano sul tuo computer e
il firmware che ne esce sulla tua scheda. Lo script installa anche un gancio
`pre-commit` che rifiuta il commit di `src/samples.cpp` finche' ci sono dentro
delle registrazioni — perche' quel file, a differenza di questa cartella, e'
tracciato da git, ed e' l'unico modo per cui un `git commit -a` distratto non
pubblichi tredici registrazioni altrui.

Per tornare ai sintetizzati: `git restore src/samples.cpp`, oppure si svuota
questa cartella e si rilancia `make_samples.py`.

## Come si chiamano i file

Il numero davanti decide su quale tasto finiscono, da sinistra a destra; il
resto del nome e' quello che il display scrive quando lo premi.

```
tools/samples/
  01 trombetta.mp3
  02 faaa.wav
  03 boom.m4a
  ...
  13 tada.wav
```

Un `-` o un `_` nel nome diventa uno spazio nella descrizione:

```
04 bruh-niente da aggiungere.wav
   ^^^^ ^^^^ ^^^^^^^^^^^^^^^^^^^
   n.   nome descrizione
```

I posti che lasci vuoti restano quelli sintetizzati dallo script: puoi
sostituirne tre e tenere gli altri dieci.

## Questa cartella non finisce su GitHub

`.gitignore` la esclude, tranne questo file. E' voluto: il repository e'
pubblico, e i suoni che girano in rete sono quasi tutti registrazioni di
qualcuno. Tenerli sulla tua scheda e' un conto — pubblicarli in un repository
che chiunque puo' clonare e' un altro, e non e' una distinzione formale: e'
esattamente la differenza fra ascoltare un disco e stamparne delle copie.

Quindi i file restano sul tuo computer, il firmware che ne esce resta sulla tua
scheda, e il repository continua a contenere solo le formule che generano i
tredici suoni di serie.

> Attenzione a `firmware/firmware.bin`: quello **e'** pubblicato, e se ci
> compili dentro i tuoi campioni li stai pubblicando. Per tenerli privati,
> carica con `pio run -t upload` e non aggiornare il file in `firmware/`.

## Quanto possono durare

Un secondo costa 16 kB. Ce ne stanno in abbondanza — la partizione
dell'applicazione ne ha piu' di due megabyte liberi — ma oltre i **quattro
secondi** per suono lo script si ferma e te lo dice: e' il limite dell'indice di
lettura del motore audio, non una preferenza.

Lo script normalizza il volume, taglia il silenzio in testa e in coda, converte
a 16 kHz mono e sfuma i due estremi perche' non facciano click.
