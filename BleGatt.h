#pragma once

#include <Arduino.h>

// Scansione e connessione GATT a periferiche Bluetooth LE nelle
// vicinanze (sensori di casa, fitness tracker, ecc.) - sostituisce la
// vecchia chat BLE (BleChat.cpp, eliminata il 26/09): qui non si invia
// nulla, si legge quello che il dispositivo espone. Non conoscendo in
// anticipo la forma dei dati di un sensore generico, il modulo scopre
// TUTTI i servizi/caratteristiche esposte invece di cercarne di
// specifiche, e lascia alla UI il compito di mostrarle senza etichette
// predefinite (vedi BleGattUI.cpp).
namespace BleGatt {

    void begin();
    void update(); // da chiamare ad ogni ciclo, non bloccante

    // Interruttore persistente (NVS), stesso schema di WiFi/LoRa/GPS:
    // risparmio reale (radio spenta), sopravvive al riavvio.
    bool isEnabled();
    void setEnabled(bool enabled);
    bool isReady(); // stack BLE inizializzato e pronto all'uso

    // Indirizzo MAC Bluetooth del T-Deck (vuoto se isReady()==false: lo
    // stack va inizializzato prima che l'indirizzo sia leggibile) - per
    // la voce "Info" del menu principale.
    String getMacAddress();

    // --- Scansione ---
    void startScan();
    bool isScanning();
    void stopScan();

    struct FoundDevice {
        String name;    // vuoto se il dispositivo non trasmette un nome
        String address; // MAC, usato per connettersi
        int rssi;
    };

    int getFoundDeviceCount();
    FoundDevice getFoundDevice(int index);

    // --- Connessione ---
    enum class ConnectionState {
        Idle,
        Connecting,
        DiscoveringServices,
        Connected,
        Failed
    };

    // Bloccante per la parte di connessione vera e propria (qualche
    // secondo, limite della libreria BLE usata - vedi il commento su
    // connectToDevice() in BleGatt.cpp): il chiamante mostra un
    // messaggio "connessione in corso" PRIMA di richiamarla.
    //
    // Se la connessione GATT fallisce, o riesce ma non trova nessuna
    // caratteristica utile, passa da sola all'ASCOLTO PASSIVO degli
    // advertisement dello stesso dispositivo (vedi isListening()) - molti
    // sensori economici (es. monitor batteria BM6) sono beacon a sola
    // trasmissione: i dati (voltaggio, ecc.) viaggiano nel pacchetto di
    // advertisement (manufacturer/service data), non in una caratteristica
    // GATT da leggere dopo il pairing. ConnectionState::Connected copre
    // ENTRAMBI i casi (connessione vera o ascolto) - il chiamante non deve
    // distinguere, getCharRowCount()/getCharRow() restituiscono le righe
    // giuste in entrambi i casi.
    void connectToDevice(int foundIndex);
    ConnectionState getConnectionState();
    void disconnect();

    // true se le righe correnti vengono da ascolto passivo (nessuna vera
    // connessione GATT) invece che da una connessione riuscita - solo per
    // un'eventuale nota informativa in UI, il funzionamento e' identico.
    bool isListening();

    // --- Caratteristiche del dispositivo connesso ---
    // Una riga per ogni caratteristica leggibile o notificabile trovata,
    // di qualunque servizio - "dinamiche" per davvero: numero e
    // contenuto dipendono solo da cosa il dispositivo espone. Il valore
    // si aggiorna da solo se la caratteristica supporta le notifiche.
    struct CharRow {
        String label; // "<UUID servizio abbreviata>/<UUID caratteristica abbreviata>"
        String value; // interpretato (testo se stampabile, altrimenti numero o hex)
        bool notifying;
        bool canWrite; // 29/09: vero se ci si puo' scrivere (vedi writeCharRow())
    };

    int getCharRowCount();
    CharRow getCharRow(int index);

    // Scrive byte grezzi su una caratteristica scrivibile (29/09,
    // richiesto: molti sensori BLE economici - es. il BM6 - non
    // notificano da soli finche' non ricevono un comando "sveglia"
    // specifico del produttore, diverso da dispositivo a dispositivo.
    // Invece di codificarlo nel firmware per un solo modello, l'utente
    // lo scrive lui stesso dalla UI, in hex). Usa writeValue() CON
    // risposta se la caratteristica lo supporta, altrimenti SENZA
    // risposta - ritorna false se nessuna delle due e' supportata o
    // l'indice non e' valido.
    bool writeCharRow(int index, const uint8_t* data, size_t length);

    // true dall'ultima volta che una notifica ha aggiornato un valore -
    // usato dalla UI per sapere quando ridisegnare senza dover
    // confrontare tutte le righe ad ogni ciclo. Si azzera leggendolo.
    bool consumeValuesChanged();
}
