#include "WifiUI.h"
#include "WifiManager.h"
#include "Display.h"
#include "Input.h"
#include "Menu.h"
#include "ConnStatus.h"

#include <Arduino.h>
#include <lvgl.h>

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

    lv_obj_t* listScreen = nullptr;
    lv_obj_t* networkButtons[MAX_NETWORKS_SHOWN] = { nullptr };
    int networkCount = 0;
    int selectedIndex = 0;

    // Il trackball deve poter raggiungere anche "Scansiona di nuovo" e
    // "Disconnetti", non solo le voci di rete - altrimenti restano
    // utilizzabili solo col touch. listFocus dice su quale dei tre
    // "gruppi" naviga attualmente su/giu'; rescanBtnRef/disconnectBtnRef
    // sono i riferimenti usati da updateListHighlight() per evidenziarli.
    enum class ListFocus {
        Rescan,
        Disconnect,
        Network
    };

    ListFocus listFocus = ListFocus::Network;
    lv_obj_t* rescanBtnRef = nullptr;
    lv_obj_t* disconnectBtnRef = nullptr;

    lv_obj_t* passwordScreen = nullptr;
    lv_obj_t* passwordTextarea = nullptr;
    lv_obj_t* passwordEyeLabel = nullptr;
    String passwordBuffer;
    bool passwordVisible = false;

    lv_obj_t* statusScreen = nullptr;
    lv_obj_t* statusLabel = nullptr;

    String selectedSsid;
    bool selectedOpen = false;

    // Password usata per il tentativo di connessione in corso: salvata in
    // WifiManager solo se la connessione va a buon fine (vedi
    // handleConnectingUpdate). Per una rete aperta resta vuota.
    String pendingPassword;
    bool credentialsSavedForAttempt = false;

    // Ultimo stato mostrato in handleConnectingUpdate(): senza questa
    // cache, lv_label_set_text() veniva richiamato (rialloca nel pool
    // LVGL) ad ogni ciclo di App::update() mentre si resta in attesa
    // della connessione, anche se il testo non cambia mai finche' lo
    // stato non transita - vedi la stessa correzione in NetGuardUI.cpp.
    WifiManager::ConnectionState lastShownConnectingState =
        WifiManager::ConnectionState::Connected; // valore "neutro": forza il primo aggiornamento

    void showListScreen();
    void showPasswordScreen();
    void showConnectingScreen();
    void rescanButtonCb(lv_event_t* event);
    void disconnectButtonCb(lv_event_t* event);
    void forgetButtonCb(lv_event_t* event);

    void closeAndReturnToMenu() {
        mode = Mode::Closed;
        Input::setTextEntryMode(false);
        Menu::showMain();
    }

    // Bordo spesso e acceso sul bottone attualmente a fuoco, normale su
    // tutti gli altri: stesso trucco usato in SettingsUI per le due righe
    // di luminosita'. Qui serve a rendere visibile il focus anche su
    // rescan/disconnetti, che non hanno un colore di sfondo dedicato.
    void setFocusBorder(lv_obj_t* obj, bool focused) {
        if (obj == nullptr) {
            return;
        }
        lv_obj_set_style_border_width(obj, focused ? 3 : 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(
            obj,
            focused ? lv_color_hex(0xFFFFFF) : Display::COLOR_ACCENT,
            LV_PART_MAIN
        );
    }

    void updateListHighlight() {
        setFocusBorder(rescanBtnRef, listFocus == ListFocus::Rescan);
        setFocusBorder(disconnectBtnRef, listFocus == ListFocus::Disconnect);

        for (int i = 0; i < networkCount; i++) {
            if (networkButtons[i] == nullptr) {
                continue;
            }

            bool selected = (listFocus == ListFocus::Network && i == selectedIndex);
            lv_obj_set_style_bg_color(
                networkButtons[i],
                selected ? Display::COLOR_ACCENT : lv_color_hex(0x0A1A2A),
                LV_PART_MAIN
            );
        }

        lv_obj_t* toScrollTo = nullptr;
        if (listFocus == ListFocus::Rescan) {
            toScrollTo = rescanBtnRef;
        } else if (listFocus == ListFocus::Disconnect) {
            toScrollTo = disconnectBtnRef;
        } else if (networkCount > 0) {
            toScrollTo = networkButtons[selectedIndex];
        }

        if (toScrollTo != nullptr) {
            lv_obj_scroll_to_view(toScrollTo, LV_ANIM_OFF);
        }
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
            // Rete gia' usata in passato: ci si riconnette subito con la
            // password salvata, senza richiederla di nuovo.
            pendingPassword = WifiManager::getSavedPassword(selectedSsid);
            WifiManager::connect(selectedSsid, pendingPassword);
            showConnectingScreen();
        } else {
            showPasswordScreen();
        }
    }

    // Chiamata SOLO subito dopo che una scansione e' appena terminata:
    // se fra i risultati c'e' almeno una rete gia' nota (password
    // salvata) e non si e' gia' connessi a qualcosa, ci si connette
    // automaticamente a quella con segnale piu' forte, senza bisogno di
    // selezionarla a mano. Le altre volte che si torna alla lista
    // (annulla password, disconnetti, tentativo fallito...) mostrano
    // solo l'elenco, senza forzare una nuova connessione a sorpresa.
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
                // RSSI in dBm (negativo): piu' vicino a 0 = segnale
                // migliore, quindi "maggiore" e' davvero "piu' forte".
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

    void networkButtonClickedCb(lv_event_t* event) {
        lv_obj_t* button = static_cast<lv_obj_t*>(lv_event_get_target(event));
        if (button == nullptr) {
            return;
        }

        int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
        if (index < 0 || index >= networkCount) {
            return;
        }

        selectedIndex = index;
        listFocus = ListFocus::Network;
        updateListHighlight();

        // Come nel menu principale: mostra l'evidenziazione un istante
        // prima di aprire la voce, anche quando arrivata da un tap.
        Display::update();
        delay(150);

        attemptConnectToSelected();
    }

    void forgetButtonCb(lv_event_t* event) {
        lv_obj_t* button = static_cast<lv_obj_t*>(lv_event_get_target(event));
        if (button == nullptr) {
            return;
        }

        int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
        if (index < 0 || index >= networkCount) {
            return;
        }

        WifiManager::Network net = WifiManager::getNetwork(index);
        WifiManager::forgetPassword(net.ssid);

        // Ricostruisce la lista: il cestino scompare per questa rete
        // (non ha piu' una password salvata) e la prossima selezione
        // richiedera' di nuovo la password.
        showListScreen();
    }

    void buildListScreen() {
        lv_obj_t* content = nullptr;
        listScreen = Display::createScreen("WiFi", &content, closeAndReturnToMenu);

        // Azzerati ad ogni ricostruzione: se la schermata precedente
        // aveva un bottone disconnetti (WiFi connesso) e questa non
        // (es. ci siamo appena disconnessi), il riferimento vecchio non
        // deve restare in giro puntando a un oggetto ormai cancellato.
        rescanBtnRef = nullptr;
        disconnectBtnRef = nullptr;

        lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(content, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

        lv_obj_set_layout(content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(content, 4, LV_PART_MAIN);

        // Riga di stato: rete attuale + IP locale se connessi, con un
        // bottone per disconnettersi (risparmia batteria quando il WiFi
        // non serve). ConnStatus::isWifiConnected() legge WiFi.status()
        // in tempo reale, quindi resta sempre allineata alla realta'.
        if (ConnStatus::isWifiConnected()) {
            lv_obj_t* statusBox = lv_obj_create(content);
            lv_obj_remove_style_all(statusBox);
            lv_obj_set_width(statusBox, LV_PCT(100));
            lv_obj_set_height(statusBox, LV_SIZE_CONTENT);
            lv_obj_set_style_border_width(statusBox, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(statusBox, Display::COLOR_ACCENT, LV_PART_MAIN);
            lv_obj_set_style_radius(statusBox, 4, LV_PART_MAIN);
            lv_obj_set_style_pad_all(statusBox, 6, LV_PART_MAIN);
            lv_obj_clear_flag(statusBox, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* statusText = lv_label_create(statusBox);
            String info = "Connesso: " + WifiManager::getConnectedSsid()
                + "\nIP: " + WifiManager::getLocalIP();
            lv_label_set_text(statusText, info.c_str());
            lv_obj_set_style_text_color(statusText, Display::COLOR_TEXT, 0);
            lv_obj_set_style_text_font(statusText, &lv_font_montserrat_14, 0);
            lv_obj_align(statusText, LV_ALIGN_TOP_LEFT, 0, 0);

            lv_obj_t* disconnectBtn = lv_btn_create(statusBox);
            lv_obj_set_size(disconnectBtn, 90, 30);
            lv_obj_align(disconnectBtn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
            lv_obj_set_style_bg_color(disconnectBtn, lv_color_hex(0x5A1A1A), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(disconnectBtn, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_color(disconnectBtn, Display::COLOR_ACCENT, LV_PART_MAIN);
            lv_obj_set_style_border_width(disconnectBtn, 1, LV_PART_MAIN);
            lv_obj_set_style_radius(disconnectBtn, 4, LV_PART_MAIN);
            lv_obj_add_event_cb(disconnectBtn, disconnectButtonCb, LV_EVENT_CLICKED, nullptr);

            lv_obj_t* disconnectLabel = lv_label_create(disconnectBtn);
            lv_label_set_text(disconnectLabel, "Disconn.");
            lv_obj_center(disconnectLabel);
            lv_obj_set_style_text_color(disconnectLabel, Display::COLOR_TEXT, 0);
            lv_obj_set_style_text_font(disconnectLabel, &lv_font_montserrat_14, 0);

            disconnectBtnRef = disconnectBtn;
        }

        // Bottone scansiona di nuovo: sempre presente, per aggiornare
        // l'elenco senza dover uscire e rientrare nella schermata WiFi.
        lv_obj_t* rescanBtn = lv_btn_create(content);
        lv_obj_set_width(rescanBtn, LV_PCT(100));
        lv_obj_set_height(rescanBtn, 34);
        lv_obj_set_style_bg_color(rescanBtn, lv_color_hex(0x0A1A2A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(rescanBtn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(rescanBtn, Display::COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(rescanBtn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(rescanBtn, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(rescanBtn, rescanButtonCb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* rescanLabel = lv_label_create(rescanBtn);
        lv_label_set_text(rescanLabel, LV_SYMBOL_REFRESH " Scansiona di nuovo");
        lv_obj_center(rescanLabel);
        lv_obj_set_style_text_color(rescanLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(rescanLabel, &lv_font_montserrat_14, 0);

        rescanBtnRef = rescanBtn;

        networkCount = WifiManager::getNetworkCount();
        if (networkCount > MAX_NETWORKS_SHOWN) {
            networkCount = MAX_NETWORKS_SHOWN;
        }

        if (networkCount == 0) {
            lv_obj_t* label = lv_label_create(content);
            lv_label_set_text(label, "Nessuna rete trovata.\nTorna indietro per riprovare.");
            lv_obj_set_style_text_color(label, Display::COLOR_TEXT, 0);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        }

        for (int i = 0; i < networkCount; i++) {
            WifiManager::Network net = WifiManager::getNetwork(i);

            // Verde per le reti gia' note (password salvata, oltre al
            // cestino per dimenticarla), azzurro per le altre: un modo
            // per riconoscerle a colpo d'occhio senza leggere le icone.
            bool known = !net.open && WifiManager::hasSavedPassword(net.ssid);

            lv_obj_t* button = lv_btn_create(content);
            lv_obj_set_width(button, LV_PCT(100));
            lv_obj_set_height(button, 36);
            lv_obj_set_style_bg_color(button, lv_color_hex(0x0A1A2A), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_color(
                button,
                known ? lv_color_hex(0x2ECC71) : Display::COLOR_ACCENT,
                LV_PART_MAIN
            );
            lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
            lv_obj_set_style_radius(button, 4, LV_PART_MAIN);

            lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            lv_obj_add_event_cb(button, networkButtonClickedCb, LV_EVENT_CLICKED, nullptr);

            lv_obj_set_layout(button, LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(
                button,
                LV_FLEX_ALIGN_SPACE_BETWEEN,
                LV_FLEX_ALIGN_CENTER,
                LV_FLEX_ALIGN_CENTER
            );
            lv_obj_set_style_pad_left(button, 10, 0);
            lv_obj_set_style_pad_right(button, 10, 0);

            lv_obj_t* nameLabel = lv_label_create(button);
            String displayName = (net.ssid.length() > 0) ? net.ssid : "(nascosta)";
            lv_label_set_text(nameLabel, displayName.c_str());
            lv_obj_set_style_text_color(nameLabel, Display::COLOR_TEXT, 0);
            lv_obj_set_style_text_font(nameLabel, &lv_font_montserrat_14, 0);

            // Icona wifi/lucchetto +, se c'e' una password salvata per
            // questa rete, un piccolo cestino per "dimenticarla" (utile
            // se il router e' stato riconfigurato con una password
            // diversa: senza questo non ci si potrebbe piu' riconnettere,
            // perche' verrebbe sempre riprovata quella vecchia salvata).
            lv_obj_t* rightGroup = lv_obj_create(button);
            lv_obj_remove_style_all(rightGroup);
            lv_obj_set_size(rightGroup, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_layout(rightGroup, LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(rightGroup, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(
                rightGroup,
                LV_FLEX_ALIGN_END,
                LV_FLEX_ALIGN_CENTER,
                LV_FLEX_ALIGN_CENTER
            );
            lv_obj_set_style_pad_column(rightGroup, 8, 0);
            lv_obj_clear_flag(rightGroup, LV_OBJ_FLAG_SCROLLABLE);

            if (known) {
                lv_obj_t* forgetBtn = lv_btn_create(rightGroup);
                lv_obj_set_size(forgetBtn, 26, 26);
                lv_obj_set_style_bg_color(forgetBtn, lv_color_hex(0x5A1A1A), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(forgetBtn, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_border_width(forgetBtn, 0, LV_PART_MAIN);
                lv_obj_set_style_radius(forgetBtn, 4, LV_PART_MAIN);
                lv_obj_set_style_pad_all(forgetBtn, 0, LV_PART_MAIN);
                // Indice (non lo ssid: una String non e' sicura da tenere
                // come puntatore grezzo nello user_data) - lo stesso
                // schema usato per il bottone rete stesso.
                lv_obj_set_user_data(forgetBtn, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
                lv_obj_add_event_cb(forgetBtn, forgetButtonCb, LV_EVENT_CLICKED, nullptr);

                lv_obj_t* forgetIcon = lv_label_create(forgetBtn);
                lv_label_set_text(forgetIcon, LV_SYMBOL_TRASH);
                lv_obj_set_style_text_color(forgetIcon, Display::COLOR_TEXT, 0);
                lv_obj_set_style_text_font(forgetIcon, &lv_font_montserrat_14, 0);
                lv_obj_center(forgetIcon);
            }

            lv_obj_t* iconLabel = lv_label_create(rightGroup);
            lv_label_set_text(iconLabel, net.open ? LV_SYMBOL_WIFI : LV_SYMBOL_WIFI " " LV_SYMBOL_EYE_CLOSE);
            lv_obj_set_style_text_color(iconLabel, Display::COLOR_ACCENT, 0);
            lv_obj_set_style_text_font(iconLabel, &lv_font_montserrat_14, 0);

            networkButtons[i] = button;
        }

        selectedIndex = 0;
        // Se non ci sono reti, non ha senso partire col focus su "Network"
        // (non c'e' nulla da selezionare li'): si parte da Rescan, cosi'
        // Select riprova subito la scansione senza dover premere Su prima.
        listFocus = (networkCount > 0) ? ListFocus::Network : ListFocus::Rescan;
        updateListHighlight();
    }

    void showListScreen() {
        mode = Mode::List;
        Input::setTextEntryMode(false);

        if (listScreen != nullptr) {
            lv_obj_del(listScreen);
            listScreen = nullptr;
        }
        for (int i = 0; i < MAX_NETWORKS_SHOWN; i++) {
            networkButtons[i] = nullptr;
        }

        buildListScreen();
        lv_scr_load(listScreen);
    }

    void buildStatusScreen(const char* title, void (*onBack)()) {
        if (statusScreen != nullptr) {
            lv_obj_del(statusScreen);
            statusScreen = nullptr;
        }

        lv_obj_t* content = nullptr;
        statusScreen = Display::createScreen(title, &content, onBack);

        statusLabel = lv_label_create(content);
        lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(statusLabel, Display::COLOR_TEXT, 0);
        lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(statusLabel, LV_PCT(100));
        lv_obj_align(statusLabel, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    void showScanningScreen() {
        mode = Mode::Scanning;
        Input::setTextEntryMode(false);
        buildStatusScreen("WiFi", closeAndReturnToMenu);
        lv_label_set_text(statusLabel, "Scansione delle reti in corso...");
        lv_scr_load(statusScreen);
    }

    // Riusata sia dall'apertura iniziale di WifiUI sia dal bottone
    // "Scansiona di nuovo" nella lista.
    void rescan() {
        WifiManager::startScan();
        showScanningScreen();
    }

    void rescanButtonCb(lv_event_t* event) {
        (void)event;
        rescan();
    }

    void disconnectButtonCb(lv_event_t* event) {
        (void)event;
        WifiManager::disconnect();
        showListScreen();
    }

    void showConnectingScreen() {
        mode = Mode::Connecting;
        credentialsSavedForAttempt = false;
        Input::setTextEntryMode(false);
        buildStatusScreen("WiFi", showListScreen);
        String msg = "Connessione a " + selectedSsid + " in corso...";
        lv_label_set_text(statusLabel, msg.c_str());
        lv_scr_load(statusScreen);
    }

    void toggleEyeCb(lv_event_t* event) {
        (void)event;
        passwordVisible = !passwordVisible;
        lv_textarea_set_password_mode(passwordTextarea, !passwordVisible);

        if (passwordEyeLabel != nullptr) {
            lv_label_set_text(passwordEyeLabel, passwordVisible ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
        }
    }

    void buildPasswordScreen() {
        if (passwordScreen != nullptr) {
            lv_obj_del(passwordScreen);
            passwordScreen = nullptr;
        }

        lv_obj_t* content = nullptr;
        String title = "Password: " + selectedSsid;
        passwordScreen = Display::createScreen(title.c_str(), &content, showListScreen);

        passwordBuffer = "";
        passwordVisible = false;

        passwordTextarea = lv_textarea_create(content);
        lv_textarea_set_password_mode(passwordTextarea, true);
        lv_textarea_set_one_line(passwordTextarea, true);
        lv_textarea_set_text(passwordTextarea, "");
        lv_obj_set_width(passwordTextarea, LV_PCT(100) - 40);
        lv_obj_align(passwordTextarea, LV_ALIGN_TOP_LEFT, 0, 0);

        // Tasto "occhio": mostra/nasconde la password digitata. Utile
        // perche' senza schermo non c'e' altro modo per controllare di
        // aver scritto la password giusta prima di connettersi.
        lv_obj_t* eyeBtn = lv_btn_create(content);
        lv_obj_set_size(eyeBtn, 36, 36);
        lv_obj_align_to(eyeBtn, passwordTextarea, LV_ALIGN_OUT_RIGHT_MID, 4, 0);
        lv_obj_set_style_bg_color(eyeBtn, lv_color_hex(0x0A1A2A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(eyeBtn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(eyeBtn, Display::COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(eyeBtn, 1, LV_PART_MAIN);
        lv_obj_add_event_cb(eyeBtn, toggleEyeCb, LV_EVENT_CLICKED, nullptr);

        passwordEyeLabel = lv_label_create(eyeBtn);
        lv_label_set_text(passwordEyeLabel, LV_SYMBOL_EYE_CLOSE);
        lv_obj_set_style_text_color(passwordEyeLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(passwordEyeLabel, &lv_font_montserrat_14, 0);
        lv_obj_center(passwordEyeLabel);

        lv_obj_t* hint = lv_label_create(content);
        lv_label_set_text(hint, "Scrivi con la tastiera fisica, poi Invio per connettere.");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hint, Display::COLOR_TEXT, 0);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_align_to(hint, passwordTextarea, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);

        // Il bottone "Indietro" standard (passato a createScreen sopra)
        // e' l'unico modo garantito per annullare qui: in modo
        // "inserimento testo" la tastiera fisica non genera piu' l'evento
        // Back (altrimenti una password con la lettera 'b' non si
        // potrebbe scrivere), quindi serve per forza un bottone toccabile.
    }

    void showPasswordScreen() {
        mode = Mode::Password;
        Input::setTextEntryMode(true);
        buildPasswordScreen();
        lv_scr_load(passwordScreen);
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

    // Ordine di navigazione su/giu' col trackball, dall'alto verso il
    // basso (stesso ordine in cui i widget compaiono nello schermo):
    // Disconnetti (se presente) -> Scansiona -> voci di rete.
    void handleListInput() {
        InputEvent event = Input::getEvent();
        if (event == InputEvent::None) {
            return;
        }

        switch (event) {
            case InputEvent::Up:
                if (listFocus == ListFocus::Network) {
                    if (selectedIndex > 0) {
                        selectedIndex--;
                    } else {
                        listFocus = ListFocus::Rescan;
                    }
                } else if (listFocus == ListFocus::Rescan) {
                    if (disconnectBtnRef != nullptr) {
                        listFocus = ListFocus::Disconnect;
                    }
                    // Se non c'e' il bottone disconnetti, Rescan e' gia'
                    // il punto piu' in alto: nessun altro posto dove andare.
                }
                // Da Disconnect non si sale oltre: e' il punto piu' in alto.
                updateListHighlight();
                break;

            case InputEvent::Down:
                if (listFocus == ListFocus::Disconnect) {
                    listFocus = ListFocus::Rescan;
                } else if (listFocus == ListFocus::Rescan) {
                    if (networkCount > 0) {
                        listFocus = ListFocus::Network;
                        selectedIndex = 0;
                    }
                    // Senza reti trovate, Rescan resta il punto piu' in
                    // basso: nessun altro posto dove andare.
                } else if (listFocus == ListFocus::Network) {
                    if (selectedIndex < networkCount - 1) {
                        selectedIndex++;
                    }
                }
                updateListHighlight();
                break;

            case InputEvent::Select:
                if (listFocus == ListFocus::Rescan) {
                    rescan();
                } else if (listFocus == ListFocus::Disconnect) {
                    WifiManager::disconnect();
                    showListScreen();
                } else if (networkCount > 0) {
                    Display::update();
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

    void handlePasswordInput() {
        InputEvent event = Input::getEvent();
        if (event == InputEvent::None) {
            return;
        }

        if (event == InputEvent::Select) {
            // Invio sulla tastiera fisica: prova a connettersi con la
            // password digitata finora.
            pendingPassword = passwordBuffer;
            WifiManager::connect(selectedSsid, pendingPassword);
            showConnectingScreen();
            return;
        }

        if (event == InputEvent::Key) {
            char key = Input::getKey();

            if (key == 0x08 || key == 0x7F) {
                // Backspace (i codici comuni per la cancellazione sulla
                // tastiera fisica del T-Deck).
                if (passwordBuffer.length() > 0) {
                    passwordBuffer.remove(passwordBuffer.length() - 1);
                }
            } else if (key >= 32 && key < 127) {
                // Solo caratteri ASCII stampabili: una password WiFi non
                // ha senso con caratteri di controllo.
                passwordBuffer += key;
            }

            lv_textarea_set_text(passwordTextarea, passwordBuffer.c_str());
        }
    }

    void handleConnectingUpdate() {
        WifiManager::ConnectionState state = WifiManager::getConnectionState();

        if (state == WifiManager::ConnectionState::Connected) {
            // Salva la password solo la prima volta che si osserva lo
            // stato Connected per questo tentativo (lo stato resterebbe
            // Connected anche ai cicli successivi, se non uscissimo
            // subito da qui). Per le reti aperte pendingPassword e'
            // vuota: salvarla comunque non fa danni e permette a
            // hasSavedPassword() di riconoscerla come "gia' vista".
            if (!credentialsSavedForAttempt) {
                WifiManager::saveCredentials(selectedSsid, pendingPassword);
                credentialsSavedForAttempt = true;
            }

            // Torna subito alla lista appena connesso, senza dover
            // premere nulla: la lista mostra gia' SSID+IP in cima
            // (vedi buildListScreen), quindi l'esito resta visibile
            // comunque, senza un passaggio in piu' da confermare.
            showListScreen();
            return;
        }

        if (state != lastShownConnectingState) {
            lastShownConnectingState = state;
            const char* text = "Connessione in corso...";
            if (state == WifiManager::ConnectionState::Failed) {
                text = "Connessione fallita. Premi Indietro per riprovare.";
            }
            lv_label_set_text(statusLabel, text);
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
