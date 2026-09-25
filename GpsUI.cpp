#include "GpsUI.h"
#include "Config.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "Menu.h"
#include "GPS.h"
#include "GeoLookup.h"
#include "WeatherUI.h"
#include "ParkUI.h"
#include "ConnStatus.h"

#include <Arduino.h>
#include <WiFi.h>

// Fase 2 della riscrittura senza LVGL (vedi project_lvgl_removal_rewrite
// in memoria): schermata GPS convertita a disegno diretto. I 3 bottoni
// in fondo (Luogo/Meteo/Park) restano navigabili sia da tastiera/
// trackball (focus + Select) sia da tap diretto, come prima - il focus
// e' indicato con lo stesso rettangolino verticale verde a sinistra gia'
// usato nel menu principale, non piu' col cambio di colore/spessore del
// bordo.
namespace {

    bool active = false;

    enum class Focus {
        Refresh = 0,
        Meteo = 1,
        Park = 2
    };
    Focus focus = Focus::Refresh;

    bool haveGeoResult = false;
    GeoLookup::Result lastGeoResult;

    uint32_t lastCoordsUpdateMs = 0;
    constexpr uint32_t COORDS_UPDATE_INTERVAL_MS = 2000;

    constexpr int16_t BTN_Y = 204;
    constexpr int16_t BTN_W = 96;
    constexpr int16_t BTN_H = 30;
    constexpr int16_t BTN_GAP = 8;
    constexpr int16_t BTN_X[3] = { 8, 112, 216 };
    const char* BTN_LABEL[3] = { "LUOGO", "METEO", "PARK" };
    int btnZone[3] = { -1, -1, -1 };

    String lastCoordsLine1, lastCoordsLine2, lastSatText;
    String lastCountryText, lastDetailsText;

    void closeGps() {
        active = false;
        Display::setDirectDrawMode(false);
        Menu::showMain();
    }

    bool hasPosition() {
        return GPS::getLatitude() != 0.0 || GPS::getLongitude() != 0.0;
    }

    void drawButtons() {
        Arduino_GFX* gfx = Display::getGfx();

        for (int i = 0; i < 3; i++) {
            gfx->fillRect(BTN_X[i], BTN_Y, BTN_W, BTN_H, Display::DD_BG);
            gfx->drawRect(BTN_X[i], BTN_Y, BTN_W, BTN_H, Display::DD_ACCENT_DIM);

            if (static_cast<int>(focus) == i) {
                gfx->fillRect(BTN_X[i] + 4, BTN_Y + 6, 4, BTN_H - 12, Display::DD_ACCENT);
            }

            gfx->setFont(nullptr);
            gfx->setTextSize(1);
            gfx->setTextColor(Display::DD_ACCENT);
            gfx->setCursor(BTN_X[i] + 14, BTN_Y + BTN_H / 2 - 3);
            gfx->print(BTN_LABEL[i]);
        }
    }

    void setFocus(Focus newFocus) {
        focus = newFocus;
        drawButtons();
    }

    // Coordinate in grande (size 2, l'informazione principale, va letta
    // al volo anche in movimento) - ridisegnate solo se il testo e'
    // davvero cambiato, stesso principio gia' applicato altrove questa
    // settimana.
    void refreshCoordsText() {
        Arduino_GFX* gfx = Display::getGfx();

        String line1, line2;
        if (hasPosition()) {
            char buf1[24], buf2[24];
            snprintf(buf1, sizeof(buf1), "LAT: %.6f", GPS::getLatitude());
            snprintf(buf2, sizeof(buf2), "LON: %.6f", GPS::getLongitude());
            line1 = buf1;
            line2 = buf2;
        } else {
            line1 = "IN ATTESA GPS...";
            line2 = "";
        }

        char satBuf[24];
        snprintf(satBuf, sizeof(satBuf), "Satelliti: %d", GPS::getSatellites());
        String satText = satBuf;

        if (line1 != lastCoordsLine1 || line2 != lastCoordsLine2) {
            gfx->fillRect(0, Display::DD_HEADER_H + 4, 320, 34, Display::DD_BG);
            gfx->setFont(nullptr);
            gfx->setTextSize(2);
            gfx->setTextColor(Display::DD_ACCENT);
            gfx->setCursor(8, Display::DD_HEADER_H + 6);
            gfx->print(line1);
            if (line2.length() > 0) {
                gfx->setCursor(8, Display::DD_HEADER_H + 22);
                gfx->print(line2);
            }
            lastCoordsLine1 = line1;
            lastCoordsLine2 = line2;
        }

        if (satText != lastSatText) {
            gfx->fillRect(0, Display::DD_HEADER_H + 40, 320, 12, Display::DD_BG);
            gfx->setTextSize(1);
            gfx->setTextColor(Display::DD_TEXT);
            gfx->setCursor(8, Display::DD_HEADER_H + 42);
            gfx->print(satText);
            lastSatText = satText;
        }
    }

    void drawGeoResult(const String& country, const String& details) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, Display::DD_HEADER_H + 56, 320, BTN_Y - (Display::DD_HEADER_H + 56), Display::DD_BG);
        Display::drawWrappedText(8, Display::DD_HEADER_H + 58, 304, country, Display::DD_TEXT);
        Display::drawWrappedText(8, Display::DD_HEADER_H + 70, 304, details, Display::DD_TEXT);
        lastCountryText = country;
        lastDetailsText = details;
    }

    void updateGeoLabels() {
        if (!haveGeoResult) {
            drawGeoResult("Nazione: -", "Dettagli: -");
            return;
        }

        String countryText = "Nazione: "
            + (lastGeoResult.country.length() > 0 ? lastGeoResult.country : String("-"));

        String detailsText = "Dettagli: ";
        bool hasLocality = (lastGeoResult.locality.length() > 0);
        bool hasRoad = (lastGeoResult.road.length() > 0);

        if (hasLocality) {
            detailsText += lastGeoResult.locality;
        }
        if (hasRoad) {
            if (hasLocality) {
                detailsText += ", ";
            }
            detailsText += lastGeoResult.road;
        }
        if (!hasLocality && !hasRoad) {
            detailsText += "-";
        }

        drawGeoResult(countryText, detailsText);
    }

    void refreshLocation() {
        if (!hasPosition()) {
            drawGeoResult("Serve prima un fix GPS valido.", "");
            return;
        }

        if (!ConnStatus::isWifiConnected()) {
            drawGeoResult("Serve una rete WiFi connessa.", "");
            return;
        }

        // Mostra subito il messaggio "in corso" prima della chiamata di
        // rete bloccante (qualche secondo), altrimenti lo schermo
        // resterebbe fermo sul testo vecchio senza nessun avviso.
        drawGeoResult("Ricerca informazioni sul luogo...", "");

        lastGeoResult = GeoLookup::reverseGeocode(GPS::getLatitude(), GPS::getLongitude());
        haveGeoResult = lastGeoResult.valid;

        if (!haveGeoResult) {
            DEBUG_PRINTF(
                "[GPSLOC] richiesta fallita pur connesso: heap_libero=%u\n",
                ESP.getFreeHeap()
            );
            drawGeoResult("Richiesta fallita, riprova.", "");
            return;
        }

        updateGeoLabels();
    }

    void activateFocused() {
        switch (focus) {
            case Focus::Refresh:
                setFocus(Focus::Refresh);
                refreshLocation();
                break;
            case Focus::Meteo:
                setFocus(Focus::Meteo);
                WeatherUI::open();
                break;
            case Focus::Park:
                setFocus(Focus::Park);
                ParkUI::open();
                break;
        }
    }

    void drawScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        // Niente riga di titolo qui (a differenza di NetGuard/Meteo/Park):
        // le coordinate in grande, subito sotto l'header, SONO gia' il
        // contenuto principale - stesso comportamento della versione
        // LVGL originale, che non mostrava un titolo separato.
        lastCoordsLine1 = "";
        lastCoordsLine2 = "";
        lastSatText = "";
        refreshCoordsText();

        updateGeoLabels();

        TouchUI::clearZones();
        for (int i = 0; i < 3; i++) {
            btnZone[i] = TouchUI::addZone(BTN_X[i], BTN_Y, BTN_W, BTN_H);
        }
        drawButtons();
    }

} // namespace


namespace GpsUI {

    void open() {
        if (!GPS::isEnabled()) {
            Display::showMessage(
                "GPS",
                "GPS disattivato per risparmiare batteria.\nRiattivalo dalle Impostazioni per usarlo.",
                []() { Menu::back(); }
            );
            return;
        }

        active = true;
        haveGeoResult = false;
        focus = Focus::Refresh;
        Display::setDirectDrawMode(true);
        drawScreen();
    }

    void showMain() {
        active = true;
        Display::setDirectDrawMode(true);
        drawScreen();
    }

    bool isActive() {
        return active;
    }

    void update() {
        if (!active) {
            return;
        }

        // Le coordinate possono cambiare mentre la schermata e' aperta
        // (il GPS ottiene o perde il fix): aggiornate periodicamente,
        // non ad ogni ciclo (non serve, e risparmia lavoro).
        uint32_t now = millis();
        if (now - lastCoordsUpdateMs > COORDS_UPDATE_INTERVAL_MS || lastCoordsUpdateMs == 0) {
            lastCoordsUpdateMs = now;
            refreshCoordsText();
        }

        int tapped = TouchUI::pollTap();
        if (tapped == btnZone[0]) {
            setFocus(Focus::Refresh);
            refreshLocation();
            return;
        } else if (tapped == btnZone[1]) {
            setFocus(Focus::Meteo);
            WeatherUI::open();
            return;
        } else if (tapped == btnZone[2]) {
            setFocus(Focus::Park);
            ParkUI::open();
            return;
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Back:
                closeGps();
                break;

            case InputEvent::Left:
                if (focus == Focus::Park) {
                    setFocus(Focus::Meteo);
                } else if (focus == Focus::Meteo) {
                    setFocus(Focus::Refresh);
                }
                break;

            case InputEvent::Right:
                if (focus == Focus::Refresh) {
                    setFocus(Focus::Meteo);
                } else if (focus == Focus::Meteo) {
                    setFocus(Focus::Park);
                }
                break;

            case InputEvent::Select:
                activateFocused();
                break;

            default:
                break;
        }
    }
}
