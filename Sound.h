#pragma once

#include <Arduino.h>

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

    // Connessione/disconnessione GATT (vedi BleGatt.h) - non riusa i
    // suoni WiFi apposta: stesso "disegno" (due note) ma CON una pausa
    // vera, cosi' non si confondono a orecchio fra i due sottosistemi.
    void playBleGattConnected();
    void playBleGattDisconnected();

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

    // Rilascia l'I2S (un solo altoparlante fisico, un solo bus) per uso
    // ESCLUSIVO da un altro modulo che deve riconfigurarlo per conto suo
    // (28/09, vedi WebRadioUI.cpp: streaming radio via libreria Audio,
    // stesso principio gia' usato per la radio LoRa/SPI - vedi
    // LoRaRadio::pauseForExternalUse()). Richiamare SEMPRE
    // resumeForOwnUse() al termine, altrimenti i bip di sistema restano
    // muti. Mentre e' in pausa i bip vengono semplicemente scartati (non
    // accodati), come se il volume fosse a 0.
    void pauseForExternalUse();
    void resumeForOwnUse();

    // Riproduce campioni PCM mono 16 bit gia' pronti (29/09, verifica
    // hardware del microfono - vedi Mic.h) - a differenza di playTone()
    // non genera un'onda, manda in uscita esattamente i campioni dati
    // (duplicati su L/R per lo stesso altoparlante mono). BLOCCANTE,
    // stessa durata dell'audio.
    void playRawSamples(const int16_t* samples, size_t sampleCount);
}
