#include "Display.h"
#include "Board.h"
#include "Config.h"

#include <Arduino.h>

// IMPORTANTE: U8g2lib.h va incluso PRIMA di Arduino_GFX_Library.h.
// Arduino_GFX supporta nativamente i font u8g2 (compressi, molte varianti)
// senza bisogno di librerie aggiuntive tipo U8g2_for_Adafruit_GFX.
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>

#include "Background.h"

#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_ACCENT  RGB565(60, 180, 255)   // azzurro cyberpunk
#define COLOR_SHADOW  0x0000

// Font terminale (u8g2) — bitmap terminal grande e nitido
#define FONT_TITLE    u8g2_font_helvB12_tf
#define FONT_BODY     u8g2_font_9x15_tf
#define FONT_MENU     u8g2_font_9x15_tf
#define FONT_SELECTED u8g2_font_9x15B_tf
#define FONT_ICONS    u8g2_font_open_iconic_all_2x_t

// Font per le celle della griglia (3 colonne = poco spazio orizzontale)
//#define FONT_GRID    u8g2_font_7Segments_26x42_mn
#define FONT_GRID     u8g2_font_lubR12_tf

// NB: usa l'host HSPI, lo stesso su cui Board::begin() ha gia'
// inizializzato i pin fisici condivisi con la radio LoRa e la SD.
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    BOARD_TFT_DC,
    BOARD_TFT_CS,
    BOARD_SPI_SCK,
    BOARD_SPI_MOSI,
    BOARD_SPI_MISO,
    HSPI
);

Arduino_GFX *gfx = new Arduino_ST7789(
    bus,
    -1,
    0,
    true,
    240, 320   // dimensioni native ST7789, ruotate poi con setRotation(1)
);

namespace {

    void drawBackground() {
        gfx->draw16bitRGBBitmap(0, 0, backgroundBitmap, BACKGROUNDBITMAP_WIDTH, BACKGROUNDBITMAP_HEIGHT);
    }

    // Testo con una leggera ombra nera dietro: resta leggibile sopra
    // qualsiasi zona dell'immagine di sfondo, chiara o scura che sia.
    // NB: con i font u8g2 la coordinata y e' il BASELINE del testo
    // (la base delle lettere), non l'angolo in alto a sinistra.
    void drawTextShadow(int16_t x, int16_t y, const char* text, uint16_t color, const uint8_t* font) {
        gfx->setFont(font);

        gfx->setTextColor(COLOR_SHADOW);
        gfx->setCursor(x + 1, y + 1);
        gfx->print(text);

        gfx->setTextColor(color);
        gfx->setCursor(x, y);
        gfx->print(text);
    }

    // Layout condiviso tra drawGridMenu() e hitTestGridMenu(): DEVONO
    // restare identici, altrimenti il tap non coincide con la cella
    // disegnata. Se cambi uno di questi valori, ricontrolla entrambe
    // le funzioni che li usano.
    struct GridLayout {
        int startY;
        int marginX;
        int cellGap;
        int rows;
        int cellWidth;
        int cellHeight;
    };

    GridLayout computeGridLayout(int itemCount, int cols) {
        GridLayout layout;

        layout.startY = 56;
        layout.marginX = 10;
        layout.cellGap = 4;
        layout.rows = (itemCount + cols - 1) / cols;

        const int marginBottom = 10;
        int gridWidth = gfx->width() - (2 * layout.marginX);
        int gridHeight = gfx->height() - layout.startY - marginBottom;

        layout.cellWidth = gridWidth / cols;
        layout.cellHeight = gridHeight / layout.rows;

        return layout;
    }
}

namespace Display {

    void begin() {
        Serial.println("[DISPLAY] avvio...");

        // L'alimentazione periferiche (BOARD_POWERON) e' gia' gestita
        // centralmente da Board::begin(), chiamato prima di Display::begin().

        // Retroilluminazione
        pinMode(BOARD_TFT_BACKLIGHT, OUTPUT);
        digitalWrite(BOARD_TFT_BACKLIGHT, HIGH);

        delay(100);

        gfx->begin();
        gfx->setRotation(1);
        gfx->fillScreen(COLOR_BLACK);

        Serial.println("[DISPLAY] pronto");
    }

    void update() {
    }

    void clear() {
        drawBackground();
    }

    void showBootScreen() {
        clear();

        drawTextShadow(20, 100, "TDeckFW", COLOR_ACCENT, FONT_TITLE);
        drawTextShadow(20, 135, "Avvio...", COLOR_WHITE, FONT_BODY);
    }

    void showMessage(const char* title, const char* message) {
        clear();

        drawTextShadow(15, 50, title, COLOR_ACCENT, FONT_TITLE);
        drawTextShadow(15, 85, message, COLOR_WHITE, FONT_BODY);
    }

    void drawMenu(
        const char* title,
        const char* const items[],
        int itemCount,
        int selectedItem
    ) {
        clear();

        drawTextShadow(15, 34, title, COLOR_ACCENT, FONT_TITLE);

        const int rowHeight = 32;
        const int startY = 70;
        const int barX = 12;
        const int barWidth = 4;

        for (int i = 0; i < itemCount; i++) {
            int y = startY + i * rowHeight;

            if (i == selectedItem) {
                // Barra laterale colorata + testo in evidenza
                // (allineata verticalmente al baseline del testo del menu)
                gfx->fillRect(barX, y - 18, barWidth, 22, COLOR_ACCENT);
                drawTextShadow(24, y, items[i], COLOR_ACCENT, FONT_MENU);
            } else {
                drawTextShadow(24, y, items[i], COLOR_WHITE, FONT_MENU);
            }
        }
    }

    void drawGridMenu(
        const char* title,
        const char* const items[],
        int itemCount,
        int cols,
        int selectedItem
    ) {
        clear();

        drawTextShadow(15, 34, title, COLOR_ACCENT, FONT_TITLE);

        GridLayout layout = computeGridLayout(itemCount, cols);

        gfx->setFont(FONT_GRID);

        for (int i = 0; i < itemCount; i++) {
            int col = i % cols;
            int row = i / cols;

            int cellX = layout.marginX + (col * layout.cellWidth);
            int cellY = layout.startY + (row * layout.cellHeight);

            int boxW = layout.cellWidth - layout.cellGap;
            int boxH = layout.cellHeight - layout.cellGap;

            bool selected = (i == selectedItem);

            if (selected) {
                gfx->fillRect(cellX, cellY, boxW, boxH, COLOR_ACCENT);
            }

            // Testo centrato verticalmente nella cella (y = baseline u8g2)
            int textX = cellX + 6;
            int textY = cellY + (boxH / 2) + 4;

            gfx->setTextColor(selected ? COLOR_BLACK : COLOR_WHITE);
            gfx->setCursor(textX, textY);
            gfx->print(items[i]);
        }
    }

    int hitTestGridMenu(
        int16_t touchX,
        int16_t touchY,
        int itemCount,
        int cols
    ) {
        GridLayout layout = computeGridLayout(itemCount, cols);

        if (touchY < layout.startY) {
            return -1; // tocco sul titolo, fuori dalla griglia
        }

        int col = (touchX - layout.marginX) / layout.cellWidth;
        int row = (touchY - layout.startY) / layout.cellHeight;

        if (col < 0 || col >= cols || row < 0 || row >= layout.rows) {
            return -1;
        }

        int index = (row * cols) + col;

        if (index >= itemCount) {
            return -1; // ultima riga parziale, cella vuota
        }

        return index;
    }
}
