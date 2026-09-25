#pragma once

// Bip brevi via l'altoparlante I2S integrato del T-Deck, per segnalare
// eventi senza dover guardare lo schermo (connessione/disconnessione
// WiFi, messaggi LoRa/BT ricevuti).
namespace Sound {
    void begin();

    void playWifiConnected();
    void playWifiDisconnected();

    // Doppio bip, un tono diverso per canale cosi' si riconosce a
    // orecchio da dove arriva la notifica senza guardare lo schermo.
    void playLoRaMessageReceived();
    void playBleMessageReceived();

    // Conferma di blocco/sblocco schermo (vedi ScreenLock.h): utile
    // proprio perche' lo schermo si spegne, quindi serve un modo per
    // sapere che il comando e' stato riconosciuto senza guardarlo.
    void playScreenLocked();
    void playScreenUnlocked();

    // Allarme NetGuard (vedi NetGuard.h): tre bip decisi, volutamente
    // piu' insistenti e distinguibili dai bip "informativi" di sopra,
    // trattandosi di una notifica di sicurezza.
    void playNetGuardAlert();

    // Volume persistente (NVS), 0-100. 0 = silenziato del tutto (nessun
    // suono viene generato, non solo abbassato al minimo udibile).
    // Sopravvive al riavvio.
    int getVolumePercent();
    void setVolumePercent(int percent);
}
