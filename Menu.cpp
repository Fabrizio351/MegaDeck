#include "Menu.h"
#include "Config.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"

#include <Arduino.h>
#include <stdint.h>

// Fase 3 della riscrittura senza LVGL (vedi project_lvgl_removal_rewrite
// in memoria): menu principale convertito a disegno diretto - la
// schermata radice da cui si parte sempre, quindi anche il punto in cui
// si decide chi "possiede" lo schermo fisico in un dato momento. Le
// icone colorate (prima cerchi LVGL con un glifo FontAwesome dentro)
// sono sparite: solo testo, come nel resto della nuova interfaccia -
// coerente con l'obiettivo originale "senza troppa grafica" di tutta
// questa riscrittura.
//
// Con 12 voci su una griglia a 2 colonne non entrano tutte in una sola
// schermata: invece dello scorrimento fluido a pixel di LVGL, qui lo
// scorrimento e' "a righe intere" (topVisibleRow) - piu' semplice da
// disegnare (nessun clipping parziale di una riga a meta'), la voce
// selezionata resta sempre visibile spostando la finestra di una riga
// alla volta quando serve.
namespace {

constexpr int MENU_ITEM_COUNT = 12;
constexpr int GRID_COLS = 2;
constexpr int GRID_ROWS = (MENU_ITEM_COUNT + GRID_COLS - 1) / GRID_COLS;

const char* mainItems[MENU_ITEM_COUNT] = {
    "VOIP", "WIFI", "BLE CHAT", "LORA CHAT",
    "GPS", "NET TOOLS", "VPN", "OPENCLAW.AI",
    "HOME ASS.", "FRIGATE", "DRONE", "IMPOSTAZIONI"
};

int selectedItem = 0;
bool mainMenuVisible = true;

// showMain() puo' essere richiamata DA DENTRO lv_timer_handler() (es. un
// bottone "Indietro" toccato su una schermata ancora LVGL, come WifiUI:
// il suo tap arriva tramite l'indev di LVGL, processato dentro
// Display::update() -> lv_timer_handler(), quindi il callback che chiama
// Menu::showMain() gira ANNIDATO li' dentro). Disegnare subito in quel
// momento e' rischioso: appena il callback ritorna, lv_timer_handler()
// prosegue il proprio giro di refresh interno e puo' riflushare il suo
// framebuffer (ormai vecchio, la schermata sta per essere cancellata)
// SOPRA quello che abbiamo appena disegnato noi (bug reale, osservato il
// 25/09 - "torna al menu principale, si blocca"). showMain() quindi non
// disegna piu' subito: si limita a mettere in pausa LVGL e a segnare
// "serve un ridisegno", che avviene poi in Menu::update() - chiamata
// SEMPRE da App::update() PRIMA di Display::update()/lv_timer_handler(),
// mai annidata dentro.
bool needsRedraw = false;

Menu::ActionCallback actionCallback = nullptr;

constexpr int16_t GRID_TOP = Display::DD_HEADER_H + 4;
constexpr int16_t GRID_LEFT = 6;
constexpr int16_t CELL_W = 152;
constexpr int16_t CELL_H = 36;
constexpr int16_t GAP = 4;
constexpr int16_t ROW_STEP = CELL_H + GAP;
constexpr int VISIBLE_ROWS = 5; // (240 - GRID_TOP) / ROW_STEP, arrotondato per difetto

int topVisibleRow = 0;
int itemZone[MENU_ITEM_COUNT];

// Accumulo dello scorrimento touch trascinato (vedi TouchUI::pollDragDeltaY):
// px sotto la soglia di una riga intera restano qui finche' non se ne
// accumula abbastanza per spostare topVisibleRow di una riga.
int dragAccumY = 0;


MenuAction actionFromIndex(int index) {
    switch (index) {
        case 0:  return MenuAction::VoIP;
        case 1:  return MenuAction::Wifi;
        case 2:  return MenuAction::Bluetooth;
        case 3:  return MenuAction::LoRa;
        case 4:  return MenuAction::Gps;
        case 5:  return MenuAction::NetTools;
        case 6:  return MenuAction::Vpn;
        case 7:  return MenuAction::OpenClaw;
        case 8:  return MenuAction::HomeAssistant;
        case 9:  return MenuAction::Frigate;
        case 10: return MenuAction::Drone;
        case 11: return MenuAction::Settings;
        default: return MenuAction::None;
    }
}


void drawScreen();


void selectCurrentItem() {
    MenuAction action = actionFromIndex(selectedItem);

    mainMenuVisible = false;

    // Il menu cede sempre lo schermo a LVGL prima di aprire qualunque
    // voce: le schermate gia' convertite lo riprendono subito da sole
    // nel proprio open() (vedi project_lvgl_removal_rewrite), quelle
    // ancora su LVGL (WifiUI, BleChatUI, LoRaChatUI, NetToolsMenu, i
    // placeholder Display::showMessage) ne hanno bisogno per funzionare.
    Display::setDirectDrawMode(false);

    if (actionCallback != nullptr) {
        actionCallback(action);
    }
}


int rowStartIndex(int index) {
    return (index / GRID_COLS) * GRID_COLS;
}


int rowEndIndex(int index) {
    int start = rowStartIndex(index);
    int end = start + GRID_COLS - 1;
    if (end >= MENU_ITEM_COUNT) {
        end = MENU_ITEM_COUNT - 1;
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


void moveUp() {
    int candidate = selectedItem - GRID_COLS;
    if (candidate < 0) {
        return;
    }
    selectedItem = candidate;
}


void moveDown() {
    int candidate = selectedItem + GRID_COLS;
    if (candidate >= MENU_ITEM_COUNT) {
        return;
    }
    selectedItem = candidate;
}


// Porta la riga della voce selezionata dentro la finestra visibile,
// spostandola di una riga alla volta se serve - equivalente "a righe
// intere" di lv_obj_scroll_to_view() usato prima.
void ensureSelectedVisible() {
    int row = selectedItem / GRID_COLS;

    if (row < topVisibleRow) {
        topVisibleRow = row;
    } else if (row >= topVisibleRow + VISIBLE_ROWS) {
        topVisibleRow = row - VISIBLE_ROWS + 1;
    }
}


void drawScreen() {
    Arduino_GFX* gfx = Display::getGfx();
    gfx->fillScreen(Display::DD_BG);
    Display::drawHeaderBar();

    gfx->setFont(nullptr);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);

    TouchUI::clearZones();
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        itemZone[i] = -1;
    }

    int lastVisibleRow = topVisibleRow + VISIBLE_ROWS - 1;
    if (lastVisibleRow >= GRID_ROWS) {
        lastVisibleRow = GRID_ROWS - 1;
    }

    for (int row = topVisibleRow; row <= lastVisibleRow; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            int i = row * GRID_COLS + col;
            if (i >= MENU_ITEM_COUNT) {
                continue;
            }

            int16_t x = GRID_LEFT + col * (CELL_W + GAP);
            int16_t y = GRID_TOP + (row - topVisibleRow) * ROW_STEP;

            gfx->drawRect(x, y, CELL_W, CELL_H, Display::DD_ACCENT_DIM);

            bool selected = (i == selectedItem);
            if (selected) {
                gfx->fillRect(x + 4, y + 6, 4, CELL_H - 12, Display::DD_ACCENT);
            }

            gfx->setTextColor(Display::DD_ACCENT);
            gfx->setCursor(x + 16, y + CELL_H / 2 - 3);
            gfx->print(mainItems[i]);

            itemZone[i] = TouchUI::addZone(x, y, CELL_W, CELL_H);
        }
    }
}


void redrawAfterMove(int previousItem) {
    (void)previousItem;
    ensureSelectedVisible();
    drawScreen();
}

} // namespace


namespace Menu {

void begin(ActionCallback callback) {
    actionCallback = callback;

    selectedItem = 0;
    topVisibleRow = 0;
    dragAccumY = 0;
    mainMenuVisible = true;

    showMain();
}


void update() {
    if (needsRedraw) {
        needsRedraw = false;
        drawScreen();
    }

    InputEvent event = Input::getEvent();

    if (mainMenuVisible) {
        int16_t dragDelta = TouchUI::pollDragDeltaY();
        if (dragDelta != 0) {
            dragAccumY += dragDelta;

            int maxTopRow = GRID_ROWS - VISIBLE_ROWS;
            if (maxTopRow < 0) {
                maxTopRow = 0;
            }

            bool scrolled = false;
            while (dragAccumY >= ROW_STEP && topVisibleRow > 0) {
                topVisibleRow--;
                dragAccumY -= ROW_STEP;
                scrolled = true;
            }
            while (dragAccumY <= -ROW_STEP && topVisibleRow < maxTopRow) {
                topVisibleRow++;
                dragAccumY += ROW_STEP;
                scrolled = true;
            }

            // Non far crescere l'accumulo all'infinito contro un bordo
            // (dito che continua a trascinare oltre inizio/fine lista).
            if (topVisibleRow == 0 && dragAccumY > ROW_STEP) {
                dragAccumY = ROW_STEP;
            }
            if (topVisibleRow == maxTopRow && dragAccumY < -ROW_STEP) {
                dragAccumY = -ROW_STEP;
            }

            if (scrolled) {
                drawScreen();
            }
        }

        int tapped = TouchUI::pollTap();
        if (tapped >= 0) {
            for (int i = 0; i < MENU_ITEM_COUNT; i++) {
                if (itemZone[i] == tapped) {
                    selectedItem = i;
                    drawScreen();

                    // Con il trackball l'evidenziazione resta visibile
                    // finche' non si preme Select; con un tap diretto
                    // invece evidenziazione e apertura avverrebbero nello
                    // stesso ciclo. Il disegno diretto e' pero' gia' sul
                    // pannello fisico non appena chiamato (a differenza
                    // di LVGL non serve un flush separato): basta un
                    // breve delay prima di aprire la voce, cosi' si fa in
                    // tempo a vedere quale e' stata toccata.
                    delay(150);

                    selectCurrentItem();
                    break;
                }
            }
            return;
        }

        switch (event) {
            case InputEvent::Up: {
                int previous = selectedItem;
                moveUp();
                redrawAfterMove(previous);
                break;
            }

            case InputEvent::Down: {
                int previous = selectedItem;
                moveDown();
                redrawAfterMove(previous);
                break;
            }

            case InputEvent::Left: {
                int previous = selectedItem;
                moveLeft();
                redrawAfterMove(previous);
                break;
            }

            case InputEvent::Right: {
                int previous = selectedItem;
                moveRight();
                redrawAfterMove(previous);
                break;
            }

            case InputEvent::Select:
                selectCurrentItem();
                break;

            default:
                break;
        }

        return;
    }

    // Schermate ancora su LVGL raggiunte da qui (placeholder
    // Display::showMessage per le voci non ancora implementate): Indietro
    // o Select tornano al menu, stesso comportamento di sempre.
    if (event == InputEvent::Back || event == InputEvent::Select) {
        back();
    }
}


void showMain() {
    mainMenuVisible = true;
    Display::setDirectDrawMode(true);
    needsRedraw = true;
}


void showCurrentScreen() {
    // Riservato a future schermate del menu
}


void back() {
    selectedItem = 0;
    topVisibleRow = 0;
    dragAccumY = 0;
    showMain();
}


bool isMainMenu() {
    return mainMenuVisible;
}

} // namespace Menu
