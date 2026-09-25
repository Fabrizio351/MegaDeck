#include "BleChat.h"
#include "Config.h"

#include <string.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEAdvertising.h>

namespace {
    // Quanto dura la trasmissione di un messaggio in invio prima di
    // fermare l'advertising (poi la radio resta comunque in scansione).
    constexpr uint32_t ADVERTISE_DURATION_MS = 3000;

    // Ogni finestra di scansione dura questi secondi, poi si riarma da
    // sola in onScanComplete(): serve a rispettare la firma dell'API
    // (che vuole una durata), non per limitare davvero la ricezione,
    // che resta di fatto continua.
    constexpr uint32_t SCAN_WINDOW_S = 5;

    // Un dispositivo che sta inviando un messaggio ripete lo stesso
    // annuncio BLE piu' volte prima di fermarsi: senza una deduplica si
    // vedrebbe lo stesso messaggio comparire piu' volte in chat.
    constexpr uint32_t DEDUPE_WINDOW_MS = 2000;

    bool bleReady = false;
    bool runtimeEnabled = true;
    bool nvsOpened = false;

    Preferences blePrefs;
    constexpr const char* BLE_PREFS_NAMESPACE = "tdeckble";
    constexpr const char* BLE_PREFS_KEY_ENABLED = "enabled";

    BLEScan* scan = nullptr;
    BLEAdvertising* advertising = nullptr;

    bool advertisingActive = false;
    uint32_t advertiseStartedMs = 0;

    volatile bool messageFlag = false;
    String lastMessage;
    int lastRssiValue = 0;

    String lastSeenAddress;
    String lastSeenPayload;
    uint32_t lastSeenMs = 0;

    // Prefisso "privato" anteposto ad ogni messaggio inviato: senza di
    // esso qualunque dispositivo BLE nelle vicinanze che trasmette dati
    // produttore per conto proprio (auricolari, fitness tracker, beacon
    // vari...) veniva scambiato per un messaggio in arrivo, causando
    // testo spurio in chat (es. una singola lettera). Solo i pacchetti
    // che iniziano esattamente con questo prefisso sono considerati
    // messaggi nostri.
    const char MAGIC_PREFIX[] = { 'T', 'D', 'k' };
    constexpr size_t MAGIC_PREFIX_LEN = sizeof(MAGIC_PREFIX);

    void ensureNvsOpen() {
        if (nvsOpened) {
            return;
        }

        blePrefs.begin(BLE_PREFS_NAMESPACE, false);
        runtimeEnabled = blePrefs.getBool(BLE_PREFS_KEY_ENABLED, true);
        nvsOpened = true;
    }

    void onScanComplete(BLEScanResults results) {
        (void)results;

        // Si riarma da sola: una finestra "finita" non significa che
        // l'utente abbia disattivato il BT, va solo ripartita. bleReady
        // (non solo scan!=nullptr) perche' dopo uno spegnimento vero
        // (vedi stopBleStack()) lo stack e' stato smontato del tutto:
        // un riarmo tardivo qui andrebbe a toccare un BLEScan ormai
        // invalido.
        if (bleReady && scan != nullptr && runtimeEnabled) {
            scan->start(SCAN_WINDOW_S, onScanComplete, false);
        }
    }

    class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
        void onResult(BLEAdvertisedDevice advertisedDevice) override {
            if (!advertisedDevice.haveManufacturerData()) {
                return;
            }

            String payload = advertisedDevice.getManufacturerData();

            // Scarta tutto cio' che non porta il nostro prefisso: e' un
            // altro dispositivo BLE nelle vicinanze che trasmette dati
            // produttore per conto proprio, non un messaggio di questa chat.
            if (payload.length() <= MAGIC_PREFIX_LEN
                || memcmp(payload.c_str(), MAGIC_PREFIX, MAGIC_PREFIX_LEN) != 0) {
                return;
            }

            payload = payload.substring(MAGIC_PREFIX_LEN);

            String addr = advertisedDevice.getAddress().toString();
            uint32_t now = millis();

            // Stesso indirizzo + stesso contenuto entro la finestra di
            // deduplica: e' ancora lo stesso annuncio in onda, non un
            // nuovo messaggio.
            if (addr == lastSeenAddress && payload == lastSeenPayload && (now - lastSeenMs) < DEDUPE_WINDOW_MS) {
                return;
            }

            lastSeenAddress = addr;
            lastSeenPayload = payload;
            lastSeenMs = now;

            lastMessage = payload;
            lastRssiValue = advertisedDevice.getRSSI();
            messageFlag = true;
        }
    };

    // true se BLEDevice::init() e' gia' stato chiamato in questa sessione.
    //
    // IMPORTANTE (trovato con un crash reale in prova, 2026-09-23):
    // BLEDevice::deinit(true) seguito da un nuovo BLEDevice::init() NON
    // e' sicuro in questa versione della libreria - l'oggetto BLEScan
    // restituito da BLEDevice::getScan() dopo il re-init resta in uno
    // stato non valido, e la prima chiamata a
    // setAdvertisedDeviceCallbacks() su di esso va in Guru Meditation
    // (LoadProhibited, EXCVADDR quasi nullo - confermato via backtrace
    // con addr2line, crash dentro BLEScan.cpp:128 della libreria, non
    // nel nostro codice). Percio' l'inizializzazione avviene una sola
    // volta per sessione (alla prima attivazione, che sia all'avvio o
    // da un riattivazione successiva dalle Impostazioni) e non viene MAI
    // smontata: disattivare il BT ferma solo scansione/advertising, la
    // memoria dello stack resta impegnata per il resto della sessione
    // una volta che e' stato acceso almeno una volta. Se non lo si
    // accende mai in una sessione, quella memoria non viene comunque
    // mai reclamata (risparmio parziale, ma l'alternativa era il crash
    // riproducibile al primo riattivazione).
    bool stackInitialized = false;

    void startBleStack() {
        if (!stackInitialized) {
            BLEDevice::init("TDeck");

            advertising = BLEDevice::getAdvertising();

            scan = BLEDevice::getScan();
            // wantDuplicates=true: vogliamo OGNI singolo annuncio ricevuto,
            // per poter fare da soli una deduplica che sappiamo essere
            // corretta (indirizzo+contenuto+tempo), invece di dipendere dal
            // filtro non documentato della libreria.
            scan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), true);
            scan->setActiveScan(true);
            // window/interval basso (30%) invece che quasi continuo (80%):
            // una scansione cosi' aggressiva teneva il radio BLE occupato
            // pressoche' sempre, rallentando il task touch (stesso core,
            // stesso bus I2C conteso). Un dispositivo che invia un messaggio
            // annuncia per ADVERTISE_DURATION_MS (3s): anche con una finestra
            // di ricezione piu' corta ci sono comunque molte occasioni di
            // intercettarlo in quella finestra di tempo.
            scan->setInterval(100);
            scan->setWindow(30);

            stackInitialized = true;
        }

        bleReady = true;
        advertisingActive = false;

        scan->start(SCAN_WINDOW_S, onScanComplete, false);
        DEBUG_PRINTLN("[BLE] pronto, in scansione");
    }

    // Ferma solo scansione/advertising: NON smonta piu' lo stack (vedi
    // il commento sopra startBleStack() sul perche').
    void stopBleStack() {
        if (!bleReady) {
            return;
        }

        if (advertisingActive && advertising != nullptr) {
            advertising->stop();
            advertisingActive = false;
        }

        if (scan != nullptr) {
            scan->stop();
        }

        bleReady = false;

        DEBUG_PRINTLN("[BLE] disattivato (scansione fermata)");
    }
}

namespace BleChat {

    void begin() {
        ensureNvsOpen();

        if (runtimeEnabled) {
            startBleStack();
        } else {
            DEBUG_PRINTLN("[BLE] disattivato dalle Impostazioni, stack non inizializzato");
        }
    }

    void update() {
        if (!bleReady) {
            return;
        }

        if (advertisingActive && millis() - advertiseStartedMs > ADVERTISE_DURATION_MS) {
            advertising->stop();
            advertisingActive = false;
        }
    }

    bool send(const String& text) {
        if (!bleReady || !runtimeEnabled) {
            return false;
        }

        String truncated = text.substring(0, MAX_MESSAGE_LEN);
        String payload = String(MAGIC_PREFIX, MAGIC_PREFIX_LEN) + truncated;

        BLEAdvertisementData advData;
        advData.setManufacturerData(payload);
        advertising->setAdvertisementData(advData);
        advertising->start();

        advertisingActive = true;
        advertiseStartedMs = millis();

        return true;
    }

    bool available() {
        return messageFlag;
    }

    String read() {
        messageFlag = false;
        String msg = lastMessage;
        lastMessage = "";
        return msg;
    }

    int lastRSSI() {
        return lastRssiValue;
    }

    bool isReady() {
        return bleReady;
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

        runtimeEnabled = enabled;
        blePrefs.putBool(BLE_PREFS_KEY_ENABLED, enabled);

        if (enabled) {
            startBleStack();
        } else {
            stopBleStack();
        }
    }
}
