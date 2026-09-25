#include "WifiUI.h"
#include "WifiManager.h"
#include "Display.h"
#include "Input.h"
#include "Menu.h"
#include "ConnStatus.h"
#include "TouchUI.h"
#include "TextInputUI.h"

#include <Arduino.h>

// Fase 3 della riscrittura senza LVGL (vedi project_lvgl_removal_rewrite in
// memoria): WifiUI convertita a disegno diretto - la schermata piu' grande
// rimasta su LVGL, prima causa sospetta dei blocchi "torna al menu e si
// blocca, serve un reset" osservati dopo la connessione (25-26/09). Stessa
// struttura a modalita' di NetScanUI (Scanning/List/Password/Connecting al
// posto di Scanning/Results/ConfirmRescan), stesso scorrimento "a righe
// intere" con trascinamento touch del menu principale, prima vera
// applicazione di TextInputUI fuori dalla schermata di test.
namespace {

    enum class Mode {
        Closed,
        Scanning,
        List,
        Password,
        Connecting
    };

    Mode mode = Mode::Closed;

    constexpr int MAX_NETWORKS_SHOWN = 20;
    constexpr uint16_t MAX_PASSWORD_LEN = 63; // limite WPA2

    int networkCount = 0;
    int selectedIndex = 0;

    // Come nella versione LVGL: il trackball deve poter raggiungere anche
    // "Scansiona di nuovo" e "Disconnetti", non solo le voci di rete.
    enum class ListFocus {
        Rescan,
        Disconnect,
        Network
    };

    ListFocus listFocus = ListFocus::Network;
    int topVisibleRow = 0;
    int dragAccumY = 0;

    String selectedSsid;
    bool selectedOpen = false;

    String pendingPassword;
    bool credentialsSavedForAttempt = false;

    String passwordBuffer;
    bool passwordVisible = false;

    WifiManager::ConnectionState lastShownConnectingState =
        WifiManager::ConnectionState::Connected; // valore "neutro": forza il primo aggiornamento

    constexpr int16_t BTN_X = 8;
    constexpr int16_t BTN_W = 304;
    constexpr int16_t BTN_H = 28;
    constexpr int16_t ROW_H = 28;
    constexpr int16_t STATUS_H = 36;

    constexpr int16_t PASS_FIELD_X = 8;
    constexpr int16_t PASS_FIELD_W = 250;
    constexpr int16_t PASS_FIELD_H = 28;
    int16_t passFieldY() { return Display::DD_HEADER_H + 30; }

    // Indice zona -> indice rete (per le righe della lista) o -1 se la
    // zona non e' una riga (rescan/disconnetti/altro). zoneIsDelete[]
    // distingue il tocco sul cestino "dimentica" dal tocco sul resto
    // della riga (connetti), registrato PRIMA nella stessa zona cosi'
    // TouchUI::pollTap() lo trova per primo (vince l'id piu' basso).
    int zoneNetIndex[TouchUI::MAX_ZONES];
    bool zoneIsDelete[TouchUI::MAX_ZONES];

    int rescanZone = -1;
    int disconnectZone = -1;
    int eyeZone = -1;
    int cancelZone = -1;

    void showListScreen();
    void showPasswordScreen();
    void showConnectingScreen();
    void drawListScreen();
    void showScanningScreen();
    void attemptConnectToSelected();
    void rescan();

    void closeAndReturnToMenu() {
        mode = Mode::Closed;
        Input::setTextEntryMode(false);
        Menu::showMain();
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

    void drawButton(int16_t y, const char* label, bool focused) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->drawRect(BTN_X, y, BTN_W, BTN_H, focused ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 12, y + BTN_H / 2 - 3);
        gfx->print(label);
    }

    // Layout dinamico: l'altezza sopra la lista cambia se c'e' o no il
    // riquadro "connesso" (dipende dallo stato WiFi in tempo reale, puo'
    // cambiare mentre la schermata lista e' aperta, es. dopo Disconnetti).
    int16_t computeListTop(bool connected) {
        int16_t y = Display::DD_HEADER_H + 24;
        if (connected) {
            y += STATUS_H + 4;
        }
        y += BTN_H + 4;
        return y;
    }

    int computeVisibleRows(int16_t listTop) {
        int rows = (240 - listTop) / ROW_H;
        return rows > 0 ? rows : 0;
    }

    void ensureNetworkVisible() {
        if (listFocus != ListFocus::Network) {
            return;
        }
        int16_t listTop = computeListTop(ConnStatus::isWifiConnected());
        int visibleRows = computeVisibleRows(listTop);
        if (selectedIndex < topVisibleRow) {
            topVisibleRow = selectedIndex;
        } else if (selectedIndex >= topVisibleRow + visibleRows) {
            topVisibleRow = selectedIndex - visibleRows + 1;
        }
    }

    void drawNetworkRow(int index, int16_t rowY) {
        Arduino_GFX* gfx = Display::getGfx();
        WifiManager::Network net = WifiManager::getNetwork(index);
        bool known = !net.open && WifiManager::hasSavedPassword(net.ssid);
        bool selected = (listFocus == ListFocus::Network && index == selectedIndex);

        gfx->fillRect(0, rowY, 320, ROW_H, Display::DD_BG);
        if (selected) {
            gfx->fillRect(2, rowY + 4, 4, ROW_H - 8, Display::DD_ACCENT);
        }

        uint16_t nameColor = known ? gfx->color565(0x2E, 0xCC, 0x71) : Display::DD_ACCENT;
        gfx->setTextColor(nameColor);
        gfx->setCursor(16, rowY + ROW_H / 2 - 3);
        String displayName = (net.ssid.length() > 0) ? net.ssid : "(nascosta)";
        gfx->print(displayName);

        if (net.open) {
            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->setCursor(230, rowY + ROW_H / 2 - 3);
            gfx->print("APERTA");
        }

        if (known) {
            uint16_t delColor = gfx->color565(0xE7, 0x4C, 0x3C);
            int16_t delX = 270;
            gfx->drawRect(delX, rowY + 2, 40, ROW_H - 4, delColor);
            gfx->setTextColor(delColor);
            gfx->setCursor(delX + 4, rowY + ROW_H / 2 - 3);
            gfx->print("DEL");

            int z = TouchUI::addZone(delX, rowY + 2, 40, ROW_H - 4);
            if (z >= 0) {
                zoneNetIndex[z] = index;
                zoneIsDelete[z] = true;
            }
        }

        int rz = TouchUI::addZone(BTN_X, rowY, BTN_W, ROW_H);
        if (rz >= 0) {
            zoneNetIndex[rz] = index;
            zoneIsDelete[rz] = false;
        }
    }

    void drawListScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("WIFI");

        TouchUI::clearZones();
        rescanZone = -1;
        disconnectZone = -1;
        for (int i = 0; i < TouchUI::MAX_ZONES; i++) {
            zoneNetIndex[i] = -1;
            zoneIsDelete[i] = false;
        }

        bool connected = ConnStatus::isWifiConnected();
        int16_t y = Display::DD_HEADER_H + 24;

        if (connected) {
            gfx->drawRect(BTN_X, y, BTN_W, STATUS_H, Display::DD_ACCENT_DIM);
            gfx->setTextColor(Display::DD_TEXT);
            gfx->setCursor(BTN_X + 6, y + 4);
            String ssidLine = "CONNESSO: " + WifiManager::getConnectedSsid();
            gfx->print(ssidLine);
            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->setCursor(BTN_X + 6, y + 18);
            String ipLine = "IP: " + WifiManager::getLocalIP() + " [TOCCA=DISCONNETTI]";
            gfx->print(ipLine);

            disconnectZone = TouchUI::addZone(BTN_X, y, BTN_W, STATUS_H);
            y += STATUS_H + 4;
        }

        drawButton(y, "[ RESCAN ] SCANSIONA DI NUOVO", listFocus == ListFocus::Rescan);
        rescanZone = TouchUI::addZone(BTN_X, y, BTN_W, BTN_H);
        y += BTN_H + 4;

        int16_t listTop = y;
        int visibleRows = computeVisibleRows(listTop);

        if (networkCount == 0) {
            Display::drawWrappedText(
                8, listTop + 4, 304,
                "Nessuna rete trovata.\nTocca Scansiona per riprovare.",
                Display::DD_TEXT_DIM
            );
            return;
        }

        int lastVisible = topVisibleRow + visibleRows - 1;
        if (lastVisible >= networkCount) {
            lastVisible = networkCount - 1;
        }
        for (int i = topVisibleRow; i <= lastVisible; i++) {
            drawNetworkRow(i, listTop + (i - topVisibleRow) * ROW_H);
        }
    }

    void showListScreen() {
        mode = Mode::List;
        Input::setTextEntryMode(false);

        networkCount = WifiManager::getNetworkCount();
        if (networkCount > MAX_NETWORKS_SHOWN) {
            networkCount = MAX_NETWORKS_SHOWN;
        }

        selectedIndex = 0;
        topVisibleRow = 0;
        dragAccumY = 0;
        listFocus = (networkCount > 0) ? ListFocus::Network : ListFocus::Rescan;

        drawListScreen();
    }

    void attemptConnectToSelected() {
        if (selectedIndex < 0 || selectedIndex >= networkCount) {
            return;
        }

        WifiManager::Network net = WifiManager::getNetwork(selectedIndex);
        selectedSsid = net.ssid;
        selectedOpen = net.open;

        if (selectedOpen) {
            pendingPassword = "";
            WifiManager::connect(selectedSsid, pendingPassword);
            showConnectingScreen();
        } else if (WifiManager::hasSavedPassword(selectedSsid)) {
            pendingPassword = WifiManager::getSavedPassword(selectedSsid);
            WifiManager::connect(selectedSsid, pendingPassword);
            showConnectingScreen();
        } else {
            showPasswordScreen();
        }
    }

    // Chiamata SOLO subito dopo che una scansione e' appena terminata: se
    // fra i risultati c'e' almeno una rete gia' nota e non si e' gia'
    // connessi a qualcosa, ci si connette automaticamente a quella con
    // segnale piu' forte, senza bisogno di selezionarla a mano.
    void showListScreenAfterScan() {
        if (!ConnStatus::isWifiConnected()) {
            int count = WifiManager::getNetworkCount();
            if (count > MAX_NETWORKS_SHOWN) {
                count = MAX_NETWORKS_SHOWN;
            }

            int bestIndex = -1;
            int32_t bestRssi = 0;

            for (int i = 0; i < count; i++) {
                WifiManager::Network net = WifiManager::getNetwork(i);
                if (!WifiManager::hasSavedPassword(net.ssid)) {
                    continue;
                }
                if (bestIndex < 0 || net.rssi > bestRssi) {
                    bestIndex = i;
                    bestRssi = net.rssi;
                }
            }

            if (bestIndex >= 0) {
                networkCount = count;
                selectedIndex = bestIndex;
                attemptConnectToSelected();
                return;
            }
        }

        showListScreen();
    }

    void drawScanningScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("WIFI");
        Display::drawWrappedText(
            8, Display::DD_HEADER_H + 26, 304,
            "Scansione delle reti in corso...",
            Display::DD_TEXT
        );
    }

    void showScanningScreen() {
        mode = Mode::Scanning;
        Input::setTextEntryMode(false);
        drawScanningScreen();
    }

    void rescan() {
        WifiManager::startScan();
        showScanningScreen();
    }

    void handleScanningUpdate() {
        if (!WifiManager::isScanning()) {
            showListScreenAfterScan();
            return;
        }

        if (Input::getEvent() == InputEvent::Back) {
            closeAndReturnToMenu();
        }
    }

    // Stesso ordine di navigazione su/giu' della versione LVGL, dall'alto
    // verso il basso: Disconnetti (se presente) -> Scansiona -> reti.
    void handleListInput() {
        int16_t dragDelta = TouchUI::pollDragDeltaY();
        if (dragDelta != 0 && networkCount > 0) {
            dragAccumY += dragDelta;

            int16_t listTop = computeListTop(ConnStatus::isWifiConnected());
            int visibleRows = computeVisibleRows(listTop);
            int maxTopRow = networkCount - visibleRows;
            if (maxTopRow < 0) {
                maxTopRow = 0;
            }

            bool scrolled = false;
            while (dragAccumY >= ROW_H && topVisibleRow > 0) {
                topVisibleRow--;
                dragAccumY -= ROW_H;
                scrolled = true;
            }
            while (dragAccumY <= -ROW_H && topVisibleRow < maxTopRow) {
                topVisibleRow++;
                dragAccumY += ROW_H;
                scrolled = true;
            }
            if (topVisibleRow == 0 && dragAccumY > ROW_H) {
                dragAccumY = ROW_H;
            }
            if (topVisibleRow == maxTopRow && dragAccumY < -ROW_H) {
                dragAccumY = -ROW_H;
            }

            if (scrolled) {
                drawListScreen();
            }
        }

        int tapped = TouchUI::pollTap();
        if (tapped >= 0) {
            if (tapped == disconnectZone) {
                WifiManager::disconnect();
                showListScreen();
                return;
            }
            if (tapped == rescanZone) {
                rescan();
                return;
            }
            if (zoneNetIndex[tapped] >= 0) {
                int idx = zoneNetIndex[tapped];
                if (zoneIsDelete[tapped]) {
                    WifiManager::Network net = WifiManager::getNetwork(idx);
                    WifiManager::forgetPassword(net.ssid);
                    showListScreen();
                } else {
                    selectedIndex = idx;
                    listFocus = ListFocus::Network;
                    drawListScreen();
                    delay(150);
                    attemptConnectToSelected();
                }
                return;
            }
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (listFocus == ListFocus::Network) {
                    if (selectedIndex > 0) {
                        selectedIndex--;
                    } else {
                        listFocus = ListFocus::Rescan;
                    }
                } else if (listFocus == ListFocus::Rescan) {
                    if (ConnStatus::isWifiConnected()) {
                        listFocus = ListFocus::Disconnect;
                    }
                }
                ensureNetworkVisible();
                drawListScreen();
                break;

            case InputEvent::Down:
                if (listFocus == ListFocus::Disconnect) {
                    listFocus = ListFocus::Rescan;
                } else if (listFocus == ListFocus::Rescan) {
                    if (networkCount > 0) {
                        listFocus = ListFocus::Network;
                        selectedIndex = 0;
                    }
                } else if (listFocus == ListFocus::Network) {
                    if (selectedIndex < networkCount - 1) {
                        selectedIndex++;
                    }
                }
                ensureNetworkVisible();
                drawListScreen();
                break;

            case InputEvent::Select:
                if (listFocus == ListFocus::Rescan) {
                    rescan();
                } else if (listFocus == ListFocus::Disconnect) {
                    WifiManager::disconnect();
                    showListScreen();
                } else if (networkCount > 0) {
                    drawListScreen();
                    delay(150);
                    attemptConnectToSelected();
                }
                break;

            case InputEvent::Back:
                closeAndReturnToMenu();
                break;

            default:
                break;
        }
    }

    void drawPasswordScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        String title = "PASSWORD: " + selectedSsid;
        gfx->print(title);

        TouchUI::clearZones();

        int16_t fieldY = passFieldY();
        TextInputUI::draw(PASS_FIELD_X, fieldY, PASS_FIELD_W, PASS_FIELD_H, passwordBuffer, !passwordVisible);

        int16_t eyeX = PASS_FIELD_X + PASS_FIELD_W + 4;
        gfx->drawRect(eyeX, fieldY, 40, PASS_FIELD_H, Display::DD_ACCENT_DIM);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(eyeX + 4, fieldY + PASS_FIELD_H / 2 - 3);
        gfx->print(passwordVisible ? "NASC." : "VEDI");
        eyeZone = TouchUI::addZone(eyeX, fieldY, 40, PASS_FIELD_H);

        Display::drawWrappedText(
            8, fieldY + PASS_FIELD_H + 10, 304,
            "Scrivi con la tastiera fisica, poi Invio per connettere.",
            Display::DD_TEXT_DIM
        );

        drawButton(200, "[ ANNULLA ]", true);
        cancelZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
    }

    void showPasswordScreen() {
        mode = Mode::Password;
        Input::setTextEntryMode(true);
        passwordBuffer = "";
        passwordVisible = false;
        drawPasswordScreen();
    }

    void handlePasswordInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == eyeZone) {
            passwordVisible = !passwordVisible;
            drawPasswordScreen();
            return;
        }
        if (tapped == cancelZone) {
            showListScreen();
            return;
        }

        TextInputUI::Result result = TextInputUI::handleKey(passwordBuffer, MAX_PASSWORD_LEN);
        if (result == TextInputUI::Result::Changed) {
            TextInputUI::draw(PASS_FIELD_X, passFieldY(), PASS_FIELD_W, PASS_FIELD_H, passwordBuffer, !passwordVisible);
        } else if (result == TextInputUI::Result::Confirmed) {
            pendingPassword = passwordBuffer;
            WifiManager::connect(selectedSsid, pendingPassword);
            showConnectingScreen();
        }
    }

    void drawConnectingText(const String& text) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, Display::DD_HEADER_H + 24, 320, 40, Display::DD_BG);
        Display::drawWrappedText(8, Display::DD_HEADER_H + 26, 304, text, Display::DD_TEXT);
    }

    void showConnectingScreen() {
        mode = Mode::Connecting;
        credentialsSavedForAttempt = false;
        Input::setTextEntryMode(false);
        // Sentinella "neutra": forza handleConnectingUpdate() a disegnare
        // subito il primo stato invece di aspettare che cambi.
        lastShownConnectingState = WifiManager::ConnectionState::Connected;

        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("WIFI");
        drawConnectingText("Connessione a " + selectedSsid + " in corso...");
    }

    void handleConnectingUpdate() {
        WifiManager::ConnectionState state = WifiManager::getConnectionState();

        if (state == WifiManager::ConnectionState::Connected) {
            if (!credentialsSavedForAttempt) {
                WifiManager::saveCredentials(selectedSsid, pendingPassword);
                credentialsSavedForAttempt = true;
            }

            // Torna subito alla lista appena connesso, senza dover premere
            // nulla: la lista mostra gia' SSID+IP in cima.
            showListScreen();
            return;
        }

        if (state != lastShownConnectingState) {
            lastShownConnectingState = state;
            String text = "Connessione in corso...";
            if (state == WifiManager::ConnectionState::Failed) {
                text = "Connessione fallita. Premi Indietro per riprovare.";
            }
            drawConnectingText(text);
        }

        if (Input::getEvent() == InputEvent::Back) {
            showListScreen();
        }
    }

} // namespace


namespace WifiUI {

    void open() {
        if (!WifiManager::isRadioEnabled()) {
            Display::showMessage(
                "WiFi",
                "WiFi disattivato per risparmiare batteria.\nRiattivalo dalle Impostazioni per usarlo.",
                []() { Menu::back(); }
            );
            return;
        }

        Display::setDirectDrawMode(true);
        Input::setTextEntryMode(false);
        rescan();
    }

    bool isActive() {
        return mode != Mode::Closed;
    }

    void update() {
        WifiManager::update();

        switch (mode) {
            case Mode::Closed:
                break;

            case Mode::Scanning:
                handleScanningUpdate();
                break;

            case Mode::List:
                handleListInput();
                break;

            case Mode::Password:
                handlePasswordInput();
                break;

            case Mode::Connecting:
                handleConnectingUpdate();
                break;
        }
    }
}
