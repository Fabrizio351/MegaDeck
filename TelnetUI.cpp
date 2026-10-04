#include "TelnetUI.h"
#include "NetScan.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "TextInputUI.h"
#include "NetToolsMenu.h"
#include "ConnStatus.h"

#include <Arduino.h>
#include <WiFi.h>

// Vedi TelnetUI.h per il contesto. Un solo host alla volta, un giro di
// controllo alla volta - niente scansione di intervalli di porte (quello
// e' gia' il lavoro di NetScan, mirato su una rete intera).
namespace {

    enum class Mode {
        Closed,
        Main,
        PickDevice,
        EditIp,
        EditCustomPorts,
        Results
    };
    Mode mode = Mode::Closed;

    // Navigazione da trackball (30/09, audit generale: la schermata era
    // touch-only) - un ordine lineare dei fuochi, Su/Giu' per scorrerli,
    // Select attiva quello a fuoco. Stesso principio "focused row" gia'
    // usato altrove nel progetto (es. VpnUI::Focus).
    enum class Focus {
        IpRow,
        PickBtn,
        Port80,
        Port443,
        Port22,
        CustomRow,
        GoBtn
    };
    Focus focusedButton = Focus::IpRow;

    String targetIp;
    bool port80On = true;
    bool port443On = true;
    bool port22On = false;
    String customPortsText; // es. "8080,3000"
    String resultText; // stato/errori brevi mostrati sulla schermata principale

    // Risultati del controllo porte (30/09) - schermata dedicata e
    // scorrevole invece di uno spazio fisso troppo corto: con molte
    // porte extra la lista non ci stava tutta (segnalato dall'utente).
    // Stesso schema scroll di NetGuardUI (topVisibleRow/dragAccumY +
    // TouchUI::pollDragDeltaY() per il trascinamento touch).
    constexpr int MAX_RESULT_LINES = 3 + 8; // porte classiche + MAX_CUSTOM_PORTS
    String resultLines[MAX_RESULT_LINES];
    int resultLineCount = 0;
    int resultsTopRow = 0;
    int resultsDragAccumY = 0;

    String editBuffer;

    constexpr int16_t BTN_X = 8;
    constexpr int16_t BTN_W = 304;
    constexpr int16_t BTN_H = 26;

    constexpr int16_t IP_ROW_Y = Display::DD_HEADER_H + 26;
    constexpr int16_t IP_ROW_H = 30;
    constexpr int16_t PICK_BTN_X = 230;
    constexpr int16_t PICK_BTN_W = 82;

    constexpr int16_t PORTS_LABEL_Y = IP_ROW_Y + IP_ROW_H + 8;
    constexpr int16_t PORT_CHIP_Y = PORTS_LABEL_Y + 12;
    constexpr int16_t PORT_CHIP_H = 26;
    constexpr int16_t PORT_CHIP_W = 60;
    constexpr int16_t PORT_CHIP_GAP = 8;

    constexpr int16_t CUSTOM_ROW_Y = PORT_CHIP_Y + PORT_CHIP_H + 14;
    constexpr int16_t CUSTOM_ROW_H = 28;

    constexpr int16_t GO_BTN_Y = CUSTOM_ROW_Y + CUSTOM_ROW_H + 10;

    constexpr int16_t RESULT_Y = GO_BTN_Y + BTN_H + 10;

    constexpr int16_t EDIT_FIELD_X = 8;
    constexpr int16_t EDIT_FIELD_Y = Display::DD_HEADER_H + 55;
    constexpr int16_t EDIT_FIELD_W = 288;
    constexpr int16_t EDIT_FIELD_H = 28;

    int ipRowZone = -1;
    int pickZone = -1;
    int port80Zone = -1;
    int port443Zone = -1;
    int port22Zone = -1;
    int customRowZone = -1;
    int goZone = -1;
    int editCancelZone = -1;

    constexpr int MAX_PICK_ROWS = 8;
    int pickRowZone[MAX_PICK_ROWS];
    int pickScroll = 0;

    void showMainScreen();

    void closeTelnet() {
        mode = Mode::Closed;
        NetToolsMenu::showMain();
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

    void drawButton(int16_t x, int16_t y, int16_t w, int16_t h, const char* label, bool focused) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(x + 1, y + 1, w - 2, h - 2, Display::DD_BG);
        gfx->drawRect(x, y, w, h, focused ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(focused ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setCursor(x + 6, y + h / 2 - 3);
        gfx->print(label);
    }

    // --- Parsing porte extra: "8080,3000" -> lista di interi ---
    constexpr int MAX_CUSTOM_PORTS = 8;

    int parseCustomPorts(int* out) {
        int count = 0;
        int start = 0;
        String text = customPortsText;
        text.trim();
        if (text.length() == 0) {
            return 0;
        }
        while (start <= static_cast<int>(text.length()) && count < MAX_CUSTOM_PORTS) {
            int comma = text.indexOf(',', start);
            String piece = (comma < 0) ? text.substring(start) : text.substring(start, comma);
            piece.trim();
            if (piece.length() > 0) {
                int port = piece.toInt();
                if (port > 0 && port <= 65535) {
                    out[count++] = port;
                }
            }
            if (comma < 0) {
                break;
            }
            start = comma + 1;
        }
        return count;
    }

    // --- Schermata principale ---

    void drawIpRow() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(BTN_X, IP_ROW_Y, BTN_W, IP_ROW_H, Display::DD_BG);
        gfx->drawRect(BTN_X, IP_ROW_Y, PICK_BTN_X - BTN_X - 4, IP_ROW_H,
            focusedButton == Focus::IpRow ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(BTN_X + 8, IP_ROW_Y + 4);
        gfx->print("IP BERSAGLIO (tocca per digitare)");
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 8, IP_ROW_Y + 16);
        gfx->print(targetIp.length() > 0 ? targetIp : "(non impostato)");

        drawButton(PICK_BTN_X, IP_ROW_Y, PICK_BTN_W, IP_ROW_H, "DA LISTA", focusedButton == Focus::PickBtn);
    }

    void drawPortChip(int16_t x, const char* label, bool on, bool focused) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(x, PORT_CHIP_Y, PORT_CHIP_W, PORT_CHIP_H, Display::DD_BG);
        gfx->drawRect(x, PORT_CHIP_Y, PORT_CHIP_W, PORT_CHIP_H,
            focused ? Display::DD_ACCENT : (on ? Display::DD_ACCENT_DIM : Display::DD_ACCENT_DIM));
        if (focused) {
            gfx->drawRect(x + 1, PORT_CHIP_Y + 1, PORT_CHIP_W - 2, PORT_CHIP_H - 2, Display::DD_ACCENT);
        }
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(on ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setCursor(x + PORT_CHIP_W / 2 - 8, PORT_CHIP_Y + PORT_CHIP_H / 2 - 3);
        gfx->print(label);
    }

    void drawPortsArea() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(BTN_X, PORTS_LABEL_Y - 10, BTN_W, PORT_CHIP_H + 20, Display::DD_BG);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(BTN_X, PORTS_LABEL_Y);
        gfx->print("PORTE CLASSICHE (tocca per attivare/disattivare):");

        drawPortChip(BTN_X, "80", port80On, focusedButton == Focus::Port80);
        drawPortChip(BTN_X + PORT_CHIP_W + PORT_CHIP_GAP, "443", port443On, focusedButton == Focus::Port443);
        drawPortChip(BTN_X + 2 * (PORT_CHIP_W + PORT_CHIP_GAP), "22", port22On, focusedButton == Focus::Port22);
    }

    void drawCustomRow() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(BTN_X, CUSTOM_ROW_Y, BTN_W, CUSTOM_ROW_H, Display::DD_BG);
        gfx->drawRect(BTN_X, CUSTOM_ROW_Y, BTN_W, CUSTOM_ROW_H,
            focusedButton == Focus::CustomRow ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(BTN_X + 6, CUSTOM_ROW_Y + 4);
        gfx->print("PORTE EXTRA (tocca, es. 8080,3000):");
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 6, CUSTOM_ROW_Y + 15);
        gfx->print(customPortsText.length() > 0 ? customPortsText : "(nessuna)");
    }

    void drawResult() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, RESULT_Y, 320, 240 - RESULT_Y, Display::DD_BG);
        Display::drawWrappedText(BTN_X, RESULT_Y, BTN_W, resultText, Display::DD_TEXT);
    }

    void drawMainScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("TELNET");

        TouchUI::clearZones();

        drawIpRow();
        ipRowZone = TouchUI::addZone(BTN_X, IP_ROW_Y, PICK_BTN_X - BTN_X - 4, IP_ROW_H);
        pickZone = TouchUI::addZone(PICK_BTN_X, IP_ROW_Y, PICK_BTN_W, IP_ROW_H);

        drawPortsArea();
        port80Zone = TouchUI::addZone(BTN_X, PORT_CHIP_Y, PORT_CHIP_W, PORT_CHIP_H);
        port443Zone = TouchUI::addZone(BTN_X + PORT_CHIP_W + PORT_CHIP_GAP, PORT_CHIP_Y, PORT_CHIP_W, PORT_CHIP_H);
        port22Zone = TouchUI::addZone(BTN_X + 2 * (PORT_CHIP_W + PORT_CHIP_GAP), PORT_CHIP_Y, PORT_CHIP_W, PORT_CHIP_H);

        drawCustomRow();
        customRowZone = TouchUI::addZone(BTN_X, CUSTOM_ROW_Y, BTN_W, CUSTOM_ROW_H);

        drawButton(BTN_X, GO_BTN_Y, BTN_W, BTN_H, "[ AVVIA ] CONTROLLA LE PORTE", focusedButton == Focus::GoBtn);
        goZone = TouchUI::addZone(BTN_X, GO_BTN_Y, BTN_W, BTN_H);

        drawResult();
    }

    void moveFocus(Focus f) {
        if (focusedButton == f) {
            return;
        }
        focusedButton = f;
        drawIpRow();
        drawPortsArea();
        drawCustomRow();
        drawButton(BTN_X, GO_BTN_Y, BTN_W, BTN_H, "[ AVVIA ] CONTROLLA LE PORTE", focusedButton == Focus::GoBtn);
    }

    void openIpEditScreen();
    void openPickScreen();
    void runCheck();
    void showResultsScreen();

    void activateFocused() {
        switch (focusedButton) {
            case Focus::IpRow:     openIpEditScreen(); break;
            case Focus::PickBtn:   openPickScreen(); break;
            case Focus::Port80:    port80On = !port80On; drawPortsArea(); break;
            case Focus::Port443:   port443On = !port443On; drawPortsArea(); break;
            case Focus::Port22:    port22On = !port22On; drawPortsArea(); break;
            case Focus::CustomRow: {
                editBuffer = customPortsText;
                mode = Mode::EditCustomPorts;
                Input::setTextEntryMode(true);
                Arduino_GFX* gfx = Display::getGfx();
                gfx->fillScreen(Display::DD_BG);
                Display::drawHeaderBar();
                gfx->setFont(nullptr);
                gfx->setTextSize(1);
                gfx->setTextColor(Display::DD_ACCENT);
                gfx->setCursor(8, Display::DD_HEADER_H + 8);
                gfx->print("PORTE EXTRA");
                TouchUI::clearZones();
                TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, editBuffer, false);
                Display::drawWrappedText(8, EDIT_FIELD_Y + EDIT_FIELD_H + 10, 304,
                    "Numeri separati da virgola, es. 8080,3000 - poi Invio.", Display::DD_TEXT_DIM);
                drawButton(BTN_X, 200, BTN_W, BTN_H, "[ ANNULLA ]", false);
                editCancelZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
                break;
            }
            case Focus::GoBtn:     runCheck(); break;
        }
    }

    void showMainScreen() {
        mode = Mode::Main;
        Input::setTextEntryMode(false);
        drawMainScreen();
    }

    // BLOCCANTE (connect() con timeout breve per porta, qualche secondo
    // in totale per l'intero giro) - va richiamata solo su azione
    // esplicita dell'utente (tasto AVVIA), mai da un ciclo periodico.
    void runCheck() {
        if (targetIp.length() == 0) {
            resultText = "Imposta prima un IP bersaglio.";
            drawResult();
            return;
        }
        if (!ConnStatus::isWifiConnected()) {
            resultText = "Serve una rete WiFi connessa.";
            drawResult();
            return;
        }

        int ports[3 + MAX_CUSTOM_PORTS];
        int portCount = 0;
        if (port80On) ports[portCount++] = 80;
        if (port443On) ports[portCount++] = 443;
        if (port22On) ports[portCount++] = 22;
        int customPorts[MAX_CUSTOM_PORTS];
        int customCount = parseCustomPorts(customPorts);
        for (int i = 0; i < customCount; i++) {
            ports[portCount++] = customPorts[i];
        }

        if (portCount == 0) {
            resultText = "Attiva almeno una porta (classica o extra).";
            drawResult();
            return;
        }

        resultText = "Controllo " + targetIp + " in corso...";
        drawResult();

        // Timeout per porta alzato a 3s su richiesta (30/09) - 1.2s dava
        // troppi falsi "chiusa" su reti/host un po' lenti a rispondere.
        constexpr uint32_t PORT_TIMEOUT_MS = 3000;

        resultLineCount = 0;
        for (int i = 0; i < portCount && resultLineCount < MAX_RESULT_LINES; i++) {
            WiFiClient client;
            bool open = client.connect(targetIp.c_str(), ports[i], PORT_TIMEOUT_MS);
            if (open) {
                client.stop();
            }
            resultLines[resultLineCount++] = String(ports[i]) + ": " + (open ? "APERTA" : "chiusa/non risponde");
        }

        resultText = ""; // non resta piu' il messaggio "in corso..." tornando alla schermata principale
        showResultsScreen();
    }

    // --- Schermata risultati, scorrevole (30/09) ---
    // Prima stavano schiacciati in uno spazio fisso troppo corto sotto
    // il bottone AVVIA - con parecchie porte extra la lista non ci stava
    // e non si vedeva tutta (segnalato dall'utente). Stesso schema di
    // scroll di NetGuardUI (vedi commenti li').

    constexpr int16_t RESULTS_LIST_TOP = Display::DD_HEADER_H + 22;
    constexpr int16_t RESULTS_ROW_H = 20;
    constexpr int RESULTS_VISIBLE_ROWS = (240 - RESULTS_LIST_TOP) / RESULTS_ROW_H;

    void drawResultsList() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, RESULTS_LIST_TOP, 320, 240 - RESULTS_LIST_TOP, Display::DD_BG);

        if (resultLineCount == 0) {
            Display::drawWrappedText(BTN_X, RESULTS_LIST_TOP, BTN_W, "Nessun risultato.", Display::DD_TEXT_DIM);
            return;
        }

        int lastVisible = resultsTopRow + RESULTS_VISIBLE_ROWS - 1;
        if (lastVisible > resultLineCount - 1) {
            lastVisible = resultLineCount - 1;
        }

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        for (int i = resultsTopRow; i <= lastVisible; i++) {
            int16_t y = RESULTS_LIST_TOP + (i - resultsTopRow) * RESULTS_ROW_H;
            bool isOpen = resultLines[i].endsWith("APERTA");
            gfx->setTextColor(isOpen ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
            gfx->setCursor(BTN_X, y + 3);
            gfx->print(resultLines[i]);
        }

        // Indicatore semplice "ci sono altre righe sopra/sotto", stesso
        // principio minimale gia' usato altrove (nessuna vera scrollbar).
        if (resultsTopRow > 0) {
            gfx->setTextColor(Display::DD_ACCENT_DIM);
            gfx->setCursor(300, RESULTS_LIST_TOP);
            gfx->print("^");
        }
        if (lastVisible < resultLineCount - 1) {
            gfx->setTextColor(Display::DD_ACCENT_DIM);
            gfx->setCursor(300, RESULTS_LIST_TOP + (RESULTS_VISIBLE_ROWS - 1) * RESULTS_ROW_H);
            gfx->print("v");
        }
    }

    void drawResultsScreen() {
        Display::getGfx()->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("RISULTATI");
        TouchUI::clearZones();
        drawResultsList();
    }

    void showResultsScreen() {
        mode = Mode::Results;
        resultsTopRow = 0;
        resultsDragAccumY = 0;
        Input::setTextEntryMode(false);
        drawResultsScreen();
    }

    void handleResultsInput() {
        int16_t dragDelta = TouchUI::pollDragDeltaY();
        if (dragDelta != 0 && resultLineCount > 0) {
            resultsDragAccumY += dragDelta;

            int maxTop = resultLineCount - RESULTS_VISIBLE_ROWS;
            if (maxTop < 0) {
                maxTop = 0;
            }

            bool scrolled = false;
            while (resultsDragAccumY >= RESULTS_ROW_H && resultsTopRow > 0) {
                resultsTopRow--;
                resultsDragAccumY -= RESULTS_ROW_H;
                scrolled = true;
            }
            while (resultsDragAccumY <= -RESULTS_ROW_H && resultsTopRow < maxTop) {
                resultsTopRow++;
                resultsDragAccumY += RESULTS_ROW_H;
                scrolled = true;
            }
            if (scrolled) {
                drawResultsList();
            }
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (resultsTopRow > 0) {
                    resultsTopRow--;
                    drawResultsList();
                }
                break;
            case InputEvent::Down: {
                int maxTop = resultLineCount - RESULTS_VISIBLE_ROWS;
                if (maxTop < 0) maxTop = 0;
                if (resultsTopRow < maxTop) {
                    resultsTopRow++;
                    drawResultsList();
                }
                break;
            }
            case InputEvent::Back:
            case InputEvent::Select:
                showMainScreen();
                break;
            default:
                break;
        }
    }

    void openIpEditScreen() {
        editBuffer = targetIp;
        mode = Mode::EditIp;
        Input::setTextEntryMode(true);
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("IP BERSAGLIO");
        TouchUI::clearZones();
        TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, editBuffer, false);
        Display::drawWrappedText(8, EDIT_FIELD_Y + EDIT_FIELD_H + 10, 304,
            "Es. 192.168.1.10 - poi Invio per confermare.", Display::DD_TEXT_DIM);
        drawButton(BTN_X, 200, BTN_W, BTN_H, "[ ANNULLA ]", false);
        editCancelZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
    }

    int selectedPickRow = 0;

    void drawPickRow(int index, int count) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t y = Display::DD_HEADER_H + 24 + index * 24;
        NetScan::Device d = NetScan::getDevice(index);
        bool selected = (index == selectedPickRow);
        gfx->fillRect(BTN_X, y, BTN_W, 22, Display::DD_BG);
        gfx->drawRect(BTN_X, y, BTN_W, 22, selected ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 6, y + 6);
        gfx->print(d.ip + "  " + d.mac);
        pickRowZone[index] = TouchUI::addZone(BTN_X, y, BTN_W, 22);
    }

    void openPickScreen() {
        mode = Mode::PickDevice;
        pickScroll = 0;
        selectedPickRow = 0;
        Input::setTextEntryMode(false);
        Display::getGfx()->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("SCEGLI DALLA LISTA");
        TouchUI::clearZones();
        int count = NetScan::getDeviceCount();
        if (count == 0) {
            Display::drawWrappedText(BTN_X, Display::DD_HEADER_H + 26, BTN_W,
                "Nessun dispositivo in lista - fai prima un NETSCAN.", Display::DD_TEXT_DIM);
        } else {
            for (int i = 0; i < count && i < MAX_PICK_ROWS; i++) {
                drawPickRow(i, count);
            }
        }
    }

    void handleMainInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == ipRowZone) {
            moveFocus(Focus::IpRow);
            openIpEditScreen();
            return;
        }
        if (tapped == pickZone) {
            moveFocus(Focus::PickBtn);
            openPickScreen();
            return;
        }
        if (tapped == port80Zone) {
            moveFocus(Focus::Port80);
            port80On = !port80On;
            drawPortsArea();
            return;
        }
        if (tapped == port443Zone) {
            moveFocus(Focus::Port443);
            port443On = !port443On;
            drawPortsArea();
            return;
        }
        if (tapped == port22Zone) {
            moveFocus(Focus::Port22);
            port22On = !port22On;
            drawPortsArea();
            return;
        }
        if (tapped == customRowZone) {
            moveFocus(Focus::CustomRow);
            activateFocused();
            return;
        }
        if (tapped == goZone) {
            moveFocus(Focus::GoBtn);
            runCheck();
            return;
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                switch (focusedButton) {
                    case Focus::PickBtn:   moveFocus(Focus::IpRow); break;
                    case Focus::Port80:
                    case Focus::Port443:
                    case Focus::Port22:    moveFocus(Focus::IpRow); break;
                    case Focus::CustomRow: moveFocus(Focus::Port80); break;
                    case Focus::GoBtn:     moveFocus(Focus::CustomRow); break;
                    default: break;
                }
                break;
            case InputEvent::Down:
                switch (focusedButton) {
                    case Focus::IpRow:
                    case Focus::PickBtn:   moveFocus(Focus::Port80); break;
                    case Focus::Port80:
                    case Focus::Port443:
                    case Focus::Port22:    moveFocus(Focus::CustomRow); break;
                    case Focus::CustomRow: moveFocus(Focus::GoBtn); break;
                    default: break;
                }
                break;
            case InputEvent::Left:
                switch (focusedButton) {
                    case Focus::PickBtn: moveFocus(Focus::IpRow); break;
                    case Focus::Port443: moveFocus(Focus::Port80); break;
                    case Focus::Port22:  moveFocus(Focus::Port443); break;
                    default: break;
                }
                break;
            case InputEvent::Right:
                switch (focusedButton) {
                    case Focus::IpRow:   moveFocus(Focus::PickBtn); break;
                    case Focus::Port80:  moveFocus(Focus::Port443); break;
                    case Focus::Port443: moveFocus(Focus::Port22); break;
                    default: break;
                }
                break;
            case InputEvent::Select:
                activateFocused();
                break;
            case InputEvent::Back:
                closeTelnet();
                break;
            default:
                break;
        }
    }

    void handlePickInput() {
        int tapped = TouchUI::pollTap();
        int count = NetScan::getDeviceCount();
        int shown = count < MAX_PICK_ROWS ? count : MAX_PICK_ROWS;
        if (tapped >= 0) {
            for (int i = 0; i < shown; i++) {
                if (pickRowZone[i] == tapped) {
                    targetIp = NetScan::getDevice(i).ip;
                    showMainScreen();
                    return;
                }
            }
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (shown > 0 && selectedPickRow > 0) {
                    selectedPickRow--;
                    drawPickRow(selectedPickRow, count);
                    drawPickRow(selectedPickRow + 1, count);
                }
                break;
            case InputEvent::Down:
                if (shown > 0 && selectedPickRow < shown - 1) {
                    selectedPickRow++;
                    drawPickRow(selectedPickRow, count);
                    drawPickRow(selectedPickRow - 1, count);
                }
                break;
            case InputEvent::Select:
                if (shown > 0) {
                    targetIp = NetScan::getDevice(selectedPickRow).ip;
                    showMainScreen();
                }
                break;
            case InputEvent::Back:
                showMainScreen();
                break;
            default:
                break;
        }
    }

    void handleEditIpInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == editCancelZone) {
            showMainScreen();
            return;
        }
        TextInputUI::Result result = TextInputUI::handleKey(editBuffer, 39);
        if (result == TextInputUI::Result::Changed) {
            TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, editBuffer, false);
        } else if (result == TextInputUI::Result::Confirmed) {
            targetIp = editBuffer;
            targetIp.trim();
            showMainScreen();
        }
    }

    void handleEditCustomPortsInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == editCancelZone) {
            showMainScreen();
            return;
        }
        TextInputUI::Result result = TextInputUI::handleKey(editBuffer, 40);
        if (result == TextInputUI::Result::Changed) {
            TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, editBuffer, false);
        } else if (result == TextInputUI::Result::Confirmed) {
            customPortsText = editBuffer;
            customPortsText.trim();
            showMainScreen();
        }
    }

} // namespace

namespace TelnetUI {

    void open() {
        resultText = "";
        Display::setDirectDrawMode(true);
        showMainScreen();
    }

    bool isActive() {
        return mode != Mode::Closed;
    }

    void update() {
        switch (mode) {
            case Mode::Closed:
                break;
            case Mode::Main:
                handleMainInput();
                break;
            case Mode::PickDevice:
                handlePickInput();
                break;
            case Mode::EditIp:
                handleEditIpInput();
                break;
            case Mode::EditCustomPorts:
                handleEditCustomPortsInput();
                break;
            case Mode::Results:
                handleResultsInput();
                break;
        }
    }
}
