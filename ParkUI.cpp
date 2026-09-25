#include "ParkUI.h"
#include "Config.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "GpsUI.h"
#include "GPS.h"
#include "ParkLookup.h"
#include "GeoLookup.h"
#include "ConnStatus.h"

#include <Arduino.h>
#include <WiFi.h>

// Fase 2 della riscrittura senza LVGL (vedi project_lvgl_removal_rewrite
// in memoria): schermata Park convertita a disegno diretto, stesso
// schema di WeatherUI.cpp (chrome fisso disegnato una volta, area
// risultato ripulita e riscritta ad ogni fetchAndShow()).
namespace {

    bool active = false;

    constexpr int16_t RESULT_Y = Display::DD_HEADER_H + 30;

    constexpr int16_t REFRESH_X = 85;
    constexpr int16_t REFRESH_Y = 200;
    constexpr int16_t REFRESH_W = 150;
    constexpr int16_t REFRESH_H = 30;
    int refreshZone = -1;

    void closePark() {
        active = false;
        Display::setDirectDrawMode(false);
        GpsUI::showMain();
    }

    // Punto cardinale piu' vicino al rilevamento (0-360 gradi, 0=Nord):
    // 8 settori da 45 gradi, centrati sul punto cardinale stesso (es.
    // "Nord-Est" copre 22.5-67.5 gradi).
    const char* compassLabel(double degrees) {
        static const char* labels[8] = {
            "Nord", "Nord-Est", "Est", "Sud-Est", "Sud", "Sud-Ovest", "Ovest", "Nord-Ovest"
        };

        int index = static_cast<int>((degrees + 22.5) / 45.0) % 8;
        if (index < 0) {
            index += 8;
        }
        return labels[index];
    }

    void formatDistanceAndDirection(double meters, double bearing, char* out, size_t outSize) {
        const char* direction = compassLabel(bearing);

        if (meters < 1000.0) {
            snprintf(out, outSize, "Parco/area pic-nic a circa %d m a %s", static_cast<int>(meters + 0.5), direction);
        } else {
            snprintf(out, outSize, "Parco/area pic-nic a circa %.1f km a %s", meters / 1000.0, direction);
        }
    }

    void drawChrome() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print("PARK");

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

    void showText(const String& text) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, RESULT_Y, 320, REFRESH_Y - RESULT_Y, Display::DD_BG);
        Display::drawWrappedText(8, RESULT_Y, 304, text, Display::DD_ACCENT);
    }

    void fetchAndShow() {
        showText("Ricerca del parco/area pic-nic piu' vicina...");

        if (!ConnStatus::isWifiConnected()) {
            DEBUG_PRINTF(
                "[PARK] WiFi non connesso qui: wifi_stato=%d heap_libero=%u\n",
                static_cast<int>(WiFi.status()), ESP.getFreeHeap()
            );
            showText("Serve una rete WiFi connessa.");
            return;
        }

        bool hasPosition = (GPS::getLatitude() != 0.0 || GPS::getLongitude() != 0.0);
        if (!hasPosition) {
            showText("Serve prima un fix GPS valido.");
            return;
        }

        ParkLookup::Result result = ParkLookup::findNearestPark(
            GPS::getLatitude(), GPS::getLongitude()
        );

        if (!result.valid) {
            showText(
                "Nessun parco/area pic-nic mappato entro 5 km.\n"
                "(dipende dai dati OpenStreetMap disponibili in questa zona)"
            );
            return;
        }

        char text[96];
        formatDistanceAndDirection(result.distanceMeters, result.bearingDegrees, text, sizeof(text));

        // Mostra subito distanza+direzione (gia' utile per orientarsi),
        // poi cerca anche il nome della via/localita' con una seconda
        // richiesta di rete (piu' lenta): senza questo avviso intermedio
        // lo schermo resterebbe fermo sul messaggio "in corso" ancora
        // per qualche secondo senza nessun avviso.
        String withSearchNote = String(text) + "\nCerco il nome della via...";
        showText(withSearchNote);

        GeoLookup::Result geo = GeoLookup::reverseGeocode(result.lat, result.lon);

        String finalText = text;
        if (geo.valid && geo.road.length() > 0) {
            finalText += "\nVia: " + geo.road;
        } else if (geo.valid && geo.locality.length() > 0) {
            finalText += "\nZona: " + geo.locality;
        } else {
            finalText += "\n(nome via non disponibile in questa zona)";
        }

        showText(finalText);
    }

} // namespace


namespace ParkUI {

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
            closePark();
        } else if (event == InputEvent::Select) {
            fetchAndShow();
        }
    }
}
