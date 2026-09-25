#include "Display.h"
#include "Board.h"
#include "Config.h"
#include "GPS.h"
#include "ConnStatus.h"
#include "CrashLog.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <Arduino_GFX_Library.h>

// lv_refr_now() e' dichiarato in core/lv_refr.h, incluso transitivamente
// da <lvgl.h> (vedi src/core/lv_refr.h), quindi e' gia' visibile qui
// senza bisogno di altri include.

namespace {

Arduino_DataBus* bus = new Arduino_ESP32SPI(
    BOARD_TFT_DC,
    BOARD_TFT_CS,
    BOARD_SPI_SCK,
    BOARD_SPI_MOSI,
    BOARD_SPI_MISO,
    HSPI
);

Arduino_GFX* gfx = new Arduino_ST7789(
    bus,
    -1,
    0,
    true,
    240,
    320
);

// Buffer a schermo intero in PSRAM (vedi motivazione sotto, render mode FULL).
constexpr uint32_t LVGL_BUF_LINES = DISPLAY_HEIGHT;

lv_color_t* buf1 = nullptr;
lv_color_t* buf2 = nullptr;
lv_display_t* lvDisplay = nullptr;
bool buffersOk = false;

// Ultimo screen "usa e getta" creato da showMessage(). Non lo cancelliamo
// subito dopo averlo sostituito: la prima volta che si apre un messaggio,
// lo screen attivo e' mainScreen di Menu.cpp, creato una volta sola e
// riusato per sempre da Menu::back(). Cancellarlo per errore lascerebbe
// quel puntatore "dangling" in Menu.cpp, e il successivo
// lv_scr_load(mainScreen) toccherebbe memoria gia' liberata. Teniamo
// quindi traccia solo dell'ultimo screen di messaggio (mai mainScreen) e
// lo liberiamo alla chiamata successiva, quando e' sicuramente inattivo.
lv_obj_t* activeMessageScreen = nullptr;

// Barra di stato persistente: creata una sola volta su lv_layer_top(),
// il layer speciale di LVGL che resta sempre visibile sopra qualunque
// screen attivo e non viene mai toccato da lv_scr_load()/lv_scr_load_anim().
// Cosi' data/ora e batteria restano in vista sia sul menu principale sia
// sulle schermate di dettaglio, senza doverle ricreare per ognuna.
lv_obj_t* headerDateTimeLabel = nullptr;
lv_obj_t* headerRamBar = nullptr;
lv_obj_t* headerVpnIconLabel = nullptr;
lv_obj_t* headerWifiIconLabel = nullptr;
lv_obj_t* headerBluetoothIconLabel = nullptr;
lv_obj_t* headerLoraIconLabel = nullptr;
lv_obj_t* headerNetGuardIconLabel = nullptr;
lv_obj_t* headerBatteryLabel = nullptr;

// Data/ora e batteria si aggiornano solo una volta al minuto: non serve
// una precisione al secondo per nessuna delle due, e un refresh cosi'
// meno frequente evita anche eventuali "balletti" della percentuale
// batteria dovuti al rumore della singola lettura ADC (comunque
// ulteriormente attenuato mediando piu' campioni, vedi Board::getBatteryPercent()).
constexpr uint32_t HEADER_UPDATE_INTERVAL_MS = 60000;

const char* batterySymbolFor(int percent) {
    if (percent >= 90) return LV_SYMBOL_BATTERY_FULL;
    if (percent >= 65) return LV_SYMBOL_BATTERY_3;
    if (percent >= 40) return LV_SYMBOL_BATTERY_2;
    if (percent >= 15) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

// Colore di un'icona di stato in base a connesso/non connesso: acceso e
// ben visibile se connesso, spento/grigio se no (oltre alla riga barrata
// impostata una sola volta alla creazione della label).
lv_color_t statusIconColor(bool connected) {
    return connected ? Display::COLOR_ACCENT : lv_color_hex(0x555555);
}

void updateHeader() {
    // Wifi/VPN vanno controllati OGNI ciclo, non ogni minuto: l'utente
    // deve vedere l'icona cambiare appena si connette/disconnette (es.
    // col bottone "Disconnetti" della schermata WiFi), non aspettare
    // fino a un minuto dopo.
    if (headerWifiIconLabel != nullptr) {
        bool connected = ConnStatus::isWifiConnected();
        lv_obj_set_style_text_color(headerWifiIconLabel, statusIconColor(connected), 0);
        lv_obj_set_style_text_decor(
            headerWifiIconLabel,
            connected ? LV_TEXT_DECOR_NONE : LV_TEXT_DECOR_STRIKETHROUGH,
            0
        );
    }

    if (headerVpnIconLabel != nullptr) {
        bool connected = ConnStatus::isVpnConnected();
        lv_obj_set_style_text_color(headerVpnIconLabel, statusIconColor(connected), 0);
        lv_obj_set_style_text_decor(
            headerVpnIconLabel,
            connected ? LV_TEXT_DECOR_NONE : LV_TEXT_DECOR_STRIKETHROUGH,
            0
        );
    }

    if (headerBluetoothIconLabel != nullptr) {
        bool active = ConnStatus::isBluetoothActive();
        lv_obj_set_style_text_color(headerBluetoothIconLabel, statusIconColor(active), 0);
        lv_obj_set_style_text_decor(
            headerBluetoothIconLabel,
            active ? LV_TEXT_DECOR_NONE : LV_TEXT_DECOR_STRIKETHROUGH,
            0
        );
    }

    if (headerLoraIconLabel != nullptr) {
        bool active = ConnStatus::isLoraActive();
        bool transmitting = ConnStatus::isLoraTransmitting();
        bool unread = ConnStatus::hasUnreadLoraMessages();

        // Priorita': arancione mentre trasmette (risponde a "come faccio
        // a sapere se sto trasmettendo?"), altrimenti giallo se c'e' un
        // messaggio ricevuto non ancora letto (torna al colore normale
        // aprendo la chat), altrimenti il normale attivo/disattivo.
        lv_color_t color;
        if (transmitting) {
            color = lv_color_hex(0xE67E22);
        } else if (unread) {
            color = lv_color_hex(0xF1C40F);
        } else {
            color = statusIconColor(active);
        }

        lv_obj_set_style_text_color(headerLoraIconLabel, color, 0);
        lv_obj_set_style_text_decor(
            headerLoraIconLabel,
            active ? LV_TEXT_DECOR_NONE : LV_TEXT_DECOR_STRIKETHROUGH,
            0
        );
    }

    if (headerNetGuardIconLabel != nullptr) {
        bool active = ConnStatus::isNetGuardActive();
        bool waitingForWhitelist = ConnStatus::isNetGuardWaitingForWhitelist();
        bool scanningNow = ConnStatus::isNetGuardScanning();
        bool pendingAlert = ConnStatus::hasNetGuardPendingAlert();

        // Priorita': rosso se c'e' un allarme non ancora chiuso (vince su
        // tutto: e' l'informazione piu' urgente), poi arancione se e'
        // acceso ma non ha ancora nessuna whitelist da controllare (serve
        // un NetScan) o se sta facendo un giro di scansione periodico,
        // poi il normale attivo/disattivo.
        lv_color_t color;
        if (pendingAlert) {
            color = lv_color_hex(0xE74C3C);
        } else if (waitingForWhitelist || scanningNow) {
            color = lv_color_hex(0xE67E22);
        } else {
            color = statusIconColor(active);
        }

        lv_obj_set_style_text_color(headerNetGuardIconLabel, color, 0);
        lv_obj_set_style_text_decor(
            headerNetGuardIconLabel,
            active ? LV_TEXT_DECOR_NONE : LV_TEXT_DECOR_STRIKETHROUGH,
            0
        );
    }

    static uint32_t lastUpdateMs = 0;
    static bool hadGpsTimeLastCheck = false;

    uint32_t now = millis();
    bool hasGpsTimeNow = (GPS::getTimeLocal().length() > 0);

    // Eccezione al refresh ogni 60s: il PRIMO momento in cui il GPS passa
    // da "nessun orario" a "orario valido" va mostrato subito, non fino
    // a un minuto dopo - altrimenti la scritta "In attesa GPS..." resta
    // in schermo anche se internamente il GPS ha gia' agganciato l'ora.
    bool forceUpdate = (hasGpsTimeNow && !hadGpsTimeLastCheck);
    hadGpsTimeLastCheck = hasGpsTimeNow;

    if (!forceUpdate && now - lastUpdateMs < HEADER_UPDATE_INTERVAL_MS && lastUpdateMs != 0) {
        return;
    }
    lastUpdateMs = now;

    // Rettangolino verticale al posto della percentuale scritta: stesso
    // dato (RAM libera) ma occupa una frazione dello spazio nella barra
    // in alto, gia' condivisa con parecchie altre icone - il colore (non
    // una barra proporzionale, giusto per restare minimale) basta a dare
    // un'occhiata veloce senza dover leggere un numero.
    if (headerRamBar != nullptr) {
        uint32_t heapTotal = ESP.getHeapSize();
        int ramFreePercent = (heapTotal > 0)
            ? static_cast<int>((ESP.getFreeHeap() * 100UL) / heapTotal)
            : 0;

        lv_color_t ramColor;
        if (ramFreePercent >= 50) {
            ramColor = lv_color_hex(0x2ECC71);      // verde: memoria abbondante
        } else if (ramFreePercent >= 20) {
            ramColor = lv_color_hex(0xF1C40F);      // giallo: attenzione
        } else {
            ramColor = lv_color_hex(0xE74C3C);      // rosso: memoria in esaurimento
        }
        lv_obj_set_style_bg_color(headerRamBar, ramColor, 0);
    }

    if (headerDateTimeLabel != nullptr) {
        String time = GPS::getTimeLocal();

        // "..." mentre si aspetta il fix GPS, invece del testo lungo: non
        // si sovrappone alle icone di stato a destra.
        String text = (time.length() > 0) ? time : "...";

        lv_label_set_text(headerDateTimeLabel, text.c_str());
    }

    if (headerBatteryLabel != nullptr) {
        int percent = Board::getBatteryPercent();

        char text[16];
        snprintf(
            text, sizeof(text),
            "%d%% %s",
            percent, batterySymbolFor(percent)
        );

        lv_label_set_text(headerBatteryLabel, text);
    }

}


// Sfondo decorativo "piste di circuito stampato": poche linee sottili
// azzurro scuro su nero, con qualche "via" (pallino) nei punti di
// giunzione. Disegnato come primo figlio di ogni screen (quindi dietro
// a tutto il resto, vedi createScreen()): dato che ogni bottone/riga
// dell'interfaccia ha gia' un proprio sfondo opaco (vedi Menu.cpp,
// SettingsUI.cpp, ecc.), queste linee restano visibili solo negli spazi
// vuoti fra un widget e l'altro e non riducono la leggibilita' di nulla.
// Deliberatamente poche linee sparse ("qualche linea", non un reticolo
// fitto) e colore molto scuro (quasi nero, ben distinto dall'azzurro
// acceso COLOR_ACCENT usato per testo/bordi attivi).
// Coordinate fisse sui 320x240 dello schermo (in landscape, vedi
// gfx->setRotation(1) in Display::begin()): ogni spezzata e' una pista
// a "gradini" tipica di un circuito stampato, concentrata verso i bordi
// per non entrare in rotta di collisione col testo, che nelle schermate
// di messaggio occupa tipicamente la zona centrale.
const lv_point_precise_t TRACE_1[] = { {8,4},{8,34},{30,34} };
const lv_point_precise_t TRACE_2[] = { {312,8},{284,8},{284,48},{316,48} };
const lv_point_precise_t TRACE_3[] = { {4,150},{24,150},{24,206} };
const lv_point_precise_t TRACE_4[] = { {316,150},{296,150},{296,112},{270,112} };
const lv_point_precise_t TRACE_5[] = { {150,236},{150,222} };
const lv_point_precise_t TRACE_6[] = { {60,236},{60,214},{92,214} };
const lv_point_precise_t TRACE_7[] = { {232,236},{232,216},{212,216},{212,198} };

struct TraceLine {
    const lv_point_precise_t* points;
    uint32_t count;
};

const TraceLine TRACES[] = {
    { TRACE_1, sizeof(TRACE_1) / sizeof(TRACE_1[0]) },
    { TRACE_2, sizeof(TRACE_2) / sizeof(TRACE_2[0]) },
    { TRACE_3, sizeof(TRACE_3) / sizeof(TRACE_3[0]) },
    { TRACE_4, sizeof(TRACE_4) / sizeof(TRACE_4[0]) },
    { TRACE_5, sizeof(TRACE_5) / sizeof(TRACE_5[0]) },
    { TRACE_6, sizeof(TRACE_6) / sizeof(TRACE_6[0]) },
    { TRACE_7, sizeof(TRACE_7) / sizeof(TRACE_7[0]) },
};

const lv_point_precise_t VIA_DOTS[] = {
    {8,34}, {284,48}, {24,206}, {296,112}, {92,214}, {212,198}
};

// Disegnato direttamente su LV_EVENT_DRAW_MAIN dello screen stesso,
// invece di creare 7 lv_line_create() + 6 lv_obj_create() come oggetti
// LVGL veri e propri: stesso identico risultato visivo (e stesso ordine,
// "dietro" a tutto il resto, perche' il disegno del padre avviene prima
// di quello dei figli), ma zero allocazioni nel pool LVGL a ogni singolo
// cambio schermata. Con 13 oggetti in piu' creati E distrutti a ogni
// apertura di QUALSIASI schermata (menu, sottomenu, messaggi...), per
// qualcosa di puramente decorativo e identico ogni volta, questo era
// una fonte diretta di frammentazione nel pool fisso da 64KB durante
// sessioni di navigazione intensa (vedi project_lvgl_pool_exhaustion_fix
// in memoria) - convertito il 2026-09-24 dopo aver osservato nuovi
// "pool memoria esaurito" con frammentazione al 24-25% in uso reale.
void circuitBackgroundDrawCb(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);

    lv_draw_line_dsc_t lineDsc;
    lv_draw_line_dsc_init(&lineDsc);
    lineDsc.color = lv_color_hex(0x0F3348);
    lineDsc.width = 1;

    for (const TraceLine& trace : TRACES) {
        lineDsc.points = const_cast<lv_point_precise_t*>(trace.points);
        lineDsc.point_cnt = static_cast<int32_t>(trace.count);
        lv_draw_line(layer, &lineDsc);
    }

    lv_draw_rect_dsc_t dotDsc;
    lv_draw_rect_dsc_init(&dotDsc);
    dotDsc.bg_color = lv_color_hex(0x123E56);
    dotDsc.bg_opa = LV_OPA_COVER;
    dotDsc.radius = LV_RADIUS_CIRCLE;
    dotDsc.border_width = 0;

    for (const lv_point_precise_t& p : VIA_DOTS) {
        lv_area_t area;
        area.x1 = static_cast<int32_t>(p.x) - 1;
        area.y1 = static_cast<int32_t>(p.y) - 1;
        area.x2 = area.x1 + 2;
        area.y2 = area.y1 + 2;
        lv_draw_rect(layer, &dotDsc, &area);
    }
}

void drawCircuitBackground(lv_obj_t* screen) {
    lv_obj_add_event_cb(screen, circuitBackgroundDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
}

void dispFlush(
    lv_display_t* display,
    const lv_area_t* area,
    uint8_t* pxMap
) {
    int32_t width =
        area->x2 - area->x1 + 1;

    int32_t height =
        area->y2 - area->y1 + 1;

    gfx->draw16bitRGBBitmap(
        area->x1,
        area->y1,
        reinterpret_cast<uint16_t*>(pxMap),
        width,
        height
    );

    lv_display_flush_ready(display);
}

} // namespace


namespace Display {

const lv_color_t COLOR_BG =
    lv_color_hex(0x000000);

const lv_color_t COLOR_ACCENT =
    lv_color_hex(0x3CB4FF);

const lv_color_t COLOR_TEXT =
    lv_color_hex(0xFFFFFF);


void begin() {
    DEBUG_PRINTLN("[DISPLAY] avvio...");

    // Retroilluminazione: accesa a PWM da Board::begin() (chiamato prima
    // di questa funzione), non piu' qui con un semplice digitalWrite —
    // vedi Board::setBacklightPercent(), regolabile da Impostazioni.
    delay(100);

    gfx->begin();
    gfx->setRotation(1);
    gfx->fillScreen(0x0000);

    lv_init();

    size_t bufferPixels =
        DISPLAY_WIDTH * LVGL_BUF_LINES;

    size_t bufferBytes =
        bufferPixels * sizeof(lv_color_t);

    buf1 =
        reinterpret_cast<lv_color_t*>(
            heap_caps_malloc(
                bufferBytes,
                MALLOC_CAP_SPIRAM
            )
        );

    buf2 =
        reinterpret_cast<lv_color_t*>(
            heap_caps_malloc(
                bufferBytes,
                MALLOC_CAP_SPIRAM
            )
        );

    buffersOk = (buf1 != nullptr && buf2 != nullptr);

    if (!buffersOk) {
        DEBUG_PRINTLN(
            "[DISPLAY] ERRORE: allocazione buffer LVGL in PSRAM fallita"
        );
    }

    lvDisplay = lv_display_create(
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT
    );

    lv_display_set_flush_cb(
        lvDisplay,
        dispFlush
    );

    // FULL invece di PARTIAL: il buffer ora copre l'intero schermo, quindi
    // ogni refresh ridisegna e trasferisce SEMPRE l'intera area (0,0)-(319,239),
    // lo stesso identico percorso usato dal primo disegno a schermo intero
    // che sappiamo funzionare. In PARTIAL, LVGL invia al pannello solo il
    // rettangolo "sporco" (es. un singolo bottone): se l'indirizzamento di
    // una finestra parziale sul bus SPI condiviso (display+LoRa+SD) avesse
    // un problema, l'unico sintomo visibile sarebbe esattamente "il primo
    // frame si vede, i successivi aggiornamenti (piccoli) non si vedono mai"
    // - il freeze osservato. FULL elimina questa variabile.
    lv_display_set_buffers(
        lvDisplay,
        buf1,
        buf2,
        bufferBytes,
        LV_DISPLAY_RENDER_MODE_FULL
    );

    // Sfondo globale
    lv_obj_set_style_bg_color(
        lv_screen_active(),
        COLOR_BG,
        0
    );

    lv_obj_set_style_bg_opa(
        lv_screen_active(),
        LV_OPA_COVER,
        0
    );

    // Barra di stato (data/ora a sinistra, batteria a destra) su
    // lv_layer_top(): resta visibile sopra qualunque screen, sempre,
    // senza doverla ricreare per ogni schermata di Menu.cpp.
    lv_obj_t* topLayer = lv_layer_top();

    // Rettangolino verticale RAM: colore aggiornato in updateHeader().
    headerRamBar = lv_obj_create(topLayer);
    lv_obj_remove_style_all(headerRamBar);
    lv_obj_set_size(headerRamBar, 6, 12);
    lv_obj_set_style_bg_opa(headerRamBar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(headerRamBar, 1, 0);
    lv_obj_clear_flag(headerRamBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(headerRamBar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(headerRamBar, LV_ALIGN_TOP_LEFT, 5, 3);

    headerDateTimeLabel = lv_label_create(topLayer);
    lv_obj_set_style_text_font(headerDateTimeLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(headerDateTimeLabel, COLOR_TEXT, 0);
    lv_label_set_text(headerDateTimeLabel, "...");
    lv_obj_align(headerDateTimeLabel, LV_ALIGN_TOP_LEFT, 16, 1);

    headerBatteryLabel = lv_label_create(topLayer);
    lv_obj_set_style_text_font(headerBatteryLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(headerBatteryLabel, COLOR_TEXT, 0);
    lv_label_set_text(headerBatteryLabel, "--%");
    lv_obj_align(headerBatteryLabel, LV_ALIGN_TOP_RIGHT, -5, 1);

    // Icone WiFi/VPN sempre visibili (a differenza di prima): colore
    // acceso + nessuna decorazione se connesso, grigio + barrate se no
    // (vedi updateHeader(), che aggiorna colore/decorazione ogni ciclo).
    //
    // Posizione FISSA rispetto allo schermo (non piu' allineata alla
    // label batteria con lv_obj_align_to()): quell'approccio si basava
    // sulla larghezza reale della label batteria calcolata da LVGL al
    // momento dell'allineamento, che pero' non e' ancora pronta cosi'
    // presto (nemmeno con lv_obj_update_layout() in mezzo) - risultato,
    // le icone finivano sovrapposte alla percentuale. Con offset fissi,
    // scelti larghi abbastanza da stare sempre a sinistra del testo
    // batteria piu' lungo possibile ("100% "+icona), il problema non
    // si pone piu'.
    headerWifiIconLabel = lv_label_create(topLayer);
    lv_obj_set_style_text_font(headerWifiIconLabel, &lv_font_montserrat_14, 0);
    lv_label_set_text(headerWifiIconLabel, LV_SYMBOL_WIFI);
    lv_obj_align(headerWifiIconLabel, LV_ALIGN_TOP_RIGHT, -65, 1);

    headerVpnIconLabel = lv_label_create(topLayer);
    lv_obj_set_style_text_font(headerVpnIconLabel, &lv_font_montserrat_14, 0);
    lv_label_set_text(headerVpnIconLabel, LV_SYMBOL_EYE_CLOSE);
    lv_obj_align(headerVpnIconLabel, LV_ALIGN_TOP_RIGHT, -90, 1);

    headerBluetoothIconLabel = lv_label_create(topLayer);
    lv_obj_set_style_text_font(headerBluetoothIconLabel, &lv_font_montserrat_14, 0);
    lv_label_set_text(headerBluetoothIconLabel, LV_SYMBOL_BLUETOOTH);
    lv_obj_align(headerBluetoothIconLabel, LV_ALIGN_TOP_RIGHT, -115, 1);

    // Scostamento ridotto da Bluetooth (25px -> 19px): avvicina l'icona
    // LoRa a Bluetooth per lasciare spazio a sinistra ad altre icone
    // future (es. NetGuard), senza toccare il resto della fila.
    headerLoraIconLabel = lv_label_create(topLayer);
    lv_obj_set_style_text_font(headerLoraIconLabel, &lv_font_montserrat_14, 0);
    lv_label_set_text(headerLoraIconLabel, LV_SYMBOL_LOOP);
    lv_obj_align(headerLoraIconLabel, LV_ALIGN_TOP_RIGHT, -134, 1);

    headerNetGuardIconLabel = lv_label_create(topLayer);
    lv_obj_set_style_text_font(headerNetGuardIconLabel, &lv_font_montserrat_14, 0);
    lv_label_set_text(headerNetGuardIconLabel, LV_SYMBOL_WARNING);
    lv_obj_align(headerNetGuardIconLabel, LV_ALIGN_TOP_RIGHT, -154, 1);

    DEBUG_PRINTLN("[DISPLAY] pronto");
}


bool areBuffersOk() {
    return buffersOk;
}


void update() {
    updateHeader();

    lv_timer_handler();

    // lv_obj_add_state()/lv_obj_clear_state() (usati per l'evidenziazione
    // del menu, vedi Menu::updateHighlight) confrontano internamente gli
    // stili prima/dopo e possono decidere di NON invalidare un oggetto se
    // ritengono che non ci sia differenza visiva: in certi casi questo
    // impediva al pannello fisico di aggiornarsi pur cambiando
    // correttamente lo stato interno. Invece di dover garantire che ogni
    // singolo punto dell'app invalidi correttamente da solo, forziamo qui
    // un invalidate completo dello screen attivo ad ogni ciclo, poi un
    // redraw+flush immediato che ignora il timer periodico di LVGL
    // (LV_DEF_REFR_PERIOD): il pannello finisce sempre per rispecchiare lo
    // stato reale degli oggetti LVGL.
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);
}


namespace {
    void backButtonClickedCb(lv_event_t* event) {
        auto onBack = reinterpret_cast<void (*)()>(lv_event_get_user_data(event));
        if (onBack != nullptr) {
            onBack();
        }
    }
}

lv_obj_t* createScreen(
    const char* title,
    lv_obj_t** outContent,
    void (*onBack)()
) {
    // Breadcrumb per OGNI schermata: quasi tutte le schermate dell'app
    // passano da qui (anche quelle future), quindi e' il punto singolo
    // piu' efficace per sapere "dove" si trovava l'utente subito prima
    // di un crash - molto piu' granulare del solo "menu:X" registrato in
    // App.cpp, senza dover instrumentare ogni singolo modulo *UI.cpp uno
    // per uno.
    if (title != nullptr && title[0] != '\0') {
        CrashLog::breadcrumb(title);
    } else {
        CrashLog::breadcrumb("(menu principale)");
    }

    lv_obj_t* screen =
        lv_obj_create(nullptr);

    lv_obj_set_style_bg_color(
        screen,
        COLOR_BG,
        0
    );

    lv_obj_set_style_bg_opa(
        screen,
        LV_OPA_COVER,
        0
    );

    lv_obj_set_style_border_width(
        screen,
        0,
        0
    );

    lv_obj_clear_flag(
        screen,
        LV_OBJ_FLAG_SCROLLABLE
    );

    // Primo figlio = z-order piu' basso: resta sempre dietro a titolo,
    // contenuto e bottone Indietro aggiunti sotto.
    drawCircuitBackground(screen);


    // Titolo della schermata: solo se e' stato passato un testo non vuoto.
    // Il menu principale non ne ha bisogno (c'e' gia' la barra di stato
    // persistente in alto con data/ora e batteria), quindi passa "".
    if (title != nullptr && title[0] != '\0') {
    lv_obj_t* titleLabel =
        lv_label_create(screen);

    lv_label_set_text(
        titleLabel,
        title
    );

    // Font ridotto rispetto al precedente montserrat_20
    lv_obj_set_style_text_font(
        titleLabel,
        &lv_font_montserrat_14,
        0
    );

    lv_obj_set_style_text_color(
        titleLabel,
        COLOR_ACCENT,
        0
    );

    // y=22 invece di 10: lascia spazio alla barra di stato persistente
    // (data/ora + batteria) su lv_layer_top(), che occupa la fascia
    // 0-18px in cima a ogni schermata.
    lv_obj_align(
        titleLabel,
        LV_ALIGN_TOP_LEFT,
        15,
        22
    );
    }


    // Area contenuto. Senza titolo (es. menu principale) serve solo
    // spazio sotto la barra di stato in alto, non i 56px riservati anche
    // al titolo: piu' spazio verticale utile, meno bisogno di scroll.
    if (outContent != nullptr) {
        bool hasTitle = (title != nullptr && title[0] != '\0');

        lv_obj_t* content =
            lv_obj_create(screen);

        lv_obj_set_size(
            content,
            DISPLAY_WIDTH - 20,
            hasTitle ? (DISPLAY_HEIGHT - 56) : (DISPLAY_HEIGHT - 30)
        );

        lv_obj_align(
            content,
            LV_ALIGN_BOTTOM_MID,
            0,
            -10
        );

        lv_obj_set_style_bg_opa(
            content,
            LV_OPA_TRANSP,
            0
        );

        lv_obj_set_style_border_width(
            content,
            0,
            0
        );

        lv_obj_set_style_pad_all(
            content,
            4,
            0
        );

        // Il contenitore del menu gestisce
        // autonomamente lo scorrimento verticale
        lv_obj_clear_flag(
            content,
            LV_OBJ_FLAG_SCROLLABLE
        );

        *outContent = content;
    }

    // Bottone touch "Indietro": garantisce che ogni sottomenu abbia un
    // modo di tornare indietro anche solo col dito, non solo da tastiera/
    // trackball. Allineato alla riga del titolo, a destra, cosi' non
    // invade ne' la barra di stato in alto (lv_layer_top(), y 0-18) ne'
    // l'area contenuto sotto.
    if (onBack != nullptr) {
        lv_obj_t* backBtn = lv_btn_create(screen);
        lv_obj_set_size(backBtn, 70, 22);
        lv_obj_align(backBtn, LV_ALIGN_TOP_RIGHT, -8, 21);
        lv_obj_set_style_bg_color(backBtn, lv_color_hex(0x0A1A2A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(backBtn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(backBtn, COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(backBtn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(backBtn, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_all(backBtn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(
            backBtn,
            backButtonClickedCb,
            LV_EVENT_CLICKED,
            reinterpret_cast<void*>(onBack)
        );

        lv_obj_t* backLabel = lv_label_create(backBtn);
        lv_label_set_text(backLabel, LV_SYMBOL_LEFT " Indietro");
        lv_obj_set_style_text_color(backLabel, COLOR_TEXT, 0);
        lv_obj_set_style_text_font(backLabel, &lv_font_montserrat_14, 0);
        lv_obj_center(backLabel);
    }

    return screen;
}


void showBootScreen() {
    lv_obj_t* content = nullptr;

    lv_obj_t* screen =
        createScreen(
            "TDeckFW",
            &content
        );

    lv_obj_t* label =
        lv_label_create(content);

    lv_label_set_text(
        label,
        "Avvio..."
    );

    lv_obj_set_style_text_font(
        label,
        &lv_font_montserrat_14,
        0
    );

    lv_obj_set_style_text_color(
        label,
        COLOR_TEXT,
        0
    );

    lv_obj_align(
        label,
        LV_ALIGN_TOP_LEFT,
        5,
        20
    );

    lv_scr_load(screen);
}


void showCrashSkull() {
    // Schermata "usa e getta": costruita a mano (non via createScreen(),
    // niente titolo/sfondo a circuito qui) proprio per essere un avviso
    // visivo diverso da tutte le altre - un crash e' un evento diverso
    // da un semplice messaggio informativo.
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    const lv_color_t matrixGreen = lv_color_hex(0x00FF41);

    // "Pioggia digitale" sullo sfondo: poche linee verticali verdi
    // scure, stesso spirito decorativo dello sfondo a circuito delle
    // altre schermate (vedi drawCircuitBackground() sopra).
    static lv_style_t rainStyle;
    static bool rainStyleReady = false;
    if (!rainStyleReady) {
        lv_style_init(&rainStyle);
        lv_style_set_line_width(&rainStyle, 1);
        lv_style_set_line_color(&rainStyle, lv_color_hex(0x0B4D1C));
        rainStyleReady = true;
    }

    static const lv_point_precise_t RAIN_1[] = { {20, 0}, {20, 60} };
    static const lv_point_precise_t RAIN_2[] = { {60, 30}, {60, 110} };
    static const lv_point_precise_t RAIN_3[] = { {268, 10}, {268, 90} };
    static const lv_point_precise_t RAIN_4[] = { {300, 50}, {300, 150} };
    static const lv_point_precise_t RAIN_5[] = { {44, 160}, {44, 236} };
    static const lv_point_precise_t RAIN_6[] = { {282, 170}, {282, 236} };

    const lv_point_precise_t* rainLines[] = { RAIN_1, RAIN_2, RAIN_3, RAIN_4, RAIN_5, RAIN_6 };
    for (const lv_point_precise_t* pts : rainLines) {
        lv_obj_t* line = lv_line_create(screen);
        lv_obj_add_style(line, &rainStyle, 0);
        lv_line_set_points(line, pts, 2);
    }

    // Testa: forma tondeggiante piena verde.
    lv_obj_t* head = lv_obj_create(screen);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, 100, 96);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(head, matrixGreen, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    // Centrato sullo schermo: la mascella sporge sotto la testa di 14px
    // (vedi il suo align_to piu' sotto), quindi lo spostamento verso
    // l'alto compensa per centrare l'insieme testa+mascella, non solo
    // la testa da sola.
    lv_obj_align(head, LV_ALIGN_CENTER, 0, -10);

    // Mascella: rettangolo arrotondato piu' stretto, attaccato sotto.
    lv_obj_t* jaw = lv_obj_create(screen);
    lv_obj_remove_style_all(jaw);
    lv_obj_set_size(jaw, 62, 34);
    lv_obj_set_style_radius(jaw, 12, 0);
    lv_obj_set_style_bg_color(jaw, matrixGreen, 0);
    lv_obj_set_style_bg_opa(jaw, LV_OPA_COVER, 0);
    lv_obj_align_to(jaw, head, LV_ALIGN_OUT_BOTTOM_MID, 0, -14);

    // Denti: sottili strisce nere sulla mascella.
    for (int i = 0; i < 4; i++) {
        lv_obj_t* tooth = lv_obj_create(jaw);
        lv_obj_remove_style_all(tooth);
        lv_obj_set_size(tooth, 3, 14);
        lv_obj_set_style_bg_color(tooth, COLOR_BG, 0);
        lv_obj_set_style_bg_opa(tooth, LV_OPA_COVER, 0);
        lv_obj_align(tooth, LV_ALIGN_BOTTOM_LEFT, 8 + i * 14, 0);
    }

    // Occhi: due cavita' nere.
    lv_obj_t* eyeL = lv_obj_create(head);
    lv_obj_remove_style_all(eyeL);
    lv_obj_set_size(eyeL, 26, 30);
    lv_obj_set_style_radius(eyeL, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eyeL, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(eyeL, LV_OPA_COVER, 0);
    lv_obj_align(eyeL, LV_ALIGN_LEFT_MID, 12, -4);

    lv_obj_t* eyeR = lv_obj_create(head);
    lv_obj_remove_style_all(eyeR);
    lv_obj_set_size(eyeR, 26, 30);
    lv_obj_set_style_radius(eyeR, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eyeR, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(eyeR, LV_OPA_COVER, 0);
    lv_obj_align(eyeR, LV_ALIGN_RIGHT_MID, -12, -4);

    // Naso: piccola cavita' nera centrale.
    lv_obj_t* nose = lv_obj_create(head);
    lv_obj_remove_style_all(nose);
    lv_obj_set_size(nose, 14, 18);
    lv_obj_set_style_radius(nose, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(nose, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(nose, LV_OPA_COVER, 0);
    lv_obj_align(nose, LV_ALIGN_CENTER, 0, 22);

    lv_scr_load(screen);
}


void showMessage(
    const char* title,
    const char* message,
    void (*onBack)()
) {
    lv_obj_t* content = nullptr;

    lv_obj_t* screen =
        createScreen(
            title,
            &content,
            onBack
        );

    lv_obj_t* label =
        lv_label_create(content);

    lv_label_set_text(
        label,
        message
    );

    lv_obj_set_style_text_font(
        label,
        &lv_font_montserrat_14,
        0
    );

    lv_obj_set_style_text_color(
        label,
        COLOR_TEXT,
        0
    );

    lv_label_set_long_mode(
        label,
        LV_LABEL_LONG_WRAP
    );

    lv_obj_set_width(
        label,
        DISPLAY_WIDTH - 40
    );

    lv_obj_align(
        label,
        LV_ALIGN_TOP_LEFT,
        5,
        10
    );

    // lv_scr_load() semplice invece di lv_scr_load_anim(): la dissolvenza
    // usava una animazione LVGL che, combinata con l'invalidate/refresh
    // forzato ad ogni ciclo in Display::update(), poteva restare bloccata
    // a meta' transizione (schermo che sembrava non rispondere finche' non
    // si tornava indietro). lv_scr_load() e' lo stesso percorso, senza
    // animazione, gia' usato da Menu::showMain()/back() e verificato
    // affidabile.
    lv_scr_load(screen);

    // Lo screen di messaggio mostrato in precedenza (se c'e') a questo
    // punto non e' piu' quello attivo (o lo era mainScreen, o un altro
    // showMessage precedente): e' sempre sicuro liberarlo qui. Non e'
    // mai mainScreen, quindi non rischiamo di lasciare un puntatore
    // dangling in Menu.cpp.
    if (activeMessageScreen != nullptr) {
        lv_obj_del(activeMessageScreen);
    }

    activeMessageScreen = screen;
}

} // namespace Display