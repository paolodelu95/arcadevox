// sample_store.h — i tredici suoni della schermata SUONI presi da una partizione
// a parte invece che dal firmware.
//
// IL PROBLEMA CHE RISOLVE
//
// I suoni "meme" veri sono registrazioni con un padrone. Compilarli dentro
// l'eseguibile li fa finire in firmware/firmware.bin, che questo progetto
// pubblica su GitHub e manda alle schede via OTA: a quel punto non sono piu' una
// copia sul proprio strumento, sono una ridistribuzione. La cartella
// tools/samples/ e il gancio pre-commit tenevano fuori i *file*; il binario li
// rimetteva dentro dalla porta di servizio.
//
// Qui i due mondi si separano davvero. Il firmware non contiene piu' le
// registrazioni: contiene i tredici suoni **sintetizzati** come riserva, e va a
// cercare i veri in una partizione di dati che sta da un'altra parte della flash.
// Sono due caricamenti diversi, con due comandi diversi, e nessuno dei due tocca
// cio' che fa l'altro.
//
// LA CONSEGUENZA PIU' UTILE
//
// Un aggiornamento OTA scrive **solo** la partizione dell'applicazione. La
// partizione dei suoni non viene sfiorata, quindi i suoni personali sopravvivono
// a un aggiornamento del firmware — e questo era il punto: aggiornare lo
// strumento non deve costare i propri suoni.
//
// DOVE, DI PRECISO
//
// Nella partizione `spiffs` di default_8MB.csv: 1,5 MB a 0x670000, che la tabella
// prevede da sempre e che nessuno usava. Non ne creo una nuova apposta, e la
// ragione e' seria: la tabella delle partizioni **non si aggiorna via OTA** — la
// riscrive solo un caricamento completo via esptool. Una scheda che si aggiorna
// da internet terrebbe la tabella vecchia e cercherebbe i suoni a un indirizzo
// che nel suo mondo non esiste.
//
// COME CI ARRIVA UN PUNTATORE
//
// `esp_partition_mmap` mappa la flash nello spazio d'indirizzamento e restituisce
// un puntatore normale. Il motore audio legge i campioni da un `const uint8_t *`
// e non ha modo di accorgersi che quella memoria e' flash esterna invece che
// un array `PROGMEM`: nessuna copia in RAM, nessun buffer, nessuna riga da
// cambiare in audio_engine.cpp.
#pragma once

#include <Arduino.h>

#include "samples.h"

namespace SampleStore {

// Da chiamare in setup(). Se la partizione contiene un'immagine valida,
// MEME_SAMPLES e MEME_COUNT passano a puntare li'; altrimenti restano i tredici
// sintetizzati che il firmware si porta dentro, e non succede niente di male.
void begin();

// True se stanno suonando i suoni della partizione. Serve alla diagnostica e
// alla riga sulla seriale: "sento un suono che non e' quello che mi aspettavo"
// e' una domanda che deve avere una risposta.
bool usingPartition();

// Quanti byte occupa l'immagine caricata, 0 se non ce n'e' una.
uint32_t imageSize();

// Firma dell'immagine, per lo script che la genera e per chi legge il codice.
// Otto byte esatti: sei di nome e due che dicono la versione del formato, cosi'
// un domani si puo' cambiare struttura senza far suonare rumore a chi ha ancora
// la vecchia immagine sulla scheda.
#define SAMPLE_IMAGE_MAGIC "AVSND1\0"

}  // namespace SampleStore
