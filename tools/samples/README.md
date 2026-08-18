# I tuoi suoni

Qui dentro ci metti i **tuoi** file audio e diventano i tredici suoni della
schermata SUONI. Vale qualunque formato che il computer sappia leggere: `.wav`,
`.mp3`, `.m4a`, `.aiff`, `.ogg`.

```sh
python3 tools/make_samples.py     # legge questa cartella e riscrive src/samples.cpp
pio run -t upload                 # e li carica sulla scheda
```

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
