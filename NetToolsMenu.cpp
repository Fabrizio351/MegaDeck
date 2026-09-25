#include "NetToolsMenu.h"
#include "Config.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "Menu.h"
#include "NetScanUI.h"
#include "NetGuardUI.h"

#include <Arduino.h>

// Fase 3 della riscrittura senza LVGL (vedi project_lvgl_removal_rewrite
// in memoria): sottomenu Net Tools convertito a disegno diretto, stesso
// schema del menu principale ma senza bisogno di scorrimento (solo 2
// voci). Le iconette colorate sono sparite, solo testo.
namespace {

    enum class NetToolAction {
        NetScan,
        NetGuard
    };

    struct NetToolItem {
        const char* text;
        NetToolAction action;
    };

    // Nuove utility di rete si aggiungono qui: la griglia si adatta da
    // sola al numero di voci (colonne fisse, righe calcolate).
    const NetToolItem items[] = {
        { "NETSCAN", NetToolAction::NetScan },
        { "NETGUARD", NetToolAction::NetGuard },
    };

    constexpr int ITEM_COUNT = sizeof(items) / sizeof(items[0]);
    constexpr int GRID_COLS = 2;

    bool active = false;
    int selectedItem = 0;

    // Stesso motivo/correzione di Menu.cpp (25/09): showMain() puo'
    // essere richiamata da dentro lv_timer_handler() (bottone "Indietro"
    // toccato su NetScanUI, ancora LVGL) - disegnare subito in quel
    // momento rischia di farsi risovrascrivere dal refresh LVGL ancora
    // in corso appena il callback ritorna. Si disegna quindi solo al
    // giro successivo di update(), mai annidati dentro LVGL.
    bool needsRedraw = false;

    constexpr int16_t GRID_TOP = Display::DD_HEADER_H + 30;
    constexpr int16_t GRID_LEFT = 6;
    constexpr int16_t CELL_W = 152;
    constexpr int16_t CELL_H = 40;
    constexpr int16_t GAP = 4;

    int itemZone[ITEM_COUNT];

    void closeNetTools() {
        active = false;
        Menu::back();
    }

    int rowStartIndex(int index) {
        return (index / GRID_COLS) * GRID_COLS;
    }

    int rowEndIndex(int index) {
        int start = rowStartIndex(index);
        int end = start + GRID_COLS - 1;
        if (end >= ITEM_COUNT) {
            end = ITEM_COUNT - 1;
        }
        return end;
    }

    void moveLeft() {
        int start = rowStartIndex(selectedItem);
        int candidate = selectedItem - 1;
        if (candidate < start) {
            return;
        }
        selectedItem = candidate;
    }

    void moveRight() {
        int end = rowEndIndex(selectedItem);
        int candidate = selectedItem + 1;
        if (candidate > end) {
            return;
        }
        selectedItem = candidate;
    }

    void selectCurrentItem() {
        // Come in Menu.cpp: cede sempre lo schermo a LVGL prima di aprire
        // qualunque voce. NetGuardUI (gia' convertita) lo riprende subito
        // da sola, ma NetScanUI (non ancora convertita, resta LVGL) non
        // tocca affatto questo flag - senza questa riga restava attiva la
        // modalita' disegno diretto ereditata da qui, e Display::update()
        // continuava a saltare lv_timer_handler()/lv_refr_now(): lo
        // schermo di NetScan veniva costruito in memoria ma mai disegnato
        // ne' reattivo al tocco (bug reale, osservato il 25/09).
        Display::setDirectDrawMode(false);

        switch (items[selectedItem].action) {
            case NetToolAction::NetScan:
                NetScanUI::open();
                break;

            case NetToolAction::NetGuard:
                NetGuardUI::open();
                break;
        }
    }

    void drawScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("NET TOOLS");

        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(228, Display::DD_HEADER_H + 8);
        gfx->print("[B] INDIETRO");

        TouchUI::clearZones();
        for (int i = 0; i < ITEM_COUNT; i++) {
            int col = i % GRID_COLS;
            int row = i / GRID_COLS;
            int16_t x = GRID_LEFT + col * (CELL_W + GAP);
            int16_t y = GRID_TOP + row * (CELL_H + GAP);

            gfx->drawRect(x, y, CELL_W, CELL_H, Display::DD_ACCENT_DIM);

            if (i == selectedItem) {
                gfx->fillRect(x + 4, y + 8, 4, CELL_H - 16, Display::DD_ACCENT);
            }

            gfx->setTextColor(Display::DD_ACCENT);
            gfx->setCursor(x + 16, y + CELL_H / 2 - 3);
            gfx->print(items[i].text);

            itemZone[i] = TouchUI::addZone(x, y, CELL_W, CELL_H);
        }
    }

} // namespace

namespace NetToolsMenu {

    void open() {
        active = true;
        selectedItem = 0;
        Display::setDirectDrawMode(true);
        drawScreen();
    }

    void showMain() {
        active = true;
        Display::setDirectDrawMode(true);
        needsRedraw = true;
    }

    bool isActive() {
        return active;
    }

    void update() {
        if (!active) {
            return;
        }

        if (needsRedraw) {
            needsRedraw = false;
            drawScreen();
        }

        int tapped = TouchUI::pollTap();
        if (tapped >= 0) {
            for (int i = 0; i < ITEM_COUNT; i++) {
                if (itemZone[i] == tapped) {
                    selectedItem = i;
                    drawScreen();
                    delay(150);
                    selectCurrentItem();
                    return;
                }
            }
        }

        InputEvent event = Input::getEvent();

        switch (event) {
            case InputEvent::Left:
                moveLeft();
                drawScreen();
                break;

            case InputEvent::Right:
                moveRight();
                drawScreen();
                break;

            case InputEvent::Select:
                selectCurrentItem();
                break;

            case InputEvent::Back:
                closeNetTools();
                break;

            default:
                break;
        }
    }
}
