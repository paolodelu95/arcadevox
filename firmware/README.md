# Release OTA

Quello che il synth va a cercare quando, dal portale, gli chiedi di **cercare gli
aggiornamenti da internet**. L'indirizzo di default del manifest sta in
[`src/version.h`](../src/version.h) e si può cambiare dal portale stesso.

| File | Cosa contiene |
|---|---|
| `manifest.json` | versione pubblicata, URL del binario e note della release |
| `firmware.bin` | l'immagine da flashare, presa da `.pio/build/esp32-s3-devkitc-1/` |

Il manifest è un JSON piatto di tre campi — il firmware lo legge con un
estrattore di stringhe, non con un parser vero, quindi niente annidamenti:

```json
{"version": "1.2.0", "url": "https://.../firmware.bin", "notes": "cosa cambia"}
```

L'aggiornamento parte solo se `version` è **maggiore** di quella installata, nel
confronto `maggiore.minore.patch` a tre byte.

## Pubblicare una release

1. alza `FW_VERSION` in [`src/version.h`](../src/version.h)
2. `pio run -e esp32-s3-devkitc-1`
3. `cp .pio/build/esp32-s3-devkitc-1/firmware.bin firmware/firmware.bin`
4. allinea `version` e `notes` dentro `manifest.json`
5. commit e push su `main`

## Il repository deve essere pubblico

`raw.githubusercontent.com` serve i file di un repo privato **solo con un token**,
e il synth fa una GET senza credenziali: finché `paolodelu95/arcadevox` resta
privato, *cerca aggiornamenti* fallirà con un errore di download.

Due strade:

- **rendere pubblico il repository**, e l'aggiornamento da internet funziona così
  com'è;
- **lasciarlo privato** e usare l'altra via, che non dipende da nessun hosting:
  dal portale si carica a mano il `firmware.bin` preso dal telefono. È la stessa
  immagine che sta qui.

In alternativa il manifest e il binario possono stare ovunque su HTTPS: basta
puntarci l'indirizzo dal portale.
