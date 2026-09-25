#include "Menu.h"
#include "Display.h"

namespace {
    constexpr int MENU_ITEM_COUNT = 12;
    constexpr int GRID_COLS = 3;
    constexpr int GRID_ROWS = (MENU_ITEM_COUNT + GRID_COLS - 1) / GRID_COLS; // 7 righe (ultima parziale)

    const char* mainItems[MENU_ITEM_COUNT] = {
        "|> VoIP",
        "|> Wifi",
        "|> BT",
        "|> LoRA",
        "|> GPS",
        "|> Tools",
        "|> Menu1",
        "|> Menu2",
        "|> Menu3",
        "|> Menu4",
        "|> Menu5",
        "|> Menu6",

    };

    int selectedItem = 0;
    bool mainMenuVisible = true;

    Menu::ActionCallback actionCallback = nullptr;

    MenuAction actionFromIndex(int index) {
        switch (index) {
            case 0:
                return MenuAction::FirstItem;

            case 1:
                return MenuAction::SecondItem;

            case 2:
                return MenuAction::ThirdItem;

            case 3:
                return MenuAction::Settings;

            case 4:
                return MenuAction::About;

            default:
                return MenuAction::About;
        }
    }

    void selectCurrentItem() {
        MenuAction action = actionFromIndex(selectedItem);

        mainMenuVisible = false;

        if (actionCallback != nullptr) {
            actionCallback(action);
        }
    }

    // --- Navigazione a griglia (3 colonne) ---

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
        int end = rowEndIndex(selectedItem);

        selectedItem--;

        if (selectedItem < start) {
            selectedItem = end;
        }
    }

    void moveRight() {
        int start = rowStartIndex(selectedItem);
        int end = rowEndIndex(selectedItem);

        selectedItem++;

        if (selectedItem > end) {
            selectedItem = start;
        }
    }

    void moveUp() {
        int col = selectedItem % GRID_COLS;
        int candidate = selectedItem - GRID_COLS;

        if (candidate < 0) {
            // Vai all'ultima riga, stessa colonna se esiste, altrimenti ultimo elemento
            int lastRowStart = rowStartIndex(MENU_ITEM_COUNT - 1);
            candidate = lastRowStart + col;

            if (candidate >= MENU_ITEM_COUNT) {
                candidate = MENU_ITEM_COUNT - 1;
            }
        }

        selectedItem = candidate;
    }

    void moveDown() {
        int col = selectedItem % GRID_COLS;
        int candidate = selectedItem + GRID_COLS;

        if (candidate >= MENU_ITEM_COUNT) {
            // Torna alla prima riga, stessa colonna
            candidate = col;
        }

        selectedItem = candidate;
    }
}

namespace Menu {

    void begin(ActionCallback callback) {
        actionCallback = callback;
        selectedItem = 0;
        mainMenuVisible = true;

        showMain();
    }

    void update() {
        InputEvent event = Input::getEvent();

        if (mainMenuVisible) {
            switch (event) {
                case InputEvent::Up:
                    moveUp();
                    showMain();
                    break;

                case InputEvent::Down:
                    moveDown();
                    showMain();
                    break;

                case InputEvent::Left:
                    moveLeft();
                    showMain();
                    break;

                case InputEvent::Right:
                    moveRight();
                    showMain();
                    break;

                case InputEvent::Select:
                    selectCurrentItem();
                    break;

                default:
                    break;
            }

            // Tap touch: seleziona direttamente la cella toccata,
            // indipendente dagli eventi di tastiera/trackball sopra.
            if (Input::wasTapped()) {
                int tapped = Display::hitTestGridMenu(
                    Input::getTouchX(),
                    Input::getTouchY(),
                    MENU_ITEM_COUNT,
                    GRID_COLS
                );

                if (tapped >= 0) {
                    selectedItem = tapped;
                    selectCurrentItem();
                }
            }

            return;
        }

        // In una schermata secondaria, BACK ritorna al menu principale
        if (event == InputEvent::Back) {
            back();
        }
    }

    void showMain() {
        mainMenuVisible = true;

        Display::drawGridMenu(
            "Menu principale",
            mainItems,
            MENU_ITEM_COUNT,
            GRID_COLS,
            selectedItem
        );
    }

    void showCurrentScreen() {
        // Riservato a future schermate del menu
    }

    void back() {
        selectedItem = 0;
        showMain();
    }

    bool isMainMenu() {
        return mainMenuVisible;
    }
}
