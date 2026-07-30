# Simulatore del display

Rende le schermate del synth sul computer, come PNG 240x240, e misura se stanno
dentro il cerchio.

```sh
tools/simdisplay/build.sh   # compila, rende 50 scene, converte in PNG
python3 tools/simdisplay/check.py
```

## Perche' esiste

Il GC9A01 e' **tondo**. Il framebuffer invece e' un quadrato 240x240, e i quattro
angoli non esistono: quello che ci finisce dentro e' scritto, occupa flash, viene
inviato al pannello e nessuno lo vedra' mai. Guardando il codice non si distingue
un contenuto legittimo da uno che cade fuori dal vetro — servono i pixel.

E c'e' un difetto che a occhio non si trova affatto: le fasce di pulizia. Il
display ridisegna la cornice solo al cambio schermata, poi tocca solo le strisce
dei valori che cambiano. Una striscia larga quanto il quadrato, a una quota dove
il cerchio si e' gia' stretto, si mangia due morsi di cornice — e la cornice non
si ridisegna, quindi il buco resta finche' non si cambia schermata. Al primo
disegno il synth e' perfetto: si sbriciola **usandolo**. Per questo meta' delle
scene sono `*-dopo-uso`: disegno completo, poi una manciata di `Display::update()`
con valori diversi, e solo allora si scatta la fotografia.

## Come e' fatto

`sim_main.cpp` include `../../src/display.cpp`, **il file vero**. Non una copia,
non una versione adattata: se per compilare servisse cambiare il firmware, tutto
questo non proverebbe niente. Quando qualcosa non compila si aggiusta lo stub.

| | |
|---|---|
| `stub/Arduino.h` | il minimo indispensabile: String, millis, PROGMEM, Serial muto |
| `stub/Arduino_GFX_Library.h` | un Arduino_GFX che rasterizza davvero in un framebuffer RGB565 |
| `vendor/glcdfont.h` | il font 5x7 autentico, copiato da Arduino_GFX |
| `vendor/qrcode.[ch]` | la libreria QR vera, copiata dai libdeps |
| `sim_fakes.*` | AudioEngine, NetPortal, Sequencer finti — ma con le stringhe vere |
| `sim_main.cpp` | le 50 scene: ogni schermata nei suoi stati estremi |

`src/settings.cpp` viene compilato per davvero, non imitato: sono le etichette e
i `valueLabel()` reali a decidere se una riga esce dal cerchio, e una tabella
riscritta a mano mentirebbe alla prima divergenza.

## Cosa controlla `check.py`

- pixel accesi oltre r=119.5 dal centro: **fuori dal vetro**, invisibili
- pixel fra r=116 e 119.5 che non appartengono alla cornice: **le stanno addosso**
- **integrita' dell'anello**, grado per grado: elenca gli archi interrotti, che e'
  la firma di una fascia di pulizia che ha mangiato la cornice

Sulle schermate senza cornice — QR e schermata di avvio — il terzo controllo lo
salta e lo dichiara, invece di farle passare per pulite.

I mille "problemi" residui sullo splash sono la griglia in fuga che arriva agli
angoli: quelli sono **voluti**, e' l'effetto oblo'.
