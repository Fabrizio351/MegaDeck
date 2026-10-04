#include "WebRadioUI.h"
#include "Display.h"
#include "Input.h"
#include "Menu.h"
#include "TouchUI.h"
#include "TextInputUI.h"
#include "Sound.h"
#include "Config.h"
#include "ConnStatus.h"

#include <Arduino.h>
#include <Audio.h>
#include <Preferences.h>
#include <math.h>

// Interfaccia Web Radio (27/09 UI, 28/09 audio vero + volume + nomi
// modificabili) - musicassetta anni '90 disegnata a mano (stesso stile
// "solo primitive Arduino_GFX" gia' usato per il teschio crash e
// l'iconcina del menu About) con le bobine che girano quando lo stato
// e' "in play". Streaming reale via la libreria ESP32-audioI2S (gia'
// vendorizzata in libraries/, mai usata prima d'ora): un solo
// altoparlante fisico I2S condiviso con Sound.cpp, quindi Play/Stop si
// passano il possesso del bus con Sound::pauseForExternalUse()/
// resumeForOwnUse() (stesso principio gia' usato per la radio LoRa/SPI,
// vedi LoRaRadio::pauseForExternalUse()).
namespace {

    // 5->6 stazioni (04/10, richiesto esplicitamente: "vorrei che i 5
    // posti per le webradio fossero 6 cosi da avere un menu simmetrico")
    // - con 6 celle su una griglia a 2 colonne non resta piu' nessuna
    // riga "spaiata" (prima, con 5, l'ultima stazione occupava da sola
    // tutta la riga finale, vedi cellRect() sotto che gestisce comunque
    // entrambi i casi senza bisogno di modifiche).
    constexpr int STATION_COUNT = 6;

    // URL di default per tutte le 6 stazioni (04/10, lista data
    // dall'utente stazione per stazione) - verificati uno per uno con
    // curl a mano (header ICY "icy-name"/Content-Type audio, stesso
    // metodo gia' usato per DJ Osso il 28/09), non copiati da una
    // documentazione terza non verificata:
    //  0 DJ OSSO: confermato 28/09 (redirect 302 -> nodo live,
    //    Content-Type audio/mpeg, ICY "DjOsso Radio Failover").
    //  1 VIRGIN RADIO (Italia): icy.unitedradio.it/Virgin.mp3, ICY
    //    "FM_VRG_mp3_segrate", genere Rock - combacia.
    //  2 NRJ RADIO: streaming.nrjaudio.fm/oumvmk8fnozc, ICY "NRJ SAT
    //    MP3 - RK". NON esiste un web-stream separato per "NRJ Costa
    //    Azzurra" risultante dalla ricerca - le decrochage locali di NRJ
    //    in Francia restano solo su FM, online gli affiliati regionali
    //    simulcastano tutti lo stesso stream nazionale: questo e' il
    //    link piu' corretto reperibile, non un'invenzione.
    //  3 DEEP HOUSE RADIO (04/10: SOSTITUITA lo stesso giorno - la 1.FM
    //    Deep House Radio di Zug andava "a scatti", diagnosticato come
    //    limite reale: 256kbps, il doppio delle altre 5, nessun mount a
    //    bitrate piu' basso esiste per quella stazione - vedi il
    //    resoconto nella memoria di progetto. L'utente ha chiesto "deep
    //    house radio dance di bucharest romania, logo viola su sfondo
    //    nero" - trovata via radio-browser.info: "DHR FM - Deep House
    //    Radio" (www.dhr.fm, Bucarest, Romania - il logo viola/nero e'
    //    proprio il marchio DHR), live.dancemusic.ro:7000/radio.mp3, ICY
    //    "DHR FM - Deep House Radio (www.dhr.fm)", 128kbps - stesso
    //    bitrate delle altre 5, non dovrebbe avere lo stesso problema.
    //    HTTP semplice (non HTTPS): il server non offre TLS su quella
    //    porta, stesso livello di fiducia gia' assunto da NetScan/
    //    NetGuard/OpenClaw per servizi sulla rete le cui chiamate restano
    //    comunque in chiaro lato server.
    //  4 ISORADIO (RAI, notizie viabilita' italiana - "ISOARDI" nella
    //    richiesta era un refuso per "Isoradio"): icestreaming.rai.it/
    //    12.mp3, ICY "Isoradio Estero" - la variante RAI pensata per chi
    //    ascolta da fuori Italia (stesso bollettino viabilita', solo un
    //    relay pensato per essere raggiungibile anche oltreconfine); i
    //    mountpoint del flusso "nazionale" diretto hanno risposto 403
    //    durante la verifica (probabile geo-blocco dalla rete usata per
    //    la ricerca, non necessariamente dal T-Deck).
    //  5 VINCI 107.7 (autostrada, viabilita' Costa Azzurra - l'utente
    //    pensava fosse sempre 107.7 ma per la rete autostradale VINCI/
    //    ESCOTA che attraversa la Costa Azzurra, non Sanef/APRR che e'
    //    un operatore diverso su un'altra rete): str0.creacast.com/
    //    radio_vinci_autoroutes_7, confermato sia dal vivo (Content-Type
    //    audio/mpeg) sia da una fonte terza indipendente (lista
    //    community Euro Truck Simulator 2) che mappa esplicitamente il
    //    mount "_7" su "Côte-d'Azur".
    const char* DEFAULT_STATION_URLS[STATION_COUNT] = {
        "https://stream.rcs.revma.com/x3ab37sgn7uvv",
        "https://icy.unitedradio.it/Virgin.mp3",
        "https://streaming.nrjaudio.fm/oumvmk8fnozc",
        "http://live.dancemusic.ro:7000/radio.mp3",
        "https://icestreaming.rai.it/12.mp3",
        "https://str0.creacast.com/radio_vinci_autoroutes_7"
    };

    // Nomi di default (28/09: diventati modificabili dall'utente dalla
    // schermata MODIFICA LINK, "cosi mi ricordo cosa e'" - richiesto
    // esplicitamente - quindi non piu' costanti fisse ma un valore
    // iniziale, sovrascrivibile e persistito su NVS come i link).
    const char* DEFAULT_STATION_NAMES[STATION_COUNT] = {
        "DJ OSSO", "VIRGIN RADIO", "NRJ RADIO", "DEEP HOUSE RADIO", "ISORADIO", "VINCI 107.7"
    };
    constexpr uint16_t NAME_MAX_LEN = 20;

    Preferences radioPrefs;
    constexpr const char* RADIO_PREFS_NAMESPACE = "tdeckradio";
    bool nvsOpened = false;
    String stationUrls[STATION_COUNT];
    String stationNames[STATION_COUNT];

    void ensureNvsOpen() {
        if (nvsOpened) {
            return;
        }
        radioPrefs.begin(RADIO_PREFS_NAMESPACE, false);
        for (int i = 0; i < STATION_COUNT; i++) {
            String urlKey = "url" + String(i);
            stationUrls[i] = radioPrefs.getString(urlKey.c_str(), DEFAULT_STATION_URLS[i]);

            String nameKey = "name" + String(i);
            stationNames[i] = radioPrefs.getString(nameKey.c_str(), DEFAULT_STATION_NAMES[i]);
        }
        nvsOpened = true;
    }

    void saveStationUrl(int index, const String& url) {
        stationUrls[index] = url;
        radioPrefs.putString(("url" + String(index)).c_str(), url);
    }

    void saveStationName(int index, const String& name) {
        stationNames[index] = name;
        radioPrefs.putString(("name" + String(index)).c_str(), name);
    }

    // Puntatore, NON un oggetto persistente (29/09, causa reale di un bug:
    // "stop poi riavvio, non si sente piu' nulla") - Audio::stopSong()
    // NON rilascia mai il canale I2S (solo il suo DISTRUTTORE lo fa,
    // ~Audio() chiama i2s_channel_disable()+i2s_del_channel(), verificato
    // leggendo Audio.cpp), quindi con un oggetto Audio tenuto in vita per
    // sempre il canale restava agganciato anche dopo Stop - proprio
    // mentre Sound::resumeForOwnUse() cercava di riprendersi la STESSA
    // periferica fisica per i bip, andando in conflitto silenzioso
    // (nessun crash, l'hardware I2S restava semplicemente in uno stato
    // inconsistente). Ora l'oggetto Audio viene creato al Play e
    // DISTRUTTO allo Stop (vedi releaseAudio()), cosi' il canale I2S
    // viene davvero liberato ogni volta, esattamente come fa Sound con
    // pauseForExternalUse()/resumeForOwnUse().
    Audio* audio = nullptr;

    void ensureAudioCreated() {
        if (audio != nullptr) {
            return;
        }
        audio = new Audio();
        audio->setPinout(BOARD_I2S_BCLK, BOARD_I2S_WS, BOARD_I2S_DOUT);
    }

    void releaseAudio() {
        if (audio == nullptr) {
            return;
        }
        audio->stopSong();
        delete audio; // ~Audio() rilascia davvero il canale I2S
        audio = nullptr;
    }

    enum class Mode {
        Closed,
        Browse,
        EditList,
        StationDetail,
        EditField
    };
    Mode mode = Mode::Closed;

    // Vero quando MODIFICA LINK e' stato aperto direttamente da
    // Impostazioni (29/09, richiesto: "tenere tutto ordinato" - vedi
    // WebRadioUI::openEditLinks()), non passando dalla schermata
    // principale - in quel caso uscire (FATTO/Indietro) deve tornare a
    // Impostazioni, non alla schermata di riproduzione.
    bool openedFromSettings = false;

    enum class FieldKind {
        Name,
        Url
    };

    bool playing = false;
    int selectedStation = -1;
    int detailStation = -1;      // stazione aperta in StationDetail/EditField
    FieldKind editingField = FieldKind::Name;
    String editBuffer;

    // Angolo di rotazione delle bobine (radianti) - avanza solo mentre
    // playing==true, ridisegnato a ritmo moderato (vedi ANIM_INTERVAL_MS)
    // per un'animazione visibile senza ridisegnare ad ogni ciclo.
    float reelAngle = 0.0f;
    uint32_t lastAnimMs = 0;
    constexpr uint32_t ANIM_INTERVAL_MS = 150;
    constexpr float ANIM_STEP_RAD = 0.35f;

    constexpr int16_t CASSETTE_X = 60;
    constexpr int16_t CASSETTE_TOP = Display::DD_HEADER_H + 28;
    constexpr int16_t CASSETTE_W = 200;
    constexpr int16_t CASSETTE_H = 64;
    constexpr int16_t REEL_R = 20;
    constexpr int16_t REEL_CY = CASSETTE_TOP + CASSETTE_H / 2 - 4;
    constexpr int16_t REEL_L_CX = CASSETTE_X + 46;
    constexpr int16_t REEL_R_CX = CASSETTE_X + CASSETTE_W - 46;

    // Controllo volume (28/09, richiesto: "a lato della cassetta") - una
    // colonna stretta nel margine libero a destra della cassetta (che
    // occupa solo x=60..260, lasciando 260..320 vuoto), stessa altezza
    // della cassetta cosi' i due elementi appaiono allineati.
    constexpr int16_t VOL_X = 266;
    constexpr int16_t VOL_W = 48;
    constexpr int16_t VOL_BTN_H = 20;
    constexpr int16_t VOL_PLUS_Y = CASSETTE_TOP;
    constexpr int16_t VOL_LABEL_Y = CASSETTE_TOP + VOL_BTN_H + 4;
    constexpr int16_t VOL_MINUS_Y = CASSETTE_TOP + CASSETTE_H - VOL_BTN_H;
    constexpr int VOLUME_STEP = 10;

    constexpr int16_t CTRL_Y = CASSETTE_TOP + CASSETTE_H + 12;
    constexpr int16_t CTRL_H = 34;
    constexpr int16_t PLAY_X = 40;
    constexpr int16_t STOP_X = 170;
    constexpr int16_t CTRL_BTN_W = 110;

    // Griglia a due colonne per le stazioni (29/09: tolto il sesto
    // "tasto" per modificare i link - la configurazione ora si trova
    // solo in Impostazioni, vedi WebRadioUI::openEditLinks() - "cosi c'e'
    // piu' spazio per i canali"). Passate da 5 a 6 stazioni il 04/10
    // (richiesto esplicitamente: "cosi da avere un menu simmetrico") -
    // con GRID_CELLS pari la riga finale non resta piu' mai spaiata,
    // cellRect() sotto gestisce comunque entrambi i casi senza bisogno
    // di modifiche (il ramo "riga spaiata" semplicemente non si attiva
    // mai con un numero pari di stazioni).
    constexpr int16_t LIST_TOP = CTRL_Y + CTRL_H + 10;
    constexpr int GRID_COLS = 2;
    constexpr int GRID_CELLS = STATION_COUNT;
    constexpr int GRID_ROWS = (GRID_CELLS + GRID_COLS - 1) / GRID_COLS;
    constexpr int16_t GRID_LEFT = 6;
    constexpr int16_t GRID_GAP = 4;
    constexpr int16_t CELL_W = (320 - GRID_LEFT * 2 - GRID_GAP) / GRID_COLS;
    constexpr int16_t CELL_H = (240 - 4 - LIST_TOP - GRID_GAP * (GRID_ROWS - 1)) / GRID_ROWS;

    // 'w' esce piena larghezza riga SOLO se l'ultima stazione resta sola
    // nella sua riga (GRID_CELLS dispari) - con le 6 stazioni di oggi
    // questo ramo non si attiva mai, ma resta valido se in futuro
    // STATION_COUNT tornasse dispari.
    void cellRect(int index, int16_t& x, int16_t& y, int16_t& w) {
        int col = index % GRID_COLS;
        int row = index / GRID_COLS;
        x = GRID_LEFT + col * (CELL_W + GRID_GAP);
        y = LIST_TOP + row * (CELL_H + GRID_GAP);
        bool lastRowLoneCell = (row == GRID_ROWS - 1) && (GRID_CELLS % GRID_COLS != 0) && (col == 0);
        w = lastRowLoneCell ? static_cast<int16_t>(CELL_W * GRID_COLS + GRID_GAP * (GRID_COLS - 1)) : CELL_W;
    }

    int playZone = -1;
    int stopZone = -1;
    int volPlusZone = -1;
    int volMinusZone = -1;
    int stationZone[STATION_COUNT];

    // --- Schermate "modifica link" (lista -> dettaglio stazione -> campo) ---
    constexpr int16_t EL_X = 8;
    constexpr int16_t EL_W = 304;
    constexpr int16_t EL_ROW_TOP = Display::DD_HEADER_H + 30;
    // Ridotta da 30 a 26 (04/10, passate da 5 a 6 stazioni): con 6 righe
    // a 30px si sarebbe arrivati a 180px, sovrapponendosi al tasto
    // [ FATTO ] fisso a EL_BTN_Y=206 (48+180=228 > 206). A 26px le 6
    // righe restano entro 204px, appena sopra il tasto.
    constexpr int16_t EL_ROW_H = 26;
    constexpr int16_t EL_BTN_Y = 206;
    constexpr int16_t EL_BTN_H = 28;
    int elRowZone[STATION_COUNT];
    int elDoneZone = -1;
    // Riga a fuoco per la navigazione da trackball (29/09, audit
    // generale: qui prima solo il touch apriva una stazione).
    int selectedEditRow = 0;

    constexpr int16_t SD_ROW_TOP = Display::DD_HEADER_H + 30;
    constexpr int16_t SD_ROW_H = 40;
    int sdNameZone = -1;
    int sdUrlZone = -1;
    int sdBackZone = -1;
    // Riga a fuoco (NOME/LINK) per la navigazione da trackball, stesso
    // motivo di selectedEditRow sopra.
    int selectedDetailRow = 0;

    int efCancelZone = -1;
    constexpr int16_t EF_FIELD_Y = Display::DD_HEADER_H + 40;
    constexpr int16_t EF_FIELD_H = 28;
    constexpr uint16_t URL_MAX_LEN = 200;

    void showBrowseScreen();
    void showEditListScreen();
    void showStationDetailScreen(int station);
    void drawScreen();

    // Chiude SOLO la schermata (29/09, richiesto: "si puo lasciare in
    // esecuzione anche se esco dal menu?") - se sta suonando, resta a
    // suonare: audio->loop() continua a girare da WebRadioUI::
    // backgroundUpdate(), chiamata sempre da App.cpp indipendentemente
    // dalla schermata attiva. L'I2S resta quindi occupato dalla radio
    // (i bip di Sound restano muti, gia' cosi' anche prima mentre si
    // suonava con la schermata aperta) finche' non si preme Stop o lo
    // stream non si interrompe da solo.
    void closeWebRadio() {
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

    void drawButton(int16_t y, const char* label) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(EL_X + 1, y + 1, EL_W - 2, EL_BTN_H - 2, Display::DD_BG);
        gfx->drawRect(EL_X, y, EL_W, EL_BTN_H, Display::DD_ACCENT);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(EL_X + 12, y + EL_BTN_H / 2 - 3);
        gfx->print(label);
    }

    // Una bobina: cerchio esterno (corpo), mozzo centrale, e qualche
    // razza radiale che ruota di "angle" radianti - lo stesso disegno
    // per entrambe le bobine, solo centro diverso.
    void drawReel(int16_t cx, int16_t cy, float angle) {
        Arduino_GFX* gfx = Display::getGfx();

        gfx->fillCircle(cx, cy, REEL_R, Display::DD_BG);
        gfx->drawCircle(cx, cy, REEL_R, Display::DD_ACCENT_DIM);

        constexpr int SPOKES = 3;
        for (int i = 0; i < SPOKES; i++) {
            float a = angle + (i * 2.0f * PI / SPOKES);
            int16_t x1 = cx + static_cast<int16_t>(cosf(a) * 5);
            int16_t y1 = cy + static_cast<int16_t>(sinf(a) * 5);
            int16_t x2 = cx + static_cast<int16_t>(cosf(a) * (REEL_R - 4));
            int16_t y2 = cy + static_cast<int16_t>(sinf(a) * (REEL_R - 4));
            gfx->drawLine(x1, y1, x2, y2, Display::DD_ACCENT);
        }

        gfx->fillCircle(cx, cy, 5, Display::DD_ACCENT);
        gfx->fillCircle(cx, cy, 2, Display::DD_BG);
    }

    // Corpo della musicassetta + le due bobine + etichetta - ridisegnato
    // per intero ad ogni fotogramma dell'animazione (solo l'area della
    // cassetta, non tutto lo schermo).
    void drawCassette() {
        Arduino_GFX* gfx = Display::getGfx();

        gfx->fillRect(CASSETTE_X - 4, CASSETTE_TOP - 4, CASSETTE_W + 8, CASSETTE_H + 8, Display::DD_BG);

        gfx->drawRoundRect(CASSETTE_X, CASSETTE_TOP, CASSETTE_W, CASSETTE_H, 8, Display::DD_ACCENT);
        gfx->drawRoundRect(CASSETTE_X + 1, CASSETTE_TOP + 1, CASSETTE_W - 2, CASSETTE_H - 2, 7, Display::DD_ACCENT_DIM);

        // Vitine ai quattro angoli - solo dettaglio estetico.
        gfx->fillCircle(CASSETTE_X + 8, CASSETTE_TOP + 8, 2, Display::DD_ACCENT_DIM);
        gfx->fillCircle(CASSETTE_X + CASSETTE_W - 8, CASSETTE_TOP + 8, 2, Display::DD_ACCENT_DIM);
        gfx->fillCircle(CASSETTE_X + 8, CASSETTE_TOP + CASSETTE_H - 8, 2, Display::DD_ACCENT_DIM);
        gfx->fillCircle(CASSETTE_X + CASSETTE_W - 8, CASSETTE_TOP + CASSETTE_H - 8, 2, Display::DD_ACCENT_DIM);

        drawReel(REEL_L_CX, REEL_CY, reelAngle);
        drawReel(REEL_R_CX, REEL_CY, -reelAngle); // la bobina destra "avvolge", gira al contrario

        // Nastro visibile fra le due bobine (leggermente incurvato verso
        // il basso, due segmenti invece di una linea dritta).
        int16_t tapeY = REEL_CY + REEL_R - 6;
        gfx->drawLine(REEL_L_CX + REEL_R - 6, REEL_CY - 4, CASSETTE_X + CASSETTE_W / 2, tapeY, Display::DD_TEXT_DIM);
        gfx->drawLine(CASSETTE_X + CASSETTE_W / 2, tapeY, REEL_R_CX - REEL_R + 6, REEL_CY - 4, Display::DD_TEXT_DIM);

        // Etichetta centrale, sotto le bobine.
        int16_t labelW = 70;
        int16_t labelX = CASSETTE_X + CASSETTE_W / 2 - labelW / 2;
        int16_t labelY = CASSETTE_TOP + CASSETTE_H - 16;
        gfx->fillRect(labelX, labelY, labelW, 12, Display::DD_ACCENT_DIM);
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_BG);
        gfx->setCursor(labelX + 6, labelY + 2);
        gfx->print(playing ? "PLAY" : "STOP");
    }

    // Icone disegnate a mano (04/10, richiesto: "sostituire le scritte
    // start e stop con il triangolo di play e il quadrato di stop" -
    // stesso stile "solo primitive Arduino_GFX" gia' usato per la
    // musicassetta/bobine sopra, non un font/bitmap esterno) - triangolo
    // pieno rivolto a destra per Play, quadrato pieno per Stop, entrambi
    // centrati nel proprio tasto.
    void drawPlayIcon(int16_t cx, int16_t cy, uint16_t color) {
        Arduino_GFX* gfx = Display::getGfx();
        constexpr int16_t ICON_H = 20; // altezza totale (meta' sopra, meta' sotto cy)
        constexpr int16_t ICON_W = 18;
        gfx->fillTriangle(
            cx - ICON_W / 2, cy - ICON_H / 2,
            cx - ICON_W / 2, cy + ICON_H / 2,
            cx + ICON_W / 2, cy,
            color
        );
    }

    void drawStopIcon(int16_t cx, int16_t cy, uint16_t color) {
        Arduino_GFX* gfx = Display::getGfx();
        constexpr int16_t ICON_SIZE = 18;
        gfx->fillRect(cx - ICON_SIZE / 2, cy - ICON_SIZE / 2, ICON_SIZE, ICON_SIZE, color);
    }

    void drawControls() {
        Arduino_GFX* gfx = Display::getGfx();

        gfx->fillRect(PLAY_X, CTRL_Y, CTRL_BTN_W, CTRL_H, Display::DD_BG);
        gfx->drawRect(PLAY_X, CTRL_Y, CTRL_BTN_W, CTRL_H, playing ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        drawPlayIcon(PLAY_X + CTRL_BTN_W / 2, CTRL_Y + CTRL_H / 2, playing ? Display::DD_ACCENT : Display::DD_TEXT_DIM);

        gfx->fillRect(STOP_X, CTRL_Y, CTRL_BTN_W, CTRL_H, Display::DD_BG);
        gfx->drawRect(STOP_X, CTRL_Y, CTRL_BTN_W, CTRL_H, !playing ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        drawStopIcon(STOP_X + CTRL_BTN_W / 2, CTRL_Y + CTRL_H / 2, !playing ? Display::DD_ACCENT : Display::DD_TEXT_DIM);

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
    }

    void drawVolumeControl() {
        Arduino_GFX* gfx = Display::getGfx();
        int percent = Sound::getVolumePercent();

        gfx->fillRect(VOL_X, VOL_PLUS_Y, VOL_W, CASSETTE_H, Display::DD_BG);

        gfx->drawRect(VOL_X, VOL_PLUS_Y, VOL_W, VOL_BTN_H, Display::DD_ACCENT_DIM);
        gfx->setFont(nullptr);
        gfx->setTextSize(2);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(VOL_X + VOL_W / 2 - 6, VOL_PLUS_Y + VOL_BTN_H / 2 - 8);
        gfx->print("+");

        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_TEXT);
        char label[8];
        snprintf(label, sizeof(label), "%d%%", percent);
        int16_t textW = static_cast<int16_t>(strlen(label)) * 6;
        gfx->setCursor(VOL_X + VOL_W / 2 - textW / 2, VOL_LABEL_Y);
        gfx->print(label);

        gfx->drawRect(VOL_X, VOL_MINUS_Y, VOL_W, VOL_BTN_H, Display::DD_ACCENT_DIM);
        gfx->setTextSize(2);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(VOL_X + VOL_W / 2 - 6, VOL_MINUS_Y + VOL_BTN_H / 2 - 8);
        gfx->print("-");

        gfx->setTextSize(1);
    }

    // Cambia il volume globale (lo stesso di Impostazioni/bip - un solo
    // concetto di volume in tutto il firmware) e, se si sta gia'
    // riproducendo, lo applica SUBITO allo stream in corso invece di
    // aspettare un nuovo Play.
    void adjustVolume(int delta) {
        int current = Sound::getVolumePercent();
        int next = current + delta;
        if (next < 0) next = 0;
        if (next > 100) next = 100;
        if (next == current) {
            return;
        }
        Sound::setVolumePercent(next);
        if (playing && audio != nullptr) {
            audio->setVolume(map(next, 0, 100, 0, 21));
        }
        drawVolumeControl();
    }

    void drawStationRow(int index) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t x, y, w;
        cellRect(index, x, y, w);
        bool selected = (index == selectedStation);

        gfx->fillRect(x, y, w, CELL_H, Display::DD_BG);
        gfx->drawRect(x, y, w, CELL_H, selected ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);

        if (selected) {
            gfx->fillRect(x + 3, y + 4, 4, CELL_H - 8, Display::DD_ACCENT);
        }

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(selected ? Display::DD_ACCENT : Display::DD_TEXT);
        gfx->setCursor(x + 14, y + CELL_H / 2 - 3);
        gfx->print(stationNames[index]);
    }

    void drawScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("WEB RADIO");

        drawCassette();
        drawVolumeControl();
        drawControls();

        for (int i = 0; i < STATION_COUNT; i++) {
            drawStationRow(i);
        }

        TouchUI::clearZones();
        playZone = TouchUI::addZone(PLAY_X, CTRL_Y, CTRL_BTN_W, CTRL_H);
        stopZone = TouchUI::addZone(STOP_X, CTRL_Y, CTRL_BTN_W, CTRL_H);
        volPlusZone = TouchUI::addZone(VOL_X, VOL_PLUS_Y, VOL_W, VOL_BTN_H);
        volMinusZone = TouchUI::addZone(VOL_X, VOL_MINUS_Y, VOL_W, VOL_BTN_H);
        for (int i = 0; i < STATION_COUNT; i++) {
            int16_t x, y, w;
            cellRect(i, x, y, w);
            stationZone[i] = TouchUI::addZone(x, y, w, CELL_H);
        }
    }

    // Avvia lo streaming reale sulla stazione selezionata. Ritorna false
    // (e mostra un popup d'errore, senza cambiare stato) se manca la
    // selezione, il link non e' impostato, non c'e' WiFi, o la
    // connessione allo stream fallisce subito.
    bool startPlayback() {
        if (selectedStation < 0) {
            Display::showMessage("Web Radio", "Seleziona prima una stazione.", []() { drawScreen(); });
            return false;
        }

        String url = stationUrls[selectedStation];
        if (url.length() == 0) {
            Display::showMessage(
                "Web Radio",
                "Nessun link impostato per questa stazione.\nAggiungilo da Impostazioni > WEB RADIO.",
                []() { drawScreen(); }
            );
            return false;
        }

        if (!ConnStatus::isWifiConnected()) {
            Display::showMessage("Web Radio", "Devi prima connetterti a una rete WiFi.", []() { drawScreen(); });
            return false;
        }

        // ORDINE CRITICO (causa di un crash reale, 28/09): Sound DEVE
        // rilasciare l'I2S (pauseForExternalUse) PRIMA che Audio provi
        // ad agganciarselo (ensureAudioCreated -> setPinout ->
        // i2s_new_channel). Nell'ordine sbagliato i2s_new_channel()
        // fallisce perche' la porta e' ancora occupata da Sound, e un
        // bug della libreria ESP32-audioI2S non controlla l'esito prima
        // di chiamare ESP_ERROR_CHECK() sull'handle non valido -> abort()
        // immediato. Riprodotto e confermato via addr2line sul backtrace
        // del coredump (CrashLog): Audio::setPinout ->
        // i2s_channel_register_event_callback -> ESP_ERROR_CHECK -> abort.
        Sound::pauseForExternalUse();
        ensureAudioCreated();

        int volPercent = Sound::getVolumePercent();
        audio->setVolume(map(volPercent, 0, 100, 0, 21));

        bool ok = audio->connecttohost(url.c_str());
        if (!ok) {
            releaseAudio();
            Sound::resumeForOwnUse();
            Display::showMessage("Web Radio", "Impossibile connettersi allo stream.", []() { drawScreen(); });
            return false;
        }

        return true;
    }

    void setPlaying(bool p) {
        if (playing == p) {
            return;
        }

        if (p) {
            if (!startPlayback()) {
                return; // resta in stop, drawScreen() gia' richiamato dal popup
            }
        } else {
            releaseAudio();
            Sound::resumeForOwnUse();
        }

        playing = p;
        drawCassette();
        drawControls();
    }

    void selectStation(int index) {
        int previous = selectedStation;
        // Stop automatico (04/10, bug segnalato dall'utente: "quando
        // passo da una radio all'altra senza fare stop, poi non parte
        // piu' la radio successiva") - setPlaying(true) e' un no-op se
        // playing e' GIA' vero (vedi sopra, evita di ricreare l'oggetto
        // Audio ad ogni tap sulla stessa stazione), quindi selezionando
        // una stazione DIVERSA mentre una e' ancora in play, un
        // successivo tap su PLAY non faceva ripartire nulla - restava
        // "in play" sulla stazione vecchia. Fermando qui la riproduzione
        // in corso, il prossimo PLAY riparte davvero sulla nuova
        // selezione.
        if (playing && index != previous) {
            setPlaying(false);
        }
        selectedStation = index;
        if (previous >= 0) {
            drawStationRow(previous);
        }
        drawStationRow(index);
    }

    // --- Schermata elenco (lista delle 5 stazioni) ---

    void drawEditListRow(int index) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t y = EL_ROW_TOP + index * EL_ROW_H;
        bool selected = (index == selectedEditRow);

        gfx->fillRect(EL_X, y, EL_W, EL_ROW_H - 4, Display::DD_BG);
        gfx->drawRect(EL_X, y, EL_W, EL_ROW_H - 4, selected ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        if (selected) {
            gfx->fillRect(EL_X + 2, y + 3, 3, EL_ROW_H - 10, Display::DD_ACCENT);
        }

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(EL_X + 10, y + 2);
        gfx->print(stationNames[index]);

        String value = stationUrls[index];
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(EL_X + 10, y + 14);
        gfx->print(value.length() > 0 ? value : "(link vuoto)");
    }

    void drawEditListScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("MODIFICA LINK");

        TouchUI::clearZones();
        for (int i = 0; i < STATION_COUNT; i++) {
            drawEditListRow(i);
            int16_t y = EL_ROW_TOP + i * EL_ROW_H;
            elRowZone[i] = TouchUI::addZone(EL_X, y, EL_W, EL_ROW_H - 4);
        }

        drawButton(EL_BTN_Y, "[ FATTO ]");
        elDoneZone = TouchUI::addZone(EL_X, EL_BTN_Y, EL_W, EL_BTN_H);
    }

    void showEditListScreen() {
        mode = Mode::EditList;
        selectedEditRow = 0;
        Input::setTextEntryMode(false);
        drawEditListScreen();
    }

    // Esce da MODIFICA LINK verso il posto giusto: Impostazioni se e'
    // stato aperto da li', altrimenti la schermata di riproduzione
    // (comportamento di sempre).
    void exitEditListScreen() {
        if (openedFromSettings) {
            openedFromSettings = false;
            mode = Mode::Closed;
            Menu::back();
        } else {
            showBrowseScreen();
        }
    }

    // Sposta la riga a fuoco (Su/Giu' da trackball) - ridisegna solo le
    // due righe coinvolte, stesso schema gia' usato per selectStation().
    void selectEditRow(int index) {
        int previous = selectedEditRow;
        selectedEditRow = index;
        if (previous != index) {
            drawEditListRow(previous);
        }
        drawEditListRow(index);
    }

    // --- Schermata dettaglio (nome + link di UNA stazione) ---

    void drawStationDetailRow(FieldKind kind) {
        Arduino_GFX* gfx = Display::getGfx();
        int rowIndex = (kind == FieldKind::Name) ? 0 : 1;
        int16_t y = SD_ROW_TOP + rowIndex * SD_ROW_H;
        bool selected = (rowIndex == selectedDetailRow);

        gfx->fillRect(EL_X, y, EL_W, SD_ROW_H - 4, Display::DD_BG);
        gfx->drawRect(EL_X, y, EL_W, SD_ROW_H - 4, selected ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        if (selected) {
            gfx->fillRect(EL_X + 2, y + 4, 3, SD_ROW_H - 12, Display::DD_ACCENT);
        }

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(EL_X + 12, y + 4);
        gfx->print(kind == FieldKind::Name ? "NOME" : "LINK");

        String value = (kind == FieldKind::Name) ? stationNames[detailStation] : stationUrls[detailStation];
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(EL_X + 12, y + 16);
        gfx->print(value.length() > 0 ? value : "(non impostato)");
    }

    void drawStationDetailScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("DETTAGLI STAZIONE");

        TouchUI::clearZones();

        drawStationDetailRow(FieldKind::Name);
        int16_t nameY = SD_ROW_TOP;
        sdNameZone = TouchUI::addZone(EL_X, nameY, EL_W, SD_ROW_H - 4);

        drawStationDetailRow(FieldKind::Url);
        int16_t urlY = SD_ROW_TOP + SD_ROW_H;
        sdUrlZone = TouchUI::addZone(EL_X, urlY, EL_W, SD_ROW_H - 4);

        drawButton(EL_BTN_Y, "[ INDIETRO ]");
        sdBackZone = TouchUI::addZone(EL_X, EL_BTN_Y, EL_W, EL_BTN_H);
    }

    void showStationDetailScreen(int station) {
        mode = Mode::StationDetail;
        detailStation = station;
        selectedDetailRow = 0;
        Input::setTextEntryMode(false);
        drawStationDetailScreen();
    }

    // Sposta la riga a fuoco (Su/Giu' da trackball) fra NOME (0) e
    // LINK (1) - ridisegna solo le due righe coinvolte.
    void selectDetailRow(int index) {
        if (selectedDetailRow == index) {
            return;
        }
        selectedDetailRow = index;
        drawStationDetailRow(FieldKind::Name);
        drawStationDetailRow(FieldKind::Url);
    }

    // --- Schermata campo singolo (nome O link, riusata per entrambi) ---

    void drawEditFieldScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print(editingField == FieldKind::Name ? "NOME STAZIONE" : "LINK STREAM");

        TouchUI::clearZones();

        TextInputUI::draw(EL_X, EF_FIELD_Y, EL_W, EF_FIELD_H, editBuffer, false);

        Display::drawWrappedText(
            EL_X, EF_FIELD_Y + EF_FIELD_H + 10, EL_W,
            editingField == FieldKind::Name
                ? "Scrivi un nome per riconoscere questa stazione, poi Invio per confermare."
                : "Scrivi il link dello stream (es. https://host/stream.mp3) con la tastiera fisica, poi Invio per confermare.",
            Display::DD_TEXT_DIM
        );

        drawButton(200, "[ ANNULLA ]");
        efCancelZone = TouchUI::addZone(EL_X, 200, EL_W, EL_BTN_H);
    }

    void showEditFieldScreen(int station, FieldKind kind) {
        mode = Mode::EditField;
        detailStation = station;
        editingField = kind;
        editBuffer = (kind == FieldKind::Name) ? stationNames[station] : stationUrls[station];
        Input::setTextEntryMode(true);
        drawEditFieldScreen();
    }

    void confirmEditField() {
        String trimmed = editBuffer;
        trimmed.trim();

        if (editingField == FieldKind::Name) {
            // Un nome vuoto non viene salvato (resta quello precedente):
            // altrimenti la cella in griglia/lista resterebbe senza
            // etichetta leggibile.
            if (trimmed.length() > 0) {
                saveStationName(detailStation, trimmed);
            }
        } else {
            saveStationUrl(detailStation, trimmed);
        }

        showStationDetailScreen(detailStation);
    }

    void showBrowseScreen() {
        mode = Mode::Browse;
        Input::setTextEntryMode(false);
        drawScreen();
    }

    void handleBrowseInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == playZone) {
            setPlaying(true);
            return;
        }
        if (tapped == stopZone) {
            setPlaying(false);
            return;
        }
        if (tapped == volPlusZone) {
            adjustVolume(VOLUME_STEP);
            return;
        }
        if (tapped == volMinusZone) {
            adjustVolume(-VOLUME_STEP);
            return;
        }
        if (tapped >= 0) {
            for (int i = 0; i < STATION_COUNT; i++) {
                if (tapped == stationZone[i]) {
                    selectStation(i);
                    return;
                }
            }
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Back:
                closeWebRadio();
                break;

            case InputEvent::Select:
                setPlaying(!playing);
                break;

            // Navigazione a griglia (2 colonne, come il menu principale):
            // Su/Giu' si spostano di una RIGA (indice +-2), Sinistra/
            // Destra di una colonna (indice +-1) senza cambiare riga.
            // Limitata alle 5 stazioni (non raggiunge il volume,
            // riservato al touch).
            case InputEvent::Up:
                if (selectedStation < 0) {
                    selectStation(0);
                } else if (selectedStation - GRID_COLS >= 0) {
                    selectStation(selectedStation - GRID_COLS);
                }
                break;

            case InputEvent::Down:
                if (selectedStation < 0) {
                    selectStation(0);
                } else if (selectedStation + GRID_COLS < STATION_COUNT) {
                    selectStation(selectedStation + GRID_COLS);
                }
                break;

            case InputEvent::Left:
                if (selectedStation < 0) {
                    selectStation(0);
                } else if (selectedStation % GRID_COLS > 0) {
                    selectStation(selectedStation - 1);
                }
                break;

            case InputEvent::Right:
                if (selectedStation < 0) {
                    selectStation(0);
                } else if (selectedStation % GRID_COLS < GRID_COLS - 1 && selectedStation + 1 < STATION_COUNT) {
                    selectStation(selectedStation + 1);
                }
                break;

            default:
                break;
        }
    }

    void handleEditListInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == elDoneZone) {
            exitEditListScreen();
            return;
        }
        for (int i = 0; i < STATION_COUNT; i++) {
            if (tapped == elRowZone[i]) {
                showStationDetailScreen(i);
                return;
            }
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                if (selectedEditRow > 0) {
                    selectEditRow(selectedEditRow - 1);
                }
                break;

            case InputEvent::Down:
                if (selectedEditRow < STATION_COUNT - 1) {
                    selectEditRow(selectedEditRow + 1);
                }
                break;

            case InputEvent::Select:
                showStationDetailScreen(selectedEditRow);
                break;

            case InputEvent::Back:
                exitEditListScreen();
                break;

            default:
                break;
        }
    }

    void handleStationDetailInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == sdBackZone) {
            showEditListScreen();
            return;
        }
        if (tapped == sdNameZone) {
            selectDetailRow(0);
            showEditFieldScreen(detailStation, FieldKind::Name);
            return;
        }
        if (tapped == sdUrlZone) {
            selectDetailRow(1);
            showEditFieldScreen(detailStation, FieldKind::Url);
            return;
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                selectDetailRow(0);
                break;

            case InputEvent::Down:
                selectDetailRow(1);
                break;

            case InputEvent::Select:
                showEditFieldScreen(detailStation, selectedDetailRow == 0 ? FieldKind::Name : FieldKind::Url);
                break;

            case InputEvent::Back:
                showEditListScreen();
                break;

            default:
                break;
        }
    }

    void handleEditFieldInput() {
        int tapped = TouchUI::pollTap();
        if (tapped == efCancelZone) {
            showStationDetailScreen(detailStation);
            return;
        }

        uint16_t maxLen = (editingField == FieldKind::Name) ? NAME_MAX_LEN : URL_MAX_LEN;
        TextInputUI::Result result = TextInputUI::handleKey(editBuffer, maxLen);
        if (result == TextInputUI::Result::Changed) {
            TextInputUI::draw(EL_X, EF_FIELD_Y, EL_W, EF_FIELD_H, editBuffer, false);
        } else if (result == TextInputUI::Result::Confirmed) {
            confirmEditField();
        }
    }

} // namespace

namespace WebRadioUI {

    void open() {
        ensureNvsOpen();
        // Se sta gia' suonando (riaperta mentre la radio va in
        // background, vedi backgroundUpdate()) NON si resetta lo stato:
        // la stazione selezionata resta quella in riproduzione. Solo se
        // non sta suonando si riparte da zero (nessuna selezione).
        if (!playing) {
            selectedStation = -1;
        }
        reelAngle = 0.0f;
        lastAnimMs = millis();
        Display::setDirectDrawMode(true);
        showBrowseScreen();
    }

    // Apre MODIFICA LINK direttamente, senza passare dalla schermata di
    // riproduzione (29/09, richiesto: raggiungibile anche da
    // Impostazioni) - non tocca lo stato di riproduzione in corso, se
    // la radio sta suonando in background continua a suonare.
    void openEditLinks() {
        ensureNvsOpen();
        openedFromSettings = true;
        Display::setDirectDrawMode(true);
        showEditListScreen();
    }

    bool isActive() {
        return mode != Mode::Closed;
    }

    // Il "motore" dello streaming (29/09, richiesto: continuare a
    // sentire la radio uscendo dal menu) - SEMPRE chiamata da App.cpp,
    // non solo mentre questa schermata e' quella attiva, stesso
    // principio di GPS::update()/LoRaRadio::update(). L'animazione della
    // cassetta invece resta SOLO in update() (sotto), che gira solo
    // quando la schermata e' davvero a video: ridisegnare mentre e'
    // nascosta sarebbe inutile e scriverebbe sopra qualunque altra
    // schermata stia disegnando in quel momento.
    void backgroundUpdate() {
        if (!playing || audio == nullptr) {
            return;
        }
        audio->loop();
        if (!audio->isRunning()) {
            // Lo stream si e' interrotto da solo (errore di rete, fine
            // dello stream, ecc.) - riallinea lo stato locale al reale
            // stato del backend audio. Il ridisegno della cassetta/
            // controlli avviene solo se la schermata e' davvero aperta
            // in questo momento.
            releaseAudio();
            Sound::resumeForOwnUse();
            playing = false;
            if (mode == Mode::Browse) {
                drawCassette();
                drawControls();
            }
        }
    }

    void update() {
        if (mode == Mode::Closed) {
            return;
        }

        if (mode == Mode::Browse && playing && millis() - lastAnimMs >= ANIM_INTERVAL_MS) {
            lastAnimMs = millis();
            reelAngle += ANIM_STEP_RAD;
            if (reelAngle > 2.0f * PI) {
                reelAngle -= 2.0f * PI;
            }
            drawCassette();
        }

        switch (mode) {
            case Mode::Browse:
                handleBrowseInput();
                break;
            case Mode::EditList:
                handleEditListInput();
                break;
            case Mode::StationDetail:
                handleStationDetailInput();
                break;
            case Mode::EditField:
                handleEditFieldInput();
                break;
            case Mode::Closed:
                break;
        }
    }
}
