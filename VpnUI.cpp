#include "VpnUI.h"
#include "VpnManager.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "TextInputUI.h"
#include "Menu.h"

#include <Arduino.h>

// Schermata VPN (26/09) - stessa struttura di NetGuardUI.cpp: testo di
// stato ridisegnato da solo (solo la sua area, non l'intera schermata -
// vedi il fix anti-lampeggio di oggi su NetGuard) + un bottone toggle +
// una riga ping (verifica raggiungibilita' reale, la libreria WireGuard
// non espone lo stato vero dell'handshake) con IP modificabile.
//
// Senza configurazione salvata (30/09) il bottone si divide in "DA SD"
// (comportamento originale) e "DA USB" (VpnManager::
// importConfigFromSerial(), legge lo stesso wireguard.conf via seriale
// invece che da SD - niente hang SD possibile, vedi
// project_sd_spi_boot_hang_critical in memoria). Esiste anche una via
// senza toccare questa schermata: digitare/incollare "VPNIMPORT" sulla
// seriale (da qualunque schermata) innesca lo stesso import, vedi il
// trigger in Input.cpp - comodo per caricare una config da script senza
// dover navigare la UI a mano.
namespace {

    enum class Mode {
        Main,
        EditPing
    };
    Mode mode = Mode::Main;

    // Bottone a fuoco per la navigazione da trackball (29/09, richiesto:
    // audit generale - qui Su/Giu'/Select non muovevano nulla, solo il
    // touch funzionava sui 3 bottoni). Su/Giu' spostano il fuoco fra i
    // 3, Select attiva quello a fuoco - stesso principio del resto del
    // progetto (es. WebRadioUI::selectStation()).
    enum class Focus {
        Toggle,
        ToggleSerial, // "DA USB", solo quando non c'e' ancora una config (vedi drawToggleButton())
        PingRun,
        PingEdit
    };
    Focus focusedButton = Focus::Toggle;

    bool active = false;
    String lastStatusText;
    String lastPingLine;

    constexpr int16_t PING_Y = 168;
    constexpr int16_t PING_H = 26;
    constexpr int16_t PING_EDIT_W = 40; // zona "MODIFICA" sul lato destro della riga ping

    constexpr int16_t TOGGLE_X = 8;
    constexpr int16_t TOGGLE_Y = 200;
    constexpr int16_t TOGGLE_W = 304;
    constexpr int16_t TOGGLE_H = 30;
    int toggleZone = -1;
    int toggleSerialZone = -1;
    int pingRunZone = -1;
    int pingEditZone = -1;

    constexpr int16_t EDIT_FIELD_X = 8;
    constexpr int16_t EDIT_FIELD_Y = Display::DD_HEADER_H + 40;
    constexpr int16_t EDIT_FIELD_W = 200;
    constexpr int16_t EDIT_FIELD_H = 28;
    String pingEditBuffer;
    int editConfirmZone = -1;

    void closeVpn() {
        active = false;
        Menu::back();
    }

    String buildStatusText() {
        switch (VpnManager::getState()) {
            case VpnManager::State::Disabled:
                return VpnManager::hasSavedConfig()
                    ? "VPN disattivata."
                    : "VPN disattivata.\n\nNessuna configurazione importata: inserisci una SD con /wireguard.conf nella radice e tocca \"IMPORTA DA SD\" qui sotto.";

            case VpnManager::State::WaitingForWifi:
                return "In attesa del WiFi per connettersi.";

            case VpnManager::State::ConfigMissing:
                return "Nessuna configurazione valida.\nInserisci una SD con /wireguard.conf nella radice e tocca \"IMPORTA DA SD\" qui sotto.";

            case VpnManager::State::SyncingTime:
                return "Sincronizzazione orario (richiesta da WireGuard)...";

            case VpnManager::State::Connecting:
                return "Avvio del tunnel in corso...";

            case VpnManager::State::Connected: {
                String text = "Tunnel avviato.\n\nIndirizzo locale: " + VpnManager::getLocalAddress();
                text += "\nEndpoint: " + VpnManager::getEndpointHost();
                return text;
            }

            case VpnManager::State::Failed:
                return "Avvio del tunnel fallito.\nControlla che l'endpoint sia raggiungibile, poi disattiva e riattiva per riprovare.";

            default:
                return "";
        }
    }

    String buildPingLine() {
        String label = "PING " + VpnManager::getPingTarget() + ": ";
        switch (VpnManager::getPingState()) {
            case VpnManager::PingState::Idle:
                return label + "(tocca per provare)";
            case VpnManager::PingState::Running:
                return label + "in corso...";
            case VpnManager::PingState::Success:
                return label + "OK (" + String(VpnManager::getPingLastLatencyMs()) + "ms)";
            case VpnManager::PingState::Failed:
                return label + "nessuna risposta";
            default:
                return label;
        }
    }

    void drawStatusText(const String& statusText) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t top = Display::DD_HEADER_H + 26;
        gfx->fillRect(0, top, 320, PING_Y - 4 - top, Display::DD_BG);
        Display::drawWrappedText(8, top, 304, statusText, Display::DD_TEXT);
        lastStatusText = statusText;
    }

    // Registra (senza disegnare) le zone toccabili della schermata
    // principale: SEMPRE tutte e 3 insieme, mai una singola zona da sola
    // - altrimenti un ridisegno parziale (es. solo la riga ping quando
    // cambia il suo esito) senza un clearZones() davanti accumulerebbe
    // zone duplicate ad ogni aggiornamento, fino a saturare il registro
    // di TouchUI (bug reale preso e corretto subito, 26/09).
    void registerMainZones(int16_t runW, int16_t editX) {
        TouchUI::clearZones();
        pingRunZone = TouchUI::addZone(TOGGLE_X, PING_Y, runW, PING_H);
        pingEditZone = TouchUI::addZone(editX, PING_Y, PING_EDIT_W, PING_H);

        if (!VpnManager::hasSavedConfig()) {
            int16_t halfW = (TOGGLE_W - 4) / 2;
            toggleZone = TouchUI::addZone(TOGGLE_X, TOGGLE_Y, halfW, TOGGLE_H);
            toggleSerialZone = TouchUI::addZone(TOGGLE_X + halfW + 4, TOGGLE_Y, halfW, TOGGLE_H);
        } else {
            toggleZone = TouchUI::addZone(TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H);
            toggleSerialZone = -1;
        }
    }

    void drawPingRow(const String& pingLine) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, PING_Y, 320, PING_H, Display::DD_BG);

        int16_t runW = TOGGLE_W - PING_EDIT_W - 4;
        gfx->drawRect(TOGGLE_X, PING_Y, runW, PING_H,
            focusedButton == Focus::PingRun ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(TOGGLE_X + 6, PING_Y + PING_H / 2 - 3);
        gfx->print(pingLine);

        int16_t editX = TOGGLE_X + runW + 4;
        gfx->drawRect(editX, PING_Y, PING_EDIT_W, PING_H,
            focusedButton == Focus::PingEdit ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(editX + 4, PING_Y + PING_H / 2 - 3);
        gfx->print("MOD.");

        registerMainZones(runW, editX);

        lastPingLine = pingLine;
    }

    // Senza una configurazione importata, il bottone principale si divide
    // in due meta' (stesso posto, niente riga aggiuntiva da trovare sullo
    // schermo gia' pieno): "DA SD" e "DA USB" (30/09, vedi
    // VpnManager::importConfigFromSerial()) - con una config gia' salvata
    // resta un unico bottone a piena larghezza ATTIVA/DISATTIVA, attivare
    // senza credenziali fallirebbe comunque subito con ConfigMissing.
    void drawToggleButton() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H, Display::DD_BG);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        if (!VpnManager::hasSavedConfig()) {
            int16_t halfW = (TOGGLE_W - 4) / 2;
            gfx->drawRect(TOGGLE_X, TOGGLE_Y, halfW, TOGGLE_H,
                focusedButton == Focus::Toggle ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
            gfx->setTextColor(Display::DD_ACCENT);
            gfx->setCursor(TOGGLE_X + 10, TOGGLE_Y + TOGGLE_H / 2 - 3);
            gfx->print("DA SD");

            int16_t rightX = TOGGLE_X + halfW + 4;
            gfx->drawRect(rightX, TOGGLE_Y, halfW, TOGGLE_H,
                focusedButton == Focus::ToggleSerial ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
            gfx->setTextColor(Display::DD_ACCENT);
            gfx->setCursor(rightX + 10, TOGGLE_Y + TOGGLE_H / 2 - 3);
            gfx->print("DA USB");
        } else {
            gfx->drawRect(TOGGLE_X, TOGGLE_Y, TOGGLE_W, TOGGLE_H,
                focusedButton == Focus::Toggle ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
            gfx->setTextColor(Display::DD_ACCENT);
            gfx->setCursor(TOGGLE_X + 12, TOGGLE_Y + TOGGLE_H / 2 - 3);
            gfx->print(VpnManager::isEnabled() ? "DISATTIVA VPN" : "ATTIVA VPN");
        }
    }

    // Sposta il fuoco fra i 3 bottoni (Su/Giu') - ridisegna solo le zone
    // coinvolte, stesso schema gia' usato altrove nel progetto.
    void moveFocus(Focus f) {
        if (focusedButton == f) {
            return;
        }
        focusedButton = f;
        drawToggleButton();
        drawPingRow(lastPingLine);
    }

    void doToggle();
    void doImportSerial();
    void showEditScreen();

    void activateFocused() {
        switch (focusedButton) {
            case Focus::Toggle:
                doToggle();
                break;
            case Focus::ToggleSerial:
                if (!VpnManager::hasSavedConfig()) {
                    doImportSerial();
                }
                break;
            case Focus::PingRun:  VpnManager::startPingTest(); break;
            case Focus::PingEdit: showEditScreen(); break;
        }
    }

    void drawMainScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("VPN");

        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(228, Display::DD_HEADER_H + 8);
        gfx->print("[B] INDIETRO");

        drawStatusText(buildStatusText());
        drawToggleButton();

        // drawPingRow() registra anche le zone del toggle qui sopra e
        // dello status (vedi registerMainZones()) - va chiamata per
        // ultima, dopo aver disegnato tutto il resto.
        drawPingRow(buildPingLine());
    }

    void doImportSd() {
        // Messaggio "in corso" PRIMA della chiamata bloccante (fino
        // a 8s, timeout duro sul montaggio SD - vedi VpnConfig.cpp):
        // senza questo lo schermo resterebbe fermo su quello che si
        // stava vedendo prima, senza nessun segnale che qualcosa sta
        // succedendo.
        drawStatusText("Importazione dalla SD in corso...");

        // Un solo tocco: da qui in avanti l'uso normale non tocca
        // piu' la SD (vedi VpnManager::startConnecting()).
        bool ok = VpnManager::importConfigFromSd();
        if (ok) {
            focusedButton = Focus::Toggle;
            Display::showMessage(
                "VPN", "Configurazione importata. Puoi estrarre la SD.",
                []() { drawMainScreen(); }
            );
        } else {
            Display::showMessage(
                "VPN", "Importazione fallita.\nControlla che la SD sia inserita con /wireguard.conf nella radice.",
                []() { drawMainScreen(); }
            );
        }
    }

    // Alternativa (30/09) all'import da SD: legge "wireguard.conf" da USB
    // seriale, stesso file, senza toccare la SD (vedi
    // VpnManager::importConfigFromSerial()) - utile sia per il primo
    // setup sia per aggiornare la configurazione in futuro se cambia.
    void doImportSerial() {
        drawStatusText("In attesa del file via USB seriale...\n(invia wireguard.conf entro 15s)");

        bool ok = VpnManager::importConfigFromSerial();
        if (ok) {
            focusedButton = Focus::Toggle;
            Display::showMessage(
                "VPN", "Configurazione ricevuta e salvata.",
                []() { drawMainScreen(); }
            );
        } else {
            Display::showMessage(
                "VPN", "Importazione fallita.\nNessun dato ricevuto o file incompleto entro il timeout.",
                []() { drawMainScreen(); }
            );
        }
    }

    void doToggle() {
        if (!VpnManager::hasSavedConfig()) {
            doImportSd();
            return;
        }

        VpnManager::setEnabled(!VpnManager::isEnabled());
        drawMainScreen();
    }

    void drawEditScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("IP DA PINGARE");

        TouchUI::clearZones();

        TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, pingEditBuffer, false);

        Display::drawWrappedText(
            8, EDIT_FIELD_Y + EDIT_FIELD_H + 10, 304,
            "Scrivi con la tastiera fisica, poi Invio per confermare.",
            Display::DD_TEXT_DIM
        );

        gfx->drawRect(TOGGLE_X, 200, TOGGLE_W, TOGGLE_H, Display::DD_ACCENT);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(TOGGLE_X + 12, 200 + TOGGLE_H / 2 - 3);
        gfx->print("[ ANNULLA ]");
        editConfirmZone = TouchUI::addZone(TOGGLE_X, 200, TOGGLE_W, TOGGLE_H);
    }

    void showEditScreen() {
        mode = Mode::EditPing;
        Input::setTextEntryMode(true);
        pingEditBuffer = VpnManager::getPingTarget();
        drawEditScreen();
    }

    void showMainScreen() {
        mode = Mode::Main;
        Input::setTextEntryMode(false);
        drawMainScreen();
    }

    void confirmEdit() {
        String trimmed = pingEditBuffer;
        trimmed.trim();
        if (trimmed.length() > 0) {
            VpnManager::setPingTarget(trimmed);
        }
        showMainScreen();
    }

} // namespace

namespace VpnUI {

    void open() {
        active = true;
        focusedButton = Focus::Toggle;
        Display::setDirectDrawMode(true);
        showMainScreen();
    }

    bool isActive() {
        return active;
    }

    void update() {
        if (!active) {
            return;
        }

        if (mode == Mode::EditPing) {
            int tapped = TouchUI::pollTap();
            if (tapped == editConfirmZone) {
                showMainScreen();
                return;
            }

            TextInputUI::Result result = TextInputUI::handleKey(pingEditBuffer, 39);
            if (result == TextInputUI::Result::Changed) {
                TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, pingEditBuffer, false);
            } else if (result == TextInputUI::Result::Confirmed) {
                confirmEdit();
            }
            return;
        }

        String currentText = buildStatusText();
        if (currentText != lastStatusText) {
            drawStatusText(currentText);
        }

        String pingLine = buildPingLine();
        if (pingLine != lastPingLine) {
            drawPingRow(pingLine);
        }

        int tapped = TouchUI::pollTap();
        if (tapped == toggleZone) {
            moveFocus(Focus::Toggle);
            doToggle();
            return;
        }
        if (tapped == toggleSerialZone && toggleSerialZone != -1) {
            moveFocus(Focus::ToggleSerial);
            doImportSerial();
            return;
        }
        if (tapped == pingRunZone) {
            moveFocus(Focus::PingRun);
            VpnManager::startPingTest();
            return;
        }
        if (tapped == pingEditZone) {
            moveFocus(Focus::PingEdit);
            showEditScreen();
            return;
        }

        // Navigazione da trackball (29/09, richiesto: audit generale -
        // prima solo Select/Back funzionavano, i 3 bottoni erano
        // raggiungibili solo col touch). Su/Giu' spostano il fuoco fra
        // toggle/ping-run/ping-edit (nello stesso ordine in cui appaiono
        // a schermo), Select attiva quello a fuoco.
        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (focusedButton == Focus::PingEdit || focusedButton == Focus::Toggle || focusedButton == Focus::ToggleSerial) {
                    moveFocus(Focus::PingRun);
                }
                break;

            case InputEvent::Down:
                if (focusedButton == Focus::PingRun || focusedButton == Focus::PingEdit) {
                    moveFocus(Focus::Toggle);
                }
                break;

            case InputEvent::Left:
                if (focusedButton == Focus::PingEdit) {
                    moveFocus(Focus::PingRun);
                } else if (focusedButton == Focus::ToggleSerial) {
                    moveFocus(Focus::Toggle);
                }
                break;

            case InputEvent::Right:
                if (focusedButton == Focus::PingRun) {
                    moveFocus(Focus::PingEdit);
                } else if (focusedButton == Focus::Toggle && !VpnManager::hasSavedConfig()) {
                    moveFocus(Focus::ToggleSerial);
                }
                break;

            case InputEvent::Select:
                activateFocused();
                break;

            case InputEvent::Back:
                closeVpn();
                break;

            default:
                break;
        }
    }
}
