#include "SettingsUI.h"
#include "Display.h"
#include "Board.h"
#include "Input.h"
#include "TouchUI.h"
#include "Menu.h"
#include "WifiManager.h"
#include "GPS.h"
#include "BleGatt.h"
#include "Sound.h"
#include "OpenClawUI.h"
#include "WebRadioUI.h"
#include "MeshtasticUI.h"

#include <Arduino.h>

// Fase 2 della riscrittura senza LVGL (vedi project_lvgl_removal_rewrite
// in memoria): schermata Impostazioni convertita a disegno diretto,
// ultima della fase. Righe piu' compatte di prima (24px invece di 44-48)
// per stare tutte in una schermata sola senza bisogno di scorrimento.
// Ogni riga ha due zone touch (meta' sinistra/destra): su una riga
// interruttore entrambe attivano/disattivano, su una riga di livello la
// sinistra decrementa e la destra incrementa.
//
// Selezione in due passi (richiesto 26/09, vedi "armed" sotto): il primo
// tap su una riga (o il primo Invio/Sinistra/Destra da tastiera dopo
// essersi spostati con Su/Giu') la seleziona e basta, SENZA cambiarne il
// valore - serve un secondo tap sulla stessa riga (o un secondo Invio/
// Sinistra/Destra) per modificarla davvero. Prima di questa modifica,
// scorrendo le voci con dito o trackball capitava di cambiare
// un'impostazione senza volerlo.
namespace {

    constexpr int BRIGHTNESS_STEP = 10;
    constexpr int SCREEN_BRIGHTNESS_MIN = 10;   // schermo: mai a 0, illeggibile/al buio altrimenti
    constexpr int KEYBOARD_BRIGHTNESS_MIN = 0;  // tastiera: puo' essere spenta del tutto
    constexpr int SOUND_MIN = 0;                // suono: 0 = silenziato del tutto
    constexpr int GMT_STEP = 1;
    constexpr int GMT_MIN = -12;
    constexpr int GMT_MAX = 14;

    // Riga "LORA" rimossa il 02/10 insieme al vecchio protocollo di chat
    // custom che l'alimentava (LoRaRadio::isEnabled()/setEnabled()) -
    // oggi la radio LoRa non ha piu' un interruttore generale di questo
    // tipo, ogni feature (Meshtastic/LoRaWAN/LoRaScan) la prende e la
    // rilascia da sola quando serve.
    enum Row {
        ROW_WIFI = 0,
        ROW_GPS = 1,
        ROW_BLUETOOTH = 2,
        ROW_SCREEN = 3,
        ROW_KEYBOARD = 4,
        ROW_SOUND = 5,
        ROW_GMT = 6,
        // Preset modem Meshtastic (02/10, richiesto: sempre piu'
        // radioamatori in Europa passano da LongFast a MediumFast,
        // serve poter switchare per restare compatibili con la mesh
        // locale) - vedi MeshtasticUI::isMediumFastPreset()/
        // setMediumFastPreset(). Trattata come le righe interruttore
        // (isToggleRow sotto) per riusare lo stesso meccanismo di
        // selezione/conferma a due passi, ma con un'etichetta propria
        // invece di "ON"/"OFF" (vedi drawRow()).
        ROW_LORA_PRESET = 7,
        // Righe "azione" (29/09, richiesto: "tenere tutto ordinato" -
        // niente ON/OFF o percentuale, apri e basta una schermata di
        // configurazione altrove nel firmware).
        ROW_OPENCLAW = 8,
        ROW_WEBRADIO = 9,
        ROW_COUNT
    };

    const char* ROW_CAPTION[ROW_COUNT] = {
        "WIFI", "GPS", "BLUETOOTH",
        "SCHERMO", "TASTIERA", "SUONO", "GMT",
        "PRESET LORA",
        "OPENCLAW", "WEB RADIO"
    };

    bool isGmtRow(int row) {
        return row == ROW_GMT;
    }

    bool isToggleRow(int row) {
        return row == ROW_WIFI || row == ROW_GPS || row == ROW_BLUETOOTH || row == ROW_LORA_PRESET;
    }

    bool isActionRow(int row) {
        return row == ROW_OPENCLAW || row == ROW_WEBRADIO;
    }

    bool getToggleState(int row) {
        switch (row) {
            case ROW_WIFI:         return WifiManager::isRadioEnabled();
            case ROW_GPS:          return GPS::isEnabled();
            case ROW_BLUETOOTH:    return BleGatt::isEnabled();
            case ROW_LORA_PRESET:  return MeshtasticUI::isMediumFastPreset();
            default:               return false;
        }
    }

    void setToggleState(int row, bool enabled) {
        switch (row) {
            case ROW_WIFI:         WifiManager::setRadioEnabled(enabled);        break;
            case ROW_GPS:          GPS::setEnabled(enabled);                     break;
            case ROW_BLUETOOTH:    BleGatt::setEnabled(enabled);                 break;
            case ROW_LORA_PRESET:  MeshtasticUI::setMediumFastPreset(enabled);   break;
            default:                                                            break;
        }
    }

    bool active = false;
    int selectedRow = ROW_WIFI;

    // Richiesto il 26/09: prima serve selezionare esplicitamente una riga
    // (tap o click/Invio) prima che Sinistra/Destra/tap possano
    // modificarne il valore - altrimenti scorrendo con dito o trackball
    // capitava di cambiare un'impostazione senza volerlo. "armed" e'
    // vero SOLO per selectedRow, e torna falso ogni volta che la
    // selezione si sposta su un'altra riga (Su/Giu, o un tap su una riga
    // diversa).
    //
    // IMPORTANTE (corretto lo stesso giorno): armare NON puo' avvenire
    // tramite Sinistra/Destra - il trackball genera piu' impulsi
    // sinistra/destra anche per un singolo movimento fisico laterale
    // (rilevato dall'utente: "i valori cambiano subito anche se uso il
    // trackball solo per muovermi"), quindi "primo movimento arma,
    // secondo modifica" si esauriva dentro lo stesso gesto involontario.
    // Solo un'azione discreta (click/Invio o tap) puo' armare - un
    // semplice movimento (Sinistra/Destra) senza armare non fa MAI nulla.
    bool armed = false;

    constexpr int16_t ROW_X = 4;
    constexpr int16_t ROW_W = 312;
    // Ridotta da 24 a 18 (29/09, aggiunte OPENCLAW/WEB RADIO): con 10
    // righe a 24px si sarebbe usciti dai 240px dello schermo (il layout
    // originale a 8 righe era gia' al limite, vedi il commento in testa
    // al file - "per stare tutte in una schermata sola").
    constexpr int16_t ROW_H = 18;
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

    // Apre la schermata di configurazione della riga azione - unica
    // cosa che "confermare" questo tipo di riga puo' fare (niente
    // ON/OFF o percentuale da alternare). Disattiva PRIMA questa
    // schermata (active=false): altrimenti App.cpp continuerebbe a
    // guidare l'input verso SettingsUI::update() invece che verso la
    // schermata appena apertasi (controllata con "else if" in ordine,
    // vedi App.cpp - SettingsUI viene controllato prima delle altre).
    void openActionTarget(int row) {
        active = false;
        switch (row) {
            case ROW_OPENCLAW:  OpenClawUI::openConfig();     break;
            case ROW_WEBRADIO:  WebRadioUI::openEditLinks();  break;
            default:                                          break;
        }
    }

    void drawRow(int row) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t y = ROW_TOP + row * (ROW_H + ROW_GAP);

        gfx->fillRect(ROW_X, y, ROW_W, ROW_H, Display::DD_BG);
        gfx->drawRect(ROW_X, y, ROW_W, ROW_H, Display::DD_ACCENT_DIM);

        if (row == selectedRow) {
            // Chiaro (pronto a modificare) solo se armata, altrimenti
            // attenuato: distingue "riga a fuoco" da "riga selezionata E
            // confermata, Sinistra/Destra la cambiano davvero".
            gfx->fillRect(ROW_X + 3, y + 4, 4, ROW_H - 8, armed ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
            gfx->drawRect(ROW_X, y, ROW_W, ROW_H, armed ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        }

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_TEXT);
        gfx->setCursor(ROW_X + 14, y + ROW_H / 2 - 3);
        gfx->print(ROW_CAPTION[row]);

        char valueText[16];
        if (isActionRow(row)) {
            snprintf(valueText, sizeof(valueText), "[ APRI > ]");
            gfx->setTextColor(Display::DD_ACCENT);
        } else if (row == ROW_LORA_PRESET) {
            bool mediumFast = getToggleState(row);
            snprintf(valueText, sizeof(valueText), mediumFast ? "[MEDIUMFAST]" : "[ LONGFAST ]");
            gfx->setTextColor(Display::DD_ACCENT);
        } else if (isToggleRow(row)) {
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

    // Sposta il focus su un'altra riga: SEMPRE disarma (mai una modifica
    // di valore qui dentro) - usata da Su/Giu' e dal primo tap su una
    // riga diversa da quella attualmente armata.
    void selectRow(int row) {
        int previous = selectedRow;
        selectedRow = row;
        armed = false;
        if (previous != row) {
            drawRow(previous);
        }
        drawRow(row);
    }

    // Conferma la riga gia' a fuoco (deve essere gia' selectedRow):
    // ARMA senza cambiare alcun valore - un tap/Invio/Sinistra/Destra
    // successivo su questa STESSA riga la modifica davvero.
    void armSelectedRow() {
        armed = true;
        drawRow(selectedRow);
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
        gfx->print("SU/GIU: RIGA - INVIO: SELEZIONA - POI REGOLA");
    }

} // namespace


namespace SettingsUI {

    void open() {
        active = true;
        selectedRow = ROW_WIFI;
        armed = false;
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
                bool isLeft = (tapped == rowZoneLeft[row]);
                bool isRight = (tapped == rowZoneRight[row]);
                if (!isLeft && !isRight) {
                    continue;
                }

                if (row != selectedRow || !armed) {
                    // Primo tocco su questa riga (o su una riga diversa
                    // da quella gia' armata): la seleziona e la arma,
                    // NESSUNA modifica di valore - serve un secondo
                    // tocco per cambiarla davvero.
                    selectRow(row);
                    armSelectedRow();
                    return;
                }

                // Riga gia' selezionata E armata: questo tocco applica
                // davvero la modifica (o apre la schermata, per una
                // riga azione).
                if (isActionRow(row)) {
                    openActionTarget(row);
                    return;
                }
                if (isToggleRow(row)) {
                    flipToggleRow(row);
                } else {
                    changeBrightness(row, isLeft ? -1 : 1);
                }
                drawRow(row);
                return;
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
                // Un movimento puro (senza click prima) non fa MAI
                // nulla - vedi il commento su "armed" sopra: il
                // trackball puo' generare piu' impulsi Sinistra/Destra
                // per un solo gesto fisico, quindi armare da qui
                // sarebbe bastato a se stesso a "confermarsi" da solo.
                // Una riga azione non ha niente da "regolare" con
                // Sinistra/Destra (solo Select/Invio la apre).
                if (armed && !isActionRow(selectedRow)) {
                    if (isToggleRow(selectedRow)) {
                        flipToggleRow(selectedRow);
                    } else {
                        changeBrightness(selectedRow, -1);
                    }
                    drawRow(selectedRow);
                }
                break;

            case InputEvent::Right:
                if (armed && !isActionRow(selectedRow)) {
                    if (isToggleRow(selectedRow)) {
                        flipToggleRow(selectedRow);
                    } else {
                        changeBrightness(selectedRow, 1);
                    }
                    drawRow(selectedRow);
                }
                break;

            case InputEvent::Select:
                if (!armed) {
                    armSelectedRow();
                } else if (isActionRow(selectedRow)) {
                    openActionTarget(selectedRow);
                } else if (isToggleRow(selectedRow)) {
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
