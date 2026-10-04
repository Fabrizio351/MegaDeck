#include "LoRaToolsMenu.h"
#include "Config.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "Menu.h"
#include "LoRaScanUI.h"
#include "LoRaWanUI.h"
#include "MeshtasticUI.h"

#include <Arduino.h>

// Sottomenu "LoRa Tools" (26/09, stesso schema di NetToolsMenu.cpp).
// "LORA CHAT" (protocollo custom) rimossa il 02/10: Meshtastic la
// sostituisce come da decisione dell'utente (vedi
// project_lora_meshtastic_lorawan_plan in memoria) una volta che
// routing+NodeDB persistente l'hanno resa "finita".
namespace {

    enum class LoRaToolAction {
        Scan,
        Wan,
        Meshtastic
    };

    struct LoRaToolItem {
        const char* text;
        LoRaToolAction action;
    };

    const LoRaToolItem items[] = {
        { "LORA SCAN", LoRaToolAction::Scan },
        { "LORA WAN", LoRaToolAction::Wan },
        { "MESHTASTIC", LoRaToolAction::Meshtastic },
    };

    constexpr int ITEM_COUNT = sizeof(items) / sizeof(items[0]);
    constexpr int GRID_COLS = 2;

    bool active = false;
    int selectedItem = 0;

    // Stesso motivo/correzione di NetToolsMenu.cpp (25/09): showMain()
    // puo' essere richiamata da dentro lv_timer_handler() (bottone
    // "Indietro" toccato su una schermata ancora LVGL) - disegnare
    // subito in quel momento rischia di farsi risovrascrivere dal
    // refresh LVGL ancora in corso appena il callback ritorna. Si
    // disegna quindi solo al giro successivo di update().
    bool needsRedraw = false;

    constexpr int16_t GRID_TOP = Display::DD_HEADER_H + 30;
    constexpr int16_t GRID_LEFT = 6;
    constexpr int16_t CELL_W = 152;
    constexpr int16_t CELL_H = 40;
    constexpr int16_t GAP = 4;

    int itemZone[ITEM_COUNT];

    void closeLoRaTools() {
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

    // Su/Giu' tra righe della griglia (02/10, bug segnalato: "molte
    // vanno solo col touch" - con 3 voci su una griglia 2 colonne la
    // terza (MESHTASTIC, sola in seconda riga) non era raggiungibile a
    // trackball in nessun modo, solo Sinistra/Destra erano gestiti).
    // Stessa colonna quando possibile, altrimenti l'ultima valida di
    // quella riga (riga finale incompleta).
    void moveUp() {
        int col = selectedItem % GRID_COLS;
        int candidateRowStart = rowStartIndex(selectedItem) - GRID_COLS;
        if (candidateRowStart < 0) {
            return;
        }
        int candidate = candidateRowStart + col;
        int end = rowEndIndex(candidate);
        if (candidate > end) {
            candidate = end;
        }
        selectedItem = candidate;
    }

    void moveDown() {
        int col = selectedItem % GRID_COLS;
        int candidateRowStart = rowStartIndex(selectedItem) + GRID_COLS;
        if (candidateRowStart >= ITEM_COUNT) {
            return;
        }
        int candidate = candidateRowStart + col;
        int end = rowEndIndex(candidate);
        if (candidate > end) {
            candidate = end;
        }
        selectedItem = candidate;
    }

    void selectCurrentItem() {
        LoRaToolAction action = items[selectedItem].action;

        // Scan e WAN riconfigurano la radio SX1262 condivisa per conto
        // proprio (regolazioni dirette/OTAA) - se Meshtastic la sta gia'
        // usando (primo O secondo piano, vedi MeshtasticUI::isListening())
        // i due finiscono per litigare sulla stessa radio, osservato
        // come un blocco vero e proprio (02/10, segnalato: "entro in
        // lora wan e non si puo' piu' tornare indietro"). Bloccato qui
        // prima ancora di entrare, invece di gestire un arbitraggio
        // completo multi-utente della radio (sforzo sproporzionato per
        // un caso d'uso raro, gia' accettato come limite noto).
        if ((action == LoRaToolAction::Scan || action == LoRaToolAction::Wan) && MeshtasticUI::isListening()) {
            // needsRedraw=true (02/10, bug segnalato: popup che poi non
            // tornava piu' indietro) - il messaggio "usa e getta" di
            // Display occupa tutto lo schermo (fillScreen suo), quando lo
            // si chiude questo menu resta "attivo" ma nessuno lo
            // ridisegna da solo finche' non arriva questo flag, stesso
            // meccanismo gia' usato da showMain().
            Display::showMessage(
                "LORA TOOLS",
                "Funzioni disabilitate: Meshtastic e' attivo (primo o secondo piano) e sta usando la radio. Disattivalo prima di usare LoRa Scan o LoRa WAN.",
                nullptr
            );
            needsRedraw = true;
            return;
        }

        // "active" va disattivato PRIMA di aprire la voce, non solo alla
        // pressione di Indietro - stesso bug reale gia' preso e corretto
        // in NetToolsMenu.cpp (26/09): senza questa riga, isActive()
        // resterebbe vera anche dopo essere entrati in LoRaScanUI/
        // LoRaWanUI, e siccome App.cpp deve controllare questo menu
        // PRIMA delle sue voci figlie nella catena di dispatch, sarebbe
        // questo update() a intercettare per sempre ogni tap/tasto
        // destinato a loro.
        active = false;

        switch (action) {
            case LoRaToolAction::Scan:
                LoRaScanUI::open();
                break;

            case LoRaToolAction::Wan:
                LoRaWanUI::open();
                break;

            case LoRaToolAction::Meshtastic:
                MeshtasticUI::open();
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
        gfx->print("LORA TOOLS");

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

namespace LoRaToolsMenu {

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

            case InputEvent::Up:
                moveUp();
                drawScreen();
                break;

            case InputEvent::Down:
                moveDown();
                drawScreen();
                break;

            case InputEvent::Select:
                selectCurrentItem();
                break;

            case InputEvent::Back:
                closeLoRaTools();
                break;

            default:
                break;
        }
    }
}
