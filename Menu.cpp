#include "Menu.h"
#include "Config.h"
#include "Display.h"
#include "MenuIcons.h"
#include "Input.h"

#include <Arduino.h>
#include <stdint.h>

namespace {

constexpr int MENU_ITEM_COUNT = 12;
constexpr int GRID_COLS = 2;
constexpr int GRID_ROWS =
    (MENU_ITEM_COUNT + GRID_COLS - 1) / GRID_COLS;

struct MenuItem {
    int iconIndex;
    const char* text;
};

const MenuItem mainItems[MENU_ITEM_COUNT] = {
    { 0,  "VoIP"  },
    { 1,  "WiFi"  },
    { 2,  "BLE Chat" },
    { 3,  "LoRa Chat" },
    { 4,  "GPS"   },
    { 5,  "NetTools" },
    { 6,  "VPN" },
    { 7,  "OpenClaw.ai" },
    { 8,  "Home Ass." },
    { 9,  "Frigate" },
    { 10, "Drone" },
    { 11, "Impostazioni" }
};

int selectedItem = 0;
bool mainMenuVisible = true;

Menu::ActionCallback actionCallback = nullptr;

lv_obj_t* mainScreen = nullptr;
lv_obj_t* gridContainer = nullptr;

lv_obj_t* buttons[MENU_ITEM_COUNT] = {
    nullptr
};

lv_obj_t* labels[MENU_ITEM_COUNT] = {
    nullptr
};

lv_style_t styleItem;

bool stylesReady = false;

lv_coord_t colDsc[GRID_COLS + 1];
lv_coord_t rowDsc[GRID_ROWS + 1];


// Mappatura 1:1 con mainItems[] sopra: ogni voce visibile nella griglia
// ha la propria azione dedicata, niente piu' indici "a caso" che finivano
// su un'azione diversa da quella scritta sul bottone.
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


void selectCurrentItem() {
    MenuAction action = actionFromIndex(selectedItem);

    mainMenuVisible = false;

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
    // Si ferma al primo elemento della riga invece di "avvolgersi"
    // all'ultimo.
    int start = rowStartIndex(selectedItem);
    int candidate = selectedItem - 1;

    if (candidate < start) {
        return;
    }

    selectedItem = candidate;
}


void moveRight() {
    // Si ferma all'ultimo elemento della riga invece di "avvolgersi"
    // al primo.
    int end = rowEndIndex(selectedItem);
    int candidate = selectedItem + 1;

    if (candidate > end) {
        return;
    }

    selectedItem = candidate;
}


void moveUp() {
    // Si ferma alla prima riga invece di "avvolgersi" all'ultima: scorrere
    // verso l'alto oltre la cima non fa piu' ripartire dal fondo.
    int candidate = selectedItem - GRID_COLS;

    if (candidate < 0) {
        return;
    }

    selectedItem = candidate;
}


void moveDown() {
    // Si ferma all'ultima riga invece di "avvolgersi" alla prima.
    int candidate = selectedItem + GRID_COLS;

    if (candidate >= MENU_ITEM_COUNT) {
        return;
    }

    selectedItem = candidate;
}


void updateHighlight() {
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        if (buttons[i] == nullptr) {
            continue;
        }

        bool selected = (i == selectedItem);

        // lv_obj_add_state()/lv_obj_clear_state() confrontano gli stili
        // prima/dopo (lv_obj_style_state_compare) e possono saltare
        // l'invalidate se ritengono che non ci sia differenza: qui invece
        // impostiamo le proprieta' direttamente con lv_obj_set_style_*,
        // che invalida sempre l'oggetto senza scorciatoie, cosi' il
        // redraw scatta in modo affidabile ad ogni cambio di selezione.
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

    // Con 12 voci su una griglia a 2 colonne, non tutte le righe entrano
    // nell'area visibile: se non si scorre esplicitamente verso il
    // bottone selezionato, muovendosi verso il basso si esce dallo
    // schermo senza che l'utente veda mai le voci successive.
    if (buttons[selectedItem] != nullptr) {
        lv_obj_scroll_to_view(buttons[selectedItem], LV_ANIM_OFF);
    }
}


void buttonClickedCb(lv_event_t* event) {
    if (event == nullptr) {
        return;
    }

    lv_obj_t* button =
        static_cast<lv_obj_t*>(
            lv_event_get_target(event)
        );

    if (button == nullptr) {
        return;
    }

    int index =
        static_cast<int>(
            reinterpret_cast<intptr_t>(
                lv_obj_get_user_data(button)
            )
        );

    if (index < 0 || index >= MENU_ITEM_COUNT) {
        return;
    }

    DEBUG_PRINTF(
        "[MENU] click LVGL su bottone indice %d\n",
        index
    );

    selectedItem = index;
    updateHighlight();

    // Con il trackball l'evidenziazione resta visibile finche' non si
    // preme Select; con un tap diretto invece evidenziazione e apertura
    // della voce avverrebbero nello stesso identico ciclo, e il frame con
    // l'icona evidenziata non farebbe mai in tempo a essere disegnato
    // fisicamente sullo schermo. Disegniamolo esplicitamente ora e
    // teniamolo visibile un istante prima di aprire la voce, cosi' anche
    // col touch si vede quale bottone e' stato toccato.
    Display::update();
    delay(150);

    selectCurrentItem();
}


void initStyles() {
    if (stylesReady) {
        return;
    }

    lv_style_init(&styleItem);

    lv_style_set_bg_color(
        &styleItem,
        lv_color_hex(0x0A1A2A)
    );

    lv_style_set_bg_opa(
        &styleItem,
        LV_OPA_COVER
    );

    lv_style_set_border_color(
        &styleItem,
        Display::COLOR_ACCENT
    );

    lv_style_set_border_width(
        &styleItem,
        1
    );

    lv_style_set_radius(
        &styleItem,
        4
    );

    lv_style_set_text_color(
        &styleItem,
        Display::COLOR_TEXT
    );

    lv_style_set_text_font(
        &styleItem,
        &lv_font_montserrat_14
    );

        // Spaziatura verticale ridotta
        lv_style_set_pad_top(
            &styleItem,
            2
        );

        lv_style_set_pad_bottom(
            &styleItem,
            2
        );

        // Spaziatura orizzontale
        lv_style_set_pad_left(
            &styleItem,
            4
        );

        lv_style_set_pad_right(
            &styleItem,
            4
        );

    lv_style_set_shadow_width(
        &styleItem,
        0
    );


    stylesReady = true;
}


void buildMainScreen() {
    initStyles();

    // Titolo vuoto: il menu principale non ne ha bisogno, c'e' gia' la
    // barra di stato persistente in alto (data/ora + batteria).
    mainScreen = Display::createScreen(
        "",
        &gridContainer
    );

    if (mainScreen == nullptr ||
        gridContainer == nullptr) {
        DEBUG_PRINTLN(
            "[MENU] Errore: impossibile creare la schermata"
        );

        return;
    }


    // Definizione delle colonne
    for (int i = 0; i <= GRID_COLS; i++) {
        colDsc[i] =
            (i == GRID_COLS)
                ? LV_GRID_TEMPLATE_LAST
                : LV_GRID_FR(1);
    }


    // Definizione delle righe.
    // Ogni riga è alta 60 pixel.
    for (int i = 0; i <= GRID_ROWS; i++) {
        rowDsc[i] =
            (i == GRID_ROWS)
                ? LV_GRID_TEMPLATE_LAST
                : 44;
    }


    lv_obj_set_grid_dsc_array(
        gridContainer,
        colDsc,
        rowDsc
    );

    lv_obj_set_layout(
        gridContainer,
        LV_LAYOUT_GRID
    );


    // Abilita lo scorrimento verticale. Display::createScreen() disattiva
    // lo scroll di default sul content (i normali screen di messaggio non
    // ne hanno bisogno) con lv_obj_clear_flag(..., LV_OBJ_FLAG_SCROLLABLE):
    // qui lo dobbiamo riabilitare esplicitamente, altrimenti scroll_dir e
    // scrollbar_mode sotto non hanno alcun effetto (lo scroll resta
    // disattivato a livello di flag) e con 12 voci su 2 colonne le righe
    // oltre l'area visibile restano irraggiungibili.
    lv_obj_add_flag(
        gridContainer,
        LV_OBJ_FLAG_SCROLLABLE
    );

    lv_obj_set_scroll_dir(
        gridContainer,
        LV_DIR_VER
    );

    lv_obj_set_scrollbar_mode(
        gridContainer,
        LV_SCROLLBAR_MODE_AUTO
    );


    // Spaziatura della griglia
    lv_obj_set_style_pad_column(
        gridContainer,
        4,
        LV_PART_MAIN
    );

    lv_obj_set_style_pad_row(
        gridContainer,
        4,
        LV_PART_MAIN
    );


    // Creazione dei pulsanti
    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int col = i % GRID_COLS;
        int row = i / GRID_COLS;

        lv_obj_t* button =
            lv_btn_create(gridContainer);

        if (button == nullptr) {
            continue;
        }


        // Posizione del pulsante nella griglia
        lv_obj_set_grid_cell(
            button,
            LV_GRID_ALIGN_STRETCH,
            col,
            1,
            LV_GRID_ALIGN_STRETCH,
            row,
            1
        );


        // Stile base (bordo, padding, font...). Il colore di sfondo/testo
        // da evidenziato/non-evidenziato viene impostato direttamente in
        // updateHighlight() con lv_obj_set_style_*, non tramite lo stato
        // LV_STATE_CHECKED: usare entrambi i meccanismi insieme poteva
        // far apparire due bottoni evidenziati contemporaneamente perche'
        // lo stile a stato e l'impostazione diretta potevano risolversi
        // in ordine diverso a seconda del frame.
        lv_obj_add_style(
            button,
            &styleItem,
            LV_STATE_DEFAULT
        );


        // Indice associato al pulsante
        lv_obj_set_user_data(
            button,
            reinterpret_cast<void*>(
                static_cast<intptr_t>(i)
            )
        );


        // Evento click/touch
        lv_obj_add_event_cb(
            button,
            buttonClickedCb,
            LV_EVENT_CLICKED,
            nullptr
        );

        // Layout orizzontale:
        // icona a sinistra e testo a destra
        lv_obj_set_layout(
            button,
            LV_LAYOUT_FLEX
        );

        lv_obj_set_flex_flow(
            button,
            LV_FLEX_FLOW_ROW
        );

        lv_obj_set_flex_align(
            button,
            LV_FLEX_ALIGN_START,
            LV_FLEX_ALIGN_CENTER,
            LV_FLEX_ALIGN_CENTER
        );
        // Margine dal bordo sinistro
        lv_obj_set_style_pad_left(
            button,
            8,
            LV_PART_MAIN
        );

        // Spazio tra icona e testo
        lv_obj_set_style_pad_column(
            button,
            6,
            LV_PART_MAIN
        );


        // Creazione icona
        createMenuIcon(
            button,
            mainItems[i].iconIndex
        );


        // Creazione testo
        lv_obj_t* label =
            lv_label_create(button);

        if (label != nullptr) {
            lv_label_set_text(
                label,
                mainItems[i].text
            );

            lv_obj_set_style_text_font(
                label,
                &lv_font_montserrat_14,
                LV_PART_MAIN
            );

            lv_obj_set_style_text_color(
                label,
                Display::COLOR_TEXT,
                LV_PART_MAIN
            );
        }


        buttons[i] = button;
        labels[i] = label;
    }

    updateHighlight();
}

} // namespace


namespace Menu {

void begin(ActionCallback callback) {
    actionCallback = callback;

    selectedItem = 0;
    mainMenuVisible = true;

    buildMainScreen();
    showMain();
}


void update() {
    InputEvent event = Input::getEvent();

    if (mainMenuVisible) {
        if (event != InputEvent::None) {
            DEBUG_PRINTF(
                "[MENU] evento ricevuto: %d\n",
                static_cast<int>(event)
            );
        }

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

            default:
                break;
        }

        return;
    }


    // In una schermata secondaria si torna al menu principale con BACK
    // (tasto 'b' sulla tastiera fisica) o con SELECT (click centrale del
    // trackball / tap touch): quelle schermate sono statiche, non c'e'
    // nient'altro da "selezionare", quindi va bene che il click centrale
    // funzioni anche da "indietro". Senza questo, l'unica via d'uscita
    // era il tasto 'b' da tastiera: se la tastiera fisica non risponde
    // (o l'utente prova solo trackball/touch) si resta bloccati in una
    // schermata secondaria con l'impressione che tutto il dispositivo si
    // sia congelato.
    if (event != InputEvent::None) {
        DEBUG_PRINTF(
            "[MENU] (schermata secondaria) evento ricevuto: %d\n",
            static_cast<int>(event)
        );
    }

    if (event == InputEvent::Back || event == InputEvent::Select) {
        back();
    }
}


void showMain() {
    mainMenuVisible = true;

    if (mainScreen != nullptr) {
        lv_scr_load(mainScreen);
    }
}


void showCurrentScreen() {
    // Riservato a future schermate del menu
}


void back() {
    selectedItem = 0;

    showMain();
    updateHighlight();
}


bool isMainMenu() {
    return mainMenuVisible;
}

} // namespace Menu