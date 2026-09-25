#include "NetGuardUI.h"
#include "NetGuard.h"
#include "NetScan.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "NetToolsMenu.h"
#include "ConnStatus.h"

#include <Arduino.h>

// Fase 2 della riscrittura senza LVGL (vedi project_lvgl_removal_rewrite
// in memoria): prima schermata reale convertita a disegno diretto - era
// quella che restava aperta piu' a lungo (pensata come "dashboard") e
// quindi la fonte principale dei riavvii per pool LVGL esaurito (vedi
// project_lvgl_pool_exhaustion_fix). Nessun oggetto/widget LVGL qui
// dentro: solo Arduino_GFX via Display::getGfx(), tap via TouchUI,
// LVGL messo in pausa con Display::setDirectDrawMode() mentre questa
// schermata e' attiva (il resto dell'app, non ancora convertito, resta
// su LVGL).
namespace {

    bool active = false;

    // true mentre e' visibile il messaggio LVGL "serve prima una
    // whitelist" (Display::showMessage, non ancora convertito): in
    // quella finestra update() non deve toccare ne' lo schermo ne' i tap,
    // altrimenti il prossimo ridisegno di NetGuard cancellerebbe il
    // messaggio ancora in vista.
    bool showingSubMessage = false;

    String lastStatusText;

    constexpr int16_t TOGGLE_X = 8;
    constexpr int16_t TOGGLE_Y = 200;
    constexpr int16_t TOGGLE_W = 304;
    constexpr int16_t TOGGLE_H = 30;
    int toggleZone = -1;

    void closeNetGuard() {
        active = false;
        Display::setDirectDrawMode(false);
        NetToolsMenu::showMain();
    }

    String buildStatusText() {
        String text;
        int whitelistCount = NetGuard::getWhitelistCount();

        if (!NetGuard::isEnabled()) {
            text = "Sorveglianza disattivata.";
        } else if (NetGuard::isWaitingForWhitelist()) {
            text = "In attesa: nessuna whitelist. Fai prima un NetScan.";
        } else if (NetGuard::isScanning()) {
            text = "Giro di controllo in corso... " + String(NetScan::getProgressPercent()) + "%";
        } else {
            text = "Sorveglianza attiva (controllo ogni minuto).";
        }

        text += "\n\nWhitelist: " + String(whitelistCount) + " dispositivi";
        if (whitelistCount == 0) {
            text += "\n(vai su NetScan per crearla)";
        }

        String lastAlert = NetGuard::getLastAlertSummary();
        if (lastAlert.length() > 0) {
            text += "\n\nUltimo allarme:\n" + lastAlert;
        }

        return text;
    }

    // Stampa testo multi-riga rispettando i "\n" gia' presenti (buildStatusText()
    // li usa per separare i paragrafi) E spezzando le righe piu' larghe
    // della zona disponibile - il font classico e' a larghezza fissa
    // (6px/carattere a size 1), quindi basta un calcolo diretto, niente
    // getTextBounds().
    void drawWrappedText(int16_t x, int16_t y, int16_t w, const String& text, uint16_t color) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->setTextColor(color);

        constexpr int16_t CHAR_W = 6;
        constexpr int16_t LINE_H = 11;

        int maxChars = w / CHAR_W;
        if (maxChars < 1) {
            maxChars = 1;
        }

        int16_t cursorY = y;
        int pos = 0;
        int len = text.length();

        while (pos <= len) {
            int newlinePos = text.indexOf('\n', pos);
            int lineEnd = (newlinePos == -1) ? len : newlinePos;
            int lineLen = lineEnd - pos;

            if (lineLen == 0) {
                cursorY += LINE_H;
            } else {
                int segStart = pos;
                while (segStart < lineEnd) {
                    int segLen = min(maxChars, lineEnd - segStart);
                    gfx->setCursor(x, cursorY);
                    gfx->print(text.substring(segStart, segStart + segLen));
                    cursorY += LINE_H;
                    segStart += segLen;
                }
            }

            if (newlinePos == -1) {
                break;
            }
            pos = newlinePos + 1;
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
        gfx->print("NETGUARD");

        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(228, Display::DD_HEADER_H + 8);
        gfx->print("[B] INDIETRO");

        String statusText = buildStatusText();
        lastStatusText = statusText;
        drawWrappedText(8, Display::DD_HEADER_H + 26, 304, statusText, Display::DD_TEXT);

        gfx->drawRect(TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H, Display::DD_ACCENT);
        gfx->setTextColor(Display::DD_ACCENT);
        const char* btnLabel = NetGuard::isEnabled()
            ? "DISATTIVA SORVEGLIANZA"
            : "ATTIVA SORVEGLIANZA";
        gfx->setCursor(TOGGLE_X + 12, TOGGLE_Y + TOGGLE_H / 2 - 3);
        gfx->print(btnLabel);

        TouchUI::clearZones();
        toggleZone = TouchUI::addZone(TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H);
    }

    void doToggle() {
        bool wantEnabled = !NetGuard::isEnabled();
        bool ok = NetGuard::setEnabled(wantEnabled);

        if (wantEnabled && !ok) {
            // Nessuna whitelist ancora: NetGuard non puo' scansionare da
            // solo per crearla (solo NetScan lo fa), quindi si spiega
            // il motivo invece di attivare silenziosamente nulla.
            // Display::showMessage() e' ancora LVGL - LVGL va risvegliato
            // prima, e update() deve restare fermo finche' non si torna.
            showingSubMessage = true;
            Display::setDirectDrawMode(false);
            Display::showMessage(
                "NetGuard",
                "Serve prima una whitelist: apri NetScan e fai una scansione, poi torna qui ad attivare la sorveglianza.",
                []() { NetGuardUI::open(); }
            );
            return;
        }

        drawScreen();
    }

} // namespace

namespace NetGuardUI {

    void open() {
        if (!ConnStatus::isWifiConnected()) {
            Display::showMessage(
                "NetGuard",
                "Devi prima connetterti a una rete WiFi: NetGuard sorveglia i dispositivi sulla rete locale.",
                []() { NetToolsMenu::showMain(); }
            );
            return;
        }

        active = true;
        showingSubMessage = false;
        Display::setDirectDrawMode(true);
        drawScreen();
    }

    bool isActive() {
        return active;
    }

    void update() {
        if (!active || showingSubMessage) {
            return;
        }

        // Lo stato (whitelist, ultimo allarme) puo' cambiare da solo
        // mentre la schermata resta aperta - ridisegna solo se e'
        // davvero cambiato (stesso principio della correzione di questa
        // settimana su lv_label_set_text(), qui vale per l'intero
        // ridisegno dello schermo fisico).
        String currentText = buildStatusText();
        if (currentText != lastStatusText) {
            drawScreen();
        }

        int tapped = TouchUI::pollTap();
        if (tapped == toggleZone) {
            doToggle();
            return;
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Select:
                doToggle();
                break;

            case InputEvent::Back:
                closeNetGuard();
                break;

            default:
                break;
        }
    }
}
