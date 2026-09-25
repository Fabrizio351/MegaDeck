#pragma once

#include <Arduino.h>

namespace LoRaRadio {
    bool begin();
    void update();  // da chiamare ad ogni loop, non bloccante

    // NON bloccante (come Meshtastic): avvia l'invio in background via
    // radio.startTransmit() e ritorna subito - il completamento arriva
    // dallo stesso interrupt DIO1 gia' usato per la ricezione, gestito
    // in update(). true = accettato per l'invio (subito o in coda), non
    // "consegnato": non c'e' conferma di consegna. Se un invio e' gia'
    // in corso, il messaggio viene accodato (vedi TX_QUEUE_SIZE in
    // LoRaRadio.cpp) invece di bloccare il chiamante fino a quando il
    // canale si libera.
    //
    // Mesh a piu' salti: ogni messaggio parte con un TTL di 3 (vedi
    // DEFAULT_TTL in LoRaRadio.cpp) - i nodi che lo ricevono lo rilanciano
    // automaticamente (decrementando il TTL) se non l'hanno gia' visto,
    // estendendo la portata oltre il raggio radio diretto dal mittente.
    // Nessun instradamento vero e proprio (e' un flood, non un routing):
    // piu' nodi nel raggio = piu' probabilita' di copertura, ma anche
    // piu' traffico radio condiviso.
    bool send(const String& text);

    bool available();     // true se c'e' un messaggio ricevuto da leggere
    String read();         // consuma il messaggio disponibile

    float lastRSSI();
    float lastSNR();

    // true se begin() e' andato a buon fine (radio inizializzata,
    // pronta a ricevere/trasmettere se abilitata).
    bool isReady();

    // Stato di trasmissione in tempo reale, per mostrarlo in UI (es.
    // "invio in corso..."): true dal momento in cui send()/la coda
    // avviano una trasmissione fino a quando l'interrupt DIO1 ne
    // segnala il completamento in update().
    bool isTransmitting();

    // Quanti messaggi sono in coda in attesa che la trasmissione in
    // corso finisca (non conta quello eventualmente in trasmissione
    // adesso). 0 se non c'e' nulla in attesa.
    int pendingCount();

    // Interruttore persistente (NVS) per il risparmio energetico: a
    // differenza del GPS, qui il risparmio e' reale, perche' esiste
    // un vero comando radio per fermare la ricezione (standby) invece
    // di limitarsi a ignorare i dati via software. Sopravvive al
    // riavvio.
    bool isEnabled();
    void setEnabled(bool enabled);
}
