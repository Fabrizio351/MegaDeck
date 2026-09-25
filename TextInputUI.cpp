#include "TextInputUI.h"
#include "Display.h"
#include "Input.h"

namespace TextInputUI {

    Result handleKey(String& buffer, uint16_t maxLength) {
        InputEvent event = Input::getEvent();
        if (event == InputEvent::None) {
            return Result::None;
        }

        if (event == InputEvent::Select) {
            return Result::Confirmed;
        }

        if (event == InputEvent::Key) {
            char key = Input::getKey();

            if (key == 0x08 || key == 0x7F) {
                // Backspace (stessi codici gia' usati in WifiUI.cpp per
                // la password).
                if (buffer.length() > 0) {
                    buffer.remove(buffer.length() - 1);
                    return Result::Changed;
                }
                return Result::None;
            }

            if (key >= 32 && key < 127 && buffer.length() < maxLength) {
                buffer += key;
                return Result::Changed;
            }
        }

        return Result::None;
    }

    void draw(
        int16_t x, int16_t y, int16_t w, int16_t h,
        const String& buffer, bool maskAsPassword
    ) {
        Arduino_GFX* gfx = Display::getGfx();

        const uint16_t BG = gfx->color565(0x00, 0x00, 0x00);
        const uint16_t ACCENT = gfx->color565(0x39, 0xFF, 0x14);
        const uint16_t ACCENT_DIM = gfx->color565(0x0A, 0x2E, 0x10);

        // Interno del box: ripulito prima di riscrivere, altrimenti un
        // backspace lascerebbe l'ultimo carattere vecchio ancora visibile
        // (il nuovo testo e' piu' corto del precedente).
        gfx->fillRect(x + 1, y + 1, w - 2, h - 2, BG);
        gfx->drawRect(x, y, w, h, ACCENT_DIM);

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(ACCENT);

        // Font classico a size 1: 6px di avanzamento per carattere
        // (5px glifo + 1px spaziatura) - niente getTextBounds, e' a
        // larghezza fissa quindi il calcolo e' diretto.
        constexpr int16_t CHAR_ADVANCE_PX = 6;
        constexpr int16_t PAD_X = 6;
        constexpr int16_t CURSOR_W = 6;

        int16_t innerW = w - (PAD_X * 2) - CURSOR_W;
        int16_t maxVisibleChars = innerW / CHAR_ADVANCE_PX;
        if (maxVisibleChars < 0) {
            maxVisibleChars = 0;
        }

        String visible = buffer;
        if (visible.length() > static_cast<unsigned int>(maxVisibleChars)) {
            // Mostra la CODA del testo: mentre si digita interessa
            // vedere l'ultimo carattere inserito, non il primo.
            visible = visible.substring(visible.length() - maxVisibleChars);
        }

        if (maskAsPassword) {
            for (unsigned int i = 0; i < visible.length(); i++) {
                visible.setCharAt(i, '*');
            }
        }

        gfx->setCursor(x + PAD_X, y + h / 2 - 3);
        gfx->print(visible);

        // Cursore: barretta statica subito dopo l'ultimo carattere
        // visibile (niente lampeggio a tempo - vedi motivazione in
        // TextInputUI.h: andrebbe ridisegnato ad ogni ciclo anche senza
        // cambiamenti, lo stesso errore gia' corretto altrove).
        int16_t cursorX = x + PAD_X + static_cast<int16_t>(visible.length()) * CHAR_ADVANCE_PX;
        gfx->fillRect(cursorX, y + 5, 2, h - 10, ACCENT);
    }
}
