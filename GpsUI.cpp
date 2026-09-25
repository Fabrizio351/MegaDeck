#include "GpsUI.h"
#include "Config.h"
#include "Display.h"
#include "Input.h"
#include "Menu.h"
#include "GPS.h"
#include "GeoLookup.h"
#include "WeatherUI.h"
#include "ParkUI.h"
#include "ConnStatus.h"

#include <Arduino.h>
#include <lvgl.h>
#include <WiFi.h>

namespace {

    bool active = false;

    lv_obj_t* screen = nullptr;
    lv_obj_t* coordsLabel = nullptr;
    lv_obj_t* satellitesLabel = nullptr;
    lv_obj_t* countryLabel = nullptr;
    lv_obj_t* detailsLabel = nullptr;

    lv_obj_t* refreshBtnRef = nullptr;
    lv_obj_t* meteoBtnRef = nullptr;
    lv_obj_t* parkBtnRef = nullptr;

    enum class Focus {
        Refresh,
        Meteo,
        Park
    };
    Focus focus = Focus::Refresh;

    bool haveGeoResult = false;
    GeoLookup::Result lastGeoResult;

    uint32_t lastCoordsUpdateMs = 0;
    constexpr uint32_t COORDS_UPDATE_INTERVAL_MS = 2000;

    void refreshLocationCb(lv_event_t* event);

    void closeGps() {
        active = false;
        Menu::showMain();
    }

    bool hasPosition() {
        return GPS::getLatitude() != 0.0 || GPS::getLongitude() != 0.0;
    }

    void refreshCoordsText() {
        if (coordsLabel == nullptr) {
            return;
        }

        char text[40];
        if (hasPosition()) {
            snprintf(
                text, sizeof(text),
                "Lat: %.6f\nLon: %.6f",
                GPS::getLatitude(), GPS::getLongitude()
            );
        } else {
            snprintf(text, sizeof(text), "In attesa GPS...");
        }
        lv_label_set_text(coordsLabel, text);

        if (satellitesLabel != nullptr) {
            char satText[24];
            snprintf(satText, sizeof(satText), "Satelliti: %d", GPS::getSatellites());
            lv_label_set_text(satellitesLabel, satText);
        }
    }

    void updateGeoLabels() {
        if (countryLabel == nullptr || detailsLabel == nullptr) {
            return;
        }

        if (!haveGeoResult) {
            lv_label_set_text(countryLabel, "Nazione: -");
            lv_label_set_text(detailsLabel, "Dettagli: -");
            return;
        }

        String countryText = "Nazione: "
            + (lastGeoResult.country.length() > 0 ? lastGeoResult.country : String("-"));
        lv_label_set_text(countryLabel, countryText.c_str());

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

        lv_label_set_text(detailsLabel, detailsText.c_str());
    }

    void refreshLocationCb(lv_event_t* event) {
        (void)event;

        if (!hasPosition()) {
            lv_label_set_text(countryLabel, "Serve prima un fix GPS valido.");
            lv_label_set_text(detailsLabel, "");
            return;
        }

        // Controllo WiFi separato dall'esito della richiesta: prima non
        // c'era, e qualunque motivo di fallimento (memoria insufficiente
        // per la connessione cifrata, timeout, risposta non valida...)
        // veniva mostrato come "serve una rete WiFi connessa" anche
        // quando il WiFi era gia' connesso, fuorviante.
        if (!ConnStatus::isWifiConnected()) {
            lv_label_set_text(countryLabel, "Serve una rete WiFi connessa.");
            lv_label_set_text(detailsLabel, "");
            return;
        }

        // Mostra subito il messaggio "in corso" prima della chiamata di
        // rete bloccante (qualche secondo), altrimenti lo schermo
        // resterebbe fermo sul testo vecchio senza nessun avviso.
        lv_label_set_text(countryLabel, "Ricerca informazioni sul luogo...");
        lv_label_set_text(detailsLabel, "");
        Display::update();

        lastGeoResult = GeoLookup::reverseGeocode(GPS::getLatitude(), GPS::getLongitude());
        haveGeoResult = lastGeoResult.valid;

        if (!haveGeoResult) {
            // Diagnostica temporanea: capire se questi fallimenti sono
            // dovuti a poca memoria libera durante la richiesta cifrata.
            DEBUG_PRINTF(
                "[GPSLOC] richiesta fallita pur connesso: heap_libero=%u\n",
                ESP.getFreeHeap()
            );
            lv_label_set_text(countryLabel, "Richiesta fallita, riprova.");
            lv_label_set_text(detailsLabel, "");
            return;
        }

        updateGeoLabels();
    }

    void updateFocusHighlight() {
        auto setFocusBorder = [](lv_obj_t* btn, bool selected) {
            if (btn == nullptr) {
                return;
            }
            lv_obj_set_style_border_color(
                btn,
                selected ? Display::COLOR_ACCENT : lv_color_hex(0x333333),
                LV_PART_MAIN
            );
            lv_obj_set_style_border_width(btn, selected ? 2 : 1, LV_PART_MAIN);
        };

        setFocusBorder(refreshBtnRef, focus == Focus::Refresh);
        setFocusBorder(meteoBtnRef, focus == Focus::Meteo);
        setFocusBorder(parkBtnRef, focus == Focus::Park);
    }

    void meteoButtonCb(lv_event_t* event) {
        (void)event;
        focus = Focus::Meteo;
        updateFocusHighlight();
        WeatherUI::open();
    }

    void parkButtonCb(lv_event_t* event) {
        (void)event;
        focus = Focus::Park;
        updateFocusHighlight();
        ParkUI::open();
    }

    void refreshButtonTouchCb(lv_event_t* event) {
        focus = Focus::Refresh;
        updateFocusHighlight();
        refreshLocationCb(event);
    }

    void activateFocused() {
        switch (focus) {
            case Focus::Refresh: refreshLocationCb(nullptr); break;
            case Focus::Meteo:   WeatherUI::open();           break;
            case Focus::Park:    ParkUI::open();              break;
        }
    }

    lv_obj_t* createActionButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_btn_create(parent);
        lv_obj_set_size(btn, 92, 34);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x0A1A2A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, text);
        lv_obj_center(label);
        lv_obj_set_style_text_color(label, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);

        return btn;
    }

    void buildScreen() {
        lv_obj_t* content = nullptr;
        screen = Display::createScreen("GPS", &content, closeGps);

        // Coordinate in grande (montserrat_20 invece di 14) e centrate:
        // sono l'informazione principale della schermata, vanno lette al
        // volo anche in movimento. Altezza fissata a 2 righe (anche
        // quando il testo e' una sola riga, es. "In attesa GPS..."):
        // altrimenti l'etichetta cresce/si restringe passando da 1 a 2
        // righe quando arriva il fix, e le voci sotto (allineate UNA
        // VOLTA in base all'altezza di questo momento) finivano
        // sovrapposte al testo appena si allungava.
        coordsLabel = lv_label_create(content);
        lv_obj_set_style_text_color(coordsLabel, Display::COLOR_ACCENT, 0);
        lv_obj_set_style_text_font(coordsLabel, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(coordsLabel, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(coordsLabel, LV_PCT(100));
        lv_obj_set_height(coordsLabel, 44); // 2 righe montserrat_20 (line_height 22)
        lv_obj_align(coordsLabel, LV_ALIGN_TOP_RIGHT, 0, 0);

        satellitesLabel = lv_label_create(content);
        lv_obj_set_style_text_color(satellitesLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(satellitesLabel, &lv_font_montserrat_14, 0);
        lv_obj_align_to(satellitesLabel, coordsLabel, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

        refreshCoordsText();

        countryLabel = lv_label_create(content);
        lv_obj_set_style_text_color(countryLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(countryLabel, &lv_font_montserrat_14, 0);
        lv_obj_align_to(countryLabel, satellitesLabel, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

        detailsLabel = lv_label_create(content);
        lv_obj_set_style_text_color(detailsLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(detailsLabel, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(detailsLabel, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(detailsLabel, LV_PCT(100));
        lv_obj_align_to(detailsLabel, countryLabel, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

        updateGeoLabels();

        // Riga di 3 azioni in fondo: aggiorna il luogo (nazione/comune),
        // Meteo e Park. Il trackball (sinistra/destra) sposta il focus
        // fra le tre, Select attiva quella evidenziata; il tocco diretto
        // funziona comunque su ciascun bottone indipendentemente dal focus.
        lv_obj_t* actionRow = lv_obj_create(content);
        lv_obj_remove_style_all(actionRow);
        lv_obj_set_size(actionRow, LV_PCT(100), 34);
        lv_obj_align(actionRow, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_layout(actionRow, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(actionRow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(
            actionRow,
            LV_FLEX_ALIGN_SPACE_BETWEEN,
            LV_FLEX_ALIGN_CENTER,
            LV_FLEX_ALIGN_CENTER
        );
        lv_obj_clear_flag(actionRow, LV_OBJ_FLAG_SCROLLABLE);

        refreshBtnRef = createActionButton(actionRow, LV_SYMBOL_REFRESH " Luogo", refreshButtonTouchCb);
        meteoBtnRef = createActionButton(actionRow, "Meteo", meteoButtonCb);
        parkBtnRef = createActionButton(actionRow, "Park", parkButtonCb);

        focus = Focus::Refresh;
        updateFocusHighlight();
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

        if (screen != nullptr) {
            lv_obj_del(screen);
            screen = nullptr;
        }

        buildScreen();
        lv_scr_load(screen);
    }

    void showMain() {
        active = true;

        if (screen != nullptr) {
            lv_scr_load(screen);
        }

        refreshCoordsText();
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

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Back:
                closeGps();
                break;

            case InputEvent::Left:
                if (focus == Focus::Park) {
                    focus = Focus::Meteo;
                } else if (focus == Focus::Meteo) {
                    focus = Focus::Refresh;
                }
                updateFocusHighlight();
                break;

            case InputEvent::Right:
                if (focus == Focus::Refresh) {
                    focus = Focus::Meteo;
                } else if (focus == Focus::Meteo) {
                    focus = Focus::Park;
                }
                updateFocusHighlight();
                break;

            case InputEvent::Select:
                activateFocused();
                break;

            default:
                break;
        }
    }
}
