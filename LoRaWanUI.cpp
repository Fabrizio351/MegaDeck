#include "LoRaWanUI.h"
#include "LoRaRadio.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "TextInputUI.h"
#include "LoRaToolsMenu.h"
#include "Config.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <Preferences.h>

// Vedi LoRaWanUI.h per il contesto generale. Regione fissa EU868 (29/09)
// - stessa assunzione euro-centrica gia' fatta per RADIO_FREQ in
// Config.h ("868 EU / 915 US / 433 a seconda della versione del modulo
// che hai comprato"): da rendere configurabile se un giorno servisse
// una regione diversa, non ora.
//
// Nota su cosa manca ancora (scelta consapevole, non un dimenticato):
// niente persistenza della sessione/nonce su NVS - ogni apertura fa un
// nuovo join OTAA da zero. RadioLib gestisce da solo i nonce (nessun
// riuso), quindi funziona, ma alcuni network server (es. TTN) possono
// rifiutare join ripetuti troppo in fretta finche' non si abilita
// "Resets DevNonces" sul dashboard - se capita, e' quello, non un bug
// qui.
namespace {

    enum class Mode {
        Closed,
        Main,
        Config,
        EditField
    };
    Mode mode = Mode::Closed;

    enum class ConfigField {
        JoinEUI,
        DevEUI,
        AppKey
    };
    constexpr int CONFIG_FIELD_COUNT = 3;
    ConfigField editingField = ConfigField::JoinEUI;
    String editBuffer;
    int selectedConfigRow = 0;

    Preferences prefs;
    constexpr const char* PREFS_NAMESPACE = "tdecklorawan";
    constexpr const char* KEY_JOIN_EUI = "joinEui";
    constexpr const char* KEY_DEV_EUI = "devEui";
    constexpr const char* KEY_APP_KEY = "appKey";
    bool nvsOpened = false;

    // Tutte e tre come stringhe hex (piu' facili da mostrare/validare
    // che uint64_t/array di byte grezzi) - convertite solo al momento
    // del join. Default vuoti: senza credenziali vere il join fallisce
    // subito con un messaggio chiaro, non con un errore radio confuso.
    String joinEuiHex;
    String devEuiHex;
    String appKeyHex;

    void ensureNvsOpen() {
        if (nvsOpened) {
            return;
        }
        prefs.begin(PREFS_NAMESPACE, false);
        joinEuiHex = prefs.getString(KEY_JOIN_EUI, "");
        devEuiHex = prefs.getString(KEY_DEV_EUI, "");
        appKeyHex = prefs.getString(KEY_APP_KEY, "");
        nvsOpened = true;
    }

    bool isConfigured() {
        ensureNvsOpen();
        return joinEuiHex.length() == 16 && devEuiHex.length() == 16 && appKeyHex.length() == 32;
    }

    // Decodifica una stringa hex (deve avere ESATTAMENTE la lunghezza
    // 'expectedLen', niente spazi) in byte grezzi. Ritorna false se non
    // e' hex valida o la lunghezza non corrisponde.
    bool hexToBytes(const String& text, uint8_t* out, int expectedLen) {
        if (text.length() != static_cast<unsigned int>(expectedLen * 2)) {
            return false;
        }
        for (int i = 0; i < expectedLen; i++) {
            char hi = text.charAt(i * 2);
            char lo = text.charAt(i * 2 + 1);
            if (!isxdigit(hi) || !isxdigit(lo)) {
                return false;
            }
            String pair = String(hi) + String(lo);
            out[i] = static_cast<uint8_t>(strtoul(pair.c_str(), nullptr, 16));
        }
        return true;
    }

    uint64_t bytesToUint64(const uint8_t* b) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) {
            v = (v << 8) | b[i];
        }
        return v;
    }

    LoRaWANNode* node = nullptr;
    bool radioBorrowed = false;

    enum class JoinState {
        NotConfigured,
        Idle,
        Joining,
        Joined,
        JoinFailed
    };
    JoinState joinState = JoinState::Idle;
    String statusLine;
    String lastUplinkResult;

    void releaseRadio() {
        if (node != nullptr) {
            delete node;
            node = nullptr;
        }
        if (radioBorrowed) {
            LoRaRadio::resumeAfterExternalUse();
            radioBorrowed = false;
        }
        joinState = JoinState::Idle;
    }

    void closeLoRaWan() {
        releaseRadio();
        mode = Mode::Closed;
        // Torna al sottomenu LORA TOOLS (non al menu principale): questa
        // schermata e' una voce DI QUEL sottomenu, stesso trattamento di
        // LoRaChatUI/LoRaScanUI.
        LoRaToolsMenu::showMain();
    }

    void showMainScreen();
    void showConfigScreen();

    // --- Schermata principale ---

    constexpr int16_t BTN_X = 8;
    constexpr int16_t BTN_W = 304;
    constexpr int16_t BTN_H = 30;
    constexpr int16_t STATUS_Y = Display::DD_HEADER_H + 26;
    constexpr int16_t CONFIG_BTN_Y = 150;
    constexpr int16_t JOIN_BTN_Y = 186;
    constexpr int16_t TEST_BTN_Y = 186; // stessa riga di JOIN: solo uno dei due e' visibile alla volta

    int configZone = -1;
    int joinZone = -1;
    int testZone = -1;

    // Fuoco trackball (02/10, bug segnalato: "molte vanno solo col
    // touch" - CONFIG/JOIN/TEST qui erano raggiungibili solo a touch,
    // il parametro "focused" di drawButton() esisteva gia' ma non era
    // mai pilotato da Su/Giu') - 0 = CONFIG, 1 = JOIN/TEST.
    int mainFocus = 0;

    // Zona tap condivisa per l'hint "[B] INDIETRO" (02/10, segnalato:
    // "non torna indietro ne' con B ne' cliccando su indietro" - prima
    // era solo testo decorativo, non un pulsante vero; registrata qui
    // cosi' diventa un modo garantito di uscire anche se per qualche
    // motivo il tasto fisico non viene letto su questo schermo).
    int backHintZone = -1;

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
        backHintZone = TouchUI::addZone(224, Display::DD_HEADER_H, 96, Display::DD_HEADER_H);
    }

    void drawButton(int16_t y, const char* label, bool focused = true) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(BTN_X + 1, y + 1, BTN_W - 2, BTN_H - 2, Display::DD_BG);
        gfx->drawRect(BTN_X, y, BTN_W, BTN_H, focused ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(focused ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setCursor(BTN_X + 12, y + BTN_H / 2 - 3);
        gfx->print(label);
    }

    String buildStatusLine() {
        if (!isConfigured()) {
            return "Non configurato.\nTocca CONFIG per inserire Join EUI/Dev EUI/App Key\n(dal pannello del tuo network server, es. TTN).";
        }
        switch (joinState) {
            case JoinState::Idle:       return "Configurato. Tocca JOIN per collegarti alla rete.";
            case JoinState::Joining:    return "Join in corso...";
            case JoinState::Joined:     return "Connesso alla rete LoRaWAN.\n" + lastUplinkResult;
            case JoinState::JoinFailed: return "Join fallito.\n" + lastUplinkResult;
            default:                   return "";
        }
    }

    void drawStatus() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, STATUS_Y, 320, CONFIG_BTN_Y - 4 - STATUS_Y, Display::DD_BG);
        Display::drawWrappedText(BTN_X, STATUS_Y, BTN_W, buildStatusLine(), Display::DD_TEXT);
    }

    void drawMainScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        // clearZones() PRIMA di drawHeaderTitle() (02/10) - altrimenti
        // cancellerebbe subito la zona dell'hint "[B] INDIETRO" appena
        // registrata li' dentro.
        TouchUI::clearZones();

        drawHeaderTitle("LORAWAN");
        drawStatus();

        drawButton(CONFIG_BTN_Y, "[ CONFIG ] JOIN EUI / DEV EUI / APP KEY", mainFocus == 0);
        configZone = TouchUI::addZone(BTN_X, CONFIG_BTN_Y, BTN_W, BTN_H);

        if (joinState == JoinState::Joined) {
            drawButton(TEST_BTN_Y, "[ TEST ] MANDA UPLINK DI PROVA", mainFocus == 1);
            testZone = TouchUI::addZone(BTN_X, TEST_BTN_Y, BTN_W, BTN_H);
            joinZone = -1;
        } else {
            drawButton(JOIN_BTN_Y, "[ JOIN ] COLLEGATI ALLA RETE", mainFocus == 1);
            joinZone = TouchUI::addZone(BTN_X, JOIN_BTN_Y, BTN_W, BTN_H);
            testZone = -1;
        }
    }

    void showMainScreen() {
        mode = Mode::Main;
        Input::setTextEntryMode(false);
        drawMainScreen();
    }

    // Tenta il join OTAA - BLOCCANTE (qualche secondo, limite della
    // libreria/del protocollo: join + attesa della finestra di
    // risposta), va richiamato solo su azione esplicita dell'utente,
    // con un messaggio "in corso" gia' a schermo PRIMA di chiamarlo.
    void attemptJoin() {
        uint8_t joinEuiBytes[8];
        uint8_t devEuiBytes[8];
        uint8_t appKeyBytes[16];
        if (!hexToBytes(joinEuiHex, joinEuiBytes, 8) ||
            !hexToBytes(devEuiHex, devEuiBytes, 8) ||
            !hexToBytes(appKeyHex, appKeyBytes, 16)) {
            joinState = JoinState::JoinFailed;
            lastUplinkResult = "Credenziali non valide.";
            drawStatus();
            return;
        }

        if (!radioBorrowed) {
            LoRaRadio::pauseForExternalUse();
            radioBorrowed = true;
        }
        SX1262& radio = LoRaRadio::getRadioForExternalUse();

        if (node != nullptr) {
            delete node;
        }
        node = new LoRaWANNode(&radio, &EU868, 0);

        uint64_t joinEui = bytesToUint64(joinEuiBytes);
        uint64_t devEui = bytesToUint64(devEuiBytes);

        int16_t state = node->beginOTAA(joinEui, devEui, appKeyBytes, appKeyBytes);
        if (state == RADIOLIB_ERR_NONE) {
            state = node->activateOTAA();
        }

        if (state == RADIOLIB_LORAWAN_NEW_SESSION || state == RADIOLIB_LORAWAN_SESSION_RESTORED) {
            joinState = JoinState::Joined;
            lastUplinkResult = "";
        } else {
            joinState = JoinState::JoinFailed;
            lastUplinkResult = "Codice errore: " + String(state);
            delete node;
            node = nullptr;
        }
        drawMainScreen();
    }

    // Manda un piccolo payload fisso come test - BLOCCANTE (invio +
    // attesa delle finestre di ricezione RX1/RX2).
    void sendTestUplink() {
        if (node == nullptr) {
            return;
        }
        uint8_t payload[1] = { 0x2A }; // valore fisso arbitrario, solo per verificare che l'invio funzioni
        int16_t state = node->sendReceive(payload, sizeof(payload));
        if (state < RADIOLIB_ERR_NONE) {
            lastUplinkResult = "Ultimo invio: errore " + String(state);
        } else if (state > 0) {
            lastUplinkResult = "Ultimo invio: ok, downlink ricevuto.";
        } else {
            lastUplinkResult = "Ultimo invio: ok, nessun downlink.";
        }
        drawMainScreen();
    }

    // --- Schermata configurazione (Join EUI / Dev EUI / App Key) ---

    constexpr int16_t CFG_ROW_TOP = Display::DD_HEADER_H + 30;
    constexpr int16_t CFG_ROW_H = 40;
    constexpr int16_t CFG_DONE_Y = 200;
    int cfgRowZone[CONFIG_FIELD_COUNT];
    int cfgDoneZone = -1;

    constexpr int16_t EDIT_FIELD_X = 8;
    constexpr int16_t EDIT_FIELD_Y = Display::DD_HEADER_H + 55;
    constexpr int16_t EDIT_FIELD_W = 288;
    constexpr int16_t EDIT_FIELD_H = 28;
    int editCancelZone = -1;

    const char* fieldLabel(ConfigField f) {
        switch (f) {
            case ConfigField::JoinEUI: return "JOIN EUI (16 hex)";
            case ConfigField::DevEUI:  return "DEV EUI (16 hex)";
            case ConfigField::AppKey:  return "APP KEY (32 hex)";
        }
        return "?";
    }

    String fieldValue(ConfigField f) {
        switch (f) {
            case ConfigField::JoinEUI: return joinEuiHex;
            case ConfigField::DevEUI:  return devEuiHex;
            case ConfigField::AppKey:  return appKeyHex;
        }
        return "";
    }

    uint16_t fieldMaxLen(ConfigField f) {
        return (f == ConfigField::AppKey) ? 32 : 16;
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
        gfx->setCursor(BTN_X + 12, y + 4);
        gfx->print(fieldLabel(f));

        String value = fieldValue(f);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 12, y + 16);
        gfx->print(value.length() > 0 ? value : "(non impostato)");
    }

    void drawConfigScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        // clearZones() PRIMA di drawHeaderTitle() (02/10) - stesso motivo
        // di drawMainScreen().
        TouchUI::clearZones();

        drawHeaderTitle("LORAWAN CONFIG");

        Display::drawWrappedText(
            BTN_X, Display::DD_HEADER_H + 18, BTN_W,
            "Tocca una riga per modificarla (valori dal pannello del tuo network server).",
            Display::DD_TEXT_DIM
        );

        for (int i = 0; i < CONFIG_FIELD_COUNT; i++) {
            drawConfigRow(i);
            int16_t y = CFG_ROW_TOP + i * CFG_ROW_H;
            cfgRowZone[i] = TouchUI::addZone(BTN_X, y, BTN_W, CFG_ROW_H - 4);
        }

        drawButton(CFG_DONE_Y, "[ FATTO ]");
        cfgDoneZone = TouchUI::addZone(BTN_X, CFG_DONE_Y, BTN_W, BTN_H);
    }

    void showConfigScreen() {
        mode = Mode::Config;
        selectedConfigRow = 0;
        Input::setTextEntryMode(false);
        drawConfigScreen();
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
            "Solo cifre hex (0-9, A-F), lunghezza esatta - poi Invio per confermare.",
            Display::DD_TEXT_DIM
        );

        drawButton(200, "[ ANNULLA ]");
        editCancelZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
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
        trimmed.toUpperCase();

        switch (editingField) {
            case ConfigField::JoinEUI: joinEuiHex = trimmed; prefs.putString(KEY_JOIN_EUI, trimmed); break;
            case ConfigField::DevEUI:  devEuiHex = trimmed;  prefs.putString(KEY_DEV_EUI, trimmed);  break;
            case ConfigField::AppKey:  appKeyHex = trimmed;  prefs.putString(KEY_APP_KEY, trimmed);  break;
        }

        showConfigScreen();
    }

    void handleMainInput() {
        // tapped >= 0 (02/10, bug reale trovato: "non va ne' B ne'
        // trackball su LoRaWAN") - TouchUI::pollTap() ritorna -1 quando
        // non si tocca nulla, ma joinZone/testZone valgono anch'essi -1
        // quando quel pulsante non e' visibile in quel momento (es. non
        // ancora collegato) - senza questa guardia "-1 == -1" risultava
        // sempre vero, uscendo PRIMA di leggere tastiera/trackball su
        // ogni singolo ciclo finche' non ci si collegava.
        int tapped = TouchUI::pollTap();
        if (tapped >= 0 && tapped == backHintZone) {
            closeLoRaWan();
            return;
        }
        if (tapped >= 0 && tapped == configZone) {
            mainFocus = 0;
            showConfigScreen();
            return;
        }
        if (tapped >= 0 && tapped == joinZone && isConfigured() && joinState != JoinState::Joining) {
            mainFocus = 1;
            joinState = JoinState::Joining;
            drawStatus();
            attemptJoin();
            return;
        }
        if (tapped >= 0 && tapped == testZone) {
            mainFocus = 1;
            sendTestUplink();
            return;
        }

        // Su/Giu' tra CONFIG e JOIN/TEST (02/10, bug segnalato: prima
        // raggiungibili solo a touch).
        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (mainFocus > 0) {
                    mainFocus--;
                    drawMainScreen();
                }
                break;

            case InputEvent::Down:
                if (mainFocus < 1) {
                    mainFocus++;
                    drawMainScreen();
                }
                break;

            case InputEvent::Select:
                if (mainFocus == 0) {
                    showConfigScreen();
                } else if (joinState == JoinState::Joined) {
                    sendTestUplink();
                } else if (isConfigured() && joinState != JoinState::Joining) {
                    joinState = JoinState::Joining;
                    drawStatus();
                    attemptJoin();
                }
                break;

            case InputEvent::Back:
                closeLoRaWan();
                break;

            default:
                break;
        }
    }

    void handleConfigInput() {
        int tapped = TouchUI::pollTap();
        if (tapped >= 0 && tapped == backHintZone) {
            showMainScreen();
            return;
        }
        if (tapped >= 0 && tapped == cfgDoneZone) {
            showMainScreen();
            return;
        }
        for (int i = 0; i < CONFIG_FIELD_COUNT; i++) {
            if (tapped == cfgRowZone[i]) {
                selectedConfigRow = i;
                showEditScreen(static_cast<ConfigField>(i));
                return;
            }
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (selectedConfigRow > 0) {
                    selectedConfigRow--;
                    drawConfigRow(selectedConfigRow);
                    drawConfigRow(selectedConfigRow + 1);
                }
                break;

            case InputEvent::Down:
                if (selectedConfigRow < CONFIG_FIELD_COUNT - 1) {
                    selectedConfigRow++;
                    drawConfigRow(selectedConfigRow);
                    drawConfigRow(selectedConfigRow - 1);
                }
                break;

            case InputEvent::Select:
                showEditScreen(static_cast<ConfigField>(selectedConfigRow));
                break;

            case InputEvent::Back:
                showMainScreen();
                break;

            default:
                break;
        }
    }

    void handleEditInput() {
        int tapped = TouchUI::pollTap();
        if (tapped >= 0 && tapped == editCancelZone) {
            showConfigScreen();
            return;
        }

        TextInputUI::Result result = TextInputUI::handleKey(editBuffer, fieldMaxLen(editingField));
        if (result == TextInputUI::Result::Changed) {
            TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, editBuffer, false);
        } else if (result == TextInputUI::Result::Confirmed) {
            confirmEdit();
        }
    }

} // namespace

namespace LoRaWanUI {

    void open() {
        ensureNvsOpen();
        joinState = JoinState::Idle;
        lastUplinkResult = "";
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
            case Mode::Config:
                handleConfigInput();
                break;
            case Mode::EditField:
                handleEditInput();
                break;
        }
    }
}
