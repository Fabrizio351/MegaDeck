#include "ParkUI.h"
#include "Config.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "GpsUI.h"
#include "GPS.h"
#include "ParkLookup.h"
#include "ConnStatus.h"

#include <Arduino.h>
#include <WiFi.h>

// Fase 2 della riscrittura senza LVGL (vedi project_lvgl_removal_rewrite
// in memoria): schermata Park convertita a disegno diretto, stesso
// schema di WeatherUI.cpp (chrome fisso disegnato una volta, area
// risultato ripulita e riscritta ad ogni fetchAndShow()).
//
// Estesa il 26/09 da "solo parchi/aree pic-nic" a piu' categorie insieme
// (parco/pic-nic, acqua potabile, benzina) - richiesto dall'utente per
// un "quadro completo in caso di necessita'" su un'unica schermata,
// rinominata "INFO" (prima "PARK") nel bottone di GpsUI.
//
// Rivista il 30/09 (richiesto: "un po' piccole e scarne"): aggiunto il
// nome OSM del punto trovato quando disponibile (su una riga sotto,
// dettaglio secondario) e una quarta categoria, bagno pubblico
// (amenity=toilets) - piu' utile di "benzina" per chi e' a piedi, tenuta
// comunque accanto alle altre tre. La size 2 provata per il testo
// principale e' stata tolta subito dopo (troppo grossa per questa
// schermata) - resta size 1 come prima.
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

    constexpr int CATEGORY_COUNT = 4;
    const char* CATEGORY_LABELS[CATEGORY_COUNT] = { "Parco", "Acqua", "Bagno", "Benzina" };
    ParkLookup::Result categoryResults[CATEGORY_COUNT];
    bool categoryFetched[CATEGORY_COUNT] = { false, false, false, false };

    // Riga principale + riga col nome OSM indentata sotto (dettaglio
    // secondario, non sempre presente). Size 1 per entrambe (richiesto
    // 30/09: la size 2 provata prima era troppo grossa per questa
    // schermata - tornata come prima). Ritorna la Y da cui continuare
    // per la categoria successiva.
    int16_t drawCategoryLine(int16_t y, const char* label, const ParkLookup::Result* result) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->setFont(nullptr);
        gfx->setTextWrap(false);

        String mainLine;
        if (result == nullptr) {
            mainLine = String(label) + ": ricerca...";
        } else if (!result->valid) {
            mainLine = String(label) + ": non trovato";
        } else {
            const char* direction = compassLabel(result->bearingDegrees);
            char buf[32];
            if (result->distanceMeters < 1000.0) {
                snprintf(buf, sizeof(buf), "circa %d m a %s", static_cast<int>(result->distanceMeters + 0.5), direction);
            } else {
                snprintf(buf, sizeof(buf), "circa %.1f km a %s", result->distanceMeters / 1000.0, direction);
            }
            mainLine = String(label) + ": " + buf;
        }

        gfx->setTextSize(1);
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, y);
        gfx->print(mainLine);
        int16_t nextY = y + 10;

        if (result != nullptr && result->valid && result->name.length() > 0) {
            String nameLine = result->name;
            if (nameLine.length() > 32) {
                nameLine = nameLine.substring(0, 31) + "...";
            }
            gfx->setTextSize(1);
            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->setCursor(16, nextY);
            gfx->print(nameLine);
            nextY += 10;
        }

        return nextY + 2;
    }

    void redrawResults() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, RESULT_Y, 320, REFRESH_Y - RESULT_Y, Display::DD_BG);

        int16_t y = RESULT_Y;
        for (int i = 0; i < CATEGORY_COUNT; i++) {
            y = drawCategoryLine(y, CATEGORY_LABELS[i], categoryFetched[i] ? &categoryResults[i] : nullptr);
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
        gfx->print("INFO");

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
        for (int i = 0; i < CATEGORY_COUNT; i++) {
            categoryFetched[i] = false;
        }
        redrawResults();

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

        double lat = GPS::getLatitude();
        double lon = GPS::getLongitude();

        // Quattro richieste sequenziali (Overpass non supporta comodamente
        // di restituire "il piu' vicino per categoria" in una sola query
        // senza un parsing molto piu' complesso, vedi ParkLookup.cpp) -
        // ognuna aggiorna subito lo schermo appena arriva, cosi' l'utente
        // vede risultati comparire uno alla volta invece di aspettare
        // tutto fermo fino alla fine.
        categoryResults[0] = ParkLookup::findNearestPark(lat, lon);
        categoryFetched[0] = true;
        redrawResults();

        categoryResults[1] = ParkLookup::findNearestWater(lat, lon);
        categoryFetched[1] = true;
        redrawResults();

        categoryResults[2] = ParkLookup::findNearestToilet(lat, lon);
        categoryFetched[2] = true;
        redrawResults();

        categoryResults[3] = ParkLookup::findNearestFuel(lat, lon);
        categoryFetched[3] = true;
        redrawResults();
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
