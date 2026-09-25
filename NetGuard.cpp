#include "NetGuard.h"
#include "NetScan.h"
#include "Sound.h"
#include "Display.h"
#include "Menu.h"
#include "Config.h"

#include <WiFi.h>

namespace {
    // Ogni quanto ripetere il giro di sorveglianza mentre attivo. Una
    // scansione completa dei 254 host della sottorete dura circa 20s
    // (vedi ArpSweep::ARP_WAIT_MS): questo e' l'intervallo fra la FINE
    // di un giro e l'inizio del successivo, non fra un inizio e l'altro.
    // 1 minuto invece di 3: piu' dispendioso (radio/batteria), ma
    // NetGuard va acceso solo in caso di sospetta intrusione, non
    // lasciato attivo di continuo - l'utente preferisce la reattivita'.
    constexpr uint32_t SCAN_INTERVAL_MS = 60000; // 1 minuto

    // Deve coprire il MAX_DEVICES privato di ArpSweep (60): non e'
    // esposto pubblicamente, quindi lo ridichiariamo qui con lo stesso
    // valore invece di dipendere da un dettaglio interno di un altro
    // modulo.
    constexpr int MAX_SNAPSHOT = 60;

    constexpr int MAX_ALERTED = 30;

    bool enabled = false;
    bool scanInProgress = false;
    uint32_t nextScanMs = 0;

    String alertedMacs[MAX_ALERTED];
    int alertedCount = 0;

    String lastAlertSummary;
    bool pendingAlert = false;

    // Istantanea della whitelist presa subito PRIMA di ogni giro di
    // sorveglianza: pura copia di String, nessuna interazione con lo
    // stack di rete. NetScan::start() la sovrascrive durante la
    // scansione, quindi va salvata prima o andrebbe persa.
    String snapshotMacs[MAX_SNAPSHOT];
    int snapshotCount = 0;

    bool isInSnapshot(const String& mac) {
        for (int i = 0; i < snapshotCount; i++) {
            if (snapshotMacs[i].equalsIgnoreCase(mac)) {
                return true;
            }
        }
        return false;
    }

    bool alreadyAlerted(const String& mac) {
        for (int i = 0; i < alertedCount; i++) {
            if (alertedMacs[i].equalsIgnoreCase(mac)) {
                return true;
            }
        }
        return false;
    }

    void markAlerted(const String& mac) {
        if (alertedCount >= MAX_ALERTED) {
            return;
        }
        alertedMacs[alertedCount++] = mac;
    }

    void triggerAlert(int newCount, const String& details) {
        Sound::playNetGuardAlert();

        String title = newCount == 1
            ? "NetGuard: dispositivo sconosciuto"
            : "NetGuard: dispositivi sconosciuti";

        String msg = "Rilevat" + String(newCount == 1 ? "o " : "i ") + String(newCount)
            + (newCount == 1 ? " dispositivo nuovo rispetto al giro precedente:\n\n" : " dispositivi nuovi rispetto al giro precedente:\n\n")
            + details;

        lastAlertSummary = details;
        pendingAlert = true;

        // Questo allarme puo' scattare da QUALUNQUE schermata, in
        // qualunque momento (NetGuard::update() gira sempre, non solo
        // con la sua UI aperta - vedi il commento in App.cpp) - anche
        // mentre e' attiva una schermata disegnata a mano (NetScan,
        // NetGuard stessa, GPS...), che ha messo LVGL in pausa. Senza
        // questa riga l'avviso veniva costruito ma mai disegnato ne'
        // reso interattivo (LVGL restava in pausa): lo schermo sembrava
        // bloccato pur con tutto il resto (audio compreso) perfettamente
        // funzionante - bug reale, osservato il 25/09 durante scansioni
        // NetScan ripetute.
        Display::setDirectDrawMode(false);

        // Chiudere questa schermata (Indietro/tap/Select) e' l'azione di
        // "acknowledge": da li' in poi l'icona in alto torna del colore
        // normale, vedi hasPendingAlert().
        Display::showMessage(title.c_str(), msg.c_str(), []() {
            pendingAlert = false;
            Menu::back();
        });
    }

    void takeSnapshot() {
        snapshotCount = 0;
        int count = NetScan::getDeviceCount();
        for (int i = 0; i < count && snapshotCount < MAX_SNAPSHOT; i++) {
            snapshotMacs[snapshotCount++] = NetScan::getDevice(i).mac;
        }
    }

    void evaluateAgainstSnapshot() {
        int count = NetScan::getDeviceCount();
        String newDevicesText;
        int newCount = 0;

        for (int i = 0; i < count; i++) {
            NetScan::Device dev = NetScan::getDevice(i);

            if (isInSnapshot(dev.mac) || alreadyAlerted(dev.mac)) {
                continue;
            }

            markAlerted(dev.mac);
            newDevicesText += dev.ip + "  " + dev.mac + "\n";
            newCount++;
        }

        if (newCount > 0) {
            triggerAlert(newCount, newDevicesText);
        }
    }
}

namespace NetGuard {

    void begin() {
        // Non persistito volutamente: riparte sempre spento ad ogni
        // avvio, l'utente lo riaccende esplicitamente quando vuole.
        enabled = false;
        scanInProgress = false;
    }

    bool isEnabled() {
        return enabled;
    }

    bool isWaitingForWhitelist() {
        return enabled && NetScan::getDeviceCount() == 0;
    }

    bool isScanning() {
        return enabled && scanInProgress;
    }

    int getWhitelistCount() {
        return NetScan::getDeviceCount();
    }

    String getLastAlertSummary() {
        return lastAlertSummary;
    }

    bool hasPendingAlert() {
        return pendingAlert;
    }

    bool setEnabled(bool newEnabled) {
        if (newEnabled && NetScan::getDeviceCount() == 0) {
            // Niente whitelist: non c'e' nulla da controllare - il
            // chiamante mostra il motivo all'utente.
            DEBUG_PRINTF("[NETGUARD] setEnabled(true) rifiutato: whitelist vuota (device_count=%d)\n", NetScan::getDeviceCount());
            return false;
        }

        if (enabled == newEnabled) {
            return true;
        }

        DEBUG_PRINTF("[NETGUARD] enabled: %d -> %d (wifi_status=%d, device_count=%d)\n", enabled, newEnabled, WiFi.status(), NetScan::getDeviceCount());
        enabled = newEnabled;
        scanInProgress = false;

        if (newEnabled) {
            alertedCount = 0;
            lastAlertSummary = "";
            pendingAlert = false;
            // Primo giro presto (5s), non subito: da' tempo alla UI di
            // mostrare "attivo" prima che lo schermo passi eventualmente
            // a NetScan durante la scansione, se la finestra whitelist
            // e' aperta in quel momento.
            nextScanMs = millis() + 5000;
        }

        return true;
    }

    void update() {
        if (!enabled) {
            return;
        }

        // Senza rete si spegne per davvero (non solo "in pausa"):
        // l'icona in alto deve restare onesta sullo stato reale.
        if (WiFi.status() != WL_CONNECTED) {
            setEnabled(false);
            return;
        }

        if (scanInProgress) {
            // NetScan::update() la fa avanzare (chiamata sempre da
            // App::update()): qui aspettiamo solo che finisca.
            if (!NetScan::isScanning()) {
                scanInProgress = false;
                evaluateAgainstSnapshot();
                nextScanMs = millis() + SCAN_INTERVAL_MS;
            }
            return;
        }

        if (millis() >= nextScanMs) {
            takeSnapshot();
            NetScan::start();
            scanInProgress = true;
        }
    }
}
