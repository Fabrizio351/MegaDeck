#include "NetScanUI.h"
#include "NetScan.h"
#include "NetGuard.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "Menu.h"
#include "NetToolsMenu.h"
#include "ConnStatus.h"

#include <Arduino.h>

// Fase 3 della riscrittura senza LVGL (vedi project_lvgl_removal_rewrite
// in memoria): NetScanUI convertita a disegno diretto - prima vera
// lista scorrevole di contenuto dinamico (non un menu a griglia): stesso
// scorrimento "a righe intere" gia' usato nel menu principale, applicato
// qui ai dispositivi trovati.
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

    int lastScanPercent = -1;
    int topVisibleRow = 0;

    constexpr int16_t LIST_TOP = Display::DD_HEADER_H + 56;
    constexpr int16_t ROW_H = 24;
    constexpr int VISIBLE_ROWS = (240 - LIST_TOP) / ROW_H;

    constexpr int16_t BTN_X = 8;
    constexpr int16_t BTN_W = 304;
    constexpr int16_t BTN_H = 28;

    int stopZone = -1;
    int rescanZone = -1;
    int keepZone = -1;
    int updateZone = -1;

    void showScanningScreen();
    void showResultsScreen();
    void showConfirmRescanScreen();
    void drawResultsScreen();

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

    void keepList() {
        showResultsScreen();
    }

    void drawHeaderTitle(const char* title) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print(title);

        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(228, Display::DD_HEADER_H + 8);
        gfx->print("[B] INDIETRO");
    }

    void drawButton(int16_t y, const char* label, bool focused = true) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->drawRect(BTN_X, y, BTN_W, BTN_H, focused ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 12, y + BTN_H / 2 - 3);
        gfx->print(label);
    }

    void drawConfirmRescanScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("NETSCAN");

        char summary[96];
        snprintf(
            summary, sizeof(summary),
            "Hai gia' una whitelist salvata (%d dispositivi). Mantenerla o aggiornarla con una nuova scansione?",
            NetScan::getDeviceCount()
        );
        Display::drawWrappedText(8, Display::DD_HEADER_H + 26, 304, summary, Display::DD_TEXT);

        TouchUI::clearZones();
        drawButton(150, "[ OK ] MANTIENI LISTA ATTUALE", confirmFocus == ConfirmFocus::Keep);
        keepZone = TouchUI::addZone(BTN_X, 150, BTN_W, BTN_H);
        drawButton(184, "[ ~ ] AGGIORNA ORA", confirmFocus == ConfirmFocus::UpdateNow);
        updateZone = TouchUI::addZone(BTN_X, 184, BTN_W, BTN_H);
    }

    void showConfirmRescanScreen() {
        mode = Mode::ConfirmRescan;
        confirmFocus = ConfirmFocus::Keep;
        drawConfirmRescanScreen();
    }

    void handleConfirmRescanInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == keepZone) {
            keepList();
            return;
        }
        if (tapped == updateZone) {
            startManualRescan();
            return;
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
            case InputEvent::Down:
                confirmFocus = (confirmFocus == ConfirmFocus::Keep) ? ConfirmFocus::UpdateNow : ConfirmFocus::Keep;
                drawConfirmRescanScreen();
                break;

            case InputEvent::Select:
                if (confirmFocus == ConfirmFocus::Keep) {
                    keepList();
                } else {
                    startManualRescan();
                }
                break;

            case InputEvent::Back:
                keepList();
                break;

            default:
                break;
        }
    }

    void drawScanningScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("NETSCAN");

        char text[48];
        snprintf(text, sizeof(text), "Scansione della rete in corso... %d%%", NetScan::getProgressPercent());
        Display::drawWrappedText(8, Display::DD_HEADER_H + 26, 304, text, Display::DD_TEXT);

        TouchUI::clearZones();
        drawButton(200, "[ STOP ] INTERROMPI SCANSIONE");
        stopZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
    }

    void showScanningScreen() {
        mode = Mode::Scanning;
        lastScanPercent = -1;
        drawScanningScreen();
    }

    void drawResultsRow(int index) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t y = LIST_TOP + (index - topVisibleRow) * ROW_H;

        gfx->fillRect(0, y, 320, ROW_H, Display::DD_BG);

        NetScan::Device dev = NetScan::getDevice(index);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_TEXT);
        gfx->setCursor(8, y + 1);
        gfx->print(dev.ip);
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(8, y + 12);
        gfx->print(dev.mac);

        gfx->drawFastHLine(0, y + ROW_H - 1, 320, Display::DD_ACCENT_DIM);
    }

    void drawResultsScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("NETSCAN");

        int count = NetScan::getDeviceCount();
        char summary[32];
        snprintf(summary, sizeof(summary), "%d DISPOSITIVI TROVATI", count);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 24);
        gfx->print(summary);

        TouchUI::clearZones();
        drawButton(Display::DD_HEADER_H + 34, "[ RESCAN ] SCANSIONA DI NUOVO");
        rescanZone = TouchUI::addZone(BTN_X, Display::DD_HEADER_H + 34, BTN_W, BTN_H);

        gfx->fillRect(0, LIST_TOP, 320, 240 - LIST_TOP, Display::DD_BG);

        int lastVisible = topVisibleRow + VISIBLE_ROWS - 1;
        if (lastVisible >= count) {
            lastVisible = count - 1;
        }
        for (int i = topVisibleRow; i <= lastVisible; i++) {
            drawResultsRow(i);
        }
    }

    void showResultsScreen() {
        mode = Mode::Results;
        topVisibleRow = 0;
        drawResultsScreen();
    }

    void handleScanningUpdate() {
        // NetScan::update() e' ora chiamata incondizionatamente da
        // App::update(): qui ci si limita a leggere lo stato, non a
        // farla avanzare.
        int percent = NetScan::getProgressPercent();
        if (percent != lastScanPercent) {
            lastScanPercent = percent;
            char text[48];
            snprintf(text, sizeof(text), "Scansione della rete in corso... %d%%", percent);

            // drawWrappedText() non cancella da sola l'area prima di
            // scrivere (a differenza delle altre schermate, che lo fanno
            // esplicitamente prima di chiamarla): senza questo, quando la
            // percentuale cambia numero di cifre (es. "100%" -> "5%")
            // restavano pezzi del testo precedente sovrapposti - con
            // ~100 aggiornamenti durante una scansione lo schermo
            // sembrava "rotto"/bloccato pur non essendolo (nessun evento
            // nello storico crash lo conferma).
            Arduino_GFX* gfx = Display::getGfx();
            gfx->fillRect(0, Display::DD_HEADER_H + 24, 320, 22, Display::DD_BG);
            Display::drawWrappedText(8, Display::DD_HEADER_H + 26, 304, text, Display::DD_TEXT);
        }

        if (!NetScan::isScanning()) {
            showResultsScreen();
            return;
        }

        int tapped = TouchUI::pollTap();
        if (tapped == stopZone) {
            closeAndReturnToMenu();
            return;
        }

        InputEvent event = Input::getEvent();
        if (event == InputEvent::Back || event == InputEvent::Select) {
            closeAndReturnToMenu();
        }
    }

    void handleResultsInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == rescanZone) {
            requestRescan();
            return;
        }

        int count = NetScan::getDeviceCount();

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (topVisibleRow > 0) {
                    topVisibleRow--;
                    drawResultsScreen();
                }
                break;

            case InputEvent::Down:
                if (topVisibleRow + VISIBLE_ROWS < count) {
                    topVisibleRow++;
                    drawResultsScreen();
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

        Display::setDirectDrawMode(true);

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
