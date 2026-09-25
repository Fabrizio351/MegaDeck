#include "WeatherUI.h"
#include "Config.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "GpsUI.h"
#include "GPS.h"
#include "WeatherLookup.h"
#include "ConnStatus.h"

#include <Arduino.h>
#include <WiFi.h>

// Fase 2 della riscrittura senza LVGL (vedi project_lvgl_removal_rewrite
// in memoria): schermata Meteo convertita a disegno diretto. Le iconette
// (sole/nuvola/pioggia), prima costruite con lv_obj_t colorati, ora sono
// solo cerchi/rettangoli arrotondati disegnati direttamente - stesso
// aspetto, zero oggetti LVGL.
namespace {

    bool active = false;

    constexpr int16_t ICON_CENTER_X = 160;
    constexpr int16_t ICON_CENTER_Y = Display::DD_HEADER_H + 62;

    constexpr int16_t REFRESH_X = 85;
    constexpr int16_t REFRESH_Y = 200;
    constexpr int16_t REFRESH_W = 150;
    constexpr int16_t REFRESH_H = 30;
    int refreshZone = -1;

    void fetchAndShow();

    void closeWeather() {
        active = false;
        Display::setDirectDrawMode(false);
        GpsUI::showMain();
    }

    void drawSun() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillCircle(ICON_CENTER_X, ICON_CENTER_Y, 32, gfx->color565(0xF1, 0xC4, 0x0F));
    }

    void drawCloudShape(uint16_t color, int16_t yOffset) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRoundRect(ICON_CENTER_X - 36, ICON_CENTER_Y - 17 + yOffset, 72, 34, 17, color);
        gfx->fillCircle(ICON_CENTER_X - 12, ICON_CENTER_Y - 8 + yOffset, 17, color);
    }

    void drawCloud() {
        Arduino_GFX* gfx = Display::getGfx();
        drawCloudShape(gfx->color565(0xBD, 0xC3, 0xC7), 0);
    }

    void drawRain() {
        Arduino_GFX* gfx = Display::getGfx();
        drawCloudShape(gfx->color565(0x7F, 0x8C, 0x8D), -10);

        uint16_t dropColor = gfx->color565(0x3C, 0xB4, 0xFF);
        for (int i = 0; i < 3; i++) {
            int16_t dropX = ICON_CENTER_X + (i - 1) * 18 - 3;
            gfx->fillRoundRect(dropX, ICON_CENTER_Y + 14, 6, 16, 3, dropColor);
        }
    }

    // Titolo, hint indietro e bottone "Aggiorna": parte fissa dello
    // schermo, ridisegnata solo quando si apre la schermata - il
    // contenuto che cambia (icona + testo risultato) lo fa
    // showResult()/fetchAndShow() sopra questa base, senza rifare tutto.
    void drawChrome() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("METEO");

        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(228, Display::DD_HEADER_H + 8);
        gfx->print("[B] INDIETRO");

        gfx->drawRect(REFRESH_X, REFRESH_Y, REFRESH_W, REFRESH_H, Display::DD_ACCENT);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(REFRESH_X + 20, REFRESH_Y + REFRESH_H / 2 - 3);
        gfx->print("AGGIORNA");

        TouchUI::clearZones();
        refreshZone = TouchUI::addZone(REFRESH_X, REFRESH_Y, REFRESH_W, REFRESH_H);
    }

    // Ripulisce solo la zona icona+testo (non tutto lo schermo, il
    // "chrome" sopra resta) prima di ridisegnare il nuovo risultato.
    void clearResultArea() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, Display::DD_HEADER_H + 24, 320, REFRESH_Y - (Display::DD_HEADER_H + 24), Display::DD_BG);
    }

    void showText(const char* text) {
        clearResultArea();
        Display::drawWrappedText(16, ICON_CENTER_Y + 40, 288, text, Display::DD_TEXT);
    }

    void showCondition(WeatherLookup::Condition condition) {
        clearResultArea();

        const char* text = "-";
        switch (condition) {
            case WeatherLookup::Condition::Sereno:
                drawSun();
                text = "Sereno nella prossima ora";
                break;
            case WeatherLookup::Condition::Nuvoloso:
                drawCloud();
                text = "Nuvoloso nella prossima ora";
                break;
            case WeatherLookup::Condition::Pioggia:
                drawRain();
                text = "Pioggia nella prossima ora";
                break;
            case WeatherLookup::Condition::Sconosciuto:
                text = "Condizione non disponibile";
                break;
        }

        Display::drawWrappedText(16, ICON_CENTER_Y + 40, 288, text, Display::DD_TEXT);
    }

    void fetchAndShow() {
        showText("Recupero previsioni meteo...");

        if (!ConnStatus::isWifiConnected()) {
            DEBUG_PRINTF(
                "[METEO] WiFi non connesso qui: wifi_stato=%d heap_libero=%u\n",
                static_cast<int>(WiFi.status()), ESP.getFreeHeap()
            );
            showText("Serve una rete WiFi connessa.");
            return;
        }

        bool hasPosition = (GPS::getLatitude() != 0.0 || GPS::getLongitude() != 0.0);
        if (!hasPosition || GPS::getUtcHour() < 0) {
            showText("Serve prima un fix GPS valido.");
            return;
        }

        WeatherLookup::Result result = WeatherLookup::fetchNextHour(
            GPS::getLatitude(), GPS::getLongitude(), GPS::getUtcHour()
        );

        if (!result.valid) {
            showText("Previsioni non disponibili al momento.");
            return;
        }

        showCondition(result.condition);
    }

} // namespace


namespace WeatherUI {

    void open() {
        active = true;
        Display::setDirectDrawMode(true);
        drawChrome();
        fetchAndShow();
    }

    bool isActive() {
        return active;
    }

    void update() {
        if (!active) {
            return;
        }

        int tapped = TouchUI::pollTap();
        if (tapped == refreshZone) {
            fetchAndShow();
            return;
        }

        InputEvent event = Input::getEvent();
        if (event == InputEvent::Back) {
            closeWeather();
        } else if (event == InputEvent::Select) {
            fetchAndShow();
        }
    }
}
