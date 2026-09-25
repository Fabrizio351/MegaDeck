#include "App.h"
#include <Arduino.h>
#include <WiFi.h>
#include "Config.h"
#include "CrashLog.h"
#include "Board.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "TextInputUI.h"
#include "Menu.h"
#include "GPS.h"
#include "LoRaRadio.h"
#include "Sound.h"
#include "WifiUI.h"
#include "SettingsUI.h"
#include "NetScan.h"
#include "NetScanUI.h"
#include "NetGuard.h"
#include "NetGuardUI.h"
#include "NetToolsMenu.h"
#include "GpsUI.h"
#include "WeatherUI.h"
#include "ParkUI.h"
#include "LoRaChatUI.h"
#include "BleChat.h"
#include "BleChatUI.h"
#include "ScreenLock.h"

namespace {

    // Etichetta breve per la breadcrumb registrata prima di aprire una
    // voce di menu: se il dispositivo si blocca dentro una schermata,
    // l'ultima riga del log dopo il riavvio dice quale.
    const char* actionTag(MenuAction action) {
        switch (action) {
            case MenuAction::VoIP:          return "menu:VoIP";
            case MenuAction::Wifi:          return "menu:Wifi";
            case MenuAction::Bluetooth:     return "menu:Bluetooth";
            case MenuAction::LoRa:          return "menu:LoRa";
            case MenuAction::Gps:           return "menu:Gps";
            case MenuAction::NetTools:      return "menu:NetTools";
            case MenuAction::Vpn:           return "menu:Vpn";
            case MenuAction::OpenClaw:      return "menu:OpenClaw";
            case MenuAction::HomeAssistant: return "menu:HomeAssistant";
            case MenuAction::Frigate:       return "menu:Frigate";
            case MenuAction::Drone:         return "menu:Drone";
            case MenuAction::Settings:      return "menu:Settings";
            default:                        return "menu:?";
        }
    }

    void handleMenuAction(MenuAction action) {
        CrashLog::breadcrumb(actionTag(action));

        switch (action) {
            case MenuAction::VoIP:
                Display::showMessage(
                    "Gestione Chiamate e Messaggi VoIP",
                    "Inserisci qui la tua funzione",
                    Menu::back
                );
                break;

            case MenuAction::Wifi:
                WifiUI::open();
                break;

            case MenuAction::Bluetooth:
                BleChatUI::open();
                break;

            case MenuAction::LoRa:
                LoRaChatUI::open();
                break;

            case MenuAction::Gps:
                GpsUI::open();
                break;

            case MenuAction::NetTools:
                NetToolsMenu::open();
                break;

            case MenuAction::Vpn:
                Display::showMessage(
                    "VPN",
                    "Inserisci qui la tua funzione",
                    Menu::back
                );
                break;

            case MenuAction::OpenClaw:
                Display::showMessage(
                    "OpenClaw",
                    "Inserisci qui la tua funzione",
                    Menu::back
                );
                break;

            case MenuAction::HomeAssistant:
                Display::showMessage(
                    "Home Assistant",
                    "Inserisci qui la tua funzione",
                    Menu::back
                );
                break;

            case MenuAction::Frigate:
                Display::showMessage(
                    "Frigate",
                    "Inserisci qui la tua funzione",
                    Menu::back
                );
                break;

            case MenuAction::Drone:
                Display::showMessage(
                    "Drone",
                    "Inserisci qui la tua funzione",
                    Menu::back
                );
                break;

            case MenuAction::Settings:
                SettingsUI::open();
                break;

            default:
                break;
        }
    }
}

namespace App {

    void begin() {
        // Il prima possibile, prima di ogni altro modulo: rileva se
        // l'avvio precedente e' terminato con un reset anomalo e archivia
        // la traccia di quella sessione prima di azzerarla per questa.
        CrashLog::begin();

        CrashLog::breadcrumb("Board::begin");
        Board::begin();

        CrashLog::breadcrumb("Display::begin");
        Display::begin();

        // Se l'ultimo riavvio e' stato un crash, mostra un avviso visivo
        // (teschio) per qualche secondo invece del vecchio riepilogo
        // testuale: i dettagli veri (motivo, breadcrumb, coredump con
        // backtrace) restano comunque salvati da CrashLog e leggibili
        // dopo via seriale/NVS (vedi CrashLog::getLastCoreDumpSummary()),
        // non serve piu' mostrarli tutti qui a schermo.
        if (CrashLog::hadCrash()) {
            Display::showCrashSkull();
            Display::update();
            delay(5000);
        }

        CrashLog::breadcrumb("Input::begin");
        Input::begin();

        CrashLog::breadcrumb("GPS::begin");
        GPS::begin();

        CrashLog::breadcrumb("LoRaRadio::begin");
        LoRaRadio::begin();

        CrashLog::breadcrumb("BleChat::begin");
        BleChat::begin();

        CrashLog::breadcrumb("Sound::begin");
        Sound::begin();

        CrashLog::breadcrumb("NetGuard::begin");
        NetGuard::begin();

        ScreenLock::begin();

        Display::showBootScreen();

        CrashLog::breadcrumb("Menu::begin");
        Menu::begin(handleMenuAction);
    }

    void update() {
        Board::update();

        Input::update();

        // Il blocco schermo (Q poi Spazio, vedi ScreenLock.h) va
        // controllato per primo e SEMPRE: se e' attivo, salta del tutto
        // la normale gestione di menu/schermate qui sotto, cosi' nessun
        // input le raggiunge finche' non si sblocca. GPS/LoRa/BT
        // continuano comunque a funzionare in background (vedi sotto),
        // solo schermo e input restano fermi.
        ScreenLock::update();

        if (!ScreenLock::isLocked()) {
            // Mentre la schermata WiFi e' aperta gestisce lei l'input al
            // posto del menu principale (altrimenti Menu::update() muoverebbe
            // in silenzio anche la selezione della griglia dietro le quinte).
            if (WifiUI::isActive()) {
                WifiUI::update();
            } else if (SettingsUI::isActive()) {
                SettingsUI::update();
            } else if (NetScanUI::isActive()) {
                NetScanUI::update();
            } else if (WeatherUI::isActive()) {
                WeatherUI::update();
            } else if (ParkUI::isActive()) {
                ParkUI::update();
            } else if (GpsUI::isActive()) {
                GpsUI::update();
            } else if (NetToolsMenu::isActive()) {
                NetToolsMenu::update();
            } else if (NetGuardUI::isActive()) {
                NetGuardUI::update();
            } else if (LoRaChatUI::isActive()) {
                LoRaChatUI::update();
            } else if (BleChatUI::isActive()) {
                BleChatUI::update();
            } else {
                Menu::update();
            }
        }

        GPS::update();
        LoRaRadio::update();
        BleChat::update();

        // NetScan sempre attiva (non solo con NetScanUI aperta): serve
        // sia alla scansione manuale sia a NetGuard, che la riusa in
        // background per stabilire/aggiornare la whitelist (vedi
        // NetGuard.cpp) mentre l'utente puo' essere su tutt'altra
        // schermata. NetGuard::update() guida il proprio ciclo di
        // sorveglianza periodico e mostra l'allarme (schermo+suono) da
        // solo se trova qualcosa fuori whitelist, qualunque schermata
        // sia attiva in quel momento - stesso principio dei poll() sotto.
        NetScan::update();
        NetGuard::update();

        // Drena i messaggi ricevuti e suona la notifica indipendentemente
        // da quale schermata e' attiva: la ricezione radio prosegue
        // sempre in background, e senza questo la cronologia si
        // aggiornerebbe solo quando la chat e' aperta (con rischio di
        // perdere messaggi arrivati nel frattempo, vedi i commenti nei
        // rispettivi poll()).
        LoRaChatUI::poll();
        BleChatUI::poll();

        // Saltata anch'essa quando bloccato: Display::update() chiama
        // lv_timer_handler(), che elabora i tocchi tramite l'indev LVGL
        // e farebbe scattare comunque i bottoni della schermata
        // "congelata" (i loro callback LV_EVENT_CLICKED non passano dal
        // dispatch qui sopra, quindi saltare solo quello non bastava).
        // Saltare anche il redraw fa risparmiare ulteriore CPU/batteria
        // mentre e' bloccato, coerente con l'obiettivo della funzione.
        if (!ScreenLock::isLocked()) {
            Display::update();
        }

        // Diagnostica temporanea: segnalazioni dell'utente di funzioni
        // che sembrano "non accorgersi" del WiFi dopo l'aumento del pool
        // LVGL - verifica dal vivo se e' un problema di memoria libera
        // in esaurimento sotto carico reale (WiFi+BLE+LoRa+GPS insieme),
        // non riproducibile isolando un solo sottosistema alla volta.
        static uint32_t lastHeapLogMs = 0;
        if (millis() - lastHeapLogMs > 5000) {
            lastHeapLogMs = millis();
            DEBUG_PRINTF(
                "[HEAP] libera=%u minima_mai_vista=%u wifi_stato=%d reset_grezzo=%d\n",
                ESP.getFreeHeap(), ESP.getMinFreeHeap(), static_cast<int>(WiFi.status()),
                CrashLog::getLastRawResetReason()
            );

            // Pool INTERNO di LVGL (64KB fissi, separato dall'heap ESP32
            // sopra): diagnostica per confermare dal vivo se il crash
            // ricorrente durante i redraw (vedi lv_conf.h) e' esaurimento
            // puro o frammentazione (used_pct alto ma free_biggest_size
            // piccolo pur con free_size non trascurabile).
            lv_mem_monitor_t lvMon;
            lv_mem_monitor(&lvMon);
            DEBUG_PRINTF(
                "[LVMEM] usato=%u%% frammentazione=%u%% libero=%u max_blocco_libero=%u\n",
                lvMon.used_pct, lvMon.frag_pct, static_cast<unsigned>(lvMon.free_size),
                static_cast<unsigned>(lvMon.free_biggest_size)
            );

            // Diagnostica schermo nero (2026-09-23/24): valore REALE di
            // luminosita' applicato in questo momento, non solo quello
            // caricato una volta al boot - un log periodico invece che
            // "una tantum" evita del tutto il problema di dover azzeccare
            // la finestra giusta subito dopo un reset (vedi note in
            // project_crash_diagnosis_workflow sulla ri-enumerazione USB).
            DEBUG_PRINTF(
                "[BACKLIGHT] schermo=%d%% tastiera=%d%% screenlock_attivo=%d buffer_lvgl_ok=%d\n",
                Board::getBacklightPercent(), Board::getKeyboardBacklightPercent(),
                ScreenLock::isLocked() ? 1 : 0, Display::areBuffersOk() ? 1 : 0
            );
        }

        // Ristampa periodica della CRONOLOGIA crash (fino agli ultimi 5,
        // non solo l'ultimo): il print "una tantum" dentro CrashLog::
        // begin() si vede solo nella manciata di secondi subito dopo
        // quel preciso avvio, ed e' facile arrivare a collegarsi alla
        // seriale un attimo troppo tardi (la porta USB nativa impiega
        // comunque un istante a ri-enumerarsi dopo un reset). Ripetendolo
        // ogni 30s durante il normale funzionamento, basta collegarsi in
        // un qualunque momento successivo per leggerla, non serve piu'
        // azzeccare l'istante esatto del boot.
        static uint32_t lastCrashLogReprintMs = 0;
        int crashHistoryCount = CrashLog::getCrashHistoryCount();
        if (crashHistoryCount > 0 && millis() - lastCrashLogReprintMs > 30000) {
            lastCrashLogReprintMs = millis();
            DEBUG_PRINTF("[CRASHLOG] ---- cronologia crash (%d, dal piu' recente, ripetuta ogni 30s) ----\n", crashHistoryCount);
            for (int i = 0; i < crashHistoryCount; i++) {
                DEBUG_PRINTF("[CRASHLOG] #%d Motivo: ", i);
                DEBUG_PRINTLN(CrashLog::getCrashHistoryReason(i));
                String breadcrumbLog = CrashLog::getCrashHistoryLog(i);
                if (breadcrumbLog.length() > 0) {
                    DEBUG_PRINTF("[CRASHLOG] #%d Eventi prima del crash:\n", i);
                    DEBUG_PRINTLN(breadcrumbLog);
                }
                String coreDump = CrashLog::getCrashHistorySummary(i);
                if (coreDump.length() > 0) {
                    DEBUG_PRINTLN(coreDump);
                }
            }
            DEBUG_PRINTLN("[CRASHLOG] --------------------------------------------------------------------");
        }

        delay(5);
    }
}
