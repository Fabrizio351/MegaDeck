#include "VoipUI.h"
#include "SipClient.h"
#include "RtpAudio.h"
#include "Display.h"
#include "Input.h"
#include "Menu.h"
#include "TouchUI.h"
#include "TextInputUI.h"
#include "Sound.h"

#include <Arduino.h>

// Schermate VoIP (08/10) - stessa struttura di HermesUI.cpp (CONFIG a
// righe, campo di testo riusabile) ma per un account SIP invece di un
// host/token HTTP. v1 (vedi il piano approvato): inattiva/comporre
// numero, chiamata in corso, CONFIG a 5 campi - NIENTE schermata di
// chiamata in arrivo ancora (v3).
namespace {

    enum class Mode {
        Closed,
        Idle,
        Dial,
        InCall,
        Config,
        EditField
    };
    Mode mode = Mode::Closed;
    bool active = false;

    // Vero appena VOIP viene aperta la prima volta (open()/openConfig())
    // - serve solo a decidere se backgroundUpdate() deve fare qualcosa,
    // NON se SipClient e' davvero partito (per quello c'e'
    // SipClient::isStarted() - vedi ensureSocketStarted() sotto, bug
    // reale corretto l'11/10: un flag separato che significava "ci ho
    // gia' provato" invece di "e' andata bene" restava bloccato per
    // sempre se il primo tentativo falliva per WiFi non ancora connesso).
    bool voipEverOpened = false;

    // Secondo piano (08/10) - RAM-only, MAI persistito su NVS, sempre
    // spento al riavvio: stesso identico meccanismo di MeshtasticUI.cpp
    // (goToBackground()/backgroundEnabled), corretto durante la
    // revisione del piano rispetto alla mia prima bozza (non e' una riga
    // di Impostazioni). Serve a lasciare una chiamata attiva mentre si
    // usa un'altra schermata del T-Deck.
    bool backgroundEnabled = false;

    String dialBuffer;

    SipClient::CallState lastObservedCallState = SipClient::CallState::Idle;
    uint32_t callConnectedAtMs = 0;
    uint32_t lastInCallRedrawMs = 0;

    enum class ConfigField {
        Server,
        Port,
        Domain,
        User,
        Password,
        DisplayName
    };
    constexpr int CONFIG_FIELD_COUNT = 6;
    ConfigField editingField = ConfigField::Server;
    String editBuffer;
    int selectedConfigRow = 0;

    constexpr int16_t BTN_X = 8;
    constexpr int16_t BTN_W = 304;
    constexpr int16_t BTN_H = 28;
    constexpr int VOLUME_STEP = 10; // stesso passo di SettingsUI.cpp (BRIGHTNESS_STEP)

    constexpr int16_t STATUS_Y = Display::DD_HEADER_H + 20;
    constexpr int16_t STATUS2_Y = STATUS_Y + 14;

    // Colonna VOL+/VOL-/MUTO sulla schermata CHIAMATA (11/10, richiesto:
    // poter regolare senza uscire dalla schermata) - 3 bottoni a tutta
    // larghezza, impilati, stesso stile/altezza (BTN_H) degli altri
    // bottoni di questa UI (drawButton() non supporta un'altezza
    // diversa da BTN_H).
    constexpr int16_t VOL_UP_Y = 70;
    constexpr int16_t VOL_DOWN_Y = VOL_UP_Y + BTN_H + 3;
    constexpr int16_t MUTE_BTN_Y = VOL_DOWN_Y + BTN_H + 3;
    constexpr int16_t HANGUP_BTN_Y = MUTE_BTN_Y + BTN_H + 9;

    // Secondo piano: stesso bottone/riga di MeshtasticUI.cpp.
    constexpr int16_t BG_BTN_H = 16;
    constexpr int16_t BG_BTN_Y = 216;

    // Tastierino telefonico touch/trackball (09/10, richiesto: tasti
    // 0-9 a schermo + trackball, non solo tastiera fisica) - layout
    // classico a 3x4 (1 2 3 / 4 5 6 / 7 8 9 / * 0 #). Sotto, una quinta
    // "riga" di sole 2 colonne (CANC/CHIAMA) navigabile con lo stesso
    // meccanismo. ANNULLA resta un bottone separato SOLO a tocco
    // (Indietro fisico lo sostituisce per tastiera/trackball).
    constexpr int KEYPAD_ROWS = 4;
    constexpr int KEYPAD_COLS = 3;
    const char KEYPAD_CHARS[KEYPAD_ROWS][KEYPAD_COLS] = {
        {'1', '2', '3'},
        {'4', '5', '6'},
        {'7', '8', '9'},
        {'*', '0', '#'}
    };

    constexpr int16_t DIAL_STATUS_Y = Display::DD_HEADER_H + 4;
    constexpr int16_t DIAL_NUM_Y = DIAL_STATUS_Y + 10;
    constexpr int16_t DIAL_PREVIEW_Y = DIAL_NUM_Y + 16;
    constexpr int16_t KEYPAD_TOP = DIAL_PREVIEW_Y + 14;
    constexpr int16_t KEY_H = 25;
    constexpr int16_t KEY_GAP = 3;
    constexpr int16_t KEY_GAP_X = 5;
    constexpr int16_t KEY_W = (BTN_W - 2 * KEY_GAP_X) / KEYPAD_COLS;
    constexpr int16_t ACTION_ROW_Y = KEYPAD_TOP + KEYPAD_ROWS * (KEY_H + KEY_GAP);
    constexpr int16_t ACTION_KEY_W = (BTN_W - KEY_GAP_X) / 2;
    constexpr int16_t DIAL_ANNULLA_Y = ACTION_ROW_Y + KEY_H + KEY_GAP + 3;
    constexpr int MAX_DIAL_LEN = 48;

    int keypadFocusRow = 0;
    int keypadFocusCol = 0;
    int keyZone[KEYPAD_ROWS][KEYPAD_COLS];
    int cancZone = -1;
    int callZone = -1;

    constexpr int16_t EDIT_FIELD_X = 8;
    constexpr int16_t EDIT_FIELD_Y = Display::DD_HEADER_H + 55;
    constexpr int16_t EDIT_FIELD_W = 280;
    constexpr int16_t EDIT_FIELD_H = 28;

    // 6 righe (aggiunto DOMINIO il 09/10, scoperto dal vivo che serve
    // per i provider con outbound proxy separato dal dominio, es.
    // sip2sip.info - vedi SipClient.cpp) - CFG_ROW_TOP/CFG_ROW_H
    // ristretti ulteriormente per starci negli stessi 240px (stessa
    // operazione di sempre quando si aggiunge una riga).
    constexpr int16_t CFG_ROW_TOP = Display::DD_HEADER_H + 20;
    constexpr int16_t CFG_ROW_H = 27;

    int hangupZone = -1;
    int volUpZone = -1;
    int volDownZone = -1;
    int muteZone = -1;
    int bgZone = -1;
    int cancelZone = -1;
    int cfgRowZone[CONFIG_FIELD_COUNT];
    int cfgDoneZone = -1;

    void showIdleScreen();
    void showConfigScreen();
    void drawInCallScreen();
    void drawIdleScreen();
    void showInCallScreen();

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

    void drawButton(int16_t y, int16_t w, const char* label, bool enabled = true) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(BTN_X + 1, y + 1, w - 2, BTN_H - 2, Display::DD_BG);
        gfx->drawRect(BTN_X, y, w, BTN_H, enabled ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setTextColor(enabled ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setCursor(BTN_X + 12, y + BTN_H / 2 - 3);
        gfx->print(label);
    }

    // Stesso bug/fix di drawMuteButton() sopra: il ridisegno NON deve
    // registrare una nuova zona ogni volta, solo la prima volta che la
    // schermata CHIAMATA viene creata.
    void drawBackgroundButtonVisual() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(BTN_X + 1, BG_BTN_Y + 1, BTN_W - 2, BG_BTN_H - 2, Display::DD_BG);
        gfx->drawRect(BTN_X, BG_BTN_Y, BTN_W, BG_BTN_H, backgroundEnabled ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setTextColor(backgroundEnabled ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setCursor(BTN_X + 10, BG_BTN_Y + BG_BTN_H / 2 - 3);
        gfx->print(backgroundEnabled ? "[ ESEGUI VOIP IN BACKGROUND: ON ]" : "[ ESEGUI VOIP IN BACKGROUND: OFF ]");
    }

    void drawBackgroundButton() {
        drawBackgroundButtonVisual();
        bgZone = TouchUI::addZone(BTN_X, BG_BTN_Y, BTN_W, BG_BTN_H);
    }

    void toggleBackground() {
        backgroundEnabled = !backgroundEnabled;
        drawBackgroundButtonVisual();
    }

    // Riprova begin() ogni volta che viene chiamata finche' non riesce
    // DAVVERO (SipClient::isStarted()) - non un "ci ho gia' provato una
    // volta". Richiamata sia all'apertura di VOIP sia, piu' importante,
    // da backgroundUpdate() ad ogni ciclo mentre non e' ancora partito:
    // se il WiFi si connette DOPO aver aperto VOIP, questo e' il punto
    // che se ne accorge e riprova, invece di restare bloccati per
    // sempre su "Serve una rete WiFi connessa" anche a WiFi connesso.
    void ensureSocketStarted() {
        if (!SipClient::isStarted()) {
            SipClient::begin();
        }
    }

    // --- Schermata inattiva: SOLO avviso "non configurato" (09/10,
    // cambio UX richiesto - CONFIG non e' piu' raggiungibile da qui,
    // resta solo in Impostazioni; se l'account e' configurato si va
    // DIRETTAMENTE al tastierino, vedi VoipUI::open()). ---

    void drawIdleScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("VOIP");

        TouchUI::clearZones();

        Display::drawWrappedText(
            8, STATUS_Y, 304,
            "Account non configurato.\nVai su IMPOSTAZIONI > VOIP per configurarlo.",
            Display::DD_WARN
        );
    }

    void showIdleScreen() {
        mode = Mode::Idle;
        Input::setTextEntryMode(false);
        drawIdleScreen();
    }

    void closeVoip() {
        mode = Mode::Closed;
        active = false;
        Menu::back();
    }

    // --- Comporre numero (tastierino touch/trackball) ---

    String dialPreviewUri() {
        if (dialBuffer.length() == 0) {
            return "";
        }
        if (dialBuffer.startsWith("sip:")) {
            return dialBuffer;
        }
        return "sip:" + dialBuffer + "@" + SipClient::getEffectiveDomain();
    }

    void drawDialStatusLine() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, DIAL_STATUS_Y, 320, 10, Display::DD_BG);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(SipClient::isRegistered() ? Display::DD_ACCENT : Display::DD_WARN);
        gfx->setCursor(8, DIAL_STATUS_Y);
        gfx->print(SipClient::getLastStatusText());
    }

    void drawDialDisplay() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, DIAL_NUM_Y, 320, 30, Display::DD_BG);
        gfx->setFont(nullptr);
        gfx->setTextWrap(false);

        gfx->setTextSize(2);
        gfx->setTextColor(Display::DD_TEXT);
        gfx->setCursor(8, DIAL_NUM_Y);
        gfx->print(dialBuffer.length() > 0 ? dialBuffer : "_");

        Display::drawWrappedText(8, DIAL_PREVIEW_Y, 304, dialPreviewUri(), Display::DD_TEXT_DIM);
    }

    const char* actionKeyLabel(int col) {
        return (col == 0) ? "[ CANC ]" : "[ CHIAMA ]";
    }

    void drawKeypadKey(int row, int col) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t x = BTN_X + col * (KEY_W + KEY_GAP_X);
        int16_t y = KEYPAD_TOP + row * (KEY_H + KEY_GAP);
        bool focused = (keypadFocusRow == row && keypadFocusCol == col);

        gfx->fillRect(x + 1, y + 1, KEY_W - 2, KEY_H - 2, Display::DD_BG);
        gfx->drawRect(x, y, KEY_W, KEY_H, focused ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_ACCENT);
        char label[2] = { KEYPAD_CHARS[row][col], 0 };
        gfx->setCursor(x + KEY_W / 2 - 3, y + KEY_H / 2 - 3);
        gfx->print(label);
    }

    void drawActionKey(int col) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t x = BTN_X + col * (ACTION_KEY_W + KEY_GAP_X);
        bool focused = (keypadFocusRow == KEYPAD_ROWS && keypadFocusCol == col);

        gfx->fillRect(x + 1, ACTION_ROW_Y + 1, ACTION_KEY_W - 2, KEY_H - 2, Display::DD_BG);
        gfx->drawRect(x, ACTION_ROW_Y, ACTION_KEY_W, KEY_H, focused ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(x + 10, ACTION_ROW_Y + KEY_H / 2 - 3);
        gfx->print(actionKeyLabel(col));
    }

    void drawDialScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("COMPONI NUMERO");

        TouchUI::clearZones();

        drawDialStatusLine();
        drawDialDisplay();

        for (int r = 0; r < KEYPAD_ROWS; r++) {
            for (int c = 0; c < KEYPAD_COLS; c++) {
                drawKeypadKey(r, c);
                int16_t x = BTN_X + c * (KEY_W + KEY_GAP_X);
                int16_t y = KEYPAD_TOP + r * (KEY_H + KEY_GAP);
                keyZone[r][c] = TouchUI::addZone(x, y, KEY_W, KEY_H);
            }
        }

        drawActionKey(0);
        cancZone = TouchUI::addZone(BTN_X, ACTION_ROW_Y, ACTION_KEY_W, KEY_H);
        drawActionKey(1);
        callZone = TouchUI::addZone(BTN_X + ACTION_KEY_W + KEY_GAP_X, ACTION_ROW_Y, ACTION_KEY_W, KEY_H);

        drawButton(DIAL_ANNULLA_Y, BTN_W, "[ ANNULLA ]");
        cancelZone = TouchUI::addZone(BTN_X, DIAL_ANNULLA_Y, BTN_W, BTN_H);
    }

    void showDialScreen() {
        mode = Mode::Dial;
        dialBuffer = "";
        keypadFocusRow = 0;
        keypadFocusCol = 0;
        Input::setTextEntryMode(true);
        drawDialScreen();
    }

    void appendDialChar(char c) {
        if (dialBuffer.length() < MAX_DIAL_LEN) {
            dialBuffer += c;
            // Bip di conferma (10/10, richiesto: sapere di aver
            // selezionato una cifra senza guardare lo schermo) -
            // Sound e' ancora "pronto" qui perche' si e' sul
            // tastierino PRIMA che una chiamata parta (RtpAudio non ha
            // ancora preso in prestito l'I2S, vedi RtpAudio::start()).
            Sound::playVoipKeyTone();
            drawDialDisplay();
        }
    }

    void backspaceDialChar() {
        if (dialBuffer.length() > 0) {
            dialBuffer.remove(dialBuffer.length() - 1);
            drawDialDisplay();
        }
    }

    void attemptCall() {
        String number = dialBuffer;
        number.trim();
        if (number.length() == 0) {
            return;
        }
        // Passa alla schermata CHIAMATA solo se la chiamata e' DAVVERO
        // partita (vedi SipClient::call()) - altrimenti si resta sul
        // tastierino, dove la riga di stato in alto mostra il motivo
        // (es. "Non registrato: impossibile chiamare") - bug reale
        // trovato dal vivo il 10/10: prima si passava SEMPRE alla
        // schermata CHIAMATA anche se call() non aveva fatto nulla,
        // restando poi bloccati li' (CHIUDI CHIAMATA non aveva nessuno
        // stato reale da cui uscire).
        if (SipClient::call(number)) {
            showInCallScreen();
        } else {
            drawDialStatusLine();
        }
    }

    // Ridisegna solo la cella che perde/guadagna il focus, senza
    // ridisegnare tutto il tastierino.
    void redrawKeypadCell(int row, int col) {
        if (row == KEYPAD_ROWS) {
            drawActionKey(col);
        } else {
            drawKeypadKey(row, col);
        }
    }

    // Sposta il focus del tastierino di una riga/colonna - MAI con
    // "avvolgimento" (stesso comportamento di Menu.cpp/SettingsUI.cpp).
    // La riga "azioni" (KEYPAD_ROWS) ha solo 2 colonne (CANC/CHIAMA): la
    // colonna viene riadattata entrando/uscendo da quella riga.
    void moveKeypadFocus(int deltaRow, int deltaCol) {
        int previousRow = keypadFocusRow;
        int previousCol = keypadFocusCol;

        if (deltaRow != 0) {
            int newRow = keypadFocusRow + deltaRow;
            if (newRow < 0) newRow = 0;
            if (newRow > KEYPAD_ROWS) newRow = KEYPAD_ROWS;
            keypadFocusRow = newRow;
            if (keypadFocusRow == KEYPAD_ROWS && keypadFocusCol > 1) {
                keypadFocusCol = 1;
            }
        }
        if (deltaCol != 0) {
            int maxCol = (keypadFocusRow == KEYPAD_ROWS) ? 1 : (KEYPAD_COLS - 1);
            int newCol = keypadFocusCol + deltaCol;
            if (newCol < 0) newCol = 0;
            if (newCol > maxCol) newCol = maxCol;
            keypadFocusCol = newCol;
        }

        if (previousRow != keypadFocusRow || previousCol != keypadFocusCol) {
            redrawKeypadCell(previousRow, previousCol);
            redrawKeypadCell(keypadFocusRow, keypadFocusCol);
        }
    }

    // Select (trackball o Invio - indistinguibili a questo livello,
    // vedi Input.h) attiva SEMPRE il tasto a fuoco in questo momento:
    // una cifra la aggiunge, CANC cancella, CHIAMA chiama. Scrivere con
    // la tastiera fisica sposta il focus su CHIAMA (vedi
    // handleDialInput()) cosi' un Invio successivo chiama davvero,
    // invece di "premere" per sbaglio il tasto dove il focus si trovava
    // prima di iniziare a scrivere.
    void activateFocusedKey() {
        if (keypadFocusRow == KEYPAD_ROWS) {
            if (keypadFocusCol == 0) {
                backspaceDialChar();
            } else {
                attemptCall();
            }
        } else {
            appendDialChar(KEYPAD_CHARS[keypadFocusRow][keypadFocusCol]);
        }
    }

    // --- Chiamata in corso ---

    String formatDuration(uint32_t ms) {
        uint32_t totalSec = ms / 1000;
        uint32_t minutes = totalSec / 60;
        uint32_t seconds = totalSec % 60;
        char buf[8];
        snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>(minutes), static_cast<unsigned>(seconds));
        return String(buf);
    }

    void drawInCallStatusArea() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, STATUS_Y, 320, 26, Display::DD_BG);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        SipClient::CallState st = SipClient::getCallState();
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, STATUS_Y);
        // getLastStatusText() (non getStatusText(), che mostra solo lo
        // stato di REGISTRAZIONE) - qui serve il progresso/errore della
        // CHIAMATA ("Squilla...", "Chiamata fallita: ...", ecc.), bug
        // reale trovato dal vivo il 09/10: prima questa riga restava
        // sempre su "Registrato su ..." durante tutta la chiamata.
        gfx->print(SipClient::getLastStatusText());

        if (st == SipClient::CallState::InCall) {
            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->setCursor(8, STATUS2_Y);
            gfx->print("Durata: " + formatDuration(millis() - callConnectedAtMs));
        }
    }

    // Bottone MUTO - stesso stile "ON/OFF" di drawBackgroundButton()
    // (acceso quando attivo), legge/scrive direttamente
    // RtpAudio::isMuted()/setMuted() (nessuno stato duplicato qui).
    //
    // BUG REALE trovato il 16/10 ("premendo muto non si torna piu'
    // indietro, si blocca tutto"): questa funzione faceva ANCHE
    // TouchUI::addZone() ad ogni ridisegno, inclusa ogni chiamata da
    // toggleMute() - le zone non vengono mai svuotate sulla schermata
    // CHIAMATA (clearZones() gira solo quando la si ridisegna da zero),
    // quindi ogni pressione di MUTO aggiungeva una zona in piu' invece
    // di riusare quella gia' registrata. Dopo ~11 pressioni il tavolo
    // zone di TouchUI (16 slot) si riempiva, addZone() iniziava a
    // ritornare -1, e muteZone diventava -1 per sempre - che e' anche
    // il valore che TouchUI::pollTap() ritorna quando NON c'e' nessun
    // tocco in corso: "tapped == muteZone" diventava vero ad OGNI ciclo
    // anche senza toccare nulla, toggleMute() scattava di continuo e la
    // funzione usciva sempre con "return" prima di arrivare al controllo
    // del tasto Indietro piu' sotto. Fix: separare il disegno (puo'
    // ripetersi quante volte si vuole) dalla registrazione della zona
    // (una volta sola, solo quando la schermata CHIAMATA viene creata).
    void drawMuteButtonVisual() {
        bool muted = RtpAudio::isMuted();
        drawButton(MUTE_BTN_Y, BTN_W, muted ? "[ MUTO: ON ]" : "[ MUTO: OFF ]", true);
        Arduino_GFX* gfx = Display::getGfx();
        gfx->drawRect(BTN_X, MUTE_BTN_Y, BTN_W, BTN_H, muted ? Display::DD_WARN : Display::DD_ACCENT_DIM);
    }

    void drawMuteButton() {
        drawMuteButtonVisual();
        muteZone = TouchUI::addZone(BTN_X, MUTE_BTN_Y, BTN_W, BTN_H);
    }

    void toggleMute() {
        RtpAudio::setMuted(!RtpAudio::isMuted());
        drawMuteButtonVisual();
    }

    void drawInCallScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("CHIAMATA");

        TouchUI::clearZones();

        drawInCallStatusArea();

        // Colonna VOL+/VOL-/MUTO (11/10, richiesto: regolare senza
        // uscire dalla schermata) - riusa Sound::setVolumePercent(),
        // stesso valore persistito del resto del progetto.
        drawButton(VOL_UP_Y, BTN_W, "[ VOLUME + ]");
        volUpZone = TouchUI::addZone(BTN_X, VOL_UP_Y, BTN_W, BTN_H);
        drawButton(VOL_DOWN_Y, BTN_W, "[ VOLUME - ]");
        volDownZone = TouchUI::addZone(BTN_X, VOL_DOWN_Y, BTN_W, BTN_H);
        drawMuteButton();

        drawButton(HANGUP_BTN_Y, BTN_W, "[ CHIUDI CHIAMATA ]");
        hangupZone = TouchUI::addZone(BTN_X, HANGUP_BTN_Y, BTN_W, BTN_H);

        drawBackgroundButton();

        lastInCallRedrawMs = millis();
    }

    void showInCallScreen() {
        mode = Mode::InCall;
        Input::setTextEntryMode(false);
        drawInCallScreen();
    }

    // --- CONFIG (5 campi: SERVER/PORTA/UTENTE/PASSWORD/NOME) ---

    const char* fieldLabel(ConfigField f) {
        switch (f) {
            case ConfigField::Server:      return "SERVER";
            case ConfigField::Port:        return "PORTA";
            // Vuoto = usa SERVER anche come dominio (caso comune). Alcuni
            // provider (es. sip2sip.info) hanno un outbound proxy diverso
            // dal dominio: SERVER e' dove si manda il traffico davvero,
            // DOMINIO e' quello usato dentro gli indirizzi SIP.
            case ConfigField::Domain:       return "DOMINIO (vuoto=SERVER)";
            case ConfigField::User:        return "UTENTE";
            case ConfigField::Password:    return "PASSWORD";
            case ConfigField::DisplayName: return "NOME";
        }
        return "?";
    }

    String fieldValue(ConfigField f) {
        switch (f) {
            case ConfigField::Server:      return SipClient::getServer();
            case ConfigField::Port:        return SipClient::getPort() > 0 ? String(SipClient::getPort()) : "";
            case ConfigField::Domain:      return SipClient::getDomain();
            case ConfigField::User:        return SipClient::getUser();
            case ConfigField::Password:    return SipClient::getPassword();
            case ConfigField::DisplayName: return SipClient::getDisplayName();
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
        gfx->setCursor(BTN_X + 14, y + 3);
        gfx->print(fieldLabel(f));

        String value = fieldValue(f);
        if (f == ConfigField::Password && value.length() > 0) {
            value = "********";
        }
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(BTN_X + 14, y + 14);
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
        drawHeaderTitle("VOIP CONFIG");

        TouchUI::clearZones();
        for (int i = 0; i < CONFIG_FIELD_COUNT; i++) {
            drawConfigRow(i);
            int16_t y = CFG_ROW_TOP + i * CFG_ROW_H;
            cfgRowZone[i] = TouchUI::addZone(BTN_X, y, BTN_W, CFG_ROW_H - 4);
        }

        drawButton(206, BTN_W, "[ FATTO ]");
        cfgDoneZone = TouchUI::addZone(BTN_X, 206, BTN_W, BTN_H);
    }

    void showConfigScreen() {
        mode = Mode::Config;
        selectedConfigRow = 0;
        Input::setTextEntryMode(false);
        drawConfigScreen();
    }

    // CONFIG e' raggiungibile SOLO da Impostazioni (SettingsUI::
    // openConfig()) - FATTO/Indietro da qui tornano sempre li'.
    void exitConfigScreen() {
        mode = Mode::Closed;
        active = false;
        Menu::back();
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

        bool mask = (editingField == ConfigField::Password);
        TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, editBuffer, mask);

        Display::drawWrappedText(
            8, EDIT_FIELD_Y + EDIT_FIELD_H + 10, 304,
            "Scrivi con la tastiera fisica, poi Invio per confermare.",
            Display::DD_TEXT_DIM
        );

        drawButton(200, BTN_W, "[ ANNULLA ]");
        cancelZone = TouchUI::addZone(BTN_X, 200, BTN_W, BTN_H);
    }

    void showEditScreen(ConfigField field) {
        mode = Mode::EditField;
        editingField = field;
        editBuffer = fieldValue(field);
        if (field == ConfigField::Password && editBuffer == "********") {
            editBuffer = SipClient::getPassword();
        }
        Input::setTextEntryMode(true);
        drawEditScreen();
    }

    void confirmEdit() {
        String trimmed = editBuffer;
        trimmed.trim();

        String srv = SipClient::getServer();
        int prt = SipClient::getPort();
        String dom = SipClient::getDomain();
        String usr = SipClient::getUser();
        String pwd = SipClient::getPassword();
        String dispName = SipClient::getDisplayName();

        switch (editingField) {
            case ConfigField::Server:      srv = trimmed;           break;
            case ConfigField::Port:        prt = trimmed.toInt();   break;
            case ConfigField::Domain:      dom = trimmed;           break;
            case ConfigField::User:        usr = trimmed;           break;
            case ConfigField::Password:    pwd = trimmed;           break;
            case ConfigField::DisplayName: dispName = trimmed;      break;
        }

        // SipClient::setConfig() si occupa gia' da solo di azzerare lo
        // stato di registrazione e forzare un nuovo tentativo con le
        // credenziali aggiornate (se il socket e' gia' aperto) - niente
        // da rifare qui.
        SipClient::setConfig(srv, prt, dom, usr, pwd, dispName);
        showConfigScreen();
    }

    // --- Osservazione cambi di stato chiamata (SOLO punto che decide
    // RtpAudio::start()/stop(), vedi il piano) ---

    void pollCallStateTransitions() {
        SipClient::CallState current = SipClient::getCallState();
        if (current == lastObservedCallState) {
            return;
        }

        if (current == SipClient::CallState::InCall) {
            RtpAudio::start(SipClient::getRemoteRtpHost(), SipClient::getRemoteRtpPort(), SipClient::getLocalRtpPort());
            callConnectedAtMs = millis();
        } else if (lastObservedCallState == SipClient::CallState::InCall) {
            RtpAudio::stop();
        }

        lastObservedCallState = current;

        if (active && mode == Mode::InCall) {
            if (current == SipClient::CallState::Idle) {
                // Chiamata terminata (dall'altra parte, o per errore) -
                // torna al tastierino (non alla schermata "non
                // configurato": se eravamo in chiamata, l'account e'
                // per forza configurato).
                showDialScreen();
            } else {
                drawInCallScreen();
            }
        }
    }

    // --- Gestione input per modalita' ---

    void handleIdleInput() {
        InputEvent event = Input::getEvent();
        if (event == InputEvent::Back) {
            closeVoip();
        }
    }

    void handleDialInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == cancelZone) {
            closeVoip();
            return;
        }
        if (tapped == cancZone) {
            backspaceDialChar();
            return;
        }
        if (tapped == callZone) {
            attemptCall();
            return;
        }
        for (int r = 0; r < KEYPAD_ROWS; r++) {
            for (int c = 0; c < KEYPAD_COLS; c++) {
                if (tapped == keyZone[r][c]) {
                    appendDialChar(KEYPAD_CHARS[r][c]);
                    return;
                }
            }
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:     moveKeypadFocus(-1, 0); return;
            case InputEvent::Down:   moveKeypadFocus(1, 0);  return;
            case InputEvent::Left:   moveKeypadFocus(0, -1); return;
            case InputEvent::Right:  moveKeypadFocus(0, 1);  return;
            case InputEvent::Select: activateFocusedKey();   return;
            case InputEvent::Back:   closeVoip();             return;
            default: break;
        }

        // Qualunque altro evento (InputEvent::Key dalla tastiera fisica,
        // per scrivere un URI SIP completo) - Select e' gia' intercettato
        // sopra come "attiva il tasto a fuoco", quindi qui arriva solo
        // testo libero.
        TextInputUI::Result result = TextInputUI::handleKey(dialBuffer, MAX_DIAL_LEN);
        if (result == TextInputUI::Result::Changed) {
            int previousRow = keypadFocusRow;
            int previousCol = keypadFocusCol;
            keypadFocusRow = KEYPAD_ROWS;
            keypadFocusCol = 1;
            drawDialDisplay();
            redrawKeypadCell(previousRow, previousCol);
            redrawKeypadCell(keypadFocusRow, keypadFocusCol);
        }
    }

    void handleInCallInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == hangupZone) {
            // Rete di sicurezza: se per qualunque motivo si arriva qui
            // senza una chiamata reale in corso (CallState::Idle), non
            // c'e' nulla da chiudere - torna semplicemente al
            // tastierino invece di restare bloccati (vedi il bug del
            // 10/10, risolto alla radice in VoipUI::attemptCall(), ma
            // vale la pena restare protetti anche qui).
            if (SipClient::getCallState() == SipClient::CallState::Idle) {
                showDialScreen();
                return;
            }
            SipClient::hangup();
            drawInCallStatusArea();
            return;
        }
        if (tapped == bgZone) {
            toggleBackground();
            return;
        }
        if (tapped == volUpZone) {
            Sound::setVolumePercent(Sound::getVolumePercent() + VOLUME_STEP);
            return;
        }
        if (tapped == volDownZone) {
            Sound::setVolumePercent(Sound::getVolumePercent() - VOLUME_STEP);
            return;
        }
        if (tapped == muteZone) {
            toggleMute();
            return;
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Back:
                if (SipClient::getCallState() == SipClient::CallState::Idle) {
                    showDialScreen();
                    break;
                }
                SipClient::hangup();
                drawInCallStatusArea();
                break;
            default:
                break;
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

        uint16_t maxLen = (editingField == ConfigField::Port) ? 5 : 64;
        TextInputUI::Result result = TextInputUI::handleKey(editBuffer, maxLen);
        if (result == TextInputUI::Result::Changed) {
            bool mask = (editingField == ConfigField::Password);
            TextInputUI::draw(EDIT_FIELD_X, EDIT_FIELD_Y, EDIT_FIELD_W, EDIT_FIELD_H, editBuffer, mask);
        } else if (result == TextInputUI::Result::Confirmed) {
            confirmEdit();
        }
    }

} // namespace

namespace VoipUI {

    void open() {
        active = true;
        voipEverOpened = true;
        Display::setDirectDrawMode(true);
        ensureSocketStarted();

        if (SipClient::getCallState() != SipClient::CallState::Idle) {
            showInCallScreen();
        } else if (!SipClient::isConfigured()) {
            showIdleScreen();
        } else {
            showDialScreen();
        }
    }

    void openConfig() {
        active = true;
        Display::setDirectDrawMode(true);
        showConfigScreen();
    }

    bool isActive() {
        return active;
    }

    bool isListening() {
        return active || backgroundEnabled;
    }

    void update() {
        if (!active) {
            return;
        }
        switch (mode) {
            case Mode::Closed:    break;
            case Mode::Idle:      handleIdleInput();   break;
            case Mode::Dial:      handleDialInput();   break;
            case Mode::InCall:    handleInCallInput(); break;
            case Mode::Config:    handleConfigInput(); break;
            case Mode::EditField: handleEditInput();   break;
        }
    }

    void backgroundUpdate() {
        if (!voipEverOpened) {
            return;
        }
        // NOTA (limite noto v1): se la schermata viene chiusa SENZA
        // accendere il secondo piano, SipClient::update() non viene piu'
        // richiamato - niente ri-registrazione/keepalive finche' non si
        // riapre VOIP. Accettabile in questa fetta (v1 non ha ancora
        // chiamate in arrivo, quindi "restare raggiungibili" non si
        // applica); da rivalutare quando arriva v3 del piano.
        if (!active && !backgroundEnabled) {
            return;
        }

        // Riprova begin() ad ogni ciclo finche' non e' davvero partito
        // (vedi ensureSocketStarted() - bug WiFi-non-ancora-connesso
        // dell'11/10): costo trascurabile una volta partito, dato che
        // SipClient::begin() stesso ritorna subito se il socket e' gia'
        // aperto.
        ensureSocketStarted();
        SipClient::update();
        pollCallStateTransitions();

        if (SipClient::hasStatusMessage()) {
            SipClient::consumeStatusMessage();
            if (active && mode == Mode::Dial) {
                drawDialStatusLine();
            }
        }

        if (active && mode == Mode::InCall && millis() - lastInCallRedrawMs > 1000) {
            lastInCallRedrawMs = millis();
            drawInCallStatusArea();
        }
    }
}
