// sample_store.cpp — vedi sample_store.h.

#include "sample_store.h"

#include <esp_partition.h>
#include <esp_spi_flash.h>
#include <string.h>

namespace SampleStore {
namespace {

// Intestazione dell'immagine, 32 byte. I campi sono little-endian come il
// processore, quindi si leggono con una struttura invece che byte per byte.
//
// `packed` non e' scaramanzia: senza, il compilatore e' libero di infilare
// riempimento fra i campi, e l'immagine la scrive uno script Python che di quel
// riempimento non sa niente.
struct __attribute__((packed)) Header {
    char magic[8];
    uint32_t count;
    uint32_t totalSize;
    uint8_t reserved[16];
};

// Una voce: quattro scostamenti dall'inizio dell'immagine. Scostamenti e non
// puntatori, perche' l'indirizzo a cui la flash viene mappata lo si scopre solo
// a runtime — e cambia.
struct __attribute__((packed)) Entry {
    uint32_t nameOff;
    uint32_t hintOff;
    uint32_t dataOff;
    uint32_t len;
};

// Tredici come i tasti. Se un domani l'immagine ne portasse di piu', i tasti
// restano tredici e il resto non si potrebbe comunque suonare.
constexpr uint8_t MAX_ENTRIES = MEME_MAX;

MemeSample entries[MAX_ENTRIES];
const void *mapped = nullptr;
// Il core Arduino qui e' il 2.0.x, cioe' IDF 4.4: la mappatura si chiama ancora
// col vocabolario di spi_flash — `spi_flash_mmap_handle_t` e `SPI_FLASH_MMAP_DATA`.
// Sull'IDF 5 quegli stessi nomi diventano `esp_partition_mmap_handle_t` e
// `SPI_FLASH_MMAP_DATA`; e' la stessa cosa con un altro nome, e questo file e'
// l'unico punto da ritoccare il giorno che platformio.ini si spinge sul core 3.x.
spi_flash_mmap_handle_t mapHandle = 0;
bool active = false;
uint32_t loadedSize = 0;

}  // namespace

void begin() {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (part == nullptr) {
        Serial.println(F("SUONI: nessuna partizione dati, uso i tredici sintetizzati."));
        return;
    }

    // Si legge prima la sola intestazione, e solo dopo si mappa. Mappare tutto e
    // poi guardare costerebbe 1,5 MB di finestra MMU per scoprire, nel caso
    // normale di partizione vuota, che non c'era niente da leggere.
    Header h;
    if (esp_partition_read(part, 0, &h, sizeof(h)) != ESP_OK) {
        Serial.println(F("SUONI: partizione illeggibile, uso i tredici sintetizzati."));
        return;
    }
    if (memcmp(h.magic, SAMPLE_IMAGE_MAGIC, 7) != 0) {
        // Il caso di gran lunga piu' comune: partizione vergine, tutta 0xFF.
        // Non e' un errore e non merita il tono di un errore.
        Serial.println(F("SUONI: nessuna immagine caricata, suonano i tredici sintetizzati."));
        return;
    }
    if (h.count == 0 || h.count > MAX_ENTRIES || h.totalSize < sizeof(Header) ||
        h.totalSize > part->size) {
        Serial.printf("SUONI: intestazione incoerente (%u suoni, %u byte), la ignoro.\n",
                      (unsigned)h.count, (unsigned)h.totalSize);
        return;
    }

    // Si mappa solo quello che l'immagine dichiara di occupare, non l'intera
    // partizione: la finestra di mappatura della flash e' una risorsa finita e
    // condivisa col codice, e prendersene 1,5 MB per usarne 400 kB sarebbe un
    // costo pagato per niente.
    if (esp_partition_mmap(part, 0, h.totalSize, SPI_FLASH_MMAP_DATA, &mapped,
                           &mapHandle) != ESP_OK) {
        Serial.println(F("SUONI: mappatura fallita, uso i tredici sintetizzati."));
        return;
    }

    const uint8_t *base = (const uint8_t *)mapped;
    const Entry *table = (const Entry *)(base + sizeof(Header));

    for (uint32_t i = 0; i < h.count; ++i) {
        const Entry &e = table[i];
        // Ogni scostamento va controllato prima di trasformarlo in puntatore.
        // Questa immagine arriva dalla flash, cioe' da fuori: un campo sbagliato
        // — per un caricamento interrotto a meta', o per un file troncato — qui
        // diventerebbe una lettura fuori dalla mappatura, che sull'ESP32 non e'
        // un valore strano, e' un riavvio.
        if (e.nameOff >= h.totalSize || e.hintOff >= h.totalSize ||
            e.dataOff >= h.totalSize || e.len == 0 ||
            e.dataOff + e.len > h.totalSize) {
            Serial.printf("SUONI: voce %u fuori dai limiti, torno ai sintetizzati.\n",
                          (unsigned)i);
            spi_flash_munmap(mapHandle);
            mapped = nullptr;
            return;
        }
        entries[i].name = (const char *)(base + e.nameOff);
        entries[i].hint = (const char *)(base + e.hintOff);
        entries[i].data = base + e.dataOff;
        entries[i].len = e.len;
    }

    MEME_SAMPLES = entries;
    MEME_COUNT = (uint8_t)h.count;
    active = true;
    loadedSize = h.totalSize;

    Serial.printf("SUONI: %u suoni dalla partizione dati (%u kB).\n", (unsigned)h.count,
                  (unsigned)(h.totalSize / 1024));
}

bool usingPartition() { return active; }
uint32_t imageSize() { return loadedSize; }

}  // namespace SampleStore
