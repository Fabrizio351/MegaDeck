#include "VpnManager.h"
#include "VpnConfig.h"
#include "Config.h"
#include "ConnStatus.h"

#include <Preferences.h>
#include <WireGuard-ESP32.h>
#include <time.h>
#include <ping/ping_sock.h>

namespace {

    bool runtimeEnabled = false;
    bool nvsOpened = false;
    Preferences vpnPrefs;
    constexpr const char* VPN_PREFS_NAMESPACE = "tdeckvpn";
    constexpr const char* VPN_PREFS_KEY_PING_IP = "pingip";
    constexpr const char* PING_IP_DEFAULT = "192.168.1.1";

    WireGuard wg;
    VpnConfig::Config cfg;
    bool cfgLoaded = false;

    VpnManager::State state = VpnManager::State::Disabled;

    uint32_t ntpStartMs = 0;
    constexpr uint32_t NTP_TIMEOUT_MS = 15000;

    String pingTargetIp;
    esp_ping_handle_t pingHandle = nullptr;
    VpnManager::PingState pingState = VpnManager::PingState::Idle;
    int pingLastLatencyMs = 0;

    // STORIA (27/09): un tentativo di import automatico da SD ~3s dopo
    // il boot (per testare il mount "a freddo" con la SD gia' inserita,
    // invece di aspettare che l'utente navighi manualmente al menu VPN)
    // e' stato provato e tolto lo stesso giorno - si blocca esattamente
    // come l'import manuale (stesso punto, spiTransferByteNL, vedi
    // project_sd_spi_boot_hang_critical in memoria), e ripetendosi ad
    // ogni riavvio produceva un ciclo di crash/riavvio continuo ogni
    // ~8s finche' la SD restava inserita - PEGGIO del comportamento
    // originale (si blocca solo quando l'utente tocca "importa"). Utile
    // pero' come dato diagnostico: il blocco avviene anche "a freddo",
    // subito dopo un avvio pulito, prima che qualunque altra attivita'
    // pesante tocchi il bus - esclude ulteriormente l'ipotesi di
    // contesa/tempistica legata all'uso prolungato del dispositivo.

    void onPingSuccess(esp_ping_handle_t hdl, void*) {
        uint32_t elapsedMs = 0;
        esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsedMs, sizeof(elapsedMs));
        pingLastLatencyMs = static_cast<int>(elapsedMs);
        pingState = VpnManager::PingState::Success;
    }

    void onPingTimeout(esp_ping_handle_t, void*) {
        // Non sovrascrive un Success gia' arrivato da un pacchetto
        // precedente nella stessa sessione (bastano 3 tentativi, 1
        // risposta valida su 3 conta comunque come "raggiungibile").
        if (pingState != VpnManager::PingState::Success) {
            pingState = VpnManager::PingState::Failed;
        }
    }

    void onPingEnd(esp_ping_handle_t hdl, void*) {
        esp_ping_delete_session(hdl);
        pingHandle = nullptr;
    }

    void ensureNvsOpen() {
        if (nvsOpened) {
            return;
        }
        vpnPrefs.begin(VPN_PREFS_NAMESPACE, false);
        // Non letto da NVS apposta (a differenza di WiFi/GPS/LoRa/
        // BleGatt): la VPN NON deve mai ripartire da sola al riavvio,
        // stesso principio gia' usato da NetGuard ("riparte sempre
        // spento") - richiesto 26/09 dopo che un tentativo di
        // connessione avviato in automatico (stato riattivato ereditato
        // dalla sessione precedente) ha bloccato il dispositivo appena
        // il WiFi si e' connesso, prima che l'utente potesse intervenire.
        runtimeEnabled = false;
        pingTargetIp = vpnPrefs.getString(VPN_PREFS_KEY_PING_IP, PING_IP_DEFAULT);
        nvsOpened = true;
    }

    void setState(VpnManager::State s) {
        state = s;
        ConnStatus::setVpnConnected(s == VpnManager::State::Connected);
    }

    // Un epoch ben oltre qualunque data plausibile di compilazione:
    // scarta il valore di default (1 gennaio 1970) che time(nullptr)
    // continua a restituire finche' l'NTP non ha davvero risposto.
    bool isTimeSynced() {
        return time(nullptr) > 1700000000;
    }

    // La rete e' caduta MENTRE la VPN stava connettendosi o era gia'
    // connessa: si disattiva del tutto (non resta "in attesa" a
    // riprovare da sola quando la rete torna) - richiesto 26/09, stesso
    // principio di NetGuard ("si spegne da solo se cade il WiFi").
    // L'utente la riattiva lui quando vuole. Diverso dal semplice "non
    // ha ancora trovato il WiFi la prima volta dopo averla accesa" (quel
    // caso resta in WaitingForWifi normalmente, non e' mai partito nulla
    // di rischioso finora).
    void disableDueToNetworkLoss() {
        DEBUG_PRINTLN("[VPN] rete persa durante la connessione - disattivata, va riattivata a mano");
        runtimeEnabled = false;
        setState(VpnManager::State::Disabled);
    }

    void startConnecting() {
        DEBUG_PRINTLN("[VPN] diagnostica: startConnecting() chiamata");
        if (!cfgLoaded) {
            // loadSaved(): mai la SD qui, solo la copia gia' importata
            // in NVS (vedi importConfigFromSd()) - l'uso normale non
            // deve mai piu' rischiare il blocco SD di startup.
            cfg = VpnConfig::loadSaved();
            cfgLoaded = true;
        }
        DEBUG_PRINTF("[VPN] diagnostica: config caricata, valido=%d\n", cfg.valid ? 1 : 0);

        if (!cfg.valid) {
            setState(VpnManager::State::ConfigMissing);
            return;
        }

        // L'handshake WireGuard richiede un orologio di sistema valido -
        // vedi il README della libreria. configTime() e' non bloccante,
        // la sincronizzazione vera avviene in background: si aspetta qui
        // (con un timeout) prima di avviare il tunnel.
        DEBUG_PRINTLN("[VPN] diagnostica: prima di configTime()");
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        DEBUG_PRINTLN("[VPN] diagnostica: dopo configTime()");
        ntpStartMs = millis();
        setState(VpnManager::State::SyncingTime);
    }

}

namespace VpnManager {

    void begin() {
        ensureNvsOpen();
        state = State::Disabled;
    }

    void update() {
        if (!runtimeEnabled) {
            return;
        }

        switch (state) {
            case State::Disabled:
            case State::WaitingForWifi: {
                static uint32_t lastPrintMs = 0;
                if (millis() - lastPrintMs > 2000) {
                    lastPrintMs = millis();
                    DEBUG_PRINTF(
                        "[VPN] diagnostica: WaitingForWifi/Disabled - runtimeEnabled=%d wifi_connesso=%d stato=%d\n",
                        runtimeEnabled ? 1 : 0, ConnStatus::isWifiConnected() ? 1 : 0, static_cast<int>(state)
                    );
                }
                if (!ConnStatus::isWifiConnected()) {
                    setState(State::WaitingForWifi);
                    return;
                }
                startConnecting();
                break;
            }

            case State::SyncingTime: {
                if (!ConnStatus::isWifiConnected()) {
                    disableDueToNetworkLoss();
                    return;
                }

                static uint32_t lastPrintMs = 0;
                if (millis() - lastPrintMs > 2000) {
                    lastPrintMs = millis();
                    DEBUG_PRINTF(
                        "[VPN] diagnostica: in attesa NTP, time(nullptr)=%ld trascorsi=%lums\n",
                        static_cast<long>(time(nullptr)), millis() - ntpStartMs
                    );
                }

                if (isTimeSynced()) {
                    IPAddress localIp;
                    localIp.fromString(cfg.localAddress);

                    DEBUG_PRINTF(
                        "[VPN] diagnostica: prima di wg.begin() localIp=%s endpoint=%s:%d\n",
                        localIp.toString().c_str(), cfg.endpointHost.c_str(), cfg.endpointPort
                    );
                    bool ok = wg.begin(
                        localIp,
                        cfg.privateKey.c_str(),
                        cfg.endpointHost.c_str(),
                        cfg.peerPublicKey.c_str(),
                        static_cast<uint16_t>(cfg.endpointPort)
                    );
                    DEBUG_PRINTF("[VPN] diagnostica: dopo wg.begin(), ok=%d\n", ok ? 1 : 0);

                    setState(ok ? State::Connected : State::Failed);
                } else if (millis() - ntpStartMs > NTP_TIMEOUT_MS) {
                    DEBUG_PRINTLN("[VPN] sincronizzazione orario NTP scaduta, tunnel non avviato");
                    setState(State::Failed);
                }
                break;
            }

            case State::Connected:
                if (!ConnStatus::isWifiConnected()) {
                    wg.end();
                    disableDueToNetworkLoss();
                }
                break;

            case State::ConfigMissing:
            case State::Failed:
                // Resta cosi' finche' l'utente non riprova esplicitamente
                // (spegni/riaccendi dalla schermata VPN) - niente retry
                // automatico continuo per un errore che quasi certamente
                // richiede un intervento umano (SD assente, file
                // incompleto, endpoint irraggiungibile).
                break;

            default:
                break;
        }
    }

    bool isEnabled() {
        ensureNvsOpen();
        return runtimeEnabled;
    }

    void setEnabled(bool enabled) {
        ensureNvsOpen();
        if (runtimeEnabled == enabled) {
            return;
        }
        // Non salvato in NVS di proposito - vedi ensureNvsOpen().
        runtimeEnabled = enabled;

        if (enabled) {
            cfgLoaded = false;
            setState(State::WaitingForWifi);
        } else {
            if (state == State::Connected) {
                wg.end();
            }
            setState(State::Disabled);
        }
    }

    bool importConfigFromSd() {
        bool ok = VpnConfig::importFromSd();
        if (ok) {
            // Forza una rilettura dalla copia appena importata al
            // prossimo tentativo di connessione, invece di restare su
            // un eventuale cfg/cfgLoaded gia' in memoria da prima.
            cfgLoaded = false;
        }
        return ok;
    }

    bool importConfigFromSerial() {
        bool ok = VpnConfig::importFromSerial();
        if (ok) {
            cfgLoaded = false;
        }
        return ok;
    }

    bool hasSavedConfig() {
        return VpnConfig::hasSavedConfig();
    }

    State getState() {
        return state;
    }

    String getLocalAddress() {
        return cfg.localAddress;
    }

    String getEndpointHost() {
        return cfg.endpointHost;
    }

    String getPingTarget() {
        ensureNvsOpen();
        return pingTargetIp;
    }

    void setPingTarget(const String& ip) {
        ensureNvsOpen();
        pingTargetIp = ip;
        vpnPrefs.putString(VPN_PREFS_KEY_PING_IP, ip);
    }

    void startPingTest() {
        if (pingHandle != nullptr) {
            esp_ping_stop(pingHandle);
            esp_ping_delete_session(pingHandle);
            pingHandle = nullptr;
        }

        IPAddress target;
        if (!target.fromString(getPingTarget())) {
            DEBUG_PRINTF("[VPN] indirizzo ping non valido: %s\n", pingTargetIp.c_str());
            pingState = PingState::Failed;
            return;
        }

        ip_addr_t addr;
        addr.type = IPADDR_TYPE_V4;
        addr.u_addr.ip4.addr = static_cast<uint32_t>(target);

        esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
        config.target_addr = addr;
        config.count = 3;
        config.timeout_ms = 1500;
        config.interval_ms = 400;

        esp_ping_callbacks_t cbs = {};
        cbs.on_ping_success = onPingSuccess;
        cbs.on_ping_timeout = onPingTimeout;
        cbs.on_ping_end = onPingEnd;

        pingState = PingState::Running;
        DEBUG_PRINTF("[VPN] diagnostica: avvio ping verso %s\n", pingTargetIp.c_str());

        if (esp_ping_new_session(&config, &cbs, &pingHandle) != ESP_OK
            || esp_ping_start(pingHandle) != ESP_OK) {
            DEBUG_PRINTLN("[VPN] avvio sessione ping fallito");
            pingState = PingState::Failed;
            pingHandle = nullptr;
            return;
        }
    }

    PingState getPingState() {
        return pingState;
    }

    int getPingLastLatencyMs() {
        return pingLastLatencyMs;
    }
}
