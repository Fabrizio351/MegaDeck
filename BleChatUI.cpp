#include "BleChatUI.h"
#include "Display.h"
#include "Input.h"
#include "Menu.h"
#include "BleChat.h"
#include "Sound.h"

#include <Arduino.h>
#include <lvgl.h>

namespace {

    constexpr int MAX_MESSAGES = 30;

    struct ChatMessage {
        String text;
        bool outgoing;
        int rssi;
    };

    ChatMessage messages[MAX_MESSAGES];
    int messageCount = 0;

    enum class Mode {
        Closed,
        Chat,
        Compose
    };
    Mode mode = Mode::Closed;

    lv_obj_t* chatScreen = nullptr;
    lv_obj_t* listContent = nullptr;

    lv_obj_t* composeScreen = nullptr;
    lv_obj_t* composeTextarea = nullptr;
    String composeBuffer;

    void showChatScreen();

    void closeChat() {
        mode = Mode::Closed;
        Menu::back();
    }

    void appendMessage(const String& text, bool outgoing, int rssi) {
        if (messageCount >= MAX_MESSAGES) {
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

    void rebuildMessageList() {
        if (listContent == nullptr) {
            return;
        }

        lv_obj_clean(listContent);

        if (messageCount == 0) {
            lv_obj_t* empty = lv_label_create(listContent);
            lv_label_set_text(
                empty,
                "Nessun messaggio ancora.\n"
                "Raggio BLE: decine di metri. Messaggi brevi (max "
                "20 caratteri, limite dell'advertising BLE)."
            );
            lv_obj_set_style_text_color(empty, Display::COLOR_TEXT, 0);
            lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
            lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(empty, LV_PCT(100));
            return;
        }

        for (int i = 0; i < messageCount; i++) {
            const ChatMessage& msg = messages[i];

            lv_obj_t* row = lv_obj_create(listContent);
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_all(row, 6, LV_PART_MAIN);
            lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
            lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
            // Blu (COLOR_ACCENT) per i tuoi, verde per i ricevuti -
            // stessa convenzione della chat LoRa, cosi' resta coerente
            // tra i due canali.
            lv_obj_set_style_border_color(
                row,
                msg.outgoing ? Display::COLOR_ACCENT : lv_color_hex(0x2ECC71),
                LV_PART_MAIN
            );
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            String text = (msg.outgoing ? "Tu: " : "BLE: ") + msg.text;
            if (!msg.outgoing) {
                char rssiSuffix[24];
                snprintf(rssiSuffix, sizeof(rssiSuffix), "  (RSSI %d)", msg.rssi);
                text += rssiSuffix;
            }

            lv_obj_t* label = lv_label_create(row);
            lv_label_set_text(label, text.c_str());
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(label, LV_PCT(100));
            lv_obj_set_style_text_color(label, Display::COLOR_TEXT, 0);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        }

        lv_obj_scroll_to_view(lv_obj_get_child(listContent, messageCount - 1), LV_ANIM_OFF);
    }

    void composeCb(lv_event_t* event);

    void buildChatScreen() {
        lv_obj_t* content = nullptr;
        chatScreen = Display::createScreen("BLE Chat", &content, closeChat);

        listContent = lv_obj_create(content);
        lv_obj_remove_style_all(listContent);
        lv_obj_set_size(listContent, LV_PCT(100), 134);
        lv_obj_align(listContent, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(listContent, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(listContent, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(listContent, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_layout(listContent, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(listContent, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(listContent, 4, LV_PART_MAIN);

        lv_obj_t* composeBtn = lv_btn_create(content);
        lv_obj_set_size(composeBtn, LV_PCT(100), 34);
        lv_obj_align(composeBtn, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(composeBtn, lv_color_hex(0x0A1A2A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(composeBtn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(composeBtn, Display::COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(composeBtn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(composeBtn, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(composeBtn, composeCb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* composeLabel = lv_label_create(composeBtn);
        lv_label_set_text(composeLabel, LV_SYMBOL_EDIT " Scrivi messaggio");
        lv_obj_center(composeLabel);
        lv_obj_set_style_text_color(composeLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(composeLabel, &lv_font_montserrat_14, 0);

        rebuildMessageList();
    }

    void showChatScreen() {
        mode = Mode::Chat;
        Input::setTextEntryMode(false);

        if (chatScreen != nullptr) {
            lv_obj_del(chatScreen);
            chatScreen = nullptr;
            listContent = nullptr;
        }

        buildChatScreen();
        lv_scr_load(chatScreen);
    }

    void sendComposedMessage() {
        String text = composeBuffer;
        text.trim();

        if (text.length() == 0) {
            return;
        }

        // BleChat::send() e' non bloccante (avvia l'advertising in
        // background e ritorna subito), come LoRaRadio::send().
        appendMessage(text, true, 0);

        bool accepted = BleChat::send(text);
        if (!accepted) {
            appendMessage("(invio non accettato: radio non pronta)", true, 0);
        }

        composeBuffer = "";
        showChatScreen();
    }

    void buildComposeScreen() {
        lv_obj_t* content = nullptr;
        composeScreen = Display::createScreen("Nuovo messaggio BT", &content, showChatScreen);

        composeBuffer = "";

        composeTextarea = lv_textarea_create(content);
        lv_textarea_set_one_line(composeTextarea, false);
        lv_textarea_set_text(composeTextarea, "");
        lv_obj_set_width(composeTextarea, LV_PCT(100));
        lv_obj_set_height(composeTextarea, 60);
        lv_obj_align(composeTextarea, LV_ALIGN_TOP_LEFT, 0, 0);

        char hintText[64];
        snprintf(
            hintText, sizeof(hintText),
            "Max %d caratteri (limite BLE). Invio per inviare.",
            BleChat::MAX_MESSAGE_LEN
        );

        lv_obj_t* hint = lv_label_create(content);
        lv_label_set_text(hint, hintText);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hint, Display::COLOR_TEXT, 0);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_align_to(hint, composeTextarea, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    }

    void showComposeScreen() {
        mode = Mode::Compose;
        Input::setTextEntryMode(true);

        if (composeScreen != nullptr) {
            lv_obj_del(composeScreen);
            composeScreen = nullptr;
        }

        buildComposeScreen();
        lv_scr_load(composeScreen);
    }

    void composeCb(lv_event_t* event) {
        (void)event;
        showComposeScreen();
    }

    void handleChatInput() {
        InputEvent event = Input::getEvent();

        if (event == InputEvent::Back) {
            closeChat();
        } else if (event == InputEvent::Select) {
            showComposeScreen();
        } else if (event == InputEvent::Up) {
            // Su/giu' scorrono la cronologia messaggi, cosi' funziona
            // anche col trackball e non solo col touch.
            if (listContent != nullptr) {
                lv_obj_scroll_by(listContent, 0, 40, LV_ANIM_ON);
            }
        } else if (event == InputEvent::Down) {
            if (listContent != nullptr) {
                lv_obj_scroll_by(listContent, 0, -40, LV_ANIM_ON);
            }
        }
    }

    void handleComposeInput() {
        InputEvent event = Input::getEvent();
        if (event == InputEvent::None) {
            return;
        }

        if (event == InputEvent::Select) {
            sendComposedMessage();
            return;
        }

        if (event == InputEvent::Key) {
            char key = Input::getKey();

            if (key == 0x08 || key == 0x7F) {
                if (composeBuffer.length() > 0) {
                    composeBuffer.remove(composeBuffer.length() - 1);
                }
            } else if (key >= 32 && key < 127 && composeBuffer.length() < BleChat::MAX_MESSAGE_LEN) {
                composeBuffer += key;
            }

            lv_textarea_set_text(composeTextarea, composeBuffer.c_str());
        }
    }

} // namespace


namespace BleChatUI {

    void open() {
        // isEnabled() PRIMA di isReady(): da quando lo stack BLE si
        // avvia solo se abilitato (per non tenere impegnata la sua
        // memoria quando non serve, vedi BleChat.cpp), "non pronto" e'
        // lo stato NORMALE mentre e' disattivato - va distinto dal caso
        // (diverso) di un guasto hardware reale.
        if (!BleChat::isEnabled()) {
            Display::showMessage(
                "BLE Chat",
                "Bluetooth disattivato per risparmiare batteria.\nRiattivalo dalle Impostazioni per usarlo.",
                []() { Menu::back(); }
            );
            return;
        }

        if (!BleChat::isReady()) {
            Display::showMessage(
                "BLE Chat",
                "Bluetooth non disponibile su questo firmware/hardware.",
                []() { Menu::back(); }
            );
            return;
        }

        showChatScreen();
    }

    bool isActive() {
        return mode != Mode::Closed;
    }

    void poll() {
        // Drena i messaggi ricevuti nella cronologia SEMPRE, non solo
        // quando la chat e' aperta (stesso motivo di LoRaChatUI::poll()):
        // BleChat ha un solo "slot" per il messaggio in arrivo, che
        // andrebbe perso se ne arrivasse un secondo prima che la chat
        // venga riaperta e lo consumi.
        if (!BleChat::available()) {
            return;
        }

        String incoming = BleChat::read();
        appendMessage(incoming, false, BleChat::lastRSSI());
        Sound::playBleMessageReceived();

        if (mode == Mode::Chat) {
            rebuildMessageList();
        }
    }

    void update() {
        if (mode == Mode::Closed) {
            return;
        }

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
