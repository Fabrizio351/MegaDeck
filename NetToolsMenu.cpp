#include "NetToolsMenu.h"
#include "Config.h"
#include "Display.h"
#include "MenuIcons.h"
#include "Input.h"
#include "Menu.h"
#include "NetScanUI.h"
#include "NetGuardUI.h"

#include <Arduino.h>
#include <lvgl.h>

namespace {

    enum class NetToolAction {
        NetScan,
        NetGuard
    };

    struct NetToolItem {
        const char* symbol;
        lv_color_t color;
        const char* text;
        NetToolAction action;
    };

    // Nuove utility di rete si aggiungono qui: la griglia si adatta da
    // sola al numero di voci (colonne fisse, righe calcolate).
    const NetToolItem items[] = {
        { LV_SYMBOL_LIST, lv_color_hex(0xF1C40F), "NetScan", NetToolAction::NetScan },
        { LV_SYMBOL_WARNING, lv_color_hex(0xE74C3C), "NetGuard", NetToolAction::NetGuard },
    };

    constexpr int ITEM_COUNT = sizeof(items) / sizeof(items[0]);
    constexpr int GRID_COLS = 2;
    constexpr int GRID_ROWS = (ITEM_COUNT + GRID_COLS - 1) / GRID_COLS;

    bool active = false;
    int selectedItem = 0;

    lv_obj_t* screen = nullptr;
    lv_obj_t* gridContainer = nullptr;
    lv_obj_t* buttons[ITEM_COUNT] = { nullptr };
    lv_obj_t* labels[ITEM_COUNT] = { nullptr };

    lv_style_t styleItem;
    bool stylesReady = false;

    lv_coord_t colDsc[GRID_COLS + 1];
    lv_coord_t rowDsc[GRID_ROWS + 1];

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

    void moveUp() {
        int candidate = selectedItem - GRID_COLS;
        if (candidate < 0) {
            return;
        }
        selectedItem = candidate;
    }

    void moveDown() {
        int candidate = selectedItem + GRID_COLS;
        if (candidate >= ITEM_COUNT) {
            return;
        }
        selectedItem = candidate;
    }

    void updateHighlight() {
        for (int i = 0; i < ITEM_COUNT; i++) {
            if (buttons[i] == nullptr) {
                continue;
            }

            bool selected = (i == selectedItem);

            lv_obj_set_style_bg_color(
                buttons[i],
                selected ? Display::COLOR_ACCENT : lv_color_hex(0x0A1A2A),
                LV_PART_MAIN
            );

            if (labels[i] != nullptr) {
                lv_obj_set_style_text_color(
                    labels[i],
                    selected ? lv_color_hex(0x000000) : Display::COLOR_TEXT,
                    LV_PART_MAIN
                );
            }
        }

        if (buttons[selectedItem] != nullptr) {
            lv_obj_scroll_to_view(buttons[selectedItem], LV_ANIM_OFF);
        }
    }

    void selectCurrentItem() {
        switch (items[selectedItem].action) {
            case NetToolAction::NetScan:
                NetScanUI::open();
                break;

            case NetToolAction::NetGuard:
                NetGuardUI::open();
                break;
        }
    }

    void buttonClickedCb(lv_event_t* event) {
        if (event == nullptr) {
            return;
        }

        lv_obj_t* button = static_cast<lv_obj_t*>(lv_event_get_target(event));
        if (button == nullptr) {
            return;
        }

        int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
        if (index < 0 || index >= ITEM_COUNT) {
            return;
        }

        selectedItem = index;
        updateHighlight();

        // Come nel menu principale: disegna l'evidenziazione e tienila
        // visibile un istante prima di aprire la voce, altrimenti col
        // touch selezione e apertura avverrebbero nello stesso frame.
        Display::update();
        delay(150);

        selectCurrentItem();
    }

    void initStyles() {
        if (stylesReady) {
            return;
        }

        lv_style_init(&styleItem);
        lv_style_set_bg_color(&styleItem, lv_color_hex(0x0A1A2A));
        lv_style_set_bg_opa(&styleItem, LV_OPA_COVER);
        lv_style_set_border_color(&styleItem, Display::COLOR_ACCENT);
        lv_style_set_border_width(&styleItem, 1);
        lv_style_set_radius(&styleItem, 4);
        lv_style_set_text_color(&styleItem, Display::COLOR_TEXT);
        lv_style_set_text_font(&styleItem, &lv_font_montserrat_14);
        lv_style_set_pad_top(&styleItem, 2);
        lv_style_set_pad_bottom(&styleItem, 2);
        lv_style_set_pad_left(&styleItem, 4);
        lv_style_set_pad_right(&styleItem, 4);
        lv_style_set_shadow_width(&styleItem, 0);

        stylesReady = true;
    }

    void buildScreen() {
        initStyles();

        screen = Display::createScreen("Net Tools", &gridContainer, closeNetTools);

        if (screen == nullptr || gridContainer == nullptr) {
            DEBUG_PRINTLN("[NETTOOLSMENU] Errore: impossibile creare la schermata");
            return;
        }

        for (int i = 0; i <= GRID_COLS; i++) {
            colDsc[i] = (i == GRID_COLS) ? LV_GRID_TEMPLATE_LAST : LV_GRID_FR(1);
        }

        for (int i = 0; i <= GRID_ROWS; i++) {
            rowDsc[i] = (i == GRID_ROWS) ? LV_GRID_TEMPLATE_LAST : 44;
        }

        lv_obj_set_grid_dsc_array(gridContainer, colDsc, rowDsc);
        lv_obj_set_layout(gridContainer, LV_LAYOUT_GRID);

        lv_obj_add_flag(gridContainer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(gridContainer, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(gridContainer, LV_SCROLLBAR_MODE_AUTO);

        lv_obj_set_style_pad_column(gridContainer, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_row(gridContainer, 4, LV_PART_MAIN);

        for (int i = 0; i < ITEM_COUNT; i++) {
            int col = i % GRID_COLS;
            int row = i / GRID_COLS;

            lv_obj_t* button = lv_btn_create(gridContainer);
            if (button == nullptr) {
                continue;
            }

            lv_obj_set_grid_cell(
                button,
                LV_GRID_ALIGN_STRETCH, col, 1,
                LV_GRID_ALIGN_STRETCH, row, 1
            );

            lv_obj_add_style(button, &styleItem, LV_STATE_DEFAULT);
            lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            lv_obj_add_event_cb(button, buttonClickedCb, LV_EVENT_CLICKED, nullptr);

            lv_obj_set_layout(button, LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_left(button, 8, LV_PART_MAIN);
            lv_obj_set_style_pad_column(button, 6, LV_PART_MAIN);

            createMenuIconCustom(button, items[i].symbol, items[i].color);

            lv_obj_t* label = lv_label_create(button);
            if (label != nullptr) {
                lv_label_set_text(label, items[i].text);
                lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
                lv_obj_set_style_text_color(label, Display::COLOR_TEXT, LV_PART_MAIN);
            }

            buttons[i] = button;
            labels[i] = label;
        }

        updateHighlight();
    }

} // namespace

namespace NetToolsMenu {

    void open() {
        active = true;
        selectedItem = 0;

        if (screen == nullptr) {
            buildScreen();
        }

        lv_scr_load(screen);
        updateHighlight();
    }

    void showMain() {
        active = true;

        if (screen != nullptr) {
            lv_scr_load(screen);
        }

        updateHighlight();
    }

    bool isActive() {
        return active;
    }

    void update() {
        if (!active) {
            return;
        }

        InputEvent event = Input::getEvent();

        switch (event) {
            case InputEvent::Up:
                moveUp();
                updateHighlight();
                break;

            case InputEvent::Down:
                moveDown();
                updateHighlight();
                break;

            case InputEvent::Left:
                moveLeft();
                updateHighlight();
                break;

            case InputEvent::Right:
                moveRight();
                updateHighlight();
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
