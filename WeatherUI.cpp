#include "WeatherUI.h"
#include "Config.h"
#include "Display.h"
#include "Input.h"
#include "GpsUI.h"
#include "GPS.h"
#include "WeatherLookup.h"
#include "ConnStatus.h"

#include <Arduino.h>
#include <lvgl.h>
#include <WiFi.h>

namespace {

    bool active = false;

    lv_obj_t* screen = nullptr;
    lv_obj_t* iconContainer = nullptr;
    lv_obj_t* resultLabel = nullptr;

    constexpr int ICON_SIZE = 96;

    void fetchAndShow();

    void closeWeather() {
        active = false;
        GpsUI::showMain();
    }

    // LVGL non ha glifi meteo pronti (il set LV_SYMBOL_* e' icone UI
    // generiche, niente sole/nuvola/pioggia): disegniamo forme semplici
    // con cerchi/rettangoli arrotondati invece di aggiungere un font o
    // delle immagini nuove (costerebbero flash per un guadagno estetico
    // marginale).
    void drawSun() {
        lv_obj_t* sun = lv_obj_create(iconContainer);
        lv_obj_remove_style_all(sun);
        lv_obj_set_size(sun, 64, 64);
        lv_obj_set_style_radius(sun, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sun, lv_color_hex(0xF1C40F), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sun, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_center(sun);
    }

    void drawCloudShape(lv_color_t color, int yOffset) {
        lv_obj_t* body = lv_obj_create(iconContainer);
        lv_obj_remove_style_all(body);
        lv_obj_set_size(body, 72, 34);
        lv_obj_set_style_radius(body, 17, LV_PART_MAIN);
        lv_obj_set_style_bg_color(body, color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(body, LV_ALIGN_CENTER, 0, yOffset + 6);

        lv_obj_t* bump = lv_obj_create(iconContainer);
        lv_obj_remove_style_all(bump);
        lv_obj_set_size(bump, 34, 34);
        lv_obj_set_style_radius(bump, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bump, color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bump, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(bump, LV_ALIGN_CENTER, -12, yOffset - 8);
    }

    void drawCloud() {
        drawCloudShape(lv_color_hex(0xBDC3C7), 0);
    }

    void drawRain() {
        drawCloudShape(lv_color_hex(0x7F8C8D), -10);

        for (int i = 0; i < 3; i++) {
            lv_obj_t* drop = lv_obj_create(iconContainer);
            lv_obj_remove_style_all(drop);
            lv_obj_set_size(drop, 6, 16);
            lv_obj_set_style_radius(drop, 3, LV_PART_MAIN);
            lv_obj_set_style_bg_color(drop, lv_color_hex(0x3CB4FF), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(drop, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_align(drop, LV_ALIGN_CENTER, (i - 1) * 18, 22);
        }
    }

    void showCondition(WeatherLookup::Condition condition) {
        lv_obj_clean(iconContainer);

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

        lv_label_set_text(resultLabel, text);
    }

    void fetchAndShow() {
        lv_obj_clean(iconContainer);
        lv_label_set_text(resultLabel, "Recupero previsioni meteo...");
        Display::update();

        if (!ConnStatus::isWifiConnected()) {
            // Diagnostica temporanea, vedi lo stesso controllo in
            // ParkUI.cpp: stato WiFi/memoria esatti in questo istante.
            DEBUG_PRINTF(
                "[METEO] WiFi non connesso qui: wifi_stato=%d heap_libero=%u\n",
                static_cast<int>(WiFi.status()), ESP.getFreeHeap()
            );
            lv_label_set_text(resultLabel, "Serve una rete WiFi connessa.");
            return;
        }

        bool hasPosition = (GPS::getLatitude() != 0.0 || GPS::getLongitude() != 0.0);
        if (!hasPosition || GPS::getUtcHour() < 0) {
            lv_label_set_text(resultLabel, "Serve prima un fix GPS valido.");
            return;
        }

        WeatherLookup::Result result = WeatherLookup::fetchNextHour(
            GPS::getLatitude(), GPS::getLongitude(), GPS::getUtcHour()
        );

        if (!result.valid) {
            lv_label_set_text(resultLabel, "Previsioni non disponibili al momento.");
            return;
        }

        showCondition(result.condition);
    }

    void refreshCb(lv_event_t* event) {
        (void)event;
        fetchAndShow();
    }

    void buildScreen() {
        lv_obj_t* content = nullptr;
        screen = Display::createScreen("Meteo", &content, closeWeather);

        iconContainer = lv_obj_create(content);
        lv_obj_remove_style_all(iconContainer);
        lv_obj_set_size(iconContainer, ICON_SIZE, ICON_SIZE);
        lv_obj_clear_flag(iconContainer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(iconContainer, LV_ALIGN_TOP_MID, 0, 4);

        resultLabel = lv_label_create(content);
        lv_obj_set_style_text_color(resultLabel, Display::COLOR_TEXT, 0);
        lv_obj_set_style_text_font(resultLabel, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(resultLabel, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(resultLabel, LV_PCT(100));
        lv_obj_set_style_text_align(resultLabel, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align_to(resultLabel, iconContainer, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

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


namespace WeatherUI {

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
            closeWeather();
        } else if (event == InputEvent::Select) {
            fetchAndShow();
        }
    }
}
