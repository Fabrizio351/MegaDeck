#include "SettingsUI.h"
#include "Display.h"
#include "Board.h"
#include "Input.h"
#include "TouchUI.h"
#include "Menu.h"
#include "WifiManager.h"
#include "GPS.h"
#include "BleChat.h"
#include "LoRaRadio.h"
#include "Sound.h"

#include <Arduino.h>

// Fase 2 della riscrittura senza LVGL (vedi project_lvgl_removal_rewrite
// in memoria): schermata Impostazioni convertita a disegno diretto,
// ultima della fase. Righe piu' compatte di prima (24px invece di 44-48)
// per stare tutte in una schermata sola senza bisogno di scorrimento.
// Touch semplificato rispetto ai bottoni tondi +/- di prima: ogni riga
// ha due zone (meta' sinistra e meta' destra) - su una riga interruttore
// entrambe selezionano+attivano/disattivano, su una riga di livello la
// sinistra decrementa e la destra incrementa (sempre selezionando la
// riga). La tastiera/trackball (su/giu'/sinistra/destra/invio) resta
// identica a prima.
namespace {

    constexpr int BRIGHTNESS_STEP = 10;
    constexpr int SCREEN_BRIGHTNESS_MIN = 10;   // schermo: mai a 0, illeggibile/al buio altrimenti
    constexpr int KEYBOARD_BRIGHTNESS_MIN = 0;  // tastiera: puo' essere spenta del tutto
    constexpr int SOUND_MIN = 0;                // suono: 0 = silenziato del tutto
    constexpr int GMT_STEP = 1;
    constexpr int GMT_MIN = -12;
    constexpr int GMT_MAX = 14;

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

    const char* ROW_CAPTION[ROW_COUNT] = {
        "WIFI", "GPS", "BLUETOOTH", "LORA",
        "SCHERMO", "TASTIERA", "SUONO", "GMT"
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

    constexpr int16_t ROW_X = 4;
    constexpr int16_t ROW_W = 312;
    constexpr int16_t ROW_H = 24;
    constexpr int16_t ROW_GAP = 2;
    constexpr int16_t ROW_TOP = Display::DD_HEADER_H + 10;

    int rowZoneLeft[ROW_COUNT];
    int rowZoneRight[ROW_COUNT];

    void closeSettings() {
        active = false;
        Display::setDirectDrawMode(false);
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

    void changeBrightness(int row, int direction) {
        int delta = direction * getRowStep(row);
        int minValue = getRowMinPercent(row);
        int maxValue = getRowMax(row);

        int newValue = getRowPercent(row) + delta;
        if (newValue < minValue) newValue = minValue;
        if (newValue > maxValue) newValue = maxValue;

        setRowPercent(row, newValue);
    }

    void flipToggleRow(int row) {
        setToggleState(row, !getToggleState(row));
    }

    void drawRow(int row) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t y = ROW_TOP + row * (ROW_H + ROW_GAP);

        gfx->fillRect(ROW_X, y, ROW_W, ROW_H, Display::DD_BG);
        gfx->drawRect(ROW_X, y, ROW_W, ROW_H, Display::DD_ACCENT_DIM);

        if (row == selectedRow) {
            gfx->fillRect(ROW_X + 3, y + 4, 4, ROW_H - 8, Display::DD_ACCENT);
        }

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_TEXT);
        gfx->setCursor(ROW_X + 14, y + ROW_H / 2 - 3);
        gfx->print(ROW_CAPTION[row]);

        char valueText[16];
        if (isToggleRow(row)) {
            bool on = getToggleState(row);
            snprintf(valueText, sizeof(valueText), on ? "[ ON ]" : "[ OFF ]");
            gfx->setTextColor(on ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        } else if (isGmtRow(row)) {
            snprintf(valueText, sizeof(valueText), "- GMT%+d +", getRowPercent(row));
            gfx->setTextColor(Display::DD_ACCENT);
        } else {
            snprintf(valueText, sizeof(valueText), "- %d%% +", getRowPercent(row));
            gfx->setTextColor(Display::DD_ACCENT);
        }

        int16_t textW = static_cast<int16_t>(strlen(valueText)) * 6;
        gfx->setCursor(ROW_X + ROW_W - 10 - textW, y + ROW_H / 2 - 3);
        gfx->print(valueText);
    }

    void selectRow(int row) {
        int previous = selectedRow;
        selectedRow = row;
        if (previous != row) {
            drawRow(previous);
        }
        drawRow(row);
    }

    void drawScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8 - 10);
        gfx->print("IMPOSTAZIONI");

        TouchUI::clearZones();
        for (int row = 0; row < ROW_COUNT; row++) {
            int16_t y = ROW_TOP + row * (ROW_H + ROW_GAP);
            rowZoneLeft[row] = TouchUI::addZone(ROW_X, y, ROW_W / 2, ROW_H);
            rowZoneRight[row] = TouchUI::addZone(ROW_X + ROW_W / 2, y, ROW_W - ROW_W / 2, ROW_H);
            drawRow(row);
        }

        int16_t hintY = ROW_TOP + ROW_COUNT * (ROW_H + ROW_GAP) + 4;
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(ROW_X, hintY);
        gfx->print("SU/GIU: RIGA - SINISTRA/DESTRA: REGOLA");
    }

} // namespace


namespace SettingsUI {

    void open() {
        active = true;
        selectedRow = ROW_WIFI;
        Display::setDirectDrawMode(true);
        drawScreen();
    }

    bool isActive() {
        return active;
    }

    void update() {
        if (!active) {
            return;
        }

        int tapped = TouchUI::pollTap();
        if (tapped != -1) {
            for (int row = 0; row < ROW_COUNT; row++) {
                if (tapped == rowZoneLeft[row]) {
                    selectRow(row);
                    if (isToggleRow(row)) {
                        flipToggleRow(row);
                    } else {
                        changeBrightness(row, -1);
                    }
                    drawRow(row);
                    return;
                }
                if (tapped == rowZoneRight[row]) {
                    selectRow(row);
                    if (isToggleRow(row)) {
                        flipToggleRow(row);
                    } else {
                        changeBrightness(row, 1);
                    }
                    drawRow(row);
                    return;
                }
            }
        }

        InputEvent event = Input::getEvent();

        switch (event) {
            case InputEvent::Up:
                // Si ferma alla prima riga invece di "avvolgersi" all'ultima
                // (stesso comportamento del menu principale).
                if (selectedRow > 0) {
                    selectRow(selectedRow - 1);
                }
                break;

            case InputEvent::Down:
                // Si ferma all'ultima riga invece di "avvolgersi" alla prima.
                if (selectedRow < ROW_COUNT - 1) {
                    selectRow(selectedRow + 1);
                }
                break;

            case InputEvent::Left:
                if (isToggleRow(selectedRow)) {
                    flipToggleRow(selectedRow);
                } else {
                    changeBrightness(selectedRow, -1);
                }
                drawRow(selectedRow);
                break;

            case InputEvent::Right:
                if (isToggleRow(selectedRow)) {
                    flipToggleRow(selectedRow);
                } else {
                    changeBrightness(selectedRow, 1);
                }
                drawRow(selectedRow);
                break;

            case InputEvent::Select:
                if (isToggleRow(selectedRow)) {
                    flipToggleRow(selectedRow);
                    drawRow(selectedRow);
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
