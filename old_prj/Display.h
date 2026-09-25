#pragma once

#include <stdint.h>

namespace Display {
    void begin();
    void update();

    void clear();
    void showBootScreen();
    void showMessage(const char* title, const char* message);

    void drawMenu(
        const char* title,
        const char* const items[],
        int itemCount,
        int selectedItem
    );

    void drawGridMenu(
        const char* title,
        const char* const items[],
        int itemCount,
        int cols,
        int selectedItem
    );

    // Ritorna l'indice dell'item toccato alle coordinate (touchX, touchY),
    // usando lo stesso layout di drawGridMenu, oppure -1 se il tocco cade
    // fuori dalla griglia (es. sul titolo).
    int hitTestGridMenu(
        int16_t touchX,
        int16_t touchY,
        int itemCount,
        int cols
    );
}
