#include "NetScanUI.h"
#include "NetScan.h"
#include "NetGuard.h"
#include "Display.h"
#include "Input.h"
#include "Menu.h"
#include "NetToolsMenu.h"
#include "ConnStatus.h"

#include <Arduino.h>
#include <lvgl.h>

namespace {

    enum class Mode {
        Closed,
        Scanning,
        Results,
        ConfirmRescan
    };

    Mode mode = Mode::Closed;

    enum class ConfirmFocus {
        Keep,
        UpdateNow
    };
    ConfirmFocus confirmFocus = ConfirmFocus::Keep;

    lv_obj_t* screen = nullptr;
    lv_obj_t* resultsContent = nullptr; // il vero contenitore scorrevole in Results
    lv_obj_t* statusLabel = nullptr; // usata sia in Scanning che come intestazione in Results
    lv_obj_t* keepBtn = nullptr;
    lv_obj_t* updateNowBtn = nullptr;

    // Ultima percentuale mostrata durante la scansione: evita di
    // richiamare lv_label_set_text() (rialloca nel pool LVGL) ad ogni
    // ciclo di App::update() quando la percentuale non e' ancora
    // cambiata - vedi la stessa correzione in NetGuardUI.cpp per il
    // dettaglio del problema.
    int lastScanPercent = -1;

    void showScanningScreen();
    void showResultsScreen();
    void showConfirmRescanScreen();

    void closeAndReturnToMenu() {
        mode = Mode::Closed;
        NetToolsMenu::showMain();
    }

    // Una scansione manuale usa lo stesso motore condiviso che NetGuard
    // usa per il suo giro periodico automatico (vedi NetGuard.h): se
    // NetGuard e' attivo va fermato prima, per non avere due richieste
    // di scansione sullo stesso motore nello stesso momento. L'utente lo
    // riattiva manualmente da NetGuard quando vuole.
    void startManualRescan() {
        if (NetGuard::isEnabled()) {
            NetGuard::setEnabled(false);
        }
        NetScan::start();
        showScanningScreen();
    }

    // Punto d'ingresso per QUALSIASI richiesta di nuova scansione mentre
    // esiste gia' una lista salvata: chiede prima conferma (mantenerla o
    // aggiornarla), invece di sovrascriverla subito senza avviso.
    void requestRescan() {
        if (NetScan::getDeviceCount() > 0) {
            showConfirmRescanScreen();
        } else {
            startManualRescan();
        }
    }

    void rescanCb(lv_event_t* event) {
        (void)event;
        requestRescan();
    }

    void stopScanCb(lv_event_t* event) {
        (void)event;
        closeAndReturnToMenu();
    }

    void keepList() {
        showResultsScreen();
    }

    void keepListCb(lv_event_t* event) {
        (void)event;
        keepList();
    }

    void confirmUpdateCb(lv_event_t* event) {
        (void)event;
        startManualRescan();
    }

    void updateConfirmFocusHighlight() {
        lv_obj_t* buttons[] = { keepBtn, updateNowBtn };
        ConfirmFocus focuses[] = { ConfirmFocus::Keep, ConfirmFocus::UpdateNow };

        for (int i = 0; i < 2; i++) {
            if (buttons[i] == nullptr) {
                continue;
            }
            bool selected = (confirmFocus == focuses[i]);
            lv_obj_set_style_bg_color(
                buttons[i],
                selected ? Display::COLOR_ACCENT : lv_color_hex(0x0A1A2A),
                LV_PART_MAIN
            );
        }
    }

    void buildConfirmRescanScreen() {
        lv_obj_t* content = nullptr;
        screen = Display::createScreen("NetScan", &content, keepList);

        char summary[96];
        snprintf(
            summary, sizeof(summary),
            "Hai gia' una whitelist salvata (%d dispositivi).\nMantenerla o aggiornarla con una nuova scansione?",
            NetScan::getDeviceCount()
        );
        lv_obj_t* label = lv_label_create(content);
        lv_label_set_text(label, summary);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, LV_PCT(100));
        lv_obj_set_style_text_color(label, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

        keepBtn = lv_btn_create(content);
        lv_obj_set_width(keepBtn, LV_PCT(100));
        lv_obj_set_height(keepBtn, 34);
        lv_obj_align(keepBtn, LV_ALIGN_BOTTOM_LEFT, 0, -40);
        lv_obj_set_style_bg_opa(keepBtn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(keepBtn, Display::COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(keepBtn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(keepBtn, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(keepBtn, keepListCb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* keepLabel = lv_label_create(keepBtn);
        lv_label_set_text(keepLabel, LV_SYMBOL_OK " Mantieni lista attuale");
        lv_obj_center(keepLabel);
        lv_obj_set_style_text_color(keepLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(keepLabel, &lv_font_montserrat_14, 0);

        updateNowBtn = lv_btn_create(content);
        lv_obj_set_width(updateNowBtn, LV_PCT(100));
        lv_obj_set_height(updateNowBtn, 34);
        lv_obj_align(updateNowBtn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_bg_opa(updateNowBtn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(updateNowBtn, Display::COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(updateNowBtn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(updateNowBtn, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(updateNowBtn, confirmUpdateCb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* updateNowLabel = lv_label_create(updateNowBtn);
        lv_label_set_text(updateNowLabel, LV_SYMBOL_REFRESH " Aggiorna ora");
        lv_obj_center(updateNowLabel);
        lv_obj_set_style_text_color(updateNowLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(updateNowLabel, &lv_font_montserrat_14, 0);

        updateConfirmFocusHighlight();
    }

    void showConfirmRescanScreen() {
        mode = Mode::ConfirmRescan;
        confirmFocus = ConfirmFocus::Keep;

        if (screen != nullptr) {
            lv_obj_del(screen);
            screen = nullptr;
        }

        buildConfirmRescanScreen();
        lv_scr_load(screen);
    }

    void handleConfirmRescanInput() {
        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
            case InputEvent::Down:
                confirmFocus = (confirmFocus == ConfirmFocus::Keep) ? ConfirmFocus::UpdateNow : ConfirmFocus::Keep;
                updateConfirmFocusHighlight();
                break;

            case InputEvent::Select:
                if (confirmFocus == ConfirmFocus::Keep) {
                    keepListCb(nullptr);
                } else {
                    confirmUpdateCb(nullptr);
                }
                break;

            case InputEvent::Back:
                keepListCb(nullptr);
                break;

            default:
                break;
        }
    }

    void buildScanningScreen() {
        lv_obj_t* content = nullptr;
        screen = Display::createScreen("NetScan", &content, closeAndReturnToMenu);

        statusLabel = lv_label_create(content);
        lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(statusLabel, Display::COLOR_TEXT, 0);
        lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(statusLabel, LV_PCT(100));
        lv_obj_align(statusLabel, LV_ALIGN_TOP_LEFT, 0, 0);

        // Bottone visibile per interrompere: il piccolo "Indietro" touch
        // in alto a destra c'e' gia', ma durante una scansione lunga
        // serve un modo piu' evidente (e raggiungibile anche col
        // trackball, vedi Select in handleScanningUpdate()) per fermarla.
        lv_obj_t* stopBtn = lv_btn_create(content);
        lv_obj_set_width(stopBtn, LV_PCT(100));
        lv_obj_set_height(stopBtn, 34);
        lv_obj_align(stopBtn, LV_ALIGN_TOP_LEFT, 0, 40);
        lv_obj_set_style_bg_color(stopBtn, lv_color_hex(0x0A1A2A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(stopBtn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(stopBtn, Display::COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(stopBtn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(stopBtn, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(stopBtn, stopScanCb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* stopLabel = lv_label_create(stopBtn);
        lv_label_set_text(stopLabel, LV_SYMBOL_STOP " Interrompi scansione");
        lv_obj_center(stopLabel);
        lv_obj_set_style_text_color(stopLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(stopLabel, &lv_font_montserrat_14, 0);
    }

    void showScanningScreen() {
        mode = Mode::Scanning;

        if (screen != nullptr) {
            lv_obj_del(screen);
            screen = nullptr;
        }

        buildScanningScreen();
        lv_label_set_text(statusLabel, "Scansione della rete in corso...");
        lastScanPercent = -1;
        lv_scr_load(screen);
    }

    void buildResultsScreen() {
        lv_obj_t* content = nullptr;
        screen = Display::createScreen("NetScan", &content, closeAndReturnToMenu);
        resultsContent = content;

        lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(content, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_layout(content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(content, 4, LV_PART_MAIN);

        int count = NetScan::getDeviceCount();

        char summary[48];
        snprintf(summary, sizeof(summary), "%d dispositivi trovati:", count);
        lv_obj_t* summaryLabel = lv_label_create(content);
        lv_label_set_text(summaryLabel, summary);
        lv_obj_set_style_text_color(summaryLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(summaryLabel, &lv_font_montserrat_14, 0);

        lv_obj_t* rescanBtn = lv_btn_create(content);
        lv_obj_set_width(rescanBtn, LV_PCT(100));
        lv_obj_set_height(rescanBtn, 34);
        lv_obj_set_style_bg_color(rescanBtn, lv_color_hex(0x0A1A2A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(rescanBtn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(rescanBtn, Display::COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(rescanBtn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(rescanBtn, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(rescanBtn, rescanCb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* rescanLabel = lv_label_create(rescanBtn);
        lv_label_set_text(rescanLabel, LV_SYMBOL_REFRESH " Scansiona di nuovo");
        lv_obj_center(rescanLabel);
        lv_obj_set_style_text_color(rescanLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(rescanLabel, &lv_font_montserrat_14, 0);

        for (int i = 0; i < count; i++) {
            NetScan::Device dev = NetScan::getDevice(i);

            lv_obj_t* row = lv_obj_create(content);
            if (row == nullptr) {
                // Pool oggetti LVGL esaurito: meglio troncare la lista
                // che proseguire scrivendo su puntatori nulli piu' sotto.
                break;
            }
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, LV_SIZE_CONTENT);
            lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(row, lv_color_hex(0x333333), LV_PART_MAIN);
            lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
            lv_obj_set_style_pad_all(row, 6, LV_PART_MAIN);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* rowLabel = lv_label_create(row);
            if (rowLabel == nullptr) {
                break;
            }
            String text = dev.ip + "\n" + dev.mac;
            lv_label_set_text(rowLabel, text.c_str());
            lv_obj_set_style_text_color(rowLabel, Display::COLOR_TEXT, 0);
            lv_obj_set_style_text_font(rowLabel, &lv_font_montserrat_14, 0);
        }
    }

    void showResultsScreen() {
        mode = Mode::Results;

        if (screen != nullptr) {
            lv_obj_del(screen);
            screen = nullptr;
        }

        buildResultsScreen();
        lv_scr_load(screen);
    }

    void handleScanningUpdate() {
        // NetScan::update() e' ora chiamata incondizionatamente da
        // App::update(): qui ci si limita a leggere lo stato, non a
        // farla avanzare.
        int percent = NetScan::getProgressPercent();
        if (percent != lastScanPercent) {
            lastScanPercent = percent;
            char text[48];
            snprintf(
                text, sizeof(text),
                "Scansione della rete in corso... %d%%",
                percent
            );
            lv_label_set_text(statusLabel, text);
        }

        if (!NetScan::isScanning()) {
            showResultsScreen();
            return;
        }

        InputEvent event = Input::getEvent();
        if (event == InputEvent::Back || event == InputEvent::Select) {
            closeAndReturnToMenu();
        }
    }

    void handleResultsInput() {
        InputEvent event = Input::getEvent();
        if (event == InputEvent::None) {
            return;
        }

        // Nessuna voce da "selezionare" qui (solo informazioni in
        // lettura): su/giu' scorrono la lista, Select riavvia la
        // scansione, Indietro esce.
        switch (event) {
            case InputEvent::Up:
                if (resultsContent != nullptr) {
                    lv_obj_scroll_by(resultsContent, 0, 40, LV_ANIM_ON);
                }
                break;

            case InputEvent::Down:
                if (resultsContent != nullptr) {
                    lv_obj_scroll_by(resultsContent, 0, -40, LV_ANIM_ON);
                }
                break;

            case InputEvent::Select:
                requestRescan();
                break;

            case InputEvent::Back:
                closeAndReturnToMenu();
                break;

            default:
                break;
        }
    }

} // namespace


namespace NetScanUI {

    void open() {
        if (!ConnStatus::isWifiConnected()) {
            Display::showMessage(
                "NetScan",
                "Devi prima connetterti a una rete WiFi per poterla scansionare.",
                []() { NetToolsMenu::showMain(); }
            );
            return;
        }

        // Se c'e' gia' una lista da una scansione precedente (anche di
        // una sessione fa, vedi NetGuard che la aggiorna in background),
        // la riproponiamo subito invece di rifare sempre la scansione da
        // zero: si puo' comunque aggiornare col bottone "Scansiona di
        // nuovo" nella schermata risultati.
        if (NetScan::getDeviceCount() > 0) {
            showResultsScreen();
            return;
        }

        NetScan::start();
        showScanningScreen();
    }

    bool isActive() {
        return mode != Mode::Closed;
    }

    void update() {
        switch (mode) {
            case Mode::Closed:
                break;

            case Mode::Scanning:
                handleScanningUpdate();
                break;

            case Mode::Results:
                handleResultsInput();
                break;

            case Mode::ConfirmRescan:
                handleConfirmRescanInput();
                break;
        }
    }
}
