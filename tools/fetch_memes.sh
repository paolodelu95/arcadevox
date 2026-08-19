#!/bin/sh
# fetch_memes.sh — scarica i tredici suoni veri della schermata SUONI.
#
# I tredici suoni di serie sono *sintetizzati*: formule dentro
# tools/make_samples.py che imitano una trombetta da stadio, un urlo con l'eco,
# un colpo grave. Si riconoscono, ma nessuno li scambia per l'originale — ed e'
# voluto, perche' quelli originali non si possono mettere in un repository
# pubblico. Questo script serve a chi vuole gli originali *sulla propria
# scheda*, che e' un'altra cosa.
#
#   sh tools/fetch_memes.sh              # scarica in tools/samples/
#   python3 tools/make_sample_image.py   # li impacchetta in tools/suoni.bin
#   sh tools/upload_sounds.sh            # e li scrive nella partizione dei suoni
#
# La riga di mezzo non e' opzionale: lo script mette dei file in una cartella, e'
# make_samples.py a trasformarli nei tredici blob del firmware. Chi salta un
# passo si ritrova la scheda con i suoni di prima e nessun messaggio d'errore,
# perche' non c'e' stato nessun errore.
#
# ---------------------------------------------------------------------------
# Dove finiscono, e perche' non su GitHub
#
# In tools/samples/, che .gitignore esclude per intero tranne il suo README. Non
# e' una precauzione formale: quei tredici file sono registrazioni con un
# padrone, e la differenza fra tenerne una copia sul proprio strumento e
# pubblicarla in un repository che chiunque puo' clonare e' la stessa che passa
# fra ascoltare un disco e stamparne delle copie.
#
# Vale anche per firmware/firmware.bin, che **e'** pubblicato: se ci si compila
# dentro questi suoni li si sta ridistribuendo lo stesso, solo dentro un binario
# invece che dentro una cartella. Per tenerli sulla propria scheda e basta si
# carica con `pio run -t upload` e non si aggiorna il file in firmware/.
#
# I campioni del piano (tools/fetch_piano.sh) sono l'eccezione che spiega la
# regola: la University of Iowa li pubblica dichiarandoli usabili senza
# restrizioni, ed e' esattamente per questo che il piano puo' stare nel binario
# che viene ridistribuito via OTA e questi tredici no.
#
# ---------------------------------------------------------------------------
# Cambiare un suono
#
# La tabella qui sotto e' `nome del file|indirizzo`, una riga per tasto. Il
# numero davanti al nome decide su quale tasto finisce, il resto e' cio' che il
# display scrive quando lo premi: "nome-descrizione", col primo trattino a
# separare i due. Per sostituirne uno si cambia l'indirizzo e si cancella il
# file gia' scaricato — oppure lo si ignora del tutto e si mette a mano il
# proprio file in tools/samples/, che e' cio' che il README di quella cartella
# racconta.
#
# Gli indirizzi sono di myinstants.com, che e' il posto in cui questi suoni
# stanno da anni. Se uno sparisce, curl lo dice e gli altri dodici arrivano
# lo stesso: cercare il nome su quel sito e incollare il nuovo indirizzo qui
# sotto e' tutto quello che serve.
set -e

DIR="$(dirname "$0")/samples"
mkdir -p "$DIR"

ok=0
ko=0

# Il separatore e' la barra verticale e non lo spazio: i nomi dei file *hanno*
# gli spazi dentro, ed e' cosi' che make_samples.py distingue il numero dal nome.
while IFS='|' read -r name url; do
    # Righe vuote e commenti dentro il blocco: si saltano.
    case "$name" in ''|'#'*) continue ;; esac

    dest="$DIR/$name"
    if [ -s "$dest" ]; then
        echo "  = $name"
        ok=$((ok + 1))
        continue
    fi

    # --fail perche' senza, curl salva allegramente una pagina d'errore HTML
    # dentro un file .mp3, e il messaggio che ne esce arriva molto piu' tardi e
    # da un'altra parte: e' make_samples.py che si lamenta di un formato che non
    # riconosce, su un file che sembra scaricato bene.
    if curl -sSL --fail --max-time 120 --retry 2 --retry-delay 1 \
            -A "Mozilla/5.0" -o "$dest" "$url"; then
        echo "  + $name"
        ok=$((ok + 1))
    else
        rm -f "$dest"
        echo "  ! $name — non scaricato, resta quello sintetizzato"
        ko=$((ko + 1))
    fi
done <<'SOUNDS'
01 trombetta-da stadio.mp3|https://www.myinstants.com/media/sounds/mlg-airhorn.mp3
02 faaa-con l'eco.mp3|https://www.myinstants.com/media/sounds/fahhhhhhhhhhhhhh.mp3
03 boom-il colpo grave.mp3|https://www.myinstants.com/media/sounds/vine-boom.mp3
04 bruh-niente da aggiungere.mp3|https://www.myinstants.com/media/sounds/bruh-sound-effect_WstdzdM.mp3
05 trombone-che tristezza.mp3|https://www.myinstants.com/media/sounds/sadtrombone.swf.mp3
06 risata-quattro sillabe.mp3|https://www.myinstants.com/media/sounds/comic003.mp3
07 applauso-bravo.mp3|https://www.myinstants.com/media/sounds/applause-sound-effect.mp3
08 rullo-ba dum tss.mp3|https://www.myinstants.com/media/sounds/badumtss.swf.mp3
09 moneta-raccolta.mp3|https://www.myinstants.com/media/sounds/super-mario-coin-sound.mp3
10 laser-pew.mp3|https://www.myinstants.com/media/sounds/laser-pew-pew-pew.mp3
11 errore-risposta sbagliata.mp3|https://www.myinstants.com/media/sounds/erro.mp3
12 scratch-ferma tutto.mp3|https://www.myinstants.com/media/sounds/record-scratch.mp3
13 tada-e' andata bene.mp3|https://www.myinstants.com/media/sounds/tada.swf.mp3
SOUNDS

# ---------------------------------------------------------------------------
# Il gancio che impedisce di pubblicarli per sbaglio
#
# src/samples.cpp e' un file **tracciato**: appena make_samples.py ci scrive
# dentro i suoni veri, un `git commit -a` distratto li mette su GitHub, ed e'
# esattamente la cosa che questo script serve a evitare. Un avviso nei commenti
# non basta — gli avvisi si leggono la prima volta e poi mai piu' — quindi qui
# si installa un gancio pre-commit che rifiuta il commit finche' quel file porta
# in testa il marcatore che make_samples.py ci scrive.
#
# I ganci git non stanno nel repository, quindi vanno installati: e' il motivo
# per cui lo fa questo script e non un file committato. Se ce n'e' gia' uno
# suo, non lo si tocca: si dice cosa aggiungerci e si va avanti.
HOOKS="$(git rev-parse --git-path hooks 2>/dev/null || true)"
if [ -n "$HOOKS" ]; then
    mkdir -p "$HOOKS"
    if [ ! -e "$HOOKS/pre-commit" ]; then
        cat > "$HOOKS/pre-commit" <<'HOOK'
#!/bin/sh
# Installato da tools/fetch_memes.sh.
#
# Ferma il commit di src/samples.cpp quando dentro ci sono i suoni veri invece
# dei tredici sintetizzati. Il marcatore lo scrive tools/make_samples.py quando
# trova dei file in tools/samples/.
if git diff --cached --name-only | grep -qx "src/samples.cpp"; then
    if git show :src/samples.cpp | head -20 | grep -q "CAMPIONI-PERSONALI"; then
        echo "src/samples.cpp contiene registrazioni vere: commit rifiutato." >&2
        echo "Quei suoni hanno un padrone e questo repository e' pubblico." >&2
        echo "" >&2
        echo "  git restore --staged --worktree src/samples.cpp   # torna ai sintetizzati" >&2
        echo "" >&2
        echo "I file scaricati restano in tools/samples/, che git ignora: per" >&2
        echo "rimetterli sulla scheda bastano make_samples.py e un upload." >&2
        exit 1
    fi
fi
HOOK
        chmod +x "$HOOKS/pre-commit"
        echo "gancio pre-commit installato: src/samples.cpp coi suoni veri non si committa"
    elif grep -q "CAMPIONI-PERSONALI" "$HOOKS/pre-commit"; then
        echo "gancio pre-commit gia' a posto"
    else
        echo "! c'e' gia' un gancio pre-commit tuo: non lo tocco."
        echo "  Per la stessa protezione aggiungici il controllo su CAMPIONI-PERSONALI"
        echo "  in testa a src/samples.cpp (vedi tools/fetch_memes.sh)."
    fi
fi

echo "--"
echo "$ok suoni in $DIR${ko:+, $ko non riusciti}"
echo "Adesso: python3 tools/make_sample_image.py && sh tools/upload_sounds.sh"
echo "Finiscono in una partizione a parte: il firmware non li contiene e l'OTA non li cancella."
