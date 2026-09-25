#include "ParkUI.h"
#include "Config.h"
#include "Display.h"
#include "Input.h"
#include "GpsUI.h"
#include "GPS.h"
#include "ParkLookup.h"
#include "GeoLookup.h"
#include "ConnStatus.h"

#include <Arduino.h>
#include <lvgl.h>
#include <WiFi.h>

namespace {

    bool active = false;

    lv_obj_t* screen = nullptr;
    lv_obj_t* resultLabel = nullptr;

    void closePark() {
        active = false;
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

    void fetchAndShow() {
        lv_label_set_text(resultLabel, "Ricerca del parco/area pic-nic piu' vicina...");
        Display::update();

        if (!ConnStatus::isWifiConnected()) {
            // Diagnostica temporanea per capire perche' risulta "non
            // connesso" anche quando l'utente vede il WiFi funzionante
            // altrove: stato WiFi esatto in questo preciso istante,
            // memoria libera in questo preciso istante.
            DEBUG_PRINTF(
                "[PARK] WiFi non connesso qui: wifi_stato=%d heap_libero=%u\n",
                static_cast<int>(WiFi.status()), ESP.getFreeHeap()
            );
            lv_label_set_text(resultLabel, "Serve una rete WiFi connessa.");
            return;
        }

        bool hasPosition = (GPS::getLatitude() != 0.0 || GPS::getLongitude() != 0.0);
        if (!hasPosition) {
            lv_label_set_text(resultLabel, "Serve prima un fix GPS valido.");
            return;
        }

        ParkLookup::Result result = ParkLookup::findNearestPark(
            GPS::getLatitude(), GPS::getLongitude()
        );

        if (!result.valid) {
            lv_label_set_text(
                resultLabel,
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
        lv_label_set_text(resultLabel, withSearchNote.c_str());
        Display::update();

        GeoLookup::Result geo = GeoLookup::reverseGeocode(result.lat, result.lon);

        String finalText = text;
        if (geo.valid && geo.road.length() > 0) {
            finalText += "\nVia: " + geo.road;
        } else if (geo.valid && geo.locality.length() > 0) {
            finalText += "\nZona: " + geo.locality;
        } else {
            finalText += "\n(nome via non disponibile in questa zona)";
        }

        lv_label_set_text(resultLabel, finalText.c_str());
    }

    void refreshCb(lv_event_t* event) {
        (void)event;
        fetchAndShow();
    }

    void buildScreen() {
        lv_obj_t* content = nullptr;
        screen = Display::createScreen("Park", &content, closePark);

        resultLabel = lv_label_create(content);
        lv_obj_set_style_text_color(resultLabel, Display::COLOR_ACCENT, 0);
        lv_obj_set_style_text_font(resultLabel, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(resultLabel, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(resultLabel, LV_PCT(100));
        lv_obj_set_style_text_align(resultLabel, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(resultLabel, LV_ALIGN_TOP_MID, 0, 20);

        lv_obj_t* refreshBtn = lv_btn_create(content);
        lv_obj_set_size(refreshBtn, 150, 34);
        lv_obj_align(refreshBtn, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(refreshBtn, lv_color_hex(0x0A1A2A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(refreshBtn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(refreshBtn, Display::COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(refreshBtn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(refreshBtn, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(refreshBtn, refreshCb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* refreshLabel = lv_label_create(refreshBtn);
        lv_label_set_text(refreshLabel, LV_SYMBOL_REFRESH " Aggiorna");
        lv_obj_center(refreshLabel);
        lv_obj_set_style_text_color(refreshLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(refreshLabel, &lv_font_montserrat_14, 0);
    }

} // namespace


namespace ParkUI {

    void open() {
        active = true;

        if (screen != nullptr) {
            lv_obj_del(screen);
            screen = nullptr;
        }

        buildScreen();
        lv_scr_load(screen);

        fetchAndShow();
    }

    bool isActive() {
        return active;
    }

    void update() {
        if (!active) {
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
