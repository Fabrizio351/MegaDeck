#include "Display.h"
#include "Board.h"
#include "Config.h"
#include "GPS.h"
#include "ConnStatus.h"
#include "CrashLog.h"
#include "TouchUI.h"
#include "Input.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <Arduino_GFX_Library.h>

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

bool buffersOk = false;

} // namespace


namespace Display {

// Palette RGB565 per le schermate disegnate a mano. gfx->color565()
// converte da R,G,B a 8 bit al formato a 16 bit reale del pannello - MAI
// scrivere qui valori esadecimali a 24 bit come se fossero gia' RGB565
// (bug reale gia' preso una volta, vedi il commit del 25/09 in git log).
const uint16_t DD_BG = gfx->color565(0x00, 0x00, 0x00);
const uint16_t DD_ACCENT = gfx->color565(0x39, 0xFF, 0x14);
const uint16_t DD_ACCENT_DIM = gfx->color565(0x0A, 0x2E, 0x10);
const uint16_t DD_TEXT = gfx->color565(0xD7, 0xF7, 0xDF);
const uint16_t DD_TEXT_DIM = gfx->color565(0x7F, 0xAE, 0x86);
const uint16_t DD_DANGER = gfx->color565(0xFF, 0x3B, 0x30);
const uint16_t DD_WARN = gfx->color565(0xFF, 0xD5, 0x1A);

bool directDrawModeActive = false;

void setDirectDrawMode(bool active) {
    directDrawModeActive = active;
}


bool isDirectDrawMode() {
    return directDrawModeActive;
}


// Barra in alto condivisa (Fase 2): stessa idea/layout del test di Fase
// 1 (barretta RAM, ora, sigle radio) ma con dati REALI invece che
// placeholder fissi - stesse fonti gia' usate da updateHeader() (LVGL).
// Aggiunta anche la percentuale batteria, assente nel test.
void drawHeaderBar() {
    gfx->fillRect(0, 0, DISPLAY_WIDTH, DD_HEADER_H, DD_BG);
    gfx->drawFastHLine(0, DD_HEADER_H, DISPLAY_WIDTH, DD_ACCENT_DIM);

    gfx->setFont(nullptr);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);

    // Soglie sull'USATO (non sul libero): verde fino al 50%, giallo fino
    // all'85%, rosso oltre (richiesto 26/09).
    uint32_t heapTotal = ESP.getHeapSize();
    int ramUsedPercent = (heapTotal > 0)
        ? static_cast<int>(((heapTotal - ESP.getFreeHeap()) * 100UL) / heapTotal)
        : 0;
    uint16_t ramColor = (ramUsedPercent <= 50) ? DD_ACCENT
        : (ramUsedPercent <= 85) ? gfx->color565(0xF1, 0xC4, 0x0F)
        : gfx->color565(0xE7, 0x4C, 0x3C);
    gfx->fillRect(4, 4, 4, 10, ramColor);

    String time = GPS::getTimeLocal();
    gfx->setTextColor(DD_TEXT_DIM);
    gfx->setCursor(12, 6);
    gfx->print(time.length() > 0 ? time : "...");

    gfx->setTextColor(ConnStatus::isVpnConnected() ? DD_ACCENT : DD_TEXT_DIM);
    gfx->setCursor(120, 6);
    gfx->print("VPN");

    gfx->setTextColor(ConnStatus::isWifiConnected() ? DD_ACCENT : DD_TEXT_DIM);
    gfx->setCursor(150, 6);
    gfx->print("WIFI");

    gfx->setTextColor(ConnStatus::isBluetoothActive() ? DD_ACCENT : DD_TEXT_DIM);
    gfx->setCursor(178, 6);
    gfx->print("BLE");

    // Rosso se c'e' un messaggio Meshtastic non ancora letto (02/10,
    // richiesto: "non li perdo"), verde se in ascolto (primo o secondo
    // piano), spento altrimenti - stesso schema di GUARD sotto.
    uint16_t loraColor = ConnStatus::hasUnreadLoraMessages()
        ? gfx->color565(0xE7, 0x4C, 0x3C)
        : (ConnStatus::isLoraActive() ? DD_ACCENT : DD_TEXT_DIM);
    gfx->setTextColor(loraColor);
    gfx->setCursor(202, 6);
    gfx->print("LORA");

    // NetGuard: era un'icona LVGL, persa nella conversione a disegno
    // diretto (riaggiunta il 26/09 su segnalazione). Rosso sia per un
    // allarme appena scattato (pending, non ancora chiuso) SIA per un
    // intruso ancora rilevato nell'ultimo giro anche a popup chiuso -
    // prima tornava verde ad ogni giro anche con un dispositivo fuori
    // whitelist ancora collegato (segnalato dall'utente il 26/09), verde
    // altrimenti, niente stato intermedio arancio.
    uint16_t guardColor = (ConnStatus::hasNetGuardPendingAlert() || ConnStatus::hasNetGuardActiveIntruder())
        ? gfx->color565(0xE7, 0x4C, 0x3C)
        : (ConnStatus::isNetGuardActive() ? DD_ACCENT : DD_TEXT_DIM);
    gfx->setTextColor(guardColor);
    gfx->setCursor(230, 6);
    gfx->print("GUARD");

    int batteryPercent = Board::getBatteryPercent();
    char battText[8];
    snprintf(battText, sizeof(battText), "%d%%", batteryPercent);
    gfx->setTextColor(DD_TEXT_DIM);
    gfx->setCursor(288, 6);
    gfx->print(battText);
}


void refreshHeaderIfNeeded() {
    if (!directDrawModeActive) {
        return;
    }

    static bool everDrawn = false;
    static bool lastWifi = false;
    static bool lastVpn = false;
    static bool lastBle = false;
    static bool lastLora = false;
    static bool lastLoraUnread = false;
    static bool lastGuardActive = false;
    static bool lastGuardAlert = false;
    static int lastBattery = -1;
    static String lastTime;
    static int lastRamBucket = -1;
    static uint32_t lastSlowSampleMs = 0;

    // Batteria e ora vanno RILETTE al massimo ogni minuto, non ad ogni
    // ciclo: una singola lettura ADC della batteria "balla" di qualche
    // punto percentuale da un ciclo all'altro (rumore, non un
    // cambiamento reale), e ricontrollarla ad ogni ciclo faceva
    // ridisegnare l'header decine di volte al secondo (segnalato 26/09:
    // "la percentuale batteria e' impazzata"). Un minuto e' anche piu'
    // parco di batteria (richiesto 26/09) - stesso intervallo del vecchio
    // header LVGL. wifi/ble/lora/guard restano controllati ogni ciclo.
    constexpr uint32_t SLOW_FIELDS_INTERVAL_MS = 60000;

    bool wifi = ConnStatus::isWifiConnected();
    bool vpn = ConnStatus::isVpnConnected();
    bool ble = ConnStatus::isBluetoothActive();
    bool lora = ConnStatus::isLoraActive();
    bool loraUnread = ConnStatus::hasUnreadLoraMessages();
    bool guardActive = ConnStatus::isNetGuardActive();
    bool guardAlert = ConnStatus::hasNetGuardPendingAlert();

    // RAM: confrontata a "fasce" (0=verde/1=giallo/2=rosso, stesse soglie
    // di drawHeaderBar()), non sul valore grezzo - l'uso heap oscilla di
    // continuo per allocazioni/deallocazioni normali, confrontare il
    // numero esatto ridisegnerebbe l'header quasi ogni ciclo per un
    // rettangolino che in pratica cambia colore di rado (stesso problema
    // gia' visto con la batteria, segnalato 26/09: "il rettangolino...
    // rimane sempre verde" - in realta' semplicemente non veniva mai
    // ridisegnato per farlo controllare di nuovo).
    uint32_t heapTotal = ESP.getHeapSize();
    int ramUsedPercent = (heapTotal > 0)
        ? static_cast<int>(((heapTotal - ESP.getFreeHeap()) * 100UL) / heapTotal)
        : 0;
    int ramBucket = (ramUsedPercent <= 50) ? 0 : (ramUsedPercent <= 85) ? 1 : 2;

    uint32_t now = millis();
    int battery = lastBattery;
    String time = lastTime;
    if (!everDrawn || now - lastSlowSampleMs >= SLOW_FIELDS_INTERVAL_MS) {
        battery = Board::getBatteryPercent();
        time = GPS::getTimeLocal();
        lastSlowSampleMs = now;
    }

    bool changed = !everDrawn
        || wifi != lastWifi || vpn != lastVpn || ble != lastBle || lora != lastLora
        || loraUnread != lastLoraUnread
        || guardActive != lastGuardActive || guardAlert != lastGuardAlert
        || battery != lastBattery || time != lastTime
        || ramBucket != lastRamBucket;

    if (!changed) {
        return;
    }

    drawHeaderBar();

    everDrawn = true;
    lastWifi = wifi;
    lastVpn = vpn;
    lastBle = ble;
    lastLora = lora;
    lastLoraUnread = loraUnread;
    lastGuardActive = guardActive;
    lastGuardAlert = guardAlert;
    lastBattery = battery;
    lastTime = time;
    lastRamBucket = ramBucket;
}


void drawWrappedText(int16_t x, int16_t y, int16_t w, const String& text, uint16_t color, uint8_t size) {
    gfx->setFont(nullptr);
    gfx->setTextSize(size);
    gfx->setTextWrap(false);
    gfx->setTextColor(color);

    const int16_t CHAR_W = 6 * size;
    const int16_t LINE_H = 11 * size;

    int maxChars = w / CHAR_W;
    if (maxChars < 1) {
        maxChars = 1;
    }

    int16_t cursorY = y;
    int pos = 0;
    int len = text.length();

    while (pos <= len) {
        int newlinePos = text.indexOf('\n', pos);
        int lineEnd = (newlinePos == -1) ? len : newlinePos;
        int lineLen = lineEnd - pos;

        if (lineLen == 0) {
            cursorY += LINE_H;
        } else {
            int segStart = pos;
            while (segStart < lineEnd) {
                int segLen = min(maxChars, lineEnd - segStart);
                gfx->setCursor(x, cursorY);
                gfx->print(text.substring(segStart, segStart + segLen));
                cursorY += LINE_H;
                segStart += segLen;
            }
        }

        if (newlinePos == -1) {
            break;
        }
        pos = newlinePos + 1;
    }
}


int16_t wrappedTextHeight(const String& text, int16_t w, uint8_t size) {
    const int16_t CHAR_W = 6 * size;
    const int16_t LINE_H = 11 * size;

    int maxChars = w / CHAR_W;
    if (maxChars < 1) {
        maxChars = 1;
    }

    int lines = 0;
    int pos = 0;
    int len = text.length();

    while (pos <= len) {
        int newlinePos = text.indexOf('\n', pos);
        int lineEnd = (newlinePos == -1) ? len : newlinePos;
        int lineLen = lineEnd - pos;

        if (lineLen == 0) {
            lines++;
        } else {
            int segStart = pos;
            while (segStart < lineEnd) {
                int segLen = min(maxChars, lineEnd - segStart);
                lines++;
                segStart += segLen;
            }
        }

        if (newlinePos == -1) {
            break;
        }
        pos = newlinePos + 1;
    }

    return static_cast<int16_t>(lines * LINE_H);
}


int wrappedLineCount(const String& text, int16_t w, uint8_t size) {
    const int16_t LINE_H = 11 * size;
    return wrappedTextHeight(text, w, size) / LINE_H;
}


void drawWrappedTextRange(int16_t x, int16_t y, int16_t w, const String& text, uint16_t color, int firstLine, int maxLines, uint8_t size) {
    gfx->setFont(nullptr);
    gfx->setTextSize(size);
    gfx->setTextWrap(false);
    gfx->setTextColor(color);

    const int16_t CHAR_W = 6 * size;
    const int16_t LINE_H = 11 * size;

    int maxChars = w / CHAR_W;
    if (maxChars < 1) {
        maxChars = 1;
    }

    int lineIndex = 0;
    int16_t cursorY = y;
    int pos = 0;
    int len = text.length();
    int lastLine = firstLine + maxLines;

    while (pos <= len && lineIndex < lastLine) {
        int newlinePos = text.indexOf('\n', pos);
        int lineEnd = (newlinePos == -1) ? len : newlinePos;
        int lineLen = lineEnd - pos;

        if (lineLen == 0) {
            if (lineIndex >= firstLine) {
                cursorY += LINE_H;
            }
            lineIndex++;
        } else {
            int segStart = pos;
            while (segStart < lineEnd && lineIndex < lastLine) {
                int segLen = min(maxChars, lineEnd - segStart);
                if (lineIndex >= firstLine) {
                    gfx->setCursor(x, cursorY);
                    gfx->print(text.substring(segStart, segStart + segLen));
                    cursorY += LINE_H;
                }
                lineIndex++;
                segStart += segLen;
            }
        }

        if (newlinePos == -1) {
            break;
        }
        pos = newlinePos + 1;
    }
}


void begin() {
    DEBUG_PRINTLN("[DISPLAY] avvio...");

    // Retroilluminazione: accesa a PWM da Board::begin() (chiamato prima
    // di questa funzione), non piu' qui con un semplice digitalWrite —
    // vedi Board::setBacklightPercent(), regolabile da Impostazioni.
    delay(100);

    gfx->begin();
    gfx->setRotation(1);
    gfx->fillScreen(0x0000);

    // Prova di allocazione in PSRAM, stessa dimensione dei vecchi buffer
    // LVGL a schermo intero: il disegno diretto scrive sul pannello senza
    // un framebuffer intermedio nostro, quindi non serve piu' tenerla,
    // ma la verifica in se' resta utile com'era - la causa storica dello
    // schermo nero "silenzioso" (flag PSRAM=opi mancante in compilazione,
    // vedi project_psram_fqbn_flag_critical in memoria) va ancora
    // rilevata qui, prima che qualunque altra cosa in PSRAM (WiFi/TLS,
    // NetScan...) ne dipenda in modo meno diagnosticabile.
    size_t testBytes = static_cast<size_t>(DISPLAY_WIDTH) * DISPLAY_HEIGHT * 2;
    void* testBuf = heap_caps_malloc(testBytes, MALLOC_CAP_SPIRAM);
    buffersOk = (testBuf != nullptr);
    if (testBuf != nullptr) {
        heap_caps_free(testBuf);
    } else {
        DEBUG_PRINTLN("[DISPLAY] ERRORE: allocazione di prova in PSRAM fallita");
    }

    DEBUG_PRINTLN("[DISPLAY] pronto");
}


bool areBuffersOk() {
    return buffersOk;
}


Arduino_GFX* getGfx() {
    return gfx;
}


// LVGL rimosso interamente dal progetto (26/09, vedi
// project_lvgl_removal_rewrite in memoria): ogni schermata e' ora
// disegno diretto Arduino_GFX, quindi non serve piu' "mettere in pausa"
// nulla - update() non ha piu' niente da fare (nessun timer/refresh
// LVGL), lasciata vuota (non rimossa: alcuni chiamanti la invocano
// ancora ogni ciclo, harmless).
void update() {
}

void showBootScreen() {
    gfx->fillScreen(DD_BG);
    gfx->drawFastHLine(0, DD_HEADER_H, DISPLAY_WIDTH, DD_ACCENT_DIM);

    gfx->setFont(nullptr);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);

    gfx->setTextColor(DD_ACCENT);
    gfx->setCursor(8, DD_HEADER_H + 8);
    gfx->print("TDECKFW");

    gfx->setTextColor(DD_TEXT);
    gfx->setCursor(8, DD_HEADER_H + 26);
    gfx->print("Avvio...");
}


// Schermata "teschio stile Matrix", disegnata con le primitive
// Arduino_GFX (cerchi/rettangoli pieni) al posto dei widget LVGL
// originali - stessa forma (testa tonda, mascella, denti, occhi/naso
// come cavita' nere), solo ricostruita senza libreria grafica.
void showCrashSkull() {
    gfx->fillScreen(DD_BG);

    uint16_t matrixGreen = gfx->color565(0x00, 0xFF, 0x41);
    uint16_t rainColor = gfx->color565(0x0B, 0x4D, 0x1C);

    // "Pioggia digitale" sullo sfondo: poche linee verticali verdi scure.
    gfx->drawLine(20, 0, 20, 60, rainColor);
    gfx->drawLine(60, 30, 60, 110, rainColor);
    gfx->drawLine(268, 10, 268, 90, rainColor);
    gfx->drawLine(300, 50, 300, 150, rainColor);
    gfx->drawLine(44, 160, 44, 236, rainColor);
    gfx->drawLine(282, 170, 282, 236, rainColor);

    // Testa: cerchio pieno verde. Spostata verso l'alto per lasciare
    // spazio alla mascella sotto.
    constexpr int16_t headCx = 160;
    constexpr int16_t headCy = 106;
    constexpr int16_t headR = 50;
    gfx->fillCircle(headCx, headCy, headR, matrixGreen);

    // Mascella: rettangolo arrotondato piu' stretto, attaccato sotto la
    // testa.
    constexpr int16_t jawW = 62;
    constexpr int16_t jawH = 34;
    constexpr int16_t jawX = headCx - jawW / 2;
    constexpr int16_t jawY = headCy + headR - 14;
    gfx->fillRoundRect(jawX, jawY, jawW, jawH, 12, matrixGreen);

    // Denti: sottili strisce nere sulla mascella.
    for (int i = 0; i < 4; i++) {
        gfx->fillRect(jawX + 8 + i * 14, jawY + jawH - 14, 3, 14, DD_BG);
    }

    // Occhi e naso: cavita' nere.
    gfx->fillCircle(headCx - 25, headCy - 4, 13, DD_BG);
    gfx->fillCircle(headCx + 25, headCy - 4, 13, DD_BG);
    gfx->fillCircle(headCx, headCy + 26, 7, DD_BG);
}


// Stato del messaggio "usa e getta" (vedi Display::isMessageActive()/
// updateMessage() in Display.h): titolo/testo copiati subito in String
// proprie, non tenuti come const char* del chiamante - alcuni passano
// il .c_str() di una String locale che smetterebbe di esistere appena
// tornano (es. NetGuard::triggerAlert()).
String messageTitle;
String messageText;
void (*messageOnBack)() = nullptr;
bool messageActive = false;
int messageBackZone = -1;
bool messageJustDismissed = false;

void drawMessageScreen() {
    gfx->fillScreen(DD_BG);
    drawHeaderBar();

    gfx->setFont(nullptr);
    gfx->setTextSize(1);
    gfx->setTextWrap(false);

    gfx->setTextColor(DD_ACCENT);
    gfx->setCursor(8, DD_HEADER_H + 8);
    gfx->print(messageTitle);

    drawWrappedText(8, DD_HEADER_H + 26, DISPLAY_WIDTH - 16, messageText, DD_TEXT);

    TouchUI::clearZones();
    constexpr int16_t BTN_X = 8;
    constexpr int16_t BTN_W = 304;
    constexpr int16_t BTN_H = 28;
    constexpr int16_t BTN_Y = 206;
    gfx->drawRect(BTN_X, BTN_Y, BTN_W, BTN_H, DD_ACCENT);
    gfx->setTextColor(DD_ACCENT);
    gfx->setCursor(BTN_X + 12, BTN_Y + BTN_H / 2 - 3);
    gfx->print("[ OK ] INDIETRO");
    messageBackZone = TouchUI::addZone(BTN_X, BTN_Y, BTN_W, BTN_H);
}


void showMessage(
    const char* title,
    const char* message,
    void (*onBack)()
) {
    messageTitle = (title != nullptr) ? title : "";
    messageText = (message != nullptr) ? message : "";
    messageOnBack = onBack;
    messageActive = true;

    CrashLog::breadcrumb(messageTitle.length() > 0 ? messageTitle.c_str() : "(messaggio)");

    drawMessageScreen();
}


bool isMessageActive() {
    return messageActive;
}


void updateMessage() {
    int tapped = TouchUI::pollTap();
    bool dismissed = (tapped == messageBackZone);

    if (!dismissed) {
        InputEvent event = Input::getEvent();
        dismissed = (event == InputEvent::Back || event == InputEvent::Select);
    }

    if (!dismissed) {
        return;
    }

    messageActive = false;
    messageJustDismissed = true;
    void (*callback)() = messageOnBack;
    messageOnBack = nullptr;

    if (callback != nullptr) {
        callback();
    }
}

bool consumeMessageJustDismissed() {
    bool v = messageJustDismissed;
    messageJustDismissed = false;
    return v;
}

} // namespace Display