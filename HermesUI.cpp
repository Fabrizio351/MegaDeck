#include "HermesUI.h"
#include "HermesClient.h"
#include "Display.h"
#include "Input.h"
#include "Menu.h"
#include "TouchUI.h"
#include "TextInputUI.h"
#include "ConnStatus.h"
#include "GPS.h"

#include <Arduino.h>

// Chat con Hermes (07/10, sostituisce TelegramUI/OpenClawUI - stessa
// forma a fumetti, tastiera fisica per scrivere) - l'invio e'
// BLOCCANTE: una richiesta HTTP con risposta diretta nella stessa
// chiamata (vedi HermesClient.h), non asincrono come il tentativo
// Telegram appena scartato.
namespace {

    constexpr int MAX_MESSAGES = 30;
    constexpr int MAX_MESSAGE_LEN = 500;

    struct ChatMessage {
        String text;
        bool outgoing;
    };

    ChatMessage messages[MAX_MESSAGES];
    int messageCount = 0;

    enum class Mode {
        Closed,
        PinEntry,
        Chat,
        Compose,
        Config,
        EditField
    };
    Mode mode = Mode::Closed;

    // PIN fisso - richiesto: "se lo perde e qualcuno lo accende senza
    // pin non puo' usare l'assistente con API a pagamento" - chiesto
    // una SOLA volta per sessione di accensione: RAM, non NVS, quindi
    // si azzera da solo spegnendo/riavviando il T-Deck. Non protegge
    // openConfig() (raggiungibile da Impostazioni): quello schermo
    // modifica solo host/porta/token, non manda mai una richiesta HTTP.
    // Valore reale di questa installazione - locale, NON committato
    // (vedi la versione pubblica su git: segnaposto "0000" li').
    constexpr const char* BOT_PIN = "0000";
    bool unlocked = false;
    String pinBuffer;
    bool pinError = false;

    // Vero quando CONFIG e' stato aperto direttamente da Impostazioni,
    // non passando dalla chat - in quel caso uscire da CONFIG (FATTO/
    // Indietro) deve tornare a Impostazioni, non alla chat (che
    // potrebbe anche non essere raggiungibile, es. senza WiFi).
    bool openedFromSettings = false;

    enum class ConfigField {
        Host,
        Port,
        Token,
        Timeout
    };
    constexpr int CONFIG_FIELD_COUNT = 4;
    ConfigField editingField = ConfigField::Host;
    String editBuffer;
    int selectedConfigRow = 0;

    int topVisibleMsgIndex = 0;
    int topMsgLineOffset = 0;
    bool followingTail = true;
    int dragAccumY = 0;
    String composeBuffer;
    bool sending = false;

    constexpr int16_t BTN_X = 8;
    constexpr int16_t BTN_W = 304;
    constexpr int16_t BTN_H = 28;
    constexpr int16_t BOX_PAD = 6;
    constexpr int16_t BOX_GAP = 4;
    constexpr int16_t TEXT_W = BTN_W - (BOX_PAD * 2) - 2;

    constexpr int16_t STATUS_Y = Display::DD_HEADER_H + 20;
    constexpr int16_t LIST_TOP = STATUS_Y + 14;
    constexpr int16_t COMPOSE_BTN_Y = 206;
    constexpr int16_t LIST_BOTTOM = COMPOSE_BTN_Y - 4;

    constexpr int16_t ROW_GAP = 4;
    constexpr int16_t WRITE_BTN_W = (BTN_W - ROW_GAP) * 2 / 3;
    constexpr int16_t GPS_BTN_X = BTN_X + WRITE_BTN_W + ROW_GAP;
    constexpr int16_t GPS_BTN_W = BTN_W - ROW_GAP - WRITE_BTN_W;
    int gpsZone = -1;

    constexpr int16_t COMPOSE_BOX_Y = Display::DD_HEADER_H + 26;
    constexpr int16_t COMPOSE_BOX_H = 90;
    constexpr int16_t SCROLL_STEP_PX = 40;

    constexpr int16_t EDIT_FIELD_X = 8;
    constexpr int16_t EDIT_FIELD_Y = Display::DD_HEADER_H + 55;
    constexpr int16_t EDIT_FIELD_W = 280;
    constexpr int16_t EDIT_FIELD_H = 28;

    constexpr int16_t CFG_ROW_TOP = Display::DD_HEADER_H + 30;
    constexpr int16_t CFG_ROW_H = 34;

    int composeZone = -1;
    int cancelZone = -1;
    int cfgRowZone[CONFIG_FIELD_COUNT];
    int cfgDoneZone = -1;

    void showPinScreen();
    void showChatScreen();
    void drawChatScreen();
    void showConfigScreen();

    void closeChat() {
        mode = Mode::Closed;
        Menu::back();
    }

    void appendMessage(const String& text, bool outgoing) {
        if (messageCount >= MAX_MESSAGES) {
            for (int i = 1; i < MAX_MESSAGES; i++) {
                messages[i - 1] = messages[i];
            }
            messageCount = MAX_MESSAGES - 1;
        }
        messages[messageCount].text = text;
        messages[messageCount].outgoing = outgoing;
        messageCount++;
    }

    String buildMessageText(int index) {
        const ChatMessage& msg = messages[index];
        return (msg.outgoing ? "TU: " : "HERMES: ") + msg.text;
    }

    int16_t messageBoxHeight(int index) {
        return Display::wrappedTextHeight(buildMessageText(index), TEXT_W) + (BOX_PAD * 2);
    }

    int computeMaxTopIndex() {
        if (messageCount == 0) {
            return 0;
        }
        int16_t viewportH = LIST_BOTTOM - LIST_TOP;
        int i = messageCount - 1;
        int16_t used = 0;
        int firstVisible = i;

        while (i >= 0) {
            int16_t h = messageBoxHeight(i) + BOX_GAP;
            if (used > 0 && used + h > viewportH) {
                break;
            }
            used += h;
            firstVisible = i;
            i--;
        }
        return firstVisible;
    }

    int visibleTextLines() {
        int16_t viewportH = LIST_BOTTOM - LIST_TOP - (BOX_PAD * 2);
        int lines = viewportH / 11;
        return lines > 0 ? lines : 1;
    }

    int topMsgOverflowLines() {
        if (messageCount == 0) {
            return 0;
        }
        int total = Display::wrappedLineCount(buildMessageText(topVisibleMsgIndex), TEXT_W);
        int overflow = total - visibleTextLines();
        return overflow > 0 ? overflow : 0;
    }

    bool scrollStepDown() {
        if (topMsgLineOffset < topMsgOverflowLines()) {
            topMsgLineOffset++;
            return true;
        }
        int maxTop = computeMaxTopIndex();
        if (topVisibleMsgIndex < maxTop) {
            topVisibleMsgIndex++;
            topMsgLineOffset = 0;
            return true;
        }
        return false;
    }

    bool scrollStepUp() {
        if (topMsgLineOffset > 0) {
            topMsgLineOffset--;
            return true;
        }
        if (topVisibleMsgIndex > 0) {
            topVisibleMsgIndex--;
            topMsgLineOffset = topMsgOverflowLines();
            return true;
        }
        return false;
    }

    void ensureBottomVisible() {
        topVisibleMsgIndex = computeMaxTopIndex();
        topMsgLineOffset = 0;
        followingTail = true;
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
        gfx->fillRect(BTN_X + 1, y + 1, BTN_W - 2, BTN_H - 2, Display::DD_BG);
        gfx->drawRect(BTN_X, y, BTN_W, BTN_H, focused ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 12, y + BTN_H / 2 - 3);
        gfx->print(label);
    }

    void drawStatusTag() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, STATUS_Y, 320, 11, Display::DD_BG);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        if (sending) {
            gfx->setTextColor(gfx->color565(0xE6, 0x7E, 0x22));
            gfx->setCursor(8, STATUS_Y);
            gfx->print("IN ATTESA DI RISPOSTA...");
        } else if (!HermesClient::isConfigured()) {
            gfx->setTextColor(Display::DD_WARN);
            gfx->setCursor(8, STATUS_Y);
            gfx->print("Non configurato - tocca CONFIG qui sotto.");
        } else {
            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->setCursor(8, STATUS_Y);
            String line = HermesClient::getHost() + ":" + String(HermesClient::getPort());
            gfx->print(line);
        }
    }

    void drawMessageBox(int index, int16_t y) {
        Arduino_GFX* gfx = Display::getGfx();
        String text = buildMessageText(index);
        int16_t boxH = messageBoxHeight(index);

        uint16_t borderColor = messages[index].outgoing
            ? Display::DD_ACCENT
            : gfx->color565(0x3D, 0xB8, 0xF5);

        gfx->drawRect(BTN_X, y, BTN_W, boxH, borderColor);
        Display::drawWrappedText(BTN_X + BOX_PAD, y + BOX_PAD, TEXT_W, text, Display::DD_TEXT);
    }

    void drawChatScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("HERMES");
        drawStatusTag();

        TouchUI::clearZones();

        if (messageCount == 0) {
            Display::drawWrappedText(
                BTN_X, LIST_TOP + 4, TEXT_W,
                "Nessun messaggio ancora.\nScrivi qualcosa per iniziare a parlare con Hermes.",
                Display::DD_TEXT_DIM
            );
        } else if (topMsgOverflowLines() > 0) {
            Arduino_GFX* gfx = Display::getGfx();
            uint16_t borderColor = messages[topVisibleMsgIndex].outgoing
                ? Display::DD_ACCENT
                : gfx->color565(0x3D, 0xB8, 0xF5);
            gfx->drawRect(BTN_X, LIST_TOP, BTN_W, LIST_BOTTOM - LIST_TOP, borderColor);
            Display::drawWrappedTextRange(
                BTN_X + BOX_PAD, LIST_TOP + BOX_PAD, TEXT_W,
                buildMessageText(topVisibleMsgIndex), Display::DD_TEXT,
                topMsgLineOffset, visibleTextLines()
            );
        } else {
            int16_t y = LIST_TOP;
            int i = topVisibleMsgIndex;
            while (i < messageCount) {
                int16_t h = messageBoxHeight(i);
                if (i != topVisibleMsgIndex && y + h > LIST_BOTTOM) {
                    break;
                }
                drawMessageBox(i, y);
                y += h + BOX_GAP;
                i++;
            }
        }

        gfx->fillRect(BTN_X + 1, COMPOSE_BTN_Y + 1, WRITE_BTN_W - 2, BTN_H - 2, Display::DD_BG);
        gfx->drawRect(BTN_X, COMPOSE_BTN_Y, WRITE_BTN_W, BTN_H, Display::DD_ACCENT);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 12, COMPOSE_BTN_Y + BTN_H / 2 - 3);
        gfx->print("[ SCRIVI ] NUOVO MESSAGGIO");
        composeZone = TouchUI::addZone(BTN_X, COMPOSE_BTN_Y, WRITE_BTN_W, BTN_H);

        bool gpsFix = GPS::hasFix();
        gfx->fillRect(GPS_BTN_X + 1, COMPOSE_BTN_Y + 1, GPS_BTN_W - 2, BTN_H - 2, Display::DD_BG);
        gfx->drawRect(GPS_BTN_X, COMPOSE_BTN_Y, GPS_BTN_W, BTN_H, gpsFix ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setTextColor(gpsFix ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setCursor(GPS_BTN_X + 8, COMPOSE_BTN_Y + BTN_H / 2 - 3);
        gfx->print("[ GPS ]");
        gpsZone = TouchUI::addZone(GPS_BTN_X, COMPOSE_BTN_Y, GPS_BTN_W, BTN_H);
    }

    void drawPinScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("HERMES - PIN");

        TouchUI::clearZones();

        TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, pinBuffer, true);

        Display::drawWrappedText(
            8, EDIT_FIELD_Y + EDIT_FIELD_H + 10, 304,
            pinError ? "PIN errato, riprova." : "Inserisci il PIN per usare Hermes.",
            pinError ? Display::DD_WARN : Display::DD_TEXT_DIM
        );

        drawButton(200, "[ ESCI ]");
        cancelZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
    }

    void showPinScreen() {
        mode = Mode::PinEntry;
        pinBuffer = "";
        pinError = false;
        Display::setDirectDrawMode(true);
        Input::setTextEntryMode(true);
        drawPinScreen();
    }

    void proceedAfterUnlock() {
        Input::setTextEntryMode(false);
        if (!ConnStatus::isWifiConnected()) {
            Display::showMessage(
                "Hermes",
                "Devi prima connetterti a una rete WiFi.",
                []() { Menu::back(); }
            );
            return;
        }
        if (!HermesClient::isConfigured()) {
            showConfigScreen();
        } else {
            showChatScreen();
        }
    }

    void confirmPin() {
        if (pinBuffer == BOT_PIN) {
            unlocked = true;
            pinBuffer = "";
            pinError = false;
            proceedAfterUnlock();
        } else {
            pinBuffer = "";
            pinError = true;
            drawPinScreen();
        }
    }

    void handlePinInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == cancelZone) {
            mode = Mode::Closed;
            Input::setTextEntryMode(false);
            Menu::back();
            return;
        }

        TextInputUI::Result result = TextInputUI::handleKey(pinBuffer, 16);
        if (result == TextInputUI::Result::Changed) {
            TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, pinBuffer, true);
        } else if (result == TextInputUI::Result::Confirmed) {
            confirmPin();
        }
    }

    void showChatScreen() {
        mode = Mode::Chat;
        Input::setTextEntryMode(false);
        ensureBottomVisible();
        drawChatScreen();
    }

    void sendComposedMessage() {
        String text = composeBuffer;
        text.trim();
        if (text.length() == 0) {
            return;
        }

        appendMessage(text, true);
        composeBuffer = "";
        sending = true;
        showChatScreen();

        HermesClient::Result result = HermesClient::sendMessage(text);
        sending = false;

        if (result.valid) {
            appendMessage(result.reply, false);
        } else {
            appendMessage("(errore: " + result.error + ")", false);
        }

        if (followingTail) {
            ensureBottomVisible();
        }
        drawChatScreen();
    }

    // Condivide la posizione GPS come normale messaggio di testo -
    // Hermes e' una chat testuale con un LLM, non c'e' un formato
    // "posizione" dedicato come in Meshtastic: le coordinate vengono
    // semplicemente scritte in chiaro nel messaggio.
    void sendGpsPosition() {
        if (!GPS::hasFix()) {
            appendMessage("(posizione non inviata: nessun fix GPS)", true);
            if (followingTail) {
                ensureBottomVisible();
            }
            drawChatScreen();
            return;
        }

        String text = "La mia posizione attuale: " + String(GPS::getLatitude(), 6) + ", " + String(GPS::getLongitude(), 6);
        appendMessage(text, true);
        sending = true;
        showChatScreen();

        HermesClient::Result result = HermesClient::sendMessage(text);
        sending = false;

        if (result.valid) {
            appendMessage(result.reply, false);
        } else {
            appendMessage("(errore: " + result.error + ")", false);
        }

        if (followingTail) {
            ensureBottomVisible();
        }
        drawChatScreen();
    }

    void drawComposeScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("NUOVO MESSAGGIO");

        TouchUI::clearZones();

        gfx->drawRect(BTN_X, COMPOSE_BOX_Y, BTN_W, COMPOSE_BOX_H, Display::DD_ACCENT_DIM);
        Display::drawWrappedText(BTN_X + BOX_PAD, COMPOSE_BOX_Y + BOX_PAD, BTN_W - (BOX_PAD * 2), composeBuffer, Display::DD_TEXT);

        Display::drawWrappedText(
            BTN_X, COMPOSE_BOX_Y + COMPOSE_BOX_H + 8, BTN_W,
            "Scrivi con la tastiera fisica, Invio per inviare.",
            Display::DD_TEXT_DIM
        );

        drawButton(200, "[ ANNULLA ]");
        cancelZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
    }

    void showComposeScreen() {
        mode = Mode::Compose;
        Input::setTextEntryMode(true);
        composeBuffer = "";
        drawComposeScreen();
    }

    // --- Configurazione (host/porta/token/timeout) ---

    const char* fieldLabel(ConfigField f) {
        switch (f) {
            case ConfigField::Host:    return "INDIRIZZO";
            case ConfigField::Port:    return "PORTA";
            case ConfigField::Token:   return "TOKEN";
            case ConfigField::Timeout: return "TIMEOUT (SECONDI)";
        }
        return "?";
    }

    String fieldValue(ConfigField f) {
        switch (f) {
            case ConfigField::Host:    return HermesClient::getHost();
            case ConfigField::Port:    return HermesClient::getPort() > 0 ? String(HermesClient::getPort()) : "";
            case ConfigField::Token:   return HermesClient::getToken();
            case ConfigField::Timeout: return String(HermesClient::getTimeoutSeconds());
        }
        return "";
    }

    void drawConfigRow(int index) {
        Arduino_GFX* gfx = Display::getGfx();
        ConfigField f = static_cast<ConfigField>(index);
        int16_t y = CFG_ROW_TOP + index * CFG_ROW_H;
        bool selected = (index == selectedConfigRow);

        gfx->fillRect(BTN_X, y, BTN_W, CFG_ROW_H - 4, Display::DD_BG);
        gfx->drawRect(BTN_X, y, BTN_W, CFG_ROW_H - 4, selected ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        if (selected) {
            gfx->fillRect(BTN_X + 3, y + 4, 4, CFG_ROW_H - 12, Display::DD_ACCENT);
        }

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(BTN_X + 14, y + 4);
        gfx->print(fieldLabel(f));

        String value = fieldValue(f);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 14, y + 16);
        gfx->print(value.length() > 0 ? value : "(non impostato)");
    }

    void selectConfigRow(int index) {
        int previous = selectedConfigRow;
        selectedConfigRow = index;
        if (previous != index) {
            drawConfigRow(previous);
        }
        drawConfigRow(index);
    }

    void drawConfigScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("HERMES CONFIG");

        Display::drawWrappedText(
            BTN_X, Display::DD_HEADER_H + 18, BTN_W,
            "Tocca una riga per modificarla (es. se sposti la macchina di Hermes altrove).",
            Display::DD_TEXT_DIM
        );

        TouchUI::clearZones();
        for (int i = 0; i < CONFIG_FIELD_COUNT; i++) {
            drawConfigRow(i);
            int16_t y = CFG_ROW_TOP + i * CFG_ROW_H;
            cfgRowZone[i] = TouchUI::addZone(BTN_X, y, BTN_W, CFG_ROW_H - 4);
        }

        drawButton(206, "[ FATTO ]");
        cfgDoneZone = TouchUI::addZone(BTN_X, 206, BTN_W, BTN_H);
    }

    void showConfigScreen() {
        mode = Mode::Config;
        selectedConfigRow = 0;
        Input::setTextEntryMode(false);
        drawConfigScreen();
    }

    void exitConfigScreen() {
        if (openedFromSettings) {
            openedFromSettings = false;
            mode = Mode::Closed;
            Menu::back();
        } else {
            showChatScreen();
        }
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
        gfx->print(fieldLabel(editingField));

        TouchUI::clearZones();

        TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, editBuffer, false);

        Display::drawWrappedText(
            8, EDIT_FIELD_Y + EDIT_FIELD_H + 10, 304,
            "Scrivi con la tastiera fisica, poi Invio per confermare.",
            Display::DD_TEXT_DIM
        );

        drawButton(200, "[ ANNULLA ]");
        cancelZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
    }

    void showEditScreen(ConfigField field) {
        mode = Mode::EditField;
        editingField = field;
        editBuffer = fieldValue(field);
        Input::setTextEntryMode(true);
        drawEditScreen();
    }

    void confirmEdit() {
        String trimmed = editBuffer;
        trimmed.trim();

        String host = HermesClient::getHost();
        int port = HermesClient::getPort();
        String token = HermesClient::getToken();
        int timeoutSeconds = HermesClient::getTimeoutSeconds();

        switch (editingField) {
            case ConfigField::Host:    host = trimmed; break;
            case ConfigField::Port:    port = trimmed.toInt(); break;
            case ConfigField::Token:   token = trimmed; break;
            case ConfigField::Timeout: timeoutSeconds = trimmed.toInt(); break;
        }

        HermesClient::setConfig(host, port, token, timeoutSeconds);
        showConfigScreen();
    }

    bool isAtBottom() {
        return topVisibleMsgIndex >= computeMaxTopIndex() && topMsgLineOffset >= topMsgOverflowLines();
    }

    void handleChatInput() {
        int16_t dragDelta = TouchUI::pollDragDeltaY();
        if (dragDelta != 0 && messageCount > 0) {
            dragAccumY += dragDelta;

            bool scrolled = false;
            while (dragAccumY >= SCROLL_STEP_PX) {
                if (!scrollStepUp()) {
                    dragAccumY = SCROLL_STEP_PX;
                    break;
                }
                dragAccumY -= SCROLL_STEP_PX;
                scrolled = true;
            }
            while (dragAccumY <= -SCROLL_STEP_PX) {
                if (!scrollStepDown()) {
                    dragAccumY = -SCROLL_STEP_PX;
                    break;
                }
                dragAccumY += SCROLL_STEP_PX;
                scrolled = true;
            }

            followingTail = isAtBottom();
            if (scrolled) {
                drawChatScreen();
            }
        }

        int tapped = TouchUI::pollTap();
        if (tapped == composeZone) {
            showComposeScreen();
            return;
        }
        if (tapped == gpsZone) {
            sendGpsPosition();
            return;
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Back:
                closeChat();
                break;

            case InputEvent::Select:
                showComposeScreen();
                break;

            case InputEvent::Right:
                sendGpsPosition();
                break;

            case InputEvent::Up:
                if (scrollStepUp()) {
                    followingTail = isAtBottom();
                    drawChatScreen();
                }
                break;

            case InputEvent::Down:
                if (scrollStepDown()) {
                    followingTail = isAtBottom();
                    drawChatScreen();
                }
                break;

            default:
                break;
        }
    }

    void handleComposeInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == cancelZone) {
            showChatScreen();
            return;
        }

        TextInputUI::Result result = TextInputUI::handleKey(composeBuffer, MAX_MESSAGE_LEN);
        if (result == TextInputUI::Result::Changed) {
            Arduino_GFX* gfx = Display::getGfx();
            gfx->fillRect(BTN_X + 1, COMPOSE_BOX_Y + 1, BTN_W - 2, COMPOSE_BOX_H - 2, Display::DD_BG);
            Display::drawWrappedText(BTN_X + BOX_PAD, COMPOSE_BOX_Y + BOX_PAD, BTN_W - (BOX_PAD * 2), composeBuffer, Display::DD_TEXT);
        } else if (result == TextInputUI::Result::Confirmed) {
            sendComposedMessage();
        }
    }

    void handleConfigInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == cfgDoneZone) {
            exitConfigScreen();
            return;
        }
        for (int i = 0; i < CONFIG_FIELD_COUNT; i++) {
            if (tapped == cfgRowZone[i]) {
                selectConfigRow(i);
                showEditScreen(static_cast<ConfigField>(i));
                return;
            }
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Back:
                exitConfigScreen();
                break;

            case InputEvent::Up:
                if (selectedConfigRow > 0) {
                    selectConfigRow(selectedConfigRow - 1);
                }
                break;

            case InputEvent::Down:
                if (selectedConfigRow < CONFIG_FIELD_COUNT - 1) {
                    selectConfigRow(selectedConfigRow + 1);
                }
                break;

            case InputEvent::Select:
                showEditScreen(static_cast<ConfigField>(selectedConfigRow));
                break;

            default:
                break;
        }
    }

    void handleEditInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == cancelZone) {
            showConfigScreen();
            return;
        }

        uint16_t maxLen = (editingField == ConfigField::Port || editingField == ConfigField::Timeout) ? 5 : 64;
        TextInputUI::Result result = TextInputUI::handleKey(editBuffer, maxLen);
        if (result == TextInputUI::Result::Changed) {
            TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, editBuffer, false);
        } else if (result == TextInputUI::Result::Confirmed) {
            confirmEdit();
        }
    }

} // namespace

namespace HermesUI {

    void open() {
        if (!unlocked) {
            showPinScreen();
            return;
        }
        proceedAfterUnlock();
    }

    void openConfig() {
        openedFromSettings = true;
        Display::setDirectDrawMode(true);
        showConfigScreen();
    }

    bool isActive() {
        return mode != Mode::Closed;
    }

    void update() {
        switch (mode) {
            case Mode::Closed:
                break;
            case Mode::PinEntry:
                handlePinInput();
                break;
            case Mode::Chat:
                handleChatInput();
                break;
            case Mode::Compose:
                handleComposeInput();
                break;
            case Mode::Config:
                handleConfigInput();
                break;
            case Mode::EditField:
                handleEditInput();
                break;
        }
    }
}
