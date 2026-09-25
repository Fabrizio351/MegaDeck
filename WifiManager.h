#pragma once

#include <Arduino.h>

// Scansione reti e connessione WiFi. Wrapper sottile sulla libreria WiFi
// dell'ESP32, pensato per essere interrogato ogni ciclo da WifiUI senza
// mai bloccare il loop principale (scan e connessione sono asincroni).
namespace WifiManager {

    struct Network {
        String ssid;
        int32_t rssi;   // dBm, piu' vicino a 0 = segnale migliore
        bool open;      // true se la rete non richiede password
    };

    enum class ConnectionState {
        Idle,
        Connecting,
        Connected,
        Failed
    };

    // Avvia una scansione asincrona (non blocca). Se una scansione e'
    // gia' in corso non fa nulla.
    void startScan();

    bool isScanning();

    // Valido solo dopo che isScanning() e' tornato false a seguito di
    // startScan(): numero di reti trovate e relativi dettagli.
    int getNetworkCount();
    Network getNetwork(int index);

    // Avvia una connessione asincrona alla rete indicata. Chiamare
    // getConnectionState() nei cicli successivi per sapere l'esito.
    void connect(const String& ssid, const String& password);

    ConnectionState getConnectionState();

    // Disconnette dalla rete attuale (es. per risparmiare batteria
    // quando il WiFi non serve). Lo stato torna a Idle.
    void disconnect();

    // Validi solo se WiFi.status()==WL_CONNECTED (usare ConnStatus::
    // isWifiConnected() per saperlo): SSID della rete attuale e IP
    // locale assegnato dal router, per mostrarli nella schermata WiFi.
    String getConnectedSsid();
    String getLocalIP();

    // Da chiamare ad ogni ciclo (es. da App::update()): aggiorna lo stato
    // interno di scan/connessione in corso.
    void update();

    // Credenziali salvate in NVS (memoria flash non volatile): permette
    // di riconnettersi a reti gia' usate senza richiedere di nuovo la
    // password. hasSavedPassword()/getSavedPassword() servono a WifiUI
    // per decidere se saltare la schermata password; saveCredentials()
    // va chiamata dopo una connessione riuscita.
    bool hasSavedPassword(const String& ssid);
    String getSavedPassword(const String& ssid);
    void saveCredentials(const String& ssid, const String& password);

    // Rimuove la password salvata per questa rete (es. se il router e'
    // stato riconfigurato con una password diversa e quella salvata non
    // funziona piu': senza un modo per "dimenticarla" non si potrebbe
    // piu' riconnettersi, perche' verrebbe sempre riprovata quella vecchia
    // invece di richiederne una nuova). Non fa nulla se non c'era nulla
    // di salvato per questo ssid.
    void forgetPassword(const String& ssid);

    // Interruttore persistente (NVS) per il risparmio energetico: quando
    // disattivato spegne davvero il radio WiFi (non solo la connessione
    // corrente). connect() non fa nulla se il radio e' disattivato.
    // Sopravvive al riavvio.
    bool isRadioEnabled();
    void setRadioEnabled(bool enabled);
}
