#include "BleGattUI.h"
#include "BleGatt.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "Menu.h"
#include "TextInputUI.h"

#include <Arduino.h>

// Prima vera schermata BLE GATT (26/09): scansiona - lista - connetti -
// mostra le caratteristiche. Stesso schema a modalita' e stessi
// componenti condivisi (TouchUI, Display::drawWrappedText) delle altre
// schermate disegnate a mano; lista risultati modellata su WifiUI.cpp
// (righe tappabili per connettersi), lista caratteristiche modellata su
// NetScanUI.cpp (righe di sola lettura, scorrimento a righe intere).
//
// Mode::WriteChar (29/09, richiesto: comando "sveglia" per sensori come
// il BM6 che non notificano da soli) - scrittura di byte hex a mano su
// una caratteristica scrivibile, invece di codificare un comando
// specifico per un solo modello di sensore nel firmware.
namespace {

    enum class Mode {
        Closed,
        Scanning,
        Results,
        Detail,
        WriteChar
    };

    Mode mode = Mode::Closed;

    constexpr int16_t BTN_X = 8;
    constexpr int16_t BTN_W = 304;
    constexpr int16_t BTN_H = 28;

    constexpr int16_t LIST_TOP = Display::DD_HEADER_H + 56;
    constexpr int16_t ROW_H = 26;
    constexpr int VISIBLE_ROWS = (240 - LIST_TOP) / ROW_H;

    int topVisibleRow = 0;
    int selectedIndex = 0;
    int dragAccumY = 0;

    int rescanZone = -1;
    int disconnectZone = -1;
    int deviceRowZone[VISIBLE_ROWS + 1];

    String connectedDeviceLabel;

    // --- Scrittura hex su una caratteristica (29/09) ---
    int charRowZone[VISIBLE_ROWS + 1]; // solo le righe scrivibili hanno una zona valida
    int writeTargetRow = -1;
    String writeHexBuffer;
    int writeCancelZone = -1;
    int writeSendZone = -1;
    constexpr uint16_t HEX_BUFFER_MAX_LEN = 64; // fino a 32 byte
    constexpr int16_t WRITE_FIELD_Y = Display::DD_HEADER_H + 60;
    constexpr int16_t WRITE_FIELD_H = 28;

    // Decodifica una stringa hex (spazi ignorati, es. "D1 55 07" o
    // "D15507") in byte grezzi. Ritorna il numero di byte scritti in
    // 'out' (capacita' outCapacity), o -1 se la stringa non e' hex
    // valida (lunghezza dispari o carattere non hex dopo aver
    // rimosso gli spazi).
    int parseHexBytes(const String& text, uint8_t* out, int outCapacity) {
        String compact;
        for (unsigned int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c != ' ') {
                compact += c;
            }
        }
        if (compact.length() == 0 || compact.length() % 2 != 0) {
            return -1;
        }

        int byteCount = 0;
        for (unsigned int i = 0; i < compact.length(); i += 2) {
            char hi = compact.charAt(i);
            char lo = compact.charAt(i + 1);
            if (!isxdigit(hi) || !isxdigit(lo)) {
                return -1;
            }
            if (byteCount >= outCapacity) {
                break;
            }
            String pair = String(hi) + String(lo);
            out[byteCount++] = static_cast<uint8_t>(strtoul(pair.c_str(), nullptr, 16));
        }
        return byteCount;
    }

    void showScanningScreen();
    void showResultsScreen();
    void showDetailScreen();
    void drawResultsScreen();
    void drawDetailScreen();
    void showWriteScreen(int row);

    void closeAndReturnToMenu() {
        mode = Mode::Closed;
        Menu::back();
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

    // --- Scansione ---

    void drawScanningScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("BLE GATT");

        Display::drawWrappedText(
            8, Display::DD_HEADER_H + 26, 304,
            "Scansione dispositivi Bluetooth nelle vicinanze in corso...",
            Display::DD_TEXT
        );

        TouchUI::clearZones();
    }

    void showScanningScreen() {
        mode = Mode::Scanning;
        drawScanningScreen();
        BleGatt::startScan();
    }

    void handleScanningUpdate() {
        if (BleGatt::isScanning()) {
            if (Input::getEvent() == InputEvent::Back) {
                BleGatt::stopScan();
                closeAndReturnToMenu();
            }
            return;
        }

        showResultsScreen();
    }

    // --- Risultati (elenco dispositivi trovati) ---

    void drawDeviceRow(int index, int16_t y) {
        Arduino_GFX* gfx = Display::getGfx();
        BleGatt::FoundDevice dev = BleGatt::getFoundDevice(index);

        gfx->fillRect(0, y, 320, ROW_H, Display::DD_BG);

        bool selected = (index == selectedIndex);
        if (selected) {
            gfx->fillRect(2, y + 4, 4, ROW_H - 8, Display::DD_ACCENT);
        }

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(16, y + 1);
        gfx->print(dev.name.length() > 0 ? dev.name : "(senza nome)");

        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(16, y + 13);
        char rssiBuf[16];
        snprintf(rssiBuf, sizeof(rssiBuf), "  RSSI %d", dev.rssi);
        gfx->print(dev.address + rssiBuf);

        int zoneIdx = index - topVisibleRow;
        if (zoneIdx >= 0 && zoneIdx < static_cast<int>(sizeof(deviceRowZone) / sizeof(deviceRowZone[0]))) {
            deviceRowZone[zoneIdx] = TouchUI::addZone(BTN_X, y, BTN_W, ROW_H);
        }
    }

    void drawResultsScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("BLE GATT");

        int count = BleGatt::getFoundDeviceCount();
        char summary[32];
        snprintf(summary, sizeof(summary), "%d TROVATI", count);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 24);
        gfx->print(summary);

        TouchUI::clearZones();
        for (size_t i = 0; i < sizeof(deviceRowZone) / sizeof(deviceRowZone[0]); i++) {
            deviceRowZone[i] = -1;
        }

        drawButton(Display::DD_HEADER_H + 34, "[ RESCAN ] SCANSIONA DI NUOVO", true);
        rescanZone = TouchUI::addZone(BTN_X, Display::DD_HEADER_H + 34, BTN_W, BTN_H);

        gfx->fillRect(0, LIST_TOP, 320, 240 - LIST_TOP, Display::DD_BG);

        if (count == 0) {
            Display::drawWrappedText(
                8, LIST_TOP + 4, 304,
                "Nessun dispositivo trovato.\nAvvicinati o riprova.",
                Display::DD_TEXT_DIM
            );
            return;
        }

        int lastVisible = topVisibleRow + VISIBLE_ROWS - 1;
        if (lastVisible >= count) {
            lastVisible = count - 1;
        }
        for (int i = topVisibleRow; i <= lastVisible; i++) {
            drawDeviceRow(i, LIST_TOP + (i - topVisibleRow) * ROW_H);
        }
    }

    void showResultsScreen() {
        mode = Mode::Results;
        selectedIndex = 0;
        topVisibleRow = 0;
        dragAccumY = 0;
        drawResultsScreen();
    }

    void attemptConnect(int index) {
        Arduino_GFX* gfx = Display::getGfx();
        BleGatt::FoundDevice dev = BleGatt::getFoundDevice(index);
        connectedDeviceLabel = dev.name.length() > 0 ? dev.name : dev.address;

        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("BLE GATT");
        Display::drawWrappedText(
            8, Display::DD_HEADER_H + 26, 304,
            "Connessione a " + connectedDeviceLabel + " in corso...",
            Display::DD_TEXT
        );

        // connectToDevice() e' bloccante (limite della libreria BLE, non
        // esiste una versione davvero asincrona per il pairing - vedi
        // BleGatt.cpp): lo schermo resta sul messaggio sopra per la
        // durata reale della connessione (di norma 1-3s), poi passa al
        // risultato.
        BleGatt::connectToDevice(index);

        if (BleGatt::getConnectionState() == BleGatt::ConnectionState::Connected) {
            showDetailScreen();
        } else {
            String msg = "Connessione a " + connectedDeviceLabel + " fallita.\nRiprova o scegli un altro dispositivo.";
            Display::showMessage("BLE GATT", msg.c_str(), []() { showResultsScreen(); });
        }
    }

    void ensureResultsVisible() {
        if (selectedIndex < topVisibleRow) {
            topVisibleRow = selectedIndex;
        } else if (selectedIndex >= topVisibleRow + VISIBLE_ROWS) {
            topVisibleRow = selectedIndex - VISIBLE_ROWS + 1;
        }
    }

    void handleResultsInput() {
        int count = BleGatt::getFoundDeviceCount();

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
                drawResultsScreen();
            }
        }

        int tapped = TouchUI::pollTap();
        if (tapped == rescanZone) {
            showScanningScreen();
            return;
        }
        for (size_t i = 0; i < sizeof(deviceRowZone) / sizeof(deviceRowZone[0]); i++) {
            if (tapped >= 0 && tapped == deviceRowZone[i]) {
                selectedIndex = topVisibleRow + static_cast<int>(i);
                attemptConnect(selectedIndex);
                return;
            }
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (selectedIndex > 0) {
                    selectedIndex--;
                    ensureResultsVisible();
                    drawResultsScreen();
                }
                break;

            case InputEvent::Down:
                if (selectedIndex < count - 1) {
                    selectedIndex++;
                    ensureResultsVisible();
                    drawResultsScreen();
                }
                break;

            case InputEvent::Select:
                if (count > 0) {
                    attemptConnect(selectedIndex);
                }
                break;

            case InputEvent::Back:
                closeAndReturnToMenu();
                break;

            default:
                break;
        }
    }

    // --- Dettaglio (caratteristiche del dispositivo connesso) ---

    void drawCharRow(int index, int16_t y) {
        Arduino_GFX* gfx = Display::getGfx();
        BleGatt::CharRow row = BleGatt::getCharRow(index);

        gfx->fillRect(0, y, 320, ROW_H, Display::DD_BG);

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(row.notifying ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setCursor(8, y + 1);
        gfx->print(row.label);
        if (row.canWrite) {
            // "[W]" (29/09, comando "sveglia"): segnala che questa riga
            // si puo' toccare per scriverci dei byte hex.
            gfx->setTextColor(Display::DD_WARN);
            gfx->print(" [W]");
        }

        gfx->setTextColor(Display::DD_TEXT);
        gfx->setCursor(8, y + 13);
        gfx->print(row.value);

        gfx->drawFastHLine(0, y + ROW_H - 1, 320, Display::DD_ACCENT_DIM);
    }

    // Unica fonte di verita' per le zone tap di questa schermata
    // (disconnetti + una per ogni riga SCRIVIBILE visibile) - clearZones()
    // e riaggiunta di tutte insieme, mai parziale (stesso principio gia'
    // applicato altrove in questa sessione, es. VpnUI/NetGuardUI). Va
    // richiamata ogni volta che la lista visibile cambia (scroll, valori
    // aggiornati da notifica), non solo all'apertura.
    void drawDetailList() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, LIST_TOP, 320, 240 - LIST_TOP, Display::DD_BG);

        TouchUI::clearZones();
        disconnectZone = TouchUI::addZone(BTN_X, Display::DD_HEADER_H + 34, BTN_W, BTN_H);
        for (int slot = 0; slot < VISIBLE_ROWS + 1; slot++) {
            charRowZone[slot] = -1;
        }

        int count = BleGatt::getCharRowCount();
        if (count == 0) {
            Display::drawWrappedText(
                8, LIST_TOP + 4, 304,
                "Nessuna caratteristica leggibile trovata su questo dispositivo.",
                Display::DD_TEXT_DIM
            );
            return;
        }

        int lastVisible = topVisibleRow + VISIBLE_ROWS - 1;
        if (lastVisible >= count) {
            lastVisible = count - 1;
        }
        for (int i = topVisibleRow; i <= lastVisible; i++) {
            int16_t y = LIST_TOP + (i - topVisibleRow) * ROW_H;
            drawCharRow(i, y);
            if (BleGatt::getCharRow(i).canWrite) {
                int slot = i - topVisibleRow;
                charRowZone[slot] = TouchUI::addZone(0, y, 320, ROW_H);
            }
        }
    }

    void drawDetailScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("BLE GATT");

        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 24);
        gfx->print(connectedDeviceLabel);

        // Nota informativa (non un errore), sulla stessa riga per non
        // rubare spazio verticale: niente vera connessione GATT, solo
        // ascolto degli advertisement - vedi il commento su
        // BleGatt::connectToDevice() (fallback per beacon come i monitor
        // batteria BM6, che non espongono caratteristiche utili da leggere).
        if (BleGatt::isListening()) {
            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->print(" (ascolto passivo)");
        }

        // Solo i pixel del bottone: la zona la (ri)registra
        // drawDetailList(), unica fonte di verita' per le zone di questa
        // schermata.
        drawButton(Display::DD_HEADER_H + 34, "[ X ] DISCONNETTI", true);

        drawDetailList();
    }

    void showDetailScreen() {
        mode = Mode::Detail;
        topVisibleRow = 0;
        Input::setTextEntryMode(false);
        drawDetailScreen();
    }

    void handleDetailInput() {
        int count = BleGatt::getCharRowCount();

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
                drawDetailList();
            }
        }

        if (BleGatt::consumeValuesChanged()) {
            drawDetailList();
        }

        int tapped = TouchUI::pollTap();
        if (tapped == disconnectZone) {
            BleGatt::disconnect();
            closeAndReturnToMenu();
            return;
        }
        if (tapped >= 0) {
            for (int slot = 0; slot < VISIBLE_ROWS + 1; slot++) {
                if (charRowZone[slot] == tapped) {
                    showWriteScreen(topVisibleRow + slot);
                    return;
                }
            }
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (topVisibleRow > 0) {
                    topVisibleRow--;
                    drawDetailList();
                }
                break;

            case InputEvent::Down: {
                int maxTop = count - VISIBLE_ROWS;
                if (maxTop < 0) {
                    maxTop = 0;
                }
                if (topVisibleRow < maxTop) {
                    topVisibleRow++;
                    drawDetailList();
                }
                break;
            }

            case InputEvent::Back:
                BleGatt::disconnect();
                closeAndReturnToMenu();
                break;

            default:
                break;
        }
    }

    // --- Scrittura hex su una caratteristica (29/09) ---

    void drawWriteScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("SCRIVI COMANDO");

        BleGatt::CharRow row = BleGatt::getCharRow(writeTargetRow);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(8, Display::DD_HEADER_H + 22);
        gfx->print(row.label);

        TouchUI::clearZones();

        TextInputUI::draw(BTN_X, WRITE_FIELD_Y, BTN_W, WRITE_FIELD_H, writeHexBuffer, false);

        Display::drawWrappedText(
            BTN_X, WRITE_FIELD_Y + WRITE_FIELD_H + 10, BTN_W,
            "Byte in hex, con o senza spazi (es. D1 55 07 00 00 00), poi Invio o INVIA per scrivere.",
            Display::DD_TEXT_DIM
        );

        drawButton(200, "[ ANNULLA ]", false);
        writeCancelZone = TouchUI::addZone(BTN_X, 200, BTN_W / 2 - 2, BTN_H);

        drawButton(200, "[ INVIA ]", true);
        writeSendZone = TouchUI::addZone(BTN_X + BTN_W / 2 + 2, 200, BTN_W / 2 - 2, BTN_H);
    }

    void showWriteScreen(int row) {
        mode = Mode::WriteChar;
        writeTargetRow = row;
        writeHexBuffer = "";
        Input::setTextEntryMode(true);
        drawWriteScreen();
    }

    // Prova a scrivere il buffer hex corrente sulla caratteristica di
    // writeTargetRow - mostra un messaggio di esito (successo/fallimento
    // di parsing o di scrittura vera) e torna al dettaglio.
    void confirmWrite() {
        uint8_t bytes[32];
        int count = parseHexBytes(writeHexBuffer, bytes, sizeof(bytes));

        if (count <= 0) {
            Display::showMessage(
                "Scrivi comando",
                "Testo non valido: servono coppie di cifre hex (0-9, A-F), spazi permessi.",
                []() { showDetailScreen(); }
            );
            return;
        }

        bool ok = BleGatt::writeCharRow(writeTargetRow, bytes, static_cast<size_t>(count));
        Display::showMessage(
            "Scrivi comando",
            ok ? "Scrittura riuscita." : "Scrittura fallita (caratteristica non scrivibile o disconnessa).",
            []() { showDetailScreen(); }
        );
    }

    void handleWriteInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == writeCancelZone) {
            showDetailScreen();
            return;
        }
        if (tapped == writeSendZone) {
            confirmWrite();
            return;
        }

        TextInputUI::Result result = TextInputUI::handleKey(writeHexBuffer, HEX_BUFFER_MAX_LEN);
        if (result == TextInputUI::Result::Changed) {
            TextInputUI::draw(BTN_X, WRITE_FIELD_Y, BTN_W, WRITE_FIELD_H, writeHexBuffer, false);
        } else if (result == TextInputUI::Result::Confirmed) {
            confirmWrite();
        }
    }

} // namespace


namespace BleGattUI {

    void open() {
        if (!BleGatt::isEnabled()) {
            Display::showMessage(
                "BLE GATT",
                "Bluetooth disattivato per risparmiare batteria.\nRiattivalo dalle Impostazioni per usarlo.",
                []() { Menu::back(); }
            );
            return;
        }

        Display::setDirectDrawMode(true);
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

            case Mode::Detail:
                handleDetailInput();
                break;

            case Mode::WriteChar:
                handleWriteInput();
                break;
        }
    }
}
