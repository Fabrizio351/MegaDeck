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
//
// Rifatta il 26/09 per aggiungere l'elenco live dei dispositivi fuori
// whitelist (rosso = visto nell'ultimo giro, giallo = appena scomparso,
// vedi NetGuard.h) - richiesto dall'utente per vedere a colpo d'occhio
// chi e' un intruso attivo e chi ha provato ed e' scomparso, non solo
// l'ultimo popup di allarme.
namespace {

    bool active = false;

    // true mentre e' visibile il messaggio LVGL "serve prima una
    // whitelist" (Display::showMessage, non ancora convertito): in
    // quella finestra update() non deve toccare ne' lo schermo ne' i tap,
    // altrimenti il prossimo ridisegno di NetGuard cancellerebbe il
    // messaggio ancora in vista.
    bool showingSubMessage = false;

    String lastStatusLine;
    int topVisibleRow = 0;
    int dragAccumY = 0;

    // "Firma" testuale dell'elenco tracciato (mac+stato di ognuno,
    // concatenati): confrontata ad ogni update() per ridisegnare la
    // lista SOLO quando e' davvero cambiata qualcosa, stesso principio
    // anti-lampeggio di drawStatusText() altrove in questo file.
    String lastTrackedSignature;

    constexpr int16_t BTN_X = 8;
    constexpr int16_t BTN_W = 304;
    constexpr int16_t BTN_H = 28;
    constexpr int16_t STATUS_Y = Display::DD_HEADER_H + 24;
    // Scostamento aumentato da 34 a 40 (26/09): con soli 10px di margine
    // dallo stato, l'area cancellata da drawStatusLine() (vedi sotto)
    // finiva per essere alta solo 6px, troppo poco per una riga di testo
    // - il testo poteva restare a contatto visivo col bottone appena
    // sotto ("righe sovrapposte", segnalato dall'utente).
    constexpr int16_t TOGGLE_Y = Display::DD_HEADER_H + 40;
    constexpr int16_t SUBHEADER_Y = TOGGLE_Y + BTN_H + 8;
    constexpr int16_t LIST_TOP = SUBHEADER_Y + 16;
    constexpr int16_t ROW_H = 24;
    constexpr int VISIBLE_ROWS = (240 - LIST_TOP) / ROW_H;

    // Bottone "+" per riga (richiesto 26/09: aggiungere manualmente un
    // dispositivo alla whitelist per il giro dopo, senza aspettare un
    // nuovo NetScan completo - capita che il primo giro non trovi subito
    // tutti i dispositivi di casa).
    constexpr int16_t ADD_BTN_W = 26;
    constexpr int16_t ADD_BTN_X = 320 - ADD_BTN_W;

    int toggleZone = -1;
    // Una zona per riga VISIBILE (non per riga totale, come in
    // BleGattUI/NetScanUI): indicizzata da "index - topVisibleRow".
    int addZone[VISIBLE_ROWS + 1];

    void closeNetGuard() {
        active = false;
        Display::setDirectDrawMode(false);
        NetToolsMenu::showMain();
    }

    String buildStatusLine() {
        if (!NetGuard::isEnabled()) {
            return "Sorveglianza disattivata.";
        }
        if (NetGuard::isWaitingForWhitelist()) {
            return "In attesa: nessuna whitelist. Fai prima un NetScan.";
        }
        if (NetGuard::isScanning()) {
            return "Giro di controllo in corso... " + String(NetScan::getProgressPercent()) + "%";
        }
        return "Attiva - prossima scansione tra " + String(NetGuard::getSecondsUntilNextScan()) + "s.";
    }

    // Firma stabile dell'elenco tracciato: basta mac+stato di ognuno,
    // l'IP puo' cambiare senza che serva un ridisegno immediato.
    String buildTrackedSignature() {
        String sig;
        int count = NetGuard::getTrackedCount();
        for (int i = 0; i < count; i++) {
            NetGuard::TrackedDevice d = NetGuard::getTracked(i);
            sig += d.mac + (d.justLeft ? "Y" : "R") + "|";
        }
        return sig;
    }

    void drawStatusLine() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, STATUS_Y, 320, TOGGLE_Y - 4 - STATUS_Y, Display::DD_BG);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_TEXT);
        gfx->setCursor(8, STATUS_Y);
        gfx->print(buildStatusLine());
    }

    // Disegna SOLO i pixel della riga (testo + bottone "+") - la
    // registrazione della zona tap del "+" e' centralizzata in
    // registerZones(), mai qui dentro: aggiungerla riga per riga senza
    // un clearZones() coordinato avrebbe duplicato le zone ad ogni
    // ridisegno parziale (stesso bug di zone-list gia' preso e corretto
    // altrove in questa sessione, es. VpnUI).
    void drawTrackedRow(int index) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t y = LIST_TOP + (index - topVisibleRow) * ROW_H;

        gfx->fillRect(0, y, 320, ROW_H, Display::DD_BG);

        NetGuard::TrackedDevice d = NetGuard::getTracked(index);
        uint16_t color = d.justLeft ? Display::DD_WARN : Display::DD_DANGER;

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(color);
        gfx->setCursor(8, y + 1);
        gfx->print(d.ip);
        gfx->print(d.justLeft ? "  [scomparso]" : "  [attivo]");
        gfx->setTextColor(color);
        gfx->setCursor(8, y + 12);
        gfx->print(d.mac);

        gfx->drawRect(ADD_BTN_X, y + 2, ADD_BTN_W - 2, ROW_H - 4, color);
        gfx->setCursor(ADD_BTN_X + 8, y + ROW_H / 2 - 3);
        gfx->print("+");

        gfx->drawFastHLine(0, y + ROW_H - 1, 320, Display::DD_ACCENT_DIM);
    }

    // Unica fonte di verita' per le zone tap di questa schermata (bottone
    // sorveglianza + un "+" per ogni riga VISIBILE): clearZones() e
    // riaggiunta di tutte insieme, mai parziale - vedi il commento sopra
    // drawTrackedRow(). Va richiamata ogni volta che l'elenco visibile
    // cambia (scroll, righe aggiunte/rimosse), non solo all'apertura.
    void registerZones() {
        TouchUI::clearZones();
        toggleZone = TouchUI::addZone(BTN_X, TOGGLE_Y, BTN_W, BTN_H);

        int count = NetGuard::getTrackedCount();
        int lastVisible = topVisibleRow + VISIBLE_ROWS - 1;
        if (lastVisible >= count) {
            lastVisible = count - 1;
        }
        for (int slot = 0; slot < VISIBLE_ROWS + 1; slot++) {
            addZone[slot] = -1;
        }
        for (int i = topVisibleRow; i <= lastVisible; i++) {
            int16_t y = LIST_TOP + (i - topVisibleRow) * ROW_H;
            int slot = i - topVisibleRow;
            if (slot >= 0 && slot < VISIBLE_ROWS + 1) {
                addZone[slot] = TouchUI::addZone(ADD_BTN_X, y, ADD_BTN_W, ROW_H);
            }
        }
    }

    void drawTrackedList() {
        Arduino_GFX* gfx = Display::getGfx();

        // fillRect prima di ristampare: senza, quando il numero cambia
        // di cifre (es. "FUORI 12" -> "FUORI 3") restavano pezzi del
        // testo precedente sovrapposti - stesso errore gia' preso e
        // corretto altrove (NetScanUI), stavolta qui.
        gfx->fillRect(0, SUBHEADER_Y, 320, LIST_TOP - SUBHEADER_Y, Display::DD_BG);

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, SUBHEADER_Y);
        char sub[40];
        snprintf(sub, sizeof(sub), "WHITELIST %d - FUORI %d", NetGuard::getWhitelistCount(), NetGuard::getTrackedCount());
        gfx->print(sub);

        gfx->fillRect(0, LIST_TOP, 320, 240 - LIST_TOP, Display::DD_BG);

        int count = NetGuard::getTrackedCount();
        if (count == 0) {
            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->setCursor(8, LIST_TOP + 4);
            gfx->print("Nessun dispositivo fuori whitelist.");
            registerZones();
            lastTrackedSignature = buildTrackedSignature();
            return;
        }

        int lastVisible = topVisibleRow + VISIBLE_ROWS - 1;
        if (lastVisible >= count) {
            lastVisible = count - 1;
        }
        for (int i = topVisibleRow; i <= lastVisible; i++) {
            drawTrackedRow(i);
        }
        registerZones();

        lastTrackedSignature = buildTrackedSignature();
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

        drawStatusLine();
        lastStatusLine = buildStatusLine();

        gfx->drawRect(BTN_X, TOGGLE_Y, BTN_W, BTN_H, Display::DD_ACCENT);
        gfx->setTextColor(Display::DD_ACCENT);
        const char* btnLabel = NetGuard::isEnabled()
            ? "DISATTIVA SORVEGLIANZA"
            : "ATTIVA SORVEGLIANZA";
        gfx->setCursor(BTN_X + 12, TOGGLE_Y + BTN_H / 2 - 3);
        gfx->print(btnLabel);

        topVisibleRow = 0;
        dragAccumY = 0;
        drawTrackedList(); // registra anche le zone tap (registerZones()), niente da aggiungere qui
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

        // L'allarme NetGuard (NetGuard::update() gira SEMPRE, anche a
        // questa schermata attiva) puo' scattare da qui, mostrare il suo
        // popup (a piena schermata) e alla chiusura tornare al menu
        // principale (Menu::back(), non a questa schermata) - "active"
        // restava vero ma nessuno ridisegnava piu' tutto lo schermo, che
        // restava quello dell'ultima schermata mostrata nel frattempo
        // (bottone mancante, righe del popup non cancellate sovrapposte
        // al contenuto nuovo - segnalato dall'utente). Un primo tentativo
        // basato su Display::isDirectDrawMode() non bastava (il menu
        // principale la riattiva anch'esso, quindi il controllo non
        // rilevava sempre il problema) - consumeMessageJustDismissed() e'
        // un segnale esplicito e affidabile, non deducibile da altro
        // stato che puo' cambiare nel frattempo.
        if (Display::consumeMessageJustDismissed()) {
            Display::setDirectDrawMode(true);
            drawScreen();
            return;
        }

        // Stato (percentuale scansione, conto alla rovescia) e elenco
        // tracciato possono cambiare da soli mentre la schermata resta
        // aperta - ridisegnati solo se davvero cambiati, e solo la loro
        // area (vedi drawStatusLine()/drawTrackedList()).
        String currentStatusLine = buildStatusLine();
        if (currentStatusLine != lastStatusLine) {
            drawStatusLine();
            lastStatusLine = currentStatusLine;
        }

        String currentSignature = buildTrackedSignature();
        if (currentSignature != lastTrackedSignature) {
            drawTrackedList();
        }

        int count = NetGuard::getTrackedCount();

        // Scorrimento touch (trascinamento verticale sulla lista) - stesso
        // schema di NetScanUI/BleGattUI, mancava qui (segnalato
        // dall'utente: "lo scroll funziona solo con trackball").
        int16_t dragDelta = TouchUI::pollDragDeltaY();
        if (dragDelta != 0 && count > 0) {
            dragAccumY += dragDelta;

            int maxTop = count - VISIBLE_ROWS;
            if (maxTop < 0) {
                maxTop = 0;
            }

            bool scrolled = false;
            while (dragAccumY >= ROW_H && topVisibleRow > 0) {
                topVisibleRow--;
                dragAccumY -= ROW_H;
                scrolled = true;
            }
            while (dragAccumY <= -ROW_H && topVisibleRow < maxTop) {
                topVisibleRow++;
                dragAccumY += ROW_H;
                scrolled = true;
            }
            if (topVisibleRow == 0 && dragAccumY > ROW_H) {
                dragAccumY = ROW_H;
            }
            if (topVisibleRow == maxTop && dragAccumY < -ROW_H) {
                dragAccumY = -ROW_H;
            }

            if (scrolled) {
                drawTrackedList();
            }
        }

        int tapped = TouchUI::pollTap();
        if (tapped == toggleZone) {
            doToggle();
            return;
        }
        if (tapped >= 0) {
            for (int slot = 0; slot < VISIBLE_ROWS + 1; slot++) {
                if (addZone[slot] == tapped) {
                    int index = topVisibleRow + slot;
                    if (index < NetGuard::getTrackedCount()) {
                        NetGuard::addToWhitelist(NetGuard::getTracked(index).mac);
                        drawTrackedList();
                    }
                    return;
                }
            }
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (topVisibleRow > 0) {
                    topVisibleRow--;
                    drawTrackedList();
                }
                break;

            case InputEvent::Down:
                if (topVisibleRow + VISIBLE_ROWS < count) {
                    topVisibleRow++;
                    drawTrackedList();
                }
                break;

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
