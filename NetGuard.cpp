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
    constexpr int MAX_TRACKED = 30;

    bool enabled = false;
    bool scanInProgress = false;
    uint32_t nextScanMs = 0;

    String alertedMacs[MAX_ALERTED];
    int alertedCount = 0;

    String lastAlertSummary;
    bool pendingAlert = false;

    // Whitelist CONGELATA una sola volta, al momento dell'accensione
    // (vedi captureWhitelist(), chiamata solo da setEnabled(true)) - MAI
    // piu' toccata mentre la sorveglianza resta attiva, nemmeno dai giri
    // periodici successivi che pure riusano lo stesso NetScan::start().
    // Prima del 26/09 veniva ripresa da NetScan::getDeviceCount() ad
    // OGNI giro, subito prima di rilanciare la scansione: dato che
    // NetScan::start() sovrascrive la propria lista con l'ultimo giro
    // (anche quelli automatici di NetGuard), la whitelist "vera" veniva
    // di fatto sostituita ogni minuto con l'ultimo giro invece di
    // restare quella iniziale - bug reale, segnalato dall'utente.
    String whitelistMacs[MAX_SNAPSHOT];
    int whitelistCount = 0;

    // Vera SOLO finche' non arriva un NUOVO NetScan manuale (vedi
    // invalidateWhitelist(), chiamata da NetScanUI): senza questo,
    // spegnere e riaccendere la sorveglianza ricatturava la whitelist da
    // NetScan::getDeviceCount() ogni volta - che pero' a quel punto
    // poteva gia' riflettere l'ULTIMO giro periodico fatto da NetGuard
    // stesso (motore condiviso), non piu' la scansione manuale originale
    // - bug reale, segnalato dall'utente il 26/09.
    bool hasFrozenWhitelist = false;

    // Dispositivi attualmente fuori whitelist, per l'elenco live in UI.
    // Stato per ciascuno: Active (visto nell'ultimo giro completato,
    // rosso) o JustLeft (era Active ma non si e' piu' visto nell'ultimo
    // giro, giallo - tolto dall'elenco al giro successivo se resta
    // assente).
    enum class TrackState { Active, JustLeft };
    String trackedMacs[MAX_TRACKED];
    String trackedIps[MAX_TRACKED];
    TrackState trackedStates[MAX_TRACKED];
    int trackedCount = 0;

    bool isInWhitelist(const String& mac) {
        for (int i = 0; i < whitelistCount; i++) {
            if (whitelistMacs[i].equalsIgnoreCase(mac)) {
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

    int findTracked(const String& mac) {
        for (int i = 0; i < trackedCount; i++) {
            if (trackedMacs[i].equalsIgnoreCase(mac)) {
                return i;
            }
        }
        return -1;
    }

    void removeTrackedAt(int index) {
        for (int i = index; i < trackedCount - 1; i++) {
            trackedMacs[i] = trackedMacs[i + 1];
            trackedIps[i] = trackedIps[i + 1];
            trackedStates[i] = trackedStates[i + 1];
        }
        trackedCount--;
    }

    void triggerAlert(int newCount, const String& details) {
        // Diagnostica (26/09): l'utente ha segnalato di non sentire il
        // suono uscendo dal menu NetGuard - questa riga conferma se
        // l'allarme scatta davvero (quindi il giro periodico gira in
        // sottofondo come da progetto) o se semplicemente non e' mai
        // arrivato nulla di nuovo da segnalare in quella prova.
        DEBUG_PRINTF("[NETGUARD] allarme: %d dispositivo/i nuovo/i, suono in arrivo\n", newCount);
        Sound::playNetGuardAlert();

        String title = newCount == 1
            ? "NetGuard: dispositivo sconosciuto"
            : "NetGuard: dispositivi sconosciuti";

        String msg = "Rilevat" + String(newCount == 1 ? "o " : "i ") + String(newCount)
            + (newCount == 1 ? " dispositivo non in whitelist:\n\n" : " dispositivi non in whitelist:\n\n")
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

    // Chiamata SOLO da setEnabled(true): congela la whitelist per tutta
    // la durata della sorveglianza (vedi il commento sopra whitelistMacs).
    void captureWhitelist() {
        whitelistCount = 0;
        int count = NetScan::getDeviceCount();
        for (int i = 0; i < count && whitelistCount < MAX_SNAPSHOT; i++) {
            whitelistMacs[whitelistCount++] = NetScan::getDevice(i).mac;
        }
    }

    // Chiamata a fine di OGNI giro periodico (mai a inizio giro): calcola
    // chi e' fuori whitelist ORA, aggiorna lo stato rosso/giallo per la
    // UI e fa scattare l'allarme (popup+suono) solo per i MAC mai visti
    // prima in questa sessione di sorveglianza.
    void evaluateAgainstWhitelist() {
        int count = NetScan::getDeviceCount();
        bool seenThisCycle[MAX_TRACKED] = { false };

        String newDevicesText;
        int newCount = 0;

        for (int i = 0; i < count; i++) {
            NetScan::Device dev = NetScan::getDevice(i);
            if (isInWhitelist(dev.mac)) {
                continue;
            }

            int idx = findTracked(dev.mac);
            if (idx >= 0) {
                trackedStates[idx] = TrackState::Active;
                trackedIps[idx] = dev.ip;
                seenThisCycle[idx] = true;
            } else if (trackedCount < MAX_TRACKED) {
                idx = trackedCount++;
                trackedMacs[idx] = dev.mac;
                trackedIps[idx] = dev.ip;
                trackedStates[idx] = TrackState::Active;
                seenThisCycle[idx] = true;
            }

            if (!alreadyAlerted(dev.mac)) {
                markAlerted(dev.mac);
                newDevicesText += dev.ip + "  " + dev.mac + "\n";
                newCount++;
            }
        }

        // Chi non e' stato rivisto in questo giro: se era rosso diventa
        // giallo per questo giro (mostrato cosi' nella UI), se era GIA'
        // giallo (quindi assente per il secondo giro di fila) viene
        // tolto del tutto dall'elenco.
        for (int i = 0; i < trackedCount; ) {
            if (seenThisCycle[i]) {
                i++;
                continue;
            }
            if (trackedStates[i] == TrackState::Active) {
                trackedStates[i] = TrackState::JustLeft;
                i++;
            } else {
                removeTrackedAt(i);
                // seenThisCycle non serve piu' da qui in poi (nessun
                // nuovo confronto), lo shift dell'array non la invalida.
            }
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
        whitelistCount = 0;
        trackedCount = 0;
        hasFrozenWhitelist = false;
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

    int getSecondsUntilNextScan() {
        if (!enabled || scanInProgress) {
            return 0;
        }
        uint32_t now = millis();
        if (nextScanMs <= now) {
            return 0;
        }
        return static_cast<int>((nextScanMs - now) / 1000);
    }

    int getWhitelistCount() {
        return whitelistCount;
    }

    String getLastAlertSummary() {
        return lastAlertSummary;
    }

    bool hasPendingAlert() {
        return pendingAlert;
    }

    bool hasActiveIntruder() {
        for (int i = 0; i < trackedCount; i++) {
            if (trackedStates[i] == TrackState::Active) {
                return true;
            }
        }
        return false;
    }

    int getTrackedCount() {
        return trackedCount;
    }

    TrackedDevice getTracked(int index) {
        TrackedDevice d;
        if (index < 0 || index >= trackedCount) {
            d.justLeft = false;
            return d;
        }
        d.mac = trackedMacs[index];
        d.ip = trackedIps[index];
        d.justLeft = (trackedStates[index] == TrackState::JustLeft);
        return d;
    }

    bool addToWhitelist(const String& mac) {
        if (isInWhitelist(mac)) {
            int idx = findTracked(mac);
            if (idx >= 0) {
                removeTrackedAt(idx);
            }
            return true;
        }
        if (whitelistCount >= MAX_SNAPSHOT) {
            DEBUG_PRINTLN("[NETGUARD] addToWhitelist: whitelist piena, rifiutato");
            return false;
        }

        whitelistMacs[whitelistCount++] = mac;
        DEBUG_PRINTF("[NETGUARD] %s aggiunto manualmente alla whitelist (solo questa sessione)\n", mac.c_str());

        int idx = findTracked(mac);
        if (idx >= 0) {
            removeTrackedAt(idx);
        }
        return true;
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
            trackedCount = 0;
            // Ricatturata SOLO se non ne esiste ancora una (vedi
            // hasFrozenWhitelist) - spegnere e riaccendere la
            // sorveglianza NON deve mai contare come "nuova whitelist",
            // solo un NetScan manuale lo fa (invalidateWhitelist()).
            if (!hasFrozenWhitelist) {
                captureWhitelist();
                hasFrozenWhitelist = true;
            }
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
                evaluateAgainstWhitelist();
                nextScanMs = millis() + SCAN_INTERVAL_MS;
            }
            return;
        }

        if (millis() >= nextScanMs) {
            // NIENTE presa di whitelist qui: e' congelata una volta sola
            // in setEnabled(true), vedi captureWhitelist(). NetScan::
            // start() sovrascrive la propria lista con questo nuovo giro,
            // ma la whitelist di NetGuard resta quella iniziale.
            NetScan::start();
            scanInProgress = true;
        }
    }

    void invalidateWhitelist() {
        hasFrozenWhitelist = false;
    }
}
