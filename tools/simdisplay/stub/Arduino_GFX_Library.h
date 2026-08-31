// Arduino_GFX_Library.h finto — rasterizza davvero, in un framebuffer da 240x240.
//
// Il punto di tutto il simulatore e' questo file. Non basta che src/display.cpp
// *compili*: deve produrre gli stessi pixel che produce sul GC9A01, altrimenti
// misurare i raggi sui PNG non dimostra niente. Percio' ogni primitiva qui sotto
// non e' "una implementazione ragionevole", e' la *trascrizione* di quella di
// Arduino_GFX.cpp:
//
//   - writeSlashLine: Bresenham con lo stesso swap steep/non-steep, stesso err
//     iniziale (dx>>1), stesso verso di percorrenza. Una riga tirata da A a B e
//     una da B ad A devono accendere gli stessi pixel del vero, non pixel
//     "equivalenti";
//   - drawCircle/fillCircle: writeEllipseHelper e writeFillEllipseHelper, che
//     NON sono il Bresenham classico di Adafruit ma la variante a ellisse della
//     libreria. Con raggio 118 la differenza e' di qualche pixel sui diagonali:
//     abbastanza da far fallire il controllo di integrita' dell'anello se qui si
//     improvvisa;
//   - drawChar sul font 5x7: cella 6*size x 8*size, glifo su 5 colonne di 8 bit
//     letti dal bit meno significativo verso l'alto, sesta colonna vuota,
//     setCursor sull'angolo alto-sinistra. Con setTextColor(c) il fondo e' uguale
//     al colore, che nella libreria vera vuol dire "sfondo trasparente": e'
//     quello su cui display.cpp conta per scrivere il testo sopra le targhette
//     piene.
//
// L'unica licenza che mi sono preso: i pixel fuori dai 240x240 vengono scartati
// invece di finire chissa' dove. Sul chip vero una writePixelPreclipped con
// coordinata negativa scrive dentro una finestra d'indirizzi sbagliata; qui
// contarli e buttarli e' piu' utile che riprodurre il disastro.
#pragma once

#include <Arduino.h>

#include "../vendor/glcdfont.h"

#define GFX_NOT_DEFINED (-1)

// Colori come li definisce Arduino_GFX.h (RGB565).
#define RGB565_BLACK 0x0000
#define RGB565_WHITE 0xFFFF
#define RGB565_RED 0xF800
#define RGB565_GREEN 0x07E0
#define RGB565_BLUE 0x001F
#define BLACK RGB565_BLACK
#define WHITE RGB565_WHITE
#define RED RGB565_RED
#define GREEN RGB565_GREEN
#define BLUE RGB565_BLUE

#define GFX_SIM_W 240
#define GFX_SIM_H 240

// Contatore delle scritture cadute fuori dal framebuffer. Non e' decorativo: se
// display.cpp centra una stringa troppo lunga, il testo comincia a x negativa e
// la libreria vera non se ne accorge. Qui il numero si vede.
extern long gfxSimClippedWrites;

class Arduino_DataBus {
   public:
    virtual ~Arduino_DataBus() {}
};

// Il bus vero parla SPI a un ESP32. Qui e' un segnaposto: serve solo perche'
// Display::begin() lo costruisce e lo passa al pannello.
class Arduino_ESP32SPI : public Arduino_DataBus {
   public:
    Arduino_ESP32SPI(int8_t dc, int8_t cs, int8_t sck, int8_t mosi, int8_t miso = GFX_NOT_DEFINED) {
        (void)dc;
        (void)cs;
        (void)sck;
        (void)mosi;
        (void)miso;
    }
};

class Arduino_GFX {
   public:
    Arduino_GFX(int16_t w = GFX_SIM_W, int16_t h = GFX_SIM_H) : _width(w), _height(h) {
        _max_x = _width - 1;
        _max_y = _height - 1;
        _min_text_x = 0;
        _min_text_y = 0;
        _max_text_x = _max_x;
        _max_text_y = _max_y;
        memset(fb, 0, sizeof(fb));
    }
    virtual ~Arduino_GFX() {}

    bool begin(int32_t speed = GFX_NOT_DEFINED) {
        (void)speed;
        return true;
    }

    int16_t width() const { return _width; }
    int16_t height() const { return _height; }
    const uint16_t *framebuffer() const { return fb; }

    // ------------------------------------------------------------- transazioni
    // Sul chip startWrite/endWrite aprono e chiudono la transazione SPI. Qui non
    // c'e' niente da aprire, ma i metodi devono esistere: display.cpp li usa nei
    // punti caldi (oscilloscopio, logo) e togliere quelle chiamate vorrebbe dire
    // toccare il sorgente.
    void startWrite() {}
    void endWrite() {}

    // ------------------------------------------------------------------ pixel
    void writePixel(int16_t x, int16_t y, uint16_t color) { writePixelPreclipped(x, y, color); }
    void drawPixel(int16_t x, int16_t y, uint16_t color) { writePixelPreclipped(x, y, color); }

    void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) {
        if (x < 0 || y < 0 || x >= _width || y >= _height) {
            ++gfxSimClippedWrites;
            return;
        }
        fb[(int)y * _width + (int)x] = color;
    }

    // ------------------------------------------------------------------ linee
    void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
        for (int16_t i = y; i < y + h; ++i) writePixel(x, i, color);
    }
    void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
        for (int16_t i = x; i < x + w; ++i) writePixel(i, y, color);
    }
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
        writeFastVLine(x, y, h, color);
    }
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
        writeFastHLine(x, y, w, color);
    }

    void writeSlashLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
        bool steep = _diff(y1, y0) > _diff(x1, x0);
        if (steep) {
            _swapi(x0, y0);
            _swapi(x1, y1);
        }
        if (x0 > x1) {
            _swapi(x0, x1);
            _swapi(y0, y1);
        }
        int16_t dx = x1 - x0;
        int16_t dy = _diff(y1, y0);
        int16_t err = dx >> 1;
        int16_t step = (y0 < y1) ? 1 : -1;
        for (; x0 <= x1; x0++) {
            if (steep) {
                writePixel(y0, x0, color);
            } else {
                writePixel(x0, y0, color);
            }
            err -= dy;
            if (err < 0) {
                err += dx;
                y0 += step;
            }
        }
    }

    void writeLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
        if (x0 == x1) {
            if (y0 > y1) _swapi(y0, y1);
            writeFastVLine(x0, y0, y1 - y0 + 1, color);
        } else if (y0 == y1) {
            if (x0 > x1) _swapi(x0, x1);
            writeFastHLine(x0, y0, x1 - x0 + 1, color);
        } else {
            writeSlashLine(x0, y0, x1, y1, color);
        }
    }

    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
        writeLine(x0, y0, x1, y1, color);
    }

    // --------------------------------------------------------------- rettangoli
    void writeFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        for (int16_t j = 0; j < h; ++j) writeFastHLine(x, y + j, w, color);
    }
    void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        writeFillRect(x, y, w, h, color);
    }
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        writeFillRect(x, y, w, h, color);
    }
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        writeFastHLine(x, y, w, color);
        writeFastHLine(x, y + h - 1, w, color);
        writeFastVLine(x, y, h, color);
        writeFastVLine(x + w - 1, y, h, color);
    }
    void fillScreen(uint16_t color) { fillRect(0, 0, _width, _height, color); }

    // ------------------------------------------------------------------ cerchi
    // Trascritta riga per riga da Arduino_GFX.cpp, come tutto il resto di questo
    // file: i quattro angoli sono quattro quarti di writeEllipseHelper, non un
    // arco improvvisato, altrimenti il controllo del cerchio darebbe differenze
    // che sul chip vero non esistono.
    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
        const int16_t max_radius = ((w < h) ? w : h) / 2;
        if (r > max_radius) r = max_radius;
        writeFastHLine(x + r, y, w - 2 * r, color);
        writeFastHLine(x + r, y + h - 1, w - 2 * r, color);
        writeFastVLine(x, y + r, h - 2 * r, color);
        writeFastVLine(x + w - 1, y + r, h - 2 * r, color);
        writeEllipseHelper(x + r, y + r, r, r, 1, color);
        writeEllipseHelper(x + w - r - 1, y + r, r, r, 2, color);
        writeEllipseHelper(x + w - r - 1, y + h - r - 1, r, r, 4, color);
        writeEllipseHelper(x + r, y + h - r - 1, r, r, 8, color);
    }

    void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {
        writeEllipseHelper(x, y, r, r, 0xf, color);
    }
    void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {
        writeFillEllipseHelper(x, y, r, r, 3, 0, color);
    }

    void writeEllipseHelper(int32_t x, int32_t y, int32_t rx, int32_t ry, uint8_t cornername,
                            uint16_t color) {
        if (rx < 0 || ry < 0 || ((rx == 0) && (ry == 0))) return;
        if (ry == 0) {
            writeFastHLine(x - rx, y, (ry << 2) + 1, color);
            return;
        }
        if (rx == 0) {
            writeFastVLine(x, y - ry, (rx << 2) + 1, color);
            return;
        }
        int32_t xt, yt, s, i;
        int32_t rx2 = rx * rx;
        int32_t ry2 = ry * ry;

        i = -1;
        xt = 0;
        yt = ry;
        s = (ry2 << 1) + rx2 * (1 - (ry << 1));
        do {
            while (s < 0) s += ry2 * ((++xt << 2) + 2);
            if (cornername & 0x1) writeFastHLine(x - xt, y - yt, xt - i, color);
            if (cornername & 0x2) writeFastHLine(x + i + 1, y - yt, xt - i, color);
            if (cornername & 0x4) writeFastHLine(x + i + 1, y + yt, xt - i, color);
            if (cornername & 0x8) writeFastHLine(x - xt, y + yt, xt - i, color);
            i = xt;
            s -= (--yt) * rx2 << 2;
        } while (ry2 * xt <= rx2 * yt);

        i = -1;
        yt = 0;
        xt = rx;
        s = (rx2 << 1) + ry2 * (1 - (rx << 1));
        do {
            while (s < 0) s += rx2 * ((++yt << 2) + 2);
            if (cornername & 0x1) writeFastVLine(x - xt, y - yt, yt - i, color);
            if (cornername & 0x2) writeFastVLine(x + xt, y - yt, yt - i, color);
            if (cornername & 0x4) writeFastVLine(x + xt, y + i + 1, yt - i, color);
            if (cornername & 0x8) writeFastVLine(x - xt, y + i + 1, yt - i, color);
            i = yt;
            s -= (--xt) * ry2 << 2;
        } while (rx2 * yt <= ry2 * xt);
    }

    void writeFillEllipseHelper(int32_t x, int32_t y, int32_t rx, int32_t ry, uint8_t corners,
                                int16_t delta, uint16_t color) {
        if (rx < 0 || ry < 0 || ((rx == 0) && (ry == 0))) return;
        if (ry == 0) {
            writeFastHLine(x - rx, y, (ry << 2) + 1, color);
            return;
        }
        if (rx == 0) {
            writeFastVLine(x, y - ry, (rx << 2) + 1, color);
            return;
        }
        int32_t xt, yt, i, s;
        int32_t rx2 = rx * rx;
        int32_t ry2 = ry * ry;

        writeFastHLine(x - rx, y, (rx << 1) + 1, color);
        i = 0;
        yt = 0;
        xt = rx;
        s = (rx2 << 1) + ry2 * (1 - (rx << 1));
        do {
            while (s < 0) s += rx2 * ((++yt << 2) + 2);
            if (corners & 1) writeFillRect(x - xt, y - yt, (xt << 1) + 1 + delta, yt - i, color);
            if (corners & 2) writeFillRect(x - xt, y + i + 1, (xt << 1) + 1 + delta, yt - i, color);
            i = yt;
            s -= (--xt) * ry2 << 2;
        } while (rx2 * yt <= ry2 * xt);

        xt = 0;
        yt = ry;
        s = (ry2 << 1) + rx2 * (1 - (ry << 1));
        do {
            while (s < 0) s += ry2 * ((++xt << 2) + 2);
            if (corners & 1) writeFastHLine(x - xt, y - yt, (xt << 1) + 1 + delta, color);
            if (corners & 2) writeFastHLine(x - xt, y + yt, (xt << 1) + 1 + delta, color);
            s -= (--yt) * rx2 << 2;
        } while (ry2 * xt <= rx2 * yt);
    }

    // --------------------------------------------------------------- triangoli
    // display.cpp non li usa (l'ho verificato con grep), ma la libreria vera li
    // espone e un domani potrebbero servire: costano dieci righe.
    void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                      uint16_t color) {
        writeLine(x0, y0, x1, y1, color);
        writeLine(x1, y1, x2, y2, color);
        writeLine(x2, y2, x0, y0, color);
    }
    void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                      uint16_t color) {
        int16_t minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        int16_t maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        int16_t miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
        int16_t maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
        for (int16_t py = miny; py <= maxy; ++py) {
            for (int16_t px = minx; px <= maxx; ++px) {
                const long d1 = (long)(px - x1) * (y0 - y1) - (long)(x0 - x1) * (py - y1);
                const long d2 = (long)(px - x2) * (y1 - y2) - (long)(x1 - x2) * (py - y2);
                const long d3 = (long)(px - x0) * (y2 - y0) - (long)(x2 - x0) * (py - y0);
                const bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
                const bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
                if (!(neg && pos)) writePixel(px, py, color);
            }
        }
    }

    // -------------------------------------------------------------------- testo
    void setTextSize(uint8_t s) { setTextSize(s, s); }
    void setTextSize(uint8_t sx, uint8_t sy) {
        textsize_x = sx ? sx : 1;
        textsize_y = sy ? sy : 1;
    }
    // Una sola tinta: sfondo = colore, cioe' fondo trasparente. E' la variante
    // che usa display.cpp ovunque, ed e' quella che permette di scrivere in nero
    // sopra una targhetta piena senza cancellarla.
    void setTextColor(uint16_t c) {
        textcolor = c;
        textbgcolor = c;
    }
    void setTextColor(uint16_t c, uint16_t bg) {
        textcolor = c;
        textbgcolor = bg;
    }
    void setCursor(int16_t x, int16_t y) {
        cursor_x = x;
        cursor_y = y;
    }
    void setTextWrap(bool w) { wrap = w; }

    // Il simulatore rende sempre dritto: qui interessa che il firmware compili e
    // che la scena sia quella giusta, non riprodurre un display montato al
    // contrario — girare l'immagine non proverebbe niente che l'occhio non sappia
    // gia' fare da solo guardando il PNG sottosopra.
    void setRotation(uint8_t r) { (void)r; }
    int16_t getCursorX() const { return cursor_x; }
    int16_t getCursorY() const { return cursor_y; }

    void drawChar(int16_t x, int16_t y, unsigned char c, uint16_t color, uint16_t bg) {
        const int16_t block_w = 6 * textsize_x;
        const int16_t block_h = 8 * textsize_y;
        if ((x > _max_text_x) || (y > _max_text_y) || ((x + block_w - 1) < _min_text_x) ||
            ((y + block_h - 1) < _min_text_y)) {
            return;
        }
        int16_t curX = x, curY;
        if (textsize_x == 1 && textsize_y == 1) {
            for (int8_t i = 0; i < 5; ++i, ++curX) {
                uint8_t line = pgm_read_byte(&font[c * 5 + i]);
                if (curX <= _max_text_x) {
                    curY = y;
                    for (int8_t j = 0; j < 8; ++j, ++curY, line >>= 1) {
                        if (curY <= _max_text_y) {
                            if (line & 1) {
                                writePixelPreclipped(curX, curY, color);
                            } else if (bg != color) {
                                writePixelPreclipped(curX, curY, bg);
                            }
                        }
                    }
                }
            }
            if (bg != color) {
                curX = x + 5;
                if (curX <= _max_text_x) {
                    const int16_t curH = ((y + 8 - 1) <= _max_text_y) ? 8 : (_max_text_y - y + 1);
                    writeFastVLine(curX, y, curH, bg);
                }
            }
        } else {
            for (int8_t i = 0; i < 5; ++i, curX += textsize_x) {
                if ((curX + textsize_x - 1) <= _max_text_x) {
                    uint8_t line = pgm_read_byte(&font[c * 5 + i]);
                    curY = y;
                    for (int8_t j = 0; j < 8; ++j, line >>= 1, curY += textsize_y) {
                        if ((curY + textsize_y - 1) <= _max_text_y) {
                            if (line & 1) {
                                writeFillRectPreclipped(curX, curY, textsize_x, textsize_y, color);
                            } else if (bg != color) {
                                writeFillRectPreclipped(curX, curY, textsize_x, textsize_y, bg);
                            }
                        }
                    }
                }
            }
            if (bg != color) {
                curX = x + 5 * textsize_x;
                if ((curX + textsize_x - 1) <= _max_text_x) {
                    int16_t curH = 8 * textsize_y;
                    while ((y + curH - 1) > _max_text_y) curH -= textsize_y;
                    writeFillRectPreclipped(curX, y, textsize_x, curH, bg);
                }
            }
        }
    }

    size_t write(uint8_t c) {
        if (c == '\n') {
            cursor_x = _min_text_x;
            cursor_y += textsize_y * 8;
        } else if (c != '\r') {
            if (wrap && ((cursor_x + (textsize_x * 6) - 1) > _max_text_x)) {
                cursor_x = _min_text_x;
                cursor_y += textsize_y * 8;
            }
            drawChar(cursor_x, cursor_y, c, textcolor, textbgcolor);
            cursor_x += textsize_x * 6;
        }
        return 1;
    }

    size_t print(const char *s) {
        size_t n = 0;
        if (!s) return 0;
        while (*s) n += write((uint8_t)*s++);
        return n;
    }
    size_t print(char c) { return write((uint8_t)c); }
    size_t print(int v) {
        char b[16];
        snprintf(b, sizeof(b), "%d", v);
        return print(b);
    }
    size_t print(unsigned int v) {
        char b[16];
        snprintf(b, sizeof(b), "%u", v);
        return print(b);
    }
    size_t print(const String &s) { return print(s.c_str()); }
    size_t println(const char *s) { return print(s) + write('\n'); }
    size_t println() { return write('\n'); }

   protected:
    static int16_t _diff(int16_t a, int16_t b) { return (a > b) ? (a - b) : (b - a); }
    static void _swapi(int16_t &a, int16_t &b) {
        int16_t t = a;
        a = b;
        b = t;
    }

    int16_t _width, _height, _max_x, _max_y;
    int16_t _min_text_x, _min_text_y, _max_text_x, _max_text_y;
    int16_t cursor_x = 0, cursor_y = 0;
    uint16_t textcolor = 0xFFFF, textbgcolor = 0xFFFF;
    uint8_t textsize_x = 1, textsize_y = 1;
    bool wrap = true;

    uint16_t fb[GFX_SIM_W * GFX_SIM_H];
};

// Il pannello vero. Qui e' solo un Arduino_GFX di 240x240: la geometria del
// GC9A01 e' tutta nel fatto che il vetro e' tondo, e quella la verifica check.py
// sui PNG, non il driver.
class Arduino_GC9A01 : public Arduino_GFX {
   public:
    Arduino_GC9A01(Arduino_DataBus *bus, int8_t rst = GFX_NOT_DEFINED, uint8_t rotation = 0,
                   bool ips = false)
        : Arduino_GFX(GFX_SIM_W, GFX_SIM_H) {
        (void)bus;
        (void)rst;
        (void)rotation;
        (void)ips;
    }
};
