// Arduino.h finto — il minimo che serve a far compilare src/display.cpp su un Mac.
//
// Non e' un emulatore di Arduino: e' una lista di promesse. display.cpp usa
// pochissimo dell'ambiente embedded (i tipi interi, millis(), delay(), le macro
// di PROGMEM) e qui ci sono solo quelle. Ogni cosa in piu' sarebbe codice che
// nessuno esegue e che un giorno divergerebbe in silenzio dal vero.
#pragma once

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// --------------------------------------------------------------------- tipi
// Sull'ESP32 `byte` e `boolean` arrivano da Arduino.h. Qui non li usa nessuno,
// ma costano una riga e tolgono una sorpresa a chi domani stubba un altro file.
typedef uint8_t byte;
typedef bool boolean;

// --------------------------------------------------------------- flash / PROGMEM
//
// Sull'ESP32 la flash e' mappata in memoria e PROGMEM e' gia' quasi un no-op;
// qui lo e' del tutto. pgm_read_byte diventa una lettura normale: la maschera
// del logo e la tabella del font si leggono esattamente come sul chip, quindi il
// disegno che ne esce e' lo stesso.
#define PROGMEM
#define PSTR(s) (s)
#define F(s) (s)
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#define pgm_read_word(addr) (*(const uint16_t *)(addr))
#define pgm_read_dword(addr) (*(const uint32_t *)(addr))
#define pgm_read_pointer(addr) (*(void *const *)(addr))

// -------------------------------------------------------------------- tempo
//
// Il tempo e' finto e lo muove il simulatore: simAdvanceMillis() e' l'unica via
// per farlo scorrere. Cosi' le schermate che dipendono dall'orologio (il picco
// del VU, che resta appeso 600 ms) sono riproducibili al millisecondo, e un PNG
// rigenerato domani e' identico a quello di oggi.
extern uint32_t simMillisNow;
void simAdvanceMillis(uint32_t ms);

inline uint32_t millis() { return simMillisNow; }
inline uint32_t micros() { return simMillisNow * 1000u; }

// delay() nell'animazione di avvio serve solo a dare il ritmo: qui non c'e'
// nessuno che guarda, quindi il tempo passa e basta. Il disegno che ne risulta
// e' l'ultimo fotogramma dell'animazione, che e' esattamente cio' che vogliamo
// fotografare.
inline void delay(uint32_t ms) { simAdvanceMillis(ms); }
inline void delayMicroseconds(uint32_t us) { simAdvanceMillis(us / 1000u); }
inline void yield() {}

// -------------------------------------------------------------------- numeri
// Sull'ESP32 random() viene da newlib. Il seme e' fisso apposta: due esecuzioni
// del simulatore devono dare gli stessi PNG, altrimenti il confronto fra due
// versioni del display diventa inservibile.
inline long random(long howbig) { return howbig > 0 ? (long)(::rand() % howbig) : 0; }
inline long random(long howsmall, long howbig) {
    return howsmall + random(howbig - howsmall);
}
inline void randomSeed(unsigned long seed) { ::srand((unsigned)seed); }

// Arduino definisce min/max come macro, e le macro rompono <algorithm>. Qui sono
// template: stesso uso, nessun danno collaterale.
template <typename T>
inline T min(T a, T b) {
    return (a < b) ? a : b;
}
template <typename T>
inline T max(T a, T b) {
    return (a > b) ? a : b;
}
template <typename T>
inline T constrain(T x, T lo, T hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}
inline long mapValue(long x, long a, long b, long c, long d) {
    return (x - a) * (d - c) / (b - a) + c;
}

// -------------------------------------------------------------------- String
// storage.h e net_portal.h parlano di String nelle firme. display.cpp non la
// tocca, ma i suoi #include tirano dentro quelle intestazioni: basta che il tipo
// esista e si comporti come una stringa.
class String : public std::string {
   public:
    String() {}
    String(const char *s) : std::string(s ? s : "") {}
    String(const std::string &s) : std::string(s) {}
    const char *c_str() const { return std::string::c_str(); }
    unsigned int length() const { return (unsigned int)std::string::length(); }
    void trim() {}
};

// -------------------------------------------------------------------- Serial
// Muto per scelta: il simulatore ha un solo canale di uscita che conta, i PNG.
// Un printf di debug in mezzo sporcherebbe l'output di check.py.
struct SimSerial {
    void begin(unsigned long) {}
    void print(const char *) {}
    void println(const char *) {}
    void println() {}
    int printf(const char *, ...) { return 0; }
    operator bool() const { return false; }
};
extern SimSerial Serial;
