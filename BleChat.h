#pragma once

#include <Arduino.h>

// Chat broadcast via Bluetooth LE: advertising (invio) + scansione
// continua (ricezione), nessuna connessione GATT vera e propria - stesso
// schema "tutti sentono tutti" della chat LoRa, ma via BLE. Pensato
// come canale di riserva a corto raggio (decine di metri, non km come
// il LoRa), utile ad esempio quando si e' vicini ad altri T-Deck ma
// fuori portata comoda per altri mezzi.
//
// Limite fisico importante: l'advertising BLE "legacy" (quello usato
// qui, compatibile con qualunque chip BLE) ha solo ~31 byte totali di
// spazio, quindi i messaggi sono per forza brevi (vedi MAX_MESSAGE_LEN)
// - a differenza della chat LoRa che permette testo molto piu' lungo.
namespace BleChat {
    // 17 invece di 20: 3 byte se ne vanno per un prefisso "privato"
    // anteposto ad ogni messaggio, usato per riconoscere i pacchetti
    // nostri e ignorare i dati produttore di qualunque altro dispositivo
    // BLE nelle vicinanze (auricolari, fitness tracker...).
    constexpr int MAX_MESSAGE_LEN = 17;

    void begin();
    void update(); // da chiamare ad ogni loop, non bloccante

    // NON bloccante: avvia una breve trasmissione (qualche secondo) via
    // advertising BLE e ritorna subito. Il testo viene troncato a
    // MAX_MESSAGE_LEN caratteri.
    bool send(const String& text);

    bool available();     // true se c'e' un messaggio ricevuto da leggere
    String read();         // consuma il messaggio disponibile
    int lastRSSI();

    bool isReady();

    // Interruttore persistente (NVS): a differenza del GPS, qui il
    // risparmio e' reale (ferma davvero scansione/advertising, non solo
    // ignora i dati via software). Sopravvive al riavvio.
    bool isEnabled();
    void setEnabled(bool enabled);
}
