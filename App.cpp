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
#include "TelnetUI.h"
#include "GpsUI.h"
#include "WeatherUI.h"
#include "ParkUI.h"
#include "LoRaScanUI.h"
#include "LoRaWanUI.h"
#include "MeshtasticUI.h"
#include "LoRaToolsMenu.h"
#include "BleGatt.h"
#include "BleGattUI.h"
#include "VpnManager.h"
#include "VpnUI.h"
#include "OpenClawUI.h"
#include "WebRadioUI.h"
#include "ScreenLock.h"

namespace {

    // Etichetta breve per la breadcrumb registrata prima di aprire una
    // voce di menu: se il dispositivo si blocca dentro una schermata,
    // l'ultima riga del log dopo il riavvio dice quale.
    const char* actionTag(MenuAction action) {
        switch (action) {
            case MenuAction::VoIP:          return "menu:VoIP";
            case MenuAction::Wifi:          return "menu:Wifi";
            case MenuAction::BleGatt:       return "menu:BleGatt";
            case MenuAction::LoRa:          return "menu:LoRa";
            case MenuAction::Gps:           return "menu:Gps";
            case MenuAction::NetTools:      return "menu:NetTools";
            case MenuAction::Vpn:           return "menu:Vpn";
            case MenuAction::OpenClaw:      return "menu:OpenClaw";
            case MenuAction::Settings:      return "menu:Settings";
            case MenuAction::WebRadio:      return "menu:WebRadio";
            case MenuAction::About:         return "menu:About";
            default:                        return "menu:?";
        }
    }

    void handleMenuAction(MenuAction action) {
        CrashLog::breadcrumb(actionTag(action));

        switch (action) {
            case MenuAction::Wifi:
                WifiUI::open();
                break;

            case MenuAction::LoRa:
                LoRaToolsMenu::open();
                break;

            case MenuAction::Gps:
                GpsUI::open();
                break;

            case MenuAction::NetTools:
                NetToolsMenu::open();
                break;

            case MenuAction::Settings:
                SettingsUI::open();
                break;

            case MenuAction::BleGatt:
                BleGattUI::open();
                break;

            case MenuAction::Vpn:
                VpnUI::open();
                break;

            case MenuAction::About: {
                // Una riga per ogni informazione, richiesto 26/09 per
                // avere a colpo d'occhio versione+data di compilazione
                // (utile dopo tante prove compila/carica in una sessione)
                // e i dati hardware significativi (MAC delle due radio,
                // chip, flash/PSRAM, heap libero).
                String msg;
                msg += String(APP_NAME) + " v" + APP_VERSION + "\n";
                msg += "Compilato: " + String(__DATE__) + " " + String(__TIME__) + "\n";
                msg += "Chip: " + String(ESP.getChipModel()) + " rev" + String(ESP.getChipRevision())
                    + " @ " + String(ESP.getCpuFreqMHz()) + "MHz\n";
                msg += "Flash: " + String(ESP.getFlashChipSize() / (1024 * 1024)) + "MB\n";
                uint32_t psramSize = ESP.getPsramSize();
                msg += "PSRAM: " + (psramSize > 0 ? String(psramSize / (1024 * 1024)) + "MB" : String("non rilevata")) + "\n";
                msg += "Heap libero: " + String(ESP.getFreeHeap() / 1024) + "KB\n";
                msg += "MAC WiFi: " + WiFi.macAddress() + "\n";
                String bleMac = BleGatt::getMacAddress();
                msg += "MAC BLE: " + (bleMac.length() > 0 ? bleMac : String("non disponibile (BLE spento)"));

                // Firma (richiesta 26/09) - teschietto in ASCII: il font
                // di sistema (setFont(nullptr), usato in tutta l'app) e'
                // solo ASCII, nessun emoji/unicode disponibile.
                msg += "\n\n(x.x) creato da bix351 + Claude Code";

                Display::showMessage("Info", msg.c_str(), []() { Menu::showMain(); });
                break;
            }

            case MenuAction::OpenClaw:
                OpenClawUI::open();
                break;

            case MenuAction::WebRadio:
                WebRadioUI::open();
                break;

            // VoIP: non ancora implementata (Home Assistant/Frigate/Drone
            // tolte del tutto dal menu il 26/09, mai implementate - la
            // loro gestione passa da OpenClaw, vedi OpenClawUI.cpp).
            // Prima mostrava un popup LVGL "Inserisci qui la tua
            // funzione" - eliminato (26/09) su richiesta, resta una voce
            // di menu presente ma senza ancora una destinazione: il tap
            // non fa nulla per ora. Menu::selectCurrentItem() disattiva
            // SEMPRE il disegno diretto prima di dispatchare qui (residuo
            // del periodo LVGL, vedi Menu.cpp) - per una voce reale ci
            // pensa il proprio open(), ma per questa, che non apre nulla,
            // nessuno lo riattivava piu': l'header restava congelato allo
            // stato del momento (sembrava un blocco generale, segnalato
            // 26/09, in realta' tutto il resto restava reattivo).
            // Il test hardware del microfono (29/09) e' stato spostato
            // dentro OpenClawUI.cpp (barra spaziatrice in composizione,
            // vedi triggerVoiceTest()) - qui VoIP torna un placeholder
            // senza destinazione, come prima.
            case MenuAction::VoIP:
            default:
                Display::setDirectDrawMode(true);
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

        CrashLog::breadcrumb("BleGatt::begin");
        BleGatt::begin();

        CrashLog::breadcrumb("Sound::begin");
        Sound::begin();

        CrashLog::breadcrumb("NetGuard::begin");
        NetGuard::begin();

        CrashLog::breadcrumb("VpnManager::begin");
        VpnManager::begin();

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
            // Un messaggio (Display::showMessage) intercetta SEMPRE
            // l'input per primo, qualunque sia la schermata "attiva" per
            // il resto del dispatch: molte chiamate a showMessage()
            // arrivano da un open() che ritorna subito dopo (es. "prima
            // connettiti al WiFi"), senza che nessun modulo specifico
            // resti isActive() per intercettare Indietro/Select/tap.
            if (Display::isMessageActive()) {
                Display::updateMessage();
            } else if (WifiUI::isActive()) {
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
            } else if (TelnetUI::isActive()) {
                TelnetUI::update();
            } else if (LoRaToolsMenu::isActive()) {
                LoRaToolsMenu::update();
            } else if (LoRaScanUI::isActive()) {
                LoRaScanUI::update();
            } else if (LoRaWanUI::isActive()) {
                LoRaWanUI::update();
            } else if (MeshtasticUI::isActive()) {
                MeshtasticUI::update();
            } else if (BleGattUI::isActive()) {
                BleGattUI::update();
            } else if (VpnUI::isActive()) {
                VpnUI::update();
            } else if (OpenClawUI::isActive()) {
                OpenClawUI::update();
            } else if (WebRadioUI::isActive()) {
                WebRadioUI::update();
            } else {
                Menu::update();
            }
        }

        GPS::update();
        BleGatt::update();

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
        VpnManager::update();

        // Web Radio: il motore dello streaming gira sempre, non solo con
        // la sua schermata aperta (29/09, richiesto: sentirla mentre si
        // fa altro) - stesso principio degli update() sopra.
        WebRadioUI::backgroundUpdate();

        // Meshtastic (02/10, richiesto: poter mettere la chat in secondo
        // piano per fare altro col T-Deck) - drena i pacchetti radio e
        // aggiorna NodeDB/storico/notifiche indipendentemente da quale
        // schermata e' in primo piano, ma SOLO se l'utente ha davvero
        // attivato il secondo piano (altrimenti, come sempre finora,
        // Meshtastic ascolta solo quando la sua schermata e' aperta -
        // stesso limite di condivisione della radio unica di sempre).
        MeshtasticUI::backgroundUpdate();

        // Saltata anch'essa quando bloccato: Display::update() chiama
        // lv_timer_handler(), che elabora i tocchi tramite l'indev LVGL
        // e farebbe scattare comunque i bottoni della schermata
        // "congelata" (i loro callback LV_EVENT_CLICKED non passano dal
        // dispatch qui sopra, quindi saltare solo quello non bastava).
        // Saltare anche il redraw fa risparmiare ulteriore CPU/batteria
        // mentre e' bloccato, coerente con l'obiettivo della funzione.
        if (!ScreenLock::isLocked()) {
            // Ridisegna la barra in alto da sola quando qualcosa cambia
            // (wifi/vpn/ble/lora/netguard/batteria/ora), anche se la
            // schermata attiva non si sta ridisegnando per conto suo -
            // altrimenti restava "congelata" allo stato dell'ultimo
            // ridisegno finche' non arrivava un input (vedi Display.h).
            Display::refreshHeaderIfNeeded();
            Display::update();
        }

        // Diagnostica periodica: memoria libera reale sotto carico
        // (WiFi+BLE+LoRa+GPS insieme), non riproducibile isolando un
        // solo sottosistema alla volta.
        static uint32_t lastHeapLogMs = 0;
        if (millis() - lastHeapLogMs > 5000) {
            lastHeapLogMs = millis();
            DEBUG_PRINTF(
                "[HEAP] libera=%u minima_mai_vista=%u wifi_stato=%d reset_grezzo=%d\n",
                ESP.getFreeHeap(), ESP.getMinFreeHeap(), static_cast<int>(WiFi.status()),
                CrashLog::getLastRawResetReason()
            );

            // Diagnostica schermo nero (2026-09-23/24): valore REALE di
            // luminosita' applicato in questo momento, non solo quello
            // caricato una volta al boot - un log periodico invece che
            // "una tantum" evita del tutto il problema di dover azzeccare
            // la finestra giusta subito dopo un reset (vedi note in
            // project_crash_diagnosis_workflow sulla ri-enumerazione USB).
            // psram_ok: vedi Display::areBuffersOk() - prova di
            // allocazione PSRAM fatta in Display::begin() (prima
            // verificava i buffer LVGL, ora e' un test diretto, stesso
            // scopo: la causa storica dello schermo nero silenzioso era
            // il flag PSRAM=opi mancante in compilazione).
            DEBUG_PRINTF(
                "[BACKLIGHT] schermo=%d%% tastiera=%d%% screenlock_attivo=%d psram_ok=%d disegno_diretto=%d\n",
                Board::getBacklightPercent(), Board::getKeyboardBacklightPercent(),
                ScreenLock::isLocked() ? 1 : 0, Display::areBuffersOk() ? 1 : 0,
                Display::isDirectDrawMode() ? 1 : 0
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
