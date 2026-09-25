#include "NetGuardUI.h"
#include "NetGuard.h"
#include "NetScan.h"
#include "Display.h"
#include "Input.h"
#include "Menu.h"
#include "NetToolsMenu.h"
#include "ConnStatus.h"

#include <Arduino.h>
#include <lvgl.h>

namespace {

    bool active = false;

    lv_obj_t* screen = nullptr;
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* toggleBtn = nullptr;
    lv_obj_t* toggleLabel = nullptr;

    // Ultimo testo assegnato a statusLabel: refreshStatusLabel() viene
    // chiamata ad OGNI ciclo di App::update() mentre la schermata resta
    // aperta (lo stato puo' cambiare da solo, es. un allarme), ma
    // lv_label_set_text() rialloca sempre il buffer di testo nel pool
    // LVGL anche quando il contenuto e' identico - con questa schermata
    // lasciata aperta a lungo (e' pensata per restare visibile come
    // "dashboard"), farlo incondizionatamente martellava il pool da 64KB
    // decine di volte al secondo per nulla, contribuendo direttamente
    // agli "pool memoria esaurito" osservati durante l'uso reale (vedi
    // project_lvgl_pool_exhaustion_fix in memoria). Confrontando col
    // testo precedente si aggiorna solo quando cambia davvero.
    String lastStatusText;

    void closeNetGuard() {
        active = false;
        NetToolsMenu::showMain();
    }

    String buildStatusText() {
        String text;
        int whitelistCount = NetGuard::getWhitelistCount();

        if (!NetGuard::isEnabled()) {
            text = "Sorveglianza disattivata.";
        } else if (NetGuard::isWaitingForWhitelist()) {
            text = "In attesa: nessuna whitelist. Fai prima un NetScan.";
        } else if (NetGuard::isScanning()) {
            text = "Giro di controllo in corso... " + String(NetScan::getProgressPercent()) + "%";
        } else {
            text = "Sorveglianza attiva (controllo ogni minuto).";
        }

        text += "\n\nWhitelist: " + String(whitelistCount) + " dispositivi";
        if (whitelistCount == 0) {
            text += "\n(vai su NetScan per crearla)";
        }

        String lastAlert = NetGuard::getLastAlertSummary();
        if (lastAlert.length() > 0) {
            text += "\n\nUltimo allarme:\n" + lastAlert;
        }

        return text;
    }

    void refreshStatusLabel() {
        if (statusLabel != nullptr) {
            String newText = buildStatusText();
            if (newText != lastStatusText) {
                lastStatusText = newText;
                lv_label_set_text(statusLabel, newText.c_str());
            }
        }
    }

    void refreshToggleLabel() {
        if (toggleLabel != nullptr) {
            lv_label_set_text(
                toggleLabel,
                NetGuard::isEnabled()
                    ? LV_SYMBOL_STOP " Disattiva sorveglianza"
                    : LV_SYMBOL_OK " Attiva sorveglianza"
            );
        }
    }

    void toggleCb(lv_event_t* event) {
        (void)event;

        bool wantEnabled = !NetGuard::isEnabled();
        bool ok = NetGuard::setEnabled(wantEnabled);

        if (wantEnabled && !ok) {
            // Nessuna whitelist ancora: NetGuard non puo' scansionare da
            // solo per crearla (solo NetScan lo fa), quindi si spiega
            // il motivo invece di attivare silenziosamente nulla.
            Display::showMessage(
                "NetGuard",
                "Serve prima una whitelist: apri NetScan e fai una scansione, poi torna qui ad attivare la sorveglianza.",
                []() { NetGuardUI::open(); }
            );
            return;
        }

        refreshToggleLabel();
        refreshStatusLabel();
    }

    void buildScreen() {
        lv_obj_t* content = nullptr;
        screen = Display::createScreen("NetGuard", &content, closeNetGuard);

        statusLabel = lv_label_create(content);
        lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(statusLabel, Display::COLOR_TEXT, 0);
        lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(statusLabel, LV_PCT(100));
        lv_obj_align(statusLabel, LV_ALIGN_TOP_LEFT, 0, 0);

        toggleBtn = lv_btn_create(content);
        lv_obj_set_width(toggleBtn, LV_PCT(100));
        lv_obj_set_height(toggleBtn, 34);
        lv_obj_align(toggleBtn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_bg_color(toggleBtn, Display::COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(toggleBtn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(toggleBtn, Display::COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_border_width(toggleBtn, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(toggleBtn, 4, LV_PART_MAIN);
        lv_obj_add_event_cb(toggleBtn, toggleCb, LV_EVENT_CLICKED, nullptr);

        toggleLabel = lv_label_create(toggleBtn);
        lv_obj_center(toggleLabel);
        lv_obj_set_style_text_color(toggleLabel, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(toggleLabel, &lv_font_montserrat_14, 0);

        // statusLabel e' appena stato ricreato: forza il primo refresh
        // (altrimenti, se il testo coincidesse per caso con l'ultimo
        // assegnato alla schermata precedente, refreshStatusLabel()
        // salterebbe l'aggiornamento e la nuova label resterebbe vuota).
        lastStatusText = "";
        refreshToggleLabel();
        refreshStatusLabel();
    }

} // namespace

namespace NetGuardUI {

    void open() {
        if (!ConnStatus::isWifiConnected()) {
            Display::showMessage(
                "NetGuard",
                "Devi prima connetterti a una rete WiFi: NetGuard sorveglia i dispositivi sulla rete locale.",
                []() { NetToolsMenu::showMain(); }
            );
            return;
        }

        active = true;

        if (screen != nullptr) {
            lv_obj_del(screen);
            screen = nullptr;
        }

        buildScreen();
        lv_scr_load(screen);
    }

    bool isActive() {
        return active;
    }

    void update() {
        if (!active) {
            return;
        }

        // Lo stato (whitelist, ultimo allarme) puo' cambiare da solo
        // mentre la schermata resta aperta: aggiornato ad ogni ciclo.
        refreshStatusLabel();

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Select:
                toggleCb(nullptr);
                break;

            case InputEvent::Back:
                closeNetGuard();
                break;

            default:
                break;
        }
    }
}
