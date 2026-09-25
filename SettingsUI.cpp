#include "SettingsUI.h"
#include "Display.h"
#include "Board.h"
#include "Input.h"
#include "Menu.h"
#include "WifiManager.h"
#include "GPS.h"
#include "BleChat.h"
#include "LoRaRadio.h"
#include "Sound.h"

#include <Arduino.h>
#include <lvgl.h>

namespace {

    constexpr int BRIGHTNESS_STEP = 10;
    constexpr int SCREEN_BRIGHTNESS_MIN = 10;   // schermo: mai a 0, illeggibile/al buio altrimenti
    constexpr int KEYBOARD_BRIGHTNESS_MIN = 0;  // tastiera: puo' essere spenta del tutto
    constexpr int SOUND_MIN = 0;                // suono: 0 = silenziato del tutto
    constexpr int GMT_STEP = 1;
    constexpr int GMT_MIN = -12;
    constexpr int GMT_MAX = 14;

    // Switch prima, i livelli regolabili per ultimi (li si tocca meno spesso).
    enum Row {
        ROW_WIFI = 0,
        ROW_GPS = 1,
        ROW_BLUETOOTH = 2,
        ROW_LORA = 3,
        ROW_SCREEN = 4,
        ROW_KEYBOARD = 5,
        ROW_SOUND = 6,
        ROW_GMT = 7,
        ROW_COUNT
    };

    bool isGmtRow(int row) {
        return row == ROW_GMT;
    }

    bool isToggleRow(int row) {
        return row == ROW_WIFI || row == ROW_GPS || row == ROW_BLUETOOTH || row == ROW_LORA;
    }

    bool getToggleState(int row) {
        switch (row) {
            case ROW_WIFI:      return WifiManager::isRadioEnabled();
            case ROW_GPS:       return GPS::isEnabled();
            case ROW_BLUETOOTH: return BleChat::isEnabled();
            case ROW_LORA:      return LoRaRadio::isEnabled();
            default:            return false;
        }
    }

    void setToggleState(int row, bool enabled) {
        switch (row) {
            case ROW_WIFI:      WifiManager::setRadioEnabled(enabled); break;
            case ROW_GPS:       GPS::setEnabled(enabled);              break;
            case ROW_BLUETOOTH: BleChat::setEnabled(enabled);          break;
            case ROW_LORA:      LoRaRadio::setEnabled(enabled);        break;
            default:                                                   break;
        }
    }

    bool active = false;
    int selectedRow = ROW_WIFI;

    lv_obj_t* screen = nullptr;
    lv_obj_t* content = nullptr;
    lv_obj_t* rowContainers[ROW_COUNT] = { nullptr };
    lv_obj_t* percentLabels[ROW_COUNT] = { nullptr };
    lv_obj_t* toggleTracks[ROW_COUNT] = { nullptr };
    lv_obj_t* toggleKnobs[ROW_COUNT] = { nullptr };

    // Firma void() semplice: serve sia da callback per il bottone touch
    // "Indietro" di Display::createScreen() sia per l'evento Indietro da
    // tastiera/trackball in update(), cosi' i due percorsi restano identici.
    void closeSettings() {
        active = false;
        Menu::showMain();
    }

    int getRowPercent(int row) {
        switch (row) {
            case ROW_SCREEN:   return Board::getBacklightPercent();
            case ROW_KEYBOARD: return Board::getKeyboardBacklightPercent();
            case ROW_SOUND:    return Sound::getVolumePercent();
            case ROW_GMT:      return GPS::getUtcOffsetHours();
            default:           return 0;
        }
    }

    void setRowPercent(int row, int percent) {
        switch (row) {
            case ROW_SCREEN:   Board::setBacklightPercent(percent);         break;
            case ROW_KEYBOARD: Board::setKeyboardBacklightPercent(percent); break;
            case ROW_SOUND:    Sound::setVolumePercent(percent);            break;
            case ROW_GMT:      GPS::setUtcOffsetHours(percent);             break;
            default:                                                       break;
        }
    }

    int getRowMinPercent(int row) {
        switch (row) {
            case ROW_SCREEN: return SCREEN_BRIGHTNESS_MIN;
            case ROW_SOUND:  return SOUND_MIN;
            case ROW_GMT:    return GMT_MIN;
            default:         return KEYBOARD_BRIGHTNESS_MIN;
        }
    }

    int getRowMax(int row) {
        return isGmtRow(row) ? GMT_MAX : 100;
    }

    int getRowStep(int row) {
        return isGmtRow(row) ? GMT_STEP : BRIGHTNESS_STEP;
    }

    void updatePercentLabel(int row) {
        if (percentLabels[row] == nullptr) {
            return;
        }

        char text[10];
        if (isGmtRow(row)) {
            // "%+d" mette sempre il segno esplicito (+1, -2, +0...), utile
            // qui perche' senza non si distinguerebbe a colpo d'occhio
            // "0" da un valore positivo.
            snprintf(text, sizeof(text), "GMT%+d", getRowPercent(row));
        } else {
            snprintf(text, sizeof(text), "%d%%", getRowPercent(row));
        }
        lv_label_set_text(percentLabels[row], text);
    }

    void changeBrightness(int row, int direction) {
        int delta = direction * getRowStep(row);
        int minValue = getRowMinPercent(row);
        int maxValue = getRowMax(row);

        int newValue = getRowPercent(row) + delta;
        if (newValue < minValue) newValue = minValue;
        if (newValue > maxValue) newValue = maxValue;

        setRowPercent(row, newValue);
        updatePercentLabel(row);
    }

    constexpr int TOGGLE_TRACK_W = 44;
    constexpr int TOGGLE_TRACK_H = 22;
    constexpr int TOGGLE_KNOB_SIZE = 18;
    constexpr int TOGGLE_KNOB_MARGIN = 2;

    // Interruttore disegnato a mano (traccia + pallino) invece del widget
    // lv_switch nativo: lv_switch affida la posizione del pallino a un
    // meccanismo di stato/animazione interno (LV_STATE_CHECKED +
    // lv_obj_add_state/clear_state) che in questo progetto si era gia'
    // rivelato inaffidabile per l'evidenziazione del menu principale (si
    // vedano i commenti in Menu.cpp/Display.cpp) - cambiava lo stato
    // internamente ma non sempre si ridisegnava subito. Impostando colore
    // e posizione direttamente con lv_obj_set_style_*/lv_obj_align (come
    // gia' fa in modo affidabile updateRowHighlight() qui sotto) si evita
    // del tutto quel meccanismo.
    void setToggleVisual(int row, bool enabled) {
        if (toggleTracks[row] == nullptr) {
            return;
        }

        lv_obj_set_style_bg_color(
            toggleTracks[row],
            enabled ? Display::COLOR_ACCENT : lv_color_hex(0x333333),
            LV_PART_MAIN
        );

        if (toggleKnobs[row] != nullptr) {
            lv_obj_align(
                toggleKnobs[row],
                enabled ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID,
                enabled ? -TOGGLE_KNOB_MARGIN : TOGGLE_KNOB_MARGIN,
                0
            );
        }
    }

    void flipToggleRow(int row) {
        bool newState = !getToggleState(row);
        setToggleState(row, newState);
        setToggleVisual(row, newState);
    }

    // Evidenzia la riga scelta col trackball (su/giu'), e la porta in
    // vista se e' fuori dall'area visibile: con 7 righe (4 switch + 3 di
    // luminosita') non entrano piu' tutte in una sola schermata senza
    // scorrimento.
    void updateRowHighlight() {
        for (int i = 0; i < ROW_COUNT; i++) {
            if (rowContainers[i] == nullptr) {
                continue;
            }

            bool selected = (i == selectedRow);
            lv_obj_set_style_border_color(
                rowContainers[i],
                selected ? Display::COLOR_ACCENT : lv_color_hex(0x333333),
                LV_PART_MAIN
            );
            lv_obj_set_style_border_width(rowContainers[i], selected ? 2 : 1, LV_PART_MAIN);
        }

        if (rowContainers[selectedRow] != nullptr) {
            lv_obj_scroll_to_view(rowContainers[selectedRow], LV_ANIM_OFF);
        }
    }

    lv_obj_t* createRoundButton(lv_obj_t* parent, const char* symbol, lv_event_cb_t cb, void* userData, int diameter) {
        lv_obj_t* button = lv_btn_create(parent);
        lv_obj_set_size(button, diameter, diameter);
        lv_obj_set_style_radius(button, diameter / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x0A1A2A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(button, Display::COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, userData);

        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, symbol);
        lv_obj_set_style_text_color(label, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_center(label);

        return button;
    }

    void decreaseCb(lv_event_t* event) {
        int row = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
        selectedRow = row;
        updateRowHighlight();
        changeBrightness(row, -1);
    }

    void increaseCb(lv_event_t* event) {
        int row = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
        selectedRow = row;
        updateRowHighlight();
        changeBrightness(row, 1);
    }

    void toggleTrackCb(lv_event_t* event) {
        int row = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));

        selectedRow = row;
        flipToggleRow(row);
        updateRowHighlight();
    }

    constexpr int BRIGHTNESS_ROW_HEIGHT = 48;
    constexpr int BRIGHTNESS_BUTTON_SIZE = 32;
    constexpr int TOGGLE_ROW_HEIGHT = 44;
    constexpr int ROW_GAP = 6;

    lv_obj_t* createRowBox(lv_obj_t* parent, int row, int height, int yOffset) {
        lv_obj_t* rowBox = lv_obj_create(parent);
        lv_obj_remove_style_all(rowBox);
        lv_obj_set_size(rowBox, LV_PCT(100), height);
        lv_obj_align(rowBox, LV_ALIGN_TOP_MID, 0, yOffset);
        lv_obj_set_style_border_width(rowBox, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(rowBox, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_all(rowBox, 6, LV_PART_MAIN);
        lv_obj_clear_flag(rowBox, LV_OBJ_FLAG_SCROLLABLE);
        rowContainers[row] = rowBox;
        return rowBox;
    }

    // Riga compatta su una sola linea (etichetta a sinistra, controlli a
    // destra): prima la percentuale stava su una riga separata sotto
    // l'etichetta, occupando quasi il doppio dello spazio verticale.
    // Stessa struttura (2 figli, SPACE_BETWEEN) delle righe switch sotto.
    void buildBrightnessRow(lv_obj_t* parent, int row, const char* caption, int yOffset) {
        lv_obj_t* rowBox = createRowBox(parent, row, BRIGHTNESS_ROW_HEIGHT, yOffset);
        lv_obj_set_layout(rowBox, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(rowBox, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(
            rowBox,
            LV_FLEX_ALIGN_SPACE_BETWEEN,
            LV_FLEX_ALIGN_CENTER,
            LV_FLEX_ALIGN_CENTER
        );

        lv_obj_t* caption_label = lv_label_create(rowBox);
        lv_label_set_text(caption_label, caption);
        lv_obj_set_style_text_color(caption_label, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(caption_label, &lv_font_montserrat_14, 0);

        // [-] [percentuale] [+] raggruppati insieme: cosi' la riga esterna
        // ha solo 2 figli (etichetta + gruppo controlli) e SPACE_BETWEEN
        // li mette uno a sinistra e l'altro a destra, come nelle righe
        // switch sotto - l'etichetta resta "allineata con" la percentuale
        // sulla stessa linea invece che sopra.
        lv_obj_t* controlsGroup = lv_obj_create(rowBox);
        lv_obj_remove_style_all(controlsGroup);
        lv_obj_set_size(controlsGroup, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_layout(controlsGroup, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(controlsGroup, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(controlsGroup, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(controlsGroup, 6, LV_PART_MAIN);
        lv_obj_clear_flag(controlsGroup, LV_OBJ_FLAG_SCROLLABLE);

        void* userData = reinterpret_cast<void*>(static_cast<intptr_t>(row));

        createRoundButton(controlsGroup, LV_SYMBOL_MINUS, decreaseCb, userData, BRIGHTNESS_BUTTON_SIZE);

        percentLabels[row] = lv_label_create(controlsGroup);
        lv_obj_set_style_text_color(percentLabels[row], Display::COLOR_ACCENT, 0);
        lv_obj_set_style_text_font(percentLabels[row], &lv_font_montserrat_14, 0);
        updatePercentLabel(row);

        createRoundButton(controlsGroup, LV_SYMBOL_PLUS, increaseCb, userData, BRIGHTNESS_BUTTON_SIZE);
    }

    void buildToggleRow(lv_obj_t* parent, int row, const char* caption, int yOffset) {
        lv_obj_t* rowBox = createRowBox(parent, row, TOGGLE_ROW_HEIGHT, yOffset);

        lv_obj_t* caption_label = lv_label_create(rowBox);
        lv_label_set_text(caption_label, caption);
        lv_obj_set_style_text_color(caption_label, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(caption_label, &lv_font_montserrat_14, 0);
        lv_obj_align(caption_label, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* track = lv_obj_create(rowBox);
        lv_obj_remove_style_all(track);
        lv_obj_set_size(track, TOGGLE_TRACK_W, TOGGLE_TRACK_H);
        lv_obj_set_style_radius(track, TOGGLE_TRACK_H / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(track, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(track, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            track,
            toggleTrackCb,
            LV_EVENT_CLICKED,
            reinterpret_cast<void*>(static_cast<intptr_t>(row))
        );

        lv_obj_t* knob = lv_obj_create(track);
        lv_obj_remove_style_all(knob);
        lv_obj_set_size(knob, TOGGLE_KNOB_SIZE, TOGGLE_KNOB_SIZE);
        lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(knob, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(knob, LV_OBJ_FLAG_CLICKABLE);

        toggleTracks[row] = track;
        toggleKnobs[row] = knob;

        setToggleVisual(row, getToggleState(row));
    }

    void buildScreen() {
        screen = Display::createScreen("Impostazioni", &content, closeSettings);

        // Con 6 righe (4 switch + 2 di luminosita') non entrano tutte
        // nell'area visibile: Display::createScreen() disattiva lo
        // scroll di default sul content, va riabilitato esplicitamente.
        lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(content, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

        int yOffset = 0;

        buildToggleRow(content, ROW_WIFI, "WiFi", yOffset);
        yOffset += TOGGLE_ROW_HEIGHT + ROW_GAP;

        buildToggleRow(content, ROW_GPS, "GPS", yOffset);
        yOffset += TOGGLE_ROW_HEIGHT + ROW_GAP;

        buildToggleRow(content, ROW_BLUETOOTH, "Bluetooth", yOffset);
        yOffset += TOGGLE_ROW_HEIGHT + ROW_GAP;

        buildToggleRow(content, ROW_LORA, "LoRa", yOffset);
        yOffset += TOGGLE_ROW_HEIGHT + ROW_GAP;

        buildBrightnessRow(content, ROW_SCREEN, "Schermo", yOffset);
        yOffset += BRIGHTNESS_ROW_HEIGHT + ROW_GAP;

        buildBrightnessRow(content, ROW_KEYBOARD, "Tastiera", yOffset);
        yOffset += BRIGHTNESS_ROW_HEIGHT + ROW_GAP;

        buildBrightnessRow(content, ROW_SOUND, "Suono", yOffset);
        yOffset += BRIGHTNESS_ROW_HEIGHT + ROW_GAP;

        buildBrightnessRow(content, ROW_GMT, "GMT", yOffset);
        yOffset += BRIGHTNESS_ROW_HEIGHT + ROW_GAP;

        updateRowHighlight();

        // Testo corto, una sola riga: il tasto "Indietro" e' gia' visibile
        // in alto a destra, ripeterlo qui costava spazio che con 8 righe
        // non c'e' piu'.
        lv_obj_t* hint = lv_label_create(content);
        lv_label_set_text(hint, "Su/giu': riga - Sinistra/Destra: regola/attiva");
        lv_obj_set_style_text_color(hint, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, yOffset);
    }

} // namespace


namespace SettingsUI {

    void open() {
        active = true;
        selectedRow = ROW_WIFI;

        if (screen != nullptr) {
            lv_obj_del(screen);
            screen = nullptr;
        }

        buildScreen();
        lv_scr_load(screen);
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
                // Si ferma alla prima riga invece di "avvolgersi" all'ultima
                // (stesso comportamento del menu principale).
                if (selectedRow > 0) {
                    selectedRow--;
                    updateRowHighlight();
                }
                break;

            case InputEvent::Down:
                // Si ferma all'ultima riga invece di "avvolgersi" alla prima.
                if (selectedRow < ROW_COUNT - 1) {
                    selectedRow++;
                    updateRowHighlight();
                }
                break;

            case InputEvent::Left:
                if (isToggleRow(selectedRow)) {
                    flipToggleRow(selectedRow);
                } else {
                    changeBrightness(selectedRow, -1);
                }
                break;

            case InputEvent::Right:
                if (isToggleRow(selectedRow)) {
                    flipToggleRow(selectedRow);
                } else {
                    changeBrightness(selectedRow, 1);
                }
                break;

            case InputEvent::Select:
                if (isToggleRow(selectedRow)) {
                    flipToggleRow(selectedRow);
                }
                break;

            case InputEvent::Back:
                closeSettings();
                break;

            default:
                break;
        }
    }
}
