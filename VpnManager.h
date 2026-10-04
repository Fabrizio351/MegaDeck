#pragma once

#include <Arduino.h>

// Tunnel WireGuard verso la rete di casa (vedi VpnConfig.h per la
// configurazione, letta da SD card). Richiede il WiFi gia' connesso
// (la libreria WireGuard-ESP32 lavora sopra la normale connessione
// WiFi, non e' un'interfaccia di rete alternativa) e l'orario di
// sistema sincronizzato via NTP (il protocollo WireGuard lo richiede
// per il proprio handshake).
namespace VpnManager {

    void begin();

    // Da chiamare ad ogni ciclo, SEMPRE (come GPS::update()): guida la
    // sequenza connessione (attesa WiFi -> lettura config -> sync
    // orario -> avvio tunnel) e si ridisattiva da sola se cade il WiFi.
    void update();

    // Interruttore, NON persistente (a differenza di WiFi/LoRa/GPS/
    // BleGatt) - riparte sempre spenta ad ogni avvio, stesso principio
    // di NetGuard. Deciso il 26/09 dopo che un tentativo di connessione
    // avviato in automatico (stato ereditato dalla sessione precedente)
    // ha bloccato il dispositivo appena il WiFi si e' connesso, prima
    // che l'utente potesse intervenire - l'utente deve sempre riattivarla
    // di proposito.
    bool isEnabled();
    void setEnabled(bool enabled);

    // IP da provare a "pingare" per verificare se il tunnel raggiunge
    // davvero la rete di casa (la libreria WireGuard non espone lo
    // stato reale dell'handshake, vedi lo stato Connected sotto) -
    // persistente in NVS, default "192.168.1.1", modificabile dalla UI.
    String getPingTarget();
    void setPingTarget(const String& ip);

    enum class PingState {
        Idle,
        Running,
        Success,
        Failed
    };

    // Avvia una breve sessione di ping (3 tentativi) verso getPingTarget()
    // - non bloccante, l'esito arriva async via getPingState()/
    // getPingLastLatencyMs(). Interrompe una sessione precedente ancora
    // in corso, se c'e'.
    void startPingTest();
    PingState getPingState();
    int getPingLastLatencyMs(); // valido solo quando getPingState()==Success

    // Importa UNA VOLTA la configurazione dalla SD alla memoria interna
    // del T-Deck (vedi VpnConfig::importFromSd()) - dopo un import
    // riuscito la SD si puo' estrarre, non serve piu' per l'uso normale.
    // Bloccante per la parte di montaggio SD (con timeout duro, vedi
    // VpnConfig.cpp) - il chiamante mostra un messaggio prima.
    bool importConfigFromSd();

    // Alternativa (30/09) all'importazione da SD: legge "wireguard.conf"
    // via USB seriale invece che dalla SD (vedi VpnConfig::
    // importFromSerial()) - stesso file, nessuna ridigitazione, e nessun
    // hang possibile visto che non tocca la SD. Bloccante fino a 15s, il
    // chiamante mostra un messaggio "in attesa" prima.
    bool importConfigFromSerial();
    bool hasSavedConfig();

    enum class State {
        Disabled,
        WaitingForWifi,
        ConfigMissing,   // nessuna configurazione importata (vedi importConfigFromSd())
        SyncingTime,
        Connecting,
        // "Connesso" qui significa "interfaccia tunnel avviata" (la
        // libreria non espone lo stato dell'handshake WireGuard vero e
        // proprio) - non e' una conferma di raggiungibilita' reale della
        // rete remota, solo che l'avvio non e' fallito.
        Connected,
        Failed
    };

    State getState();

    // Dettagli non sensibili della configurazione attualmente in uso
    // (vuoti se non ancora caricata) - per la UI, mai le chiavi.
    String getLocalAddress();
    String getEndpointHost();
}
