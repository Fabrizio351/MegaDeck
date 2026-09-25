#include "LoRaChatUI.h"
#include "Display.h"
#include "Input.h"
#include "Menu.h"
#include "LoRaRadio.h"
#include "Sound.h"
#include "TouchUI.h"
#include "TextInputUI.h"

#include <Arduino.h>

// Rimozione finale di LVGL dal progetto (26/09, vedi
// project_lvgl_removal_rewrite in memoria): ultima schermata reale
// convertita a disegno diretto. Prima vera lista a "fumetti" di altezza
// variabile (non righe fisse come Menu/NetScanUI): lo scorrimento resta
// comunque a messaggi interi (mai un fumetto tagliato a meta'), stesso
// principio del "niente clipping parziale" gia' scelto per le liste a
// riga fissa.
namespace {

    constexpr int MAX_MESSAGES = 30;
    // Limite di lunghezza messaggio: con lo spreading factor attuale
    // (RADIO_SF in Config.h) l'invio e' bloccante e piu' lungo e' il
    // messaggio piu' dura la trasmissione - tenerlo corto mantiene i
    // tempi di invio ragionevoli.
    constexpr int MAX_MESSAGE_LEN = 180;

    struct ChatMessage {
        String text;
        bool outgoing;
        float rssi;
    };

    ChatMessage messages[MAX_MESSAGES];
    int messageCount = 0;

    enum class Mode {
        Closed,
        Chat,
        Compose
    };
    Mode mode = Mode::Closed;

    // Vedi LoRaChatUI::hasUnreadMessages(): true da quando arriva un
    // messaggio a chat chiusa fino alla riapertura della chat.
    bool hasUnread = false;

    int topVisibleMsgIndex = 0;
    // true quando la vista e' "agganciata" all'ultimo messaggio: un nuovo
    // arrivo la segue automaticamente. Diventa false appena l'utente
    // scorre indietro a leggere la cronologia, per non "strapparlo" via
    // da li' con un nuovo messaggio in arrivo.
    bool followingTail = true;
    int dragAccumY = 0;
    String lastStatusText;

    String composeBuffer;

    constexpr int16_t BTN_X = 8;
    constexpr int16_t BTN_W = 304;
    constexpr int16_t BTN_H = 28;
    constexpr int16_t BOX_PAD = 6;
    constexpr int16_t BOX_GAP = 4;
    constexpr int16_t TEXT_W = BTN_W - (BOX_PAD * 2) - 2;

    constexpr int16_t STATUS_Y = Display::DD_HEADER_H + 20;
    constexpr int16_t LIST_TOP = Display::DD_HEADER_H + 34;
    constexpr int16_t COMPOSE_BTN_Y = 206;
    constexpr int16_t LIST_BOTTOM = COMPOSE_BTN_Y - 4;

    constexpr int16_t COMPOSE_BOX_Y = Display::DD_HEADER_H + 26;
    constexpr int16_t COMPOSE_BOX_H = 90;
    constexpr int16_t SCROLL_STEP_PX = 40;

    int composeZone = -1;
    int cancelZone = -1;

    void showChatScreen();
    void drawChatScreen();

    void closeChat() {
        mode = Mode::Closed;
        Menu::back();
    }

    void appendMessage(const String& text, bool outgoing, float rssi) {
        if (messageCount >= MAX_MESSAGES) {
            // Storico pieno: scarta il piu' vecchio per fare posto al
            // nuovo (una chat non ha bisogno di conservare tutto, solo
            // la conversazione recente).
            for (int i = 1; i < MAX_MESSAGES; i++) {
                messages[i - 1] = messages[i];
            }
            messageCount = MAX_MESSAGES - 1;
        }

        messages[messageCount].text = text;
        messages[messageCount].outgoing = outgoing;
        messages[messageCount].rssi = rssi;
        messageCount++;
    }

    String buildMessageText(int index) {
        const ChatMessage& msg = messages[index];
        String text = (msg.outgoing ? "TU: " : "LORA: ") + msg.text;
        if (!msg.outgoing) {
            char suffix[24];
            snprintf(suffix, sizeof(suffix), "  (RSSI %.0f)", msg.rssi);
            text += suffix;
        }
        return text;
    }

    int16_t messageBoxHeight(int index) {
        return Display::wrappedTextHeight(buildMessageText(index), TEXT_W) + (BOX_PAD * 2);
    }

    // Indice piu' in alto raggiungibile scorrendo verso il basso (verso
    // gli ultimi messaggi): quello che lascia l'ultimo messaggio ancora
    // intero e visibile in fondo, riempiendo con quanti altri entrano
    // sopra - mai un fumetto tagliato a meta'.
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

    void ensureBottomVisible() {
        topVisibleMsgIndex = computeMaxTopIndex();
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

    void drawButton(int16_t y, const char* label, bool focused) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->drawRect(BTN_X, y, BTN_W, BTN_H, focused ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 12, y + BTN_H / 2 - 3);
        gfx->print(label);
    }

    // Risponde a "come faccio a sapere se sto trasmettendo?": mostra lo
    // stato in tempo reale, arancione mentre un invio e' effettivamente
    // in corso. Ridisegna solo il pezzo di riga interessato, e solo se il
    // testo e' davvero cambiato (stesso principio del fix sul percento
    // di NetScanUI: un ridisegno ad ogni ciclo anche a testo invariato
    // e' lo stesso errore gia' preso una volta).
    void drawStatusTag() {
        Arduino_GFX* gfx = Display::getGfx();
        String text;
        uint16_t color;

        if (LoRaRadio::isTransmitting()) {
            int pending = LoRaRadio::pendingCount();
            if (pending > 0) {
                char buf[40];
                snprintf(buf, sizeof(buf), "INVIO IN CORSO... (%d IN CODA)", pending);
                text = buf;
            } else {
                text = "INVIO IN CORSO...";
            }
            color = gfx->color565(0xE6, 0x7E, 0x22);
        } else {
            text = "IN ASCOLTO";
            color = Display::DD_TEXT_DIM;
        }

        if (text == lastStatusText) {
            return;
        }
        lastStatusText = text;

        gfx->fillRect(0, STATUS_Y, 320, 11, Display::DD_BG);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(color);
        gfx->setCursor(8, STATUS_Y);
        gfx->print(text);
    }

    void drawMessageBox(int index, int16_t y) {
        Arduino_GFX* gfx = Display::getGfx();
        String text = buildMessageText(index);
        int16_t boxH = messageBoxHeight(index);

        uint16_t borderColor = messages[index].outgoing
            ? Display::DD_ACCENT
            : gfx->color565(0x2E, 0xCC, 0x71);

        gfx->drawRect(BTN_X, y, BTN_W, boxH, borderColor);
        Display::drawWrappedText(BTN_X + BOX_PAD, y + BOX_PAD, TEXT_W, text, Display::DD_TEXT);
    }

    void drawChatScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("LORA CHAT");
        lastStatusText = "";
        drawStatusTag();

        TouchUI::clearZones();
        composeZone = -1;

        if (messageCount == 0) {
            Display::drawWrappedText(
                BTN_X, LIST_TOP + 4, TEXT_W,
                "Nessun messaggio ancora.\nMesh a 3 salti: raggiungi anche nodi fuori dal tuo raggio radio diretto.",
                Display::DD_TEXT_DIM
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

        drawButton(COMPOSE_BTN_Y, "[ SCRIVI ] NUOVO MESSAGGIO", true);
        composeZone = TouchUI::addZone(BTN_X, COMPOSE_BTN_Y, BTN_W, BTN_H);
    }

    void showChatScreen() {
        mode = Mode::Chat;
        Input::setTextEntryMode(false);
        hasUnread = false;
        ensureBottomVisible();
        drawChatScreen();
    }

    void sendComposedMessage() {
        String text = composeBuffer;
        text.trim();

        if (text.length() == 0) {
            return;
        }

        // LoRaRadio::send() e' non bloccante (avvia l'invio in
        // background, come Meshtastic): torna subito, la trasmissione
        // vera e propria (che con lo spreading factor attuale puo'
        // durare anche secondi) prosegue per conto suo mentre
        // l'interfaccia resta reattiva.
        appendMessage(text, true, 0.0f);

        bool accepted = LoRaRadio::send(text);
        if (!accepted) {
            appendMessage("(invio non accettato: radio occupata/coda piena)", true, 0.0f);
        }

        composeBuffer = "";
        showChatScreen();
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

        drawButton(200, "[ ANNULLA ]", true);
        cancelZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
    }

    void showComposeScreen() {
        mode = Mode::Compose;
        Input::setTextEntryMode(true);
        composeBuffer = "";
        drawComposeScreen();
    }

    void handleChatInput() {
        int16_t dragDelta = TouchUI::pollDragDeltaY();
        if (dragDelta != 0 && messageCount > 0) {
            dragAccumY += dragDelta;

            int maxTop = computeMaxTopIndex();
            bool scrolled = false;
            while (dragAccumY >= SCROLL_STEP_PX && topVisibleMsgIndex > 0) {
                topVisibleMsgIndex--;
                dragAccumY -= SCROLL_STEP_PX;
                scrolled = true;
            }
            while (dragAccumY <= -SCROLL_STEP_PX && topVisibleMsgIndex < maxTop) {
                topVisibleMsgIndex++;
                dragAccumY += SCROLL_STEP_PX;
                scrolled = true;
            }
            if (topVisibleMsgIndex == 0 && dragAccumY > SCROLL_STEP_PX) {
                dragAccumY = SCROLL_STEP_PX;
            }
            if (topVisibleMsgIndex == maxTop && dragAccumY < -SCROLL_STEP_PX) {
                dragAccumY = -SCROLL_STEP_PX;
            }

            followingTail = (topVisibleMsgIndex >= maxTop);
            if (scrolled) {
                drawChatScreen();
            }
        }

        int tapped = TouchUI::pollTap();
        if (tapped == composeZone) {
            showComposeScreen();
            return;
        }

        drawStatusTag();

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Back:
                closeChat();
                break;

            case InputEvent::Select:
                showComposeScreen();
                break;

            case InputEvent::Up: {
                if (topVisibleMsgIndex > 0) {
                    topVisibleMsgIndex--;
                    followingTail = (topVisibleMsgIndex >= computeMaxTopIndex());
                    drawChatScreen();
                }
                break;
            }

            case InputEvent::Down: {
                int maxTop = computeMaxTopIndex();
                if (topVisibleMsgIndex < maxTop) {
                    topVisibleMsgIndex++;
                    followingTail = (topVisibleMsgIndex >= maxTop);
                    drawChatScreen();
                }
                break;
            }

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

} // namespace


namespace LoRaChatUI {

    void open() {
        if (!LoRaRadio::isReady()) {
            Display::showMessage(
                "LoRa Chat",
                "Radio LoRa non disponibile (non inizializzata: controlla hardware/antenna, o firmware compilato con LORA_ENABLED=0).",
                []() { Menu::back(); }
            );
            return;
        }

        if (!LoRaRadio::isEnabled()) {
            Display::showMessage(
                "LoRa Chat",
                "LoRa disattivato per risparmiare batteria.\nRiattivalo dalle Impostazioni per usarlo.",
                []() { Menu::back(); }
            );
            return;
        }

        Display::setDirectDrawMode(true);
        showChatScreen();
    }

    bool isActive() {
        return mode != Mode::Closed;
    }

    // Drena i messaggi ricevuti nella cronologia SEMPRE, non solo quando
    // la chat e' aperta: LoRaRadio ha un solo "slot" per il messaggio in
    // arrivo, che verrebbe sovrascritto (messaggio perso) se ne arrivasse
    // un secondo prima che la chat, riaperta piu' tardi, lo consumi. Va
    // chiamata ad ogni ciclo da App.cpp, indipendentemente dalla
    // schermata attiva.
    void poll() {
        if (!LoRaRadio::available()) {
            return;
        }

        String incoming = LoRaRadio::read();
        appendMessage(incoming, false, LoRaRadio::lastRSSI());
        Sound::playLoRaMessageReceived();

        if (mode == Mode::Chat) {
            if (followingTail) {
                ensureBottomVisible();
            }
            drawChatScreen();
        } else {
            // Non si e' sulla chat in questo momento: resta "non letto"
            // (icona gialla in alto) finche' non la si riapre.
            hasUnread = true;
        }
    }

    bool hasUnreadMessages() {
        return hasUnread;
    }

    void update() {
        switch (mode) {
            case Mode::Closed:
                break;
            case Mode::Chat:
                handleChatInput();
                break;
            case Mode::Compose:
                handleComposeInput();
                break;
        }
    }
}
