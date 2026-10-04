#include "BleGatt.h"
#include "Config.h"
#include "Sound.h"

#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>

namespace {

    constexpr int MAX_FOUND_DEVICES = 20;
    constexpr int MAX_CHAR_ROWS = 24;
    constexpr uint32_t SCAN_DURATION_S = 6;

    bool runtimeEnabled = true;
    bool nvsOpened = false;
    Preferences blePrefs;
    constexpr const char* BLE_PREFS_NAMESPACE = "tdeckblegatt";
    constexpr const char* BLE_PREFS_KEY_ENABLED = "enabled";

    // Stesso vincolo gia' trovato con BleChat (vedi git log): BLEDevice::
    // deinit()+init() non e' sicuro in questa libreria - inizializzato
    // al massimo una volta per sessione, mai smontato.
    bool stackInitialized = false;
    bool bleReady = false;

    BLEScan* scan = nullptr;
    bool scanInProgress = false;

    BLEAdvertisedDevice foundDevices[MAX_FOUND_DEVICES];
    int foundDeviceCount = 0;

    BLEClient* client = nullptr;
    BleGatt::ConnectionState connState = BleGatt::ConnectionState::Idle;

    struct CharRowInternal {
        BLERemoteCharacteristic* characteristic; // nullptr per una riga da ascolto passivo (vedi sotto)
        String label;
        String value;
        bool notifying;
        bool canWrite;
    };
    CharRowInternal charRows[MAX_CHAR_ROWS];
    int charRowCount = 0;
    volatile bool valuesChanged = false;

    // Ascolto passivo degli advertisement di UN dispositivo (richiesto
    // 26/09): molti sensori economici (es. monitor batteria BM6) sono
    // beacon a sola trasmissione - i dati viaggiano nel pacchetto di
    // advertisement (manufacturer/service data), non in una caratteristica
    // GATT da leggere dopo una connessione vera. connectToDevice() prova
    // sempre prima una connessione GATT normale; se fallisce o non trova
    // nessuna caratteristica utile, passa da sola a questo ascolto invece
    // di arrendersi - stesse righe (charRows/valuesChanged) della UI di
    // dettaglio, che non deve distinguere i due casi.
    bool listening = false;
    String listenAddress;

    int findCharRowByLabel(const String& label) {
        for (int i = 0; i < charRowCount; i++) {
            if (charRows[i].characteristic == nullptr && charRows[i].label == label) {
                return i;
            }
        }
        return -1;
    }

    void ensureNvsOpen() {
        if (nvsOpened) {
            return;
        }
        blePrefs.begin(BLE_PREFS_NAMESPACE, false);
        runtimeEnabled = blePrefs.getBool(BLE_PREFS_KEY_ENABLED, true);
        nvsOpened = true;
    }

    void onScanComplete(BLEScanResults results) {
        scanInProgress = false;
        foundDeviceCount = 0;
        int count = results.getCount();
        for (int i = 0; i < count && foundDeviceCount < MAX_FOUND_DEVICES; i++) {
            foundDevices[foundDeviceCount++] = results.getDevice(i);
        }
        DEBUG_PRINTF("[BLEGATT] scansione completata: %d dispositivi\n", foundDeviceCount);
    }

    void ensureStackInit() {
        if (stackInitialized) {
            return;
        }

        BLEDevice::init("TDeck");
        scan = BLEDevice::getScan();
        // Scan attiva (vogliamo anche il nome, nella risposta di scan) e
        // finestra/intervallo alti: a differenza della vecchia chat BLE
        // (scansione continua in sottofondo, tenuta volutamente leggera)
        // questa e' una scansione breve avviata a mano dall'utente in
        // primo piano - va bene essere piu' aggressivi per finire prima.
        scan->setActiveScan(true);
        scan->setInterval(100);
        scan->setWindow(80);

        stackInitialized = true;
    }

    // Abbrevia una UUID a 128 bit al solo "assigned number" a 32 bit (i
    // primi 8 caratteri hex): per le UUID derivate dallo standard
    // Bluetooth SIG e' la parte che le distingue davvero, il resto e' un
    // suffisso fisso identico per tutte le UUID standard. Le UUID gia'
    // corte (16 bit, dispositivi che le usano direttamente) restano
    // cosi' com'erano.
    String shortUuid(BLEUUID uuid) {
        String full = uuid.toString();
        full.toUpperCase();
        if (full.length() > 8) {
            return full.substring(0, 8);
        }
        return full;
    }

    // Interpreta i byte grezzi di una caratteristica senza conoscerne il
    // significato (richiesto 26/09: niente etichette/formati predefiniti
    // per sensore, il dispositivo può essere qualunque cosa): testo se
    // tutti i byte sono ASCII stampabile, intero little-endian (la
    // convenzione piu' comune nei sensori BLE) per 1/2/4 byte, altrimenti
    // dump hex grezzo.
    String formatValue(const uint8_t* data, size_t len) {
        if (len == 0) {
            return "(vuoto)";
        }

        bool printable = true;
        for (size_t i = 0; i < len; i++) {
            if (data[i] < 0x20 || data[i] > 0x7E) {
                printable = false;
                break;
            }
        }
        if (printable) {
            String text;
            for (size_t i = 0; i < len; i++) {
                text += static_cast<char>(data[i]);
            }
            return text;
        }

        if (len == 1) {
            return String(data[0]);
        }
        if (len == 2) {
            uint16_t v = data[0] | (static_cast<uint16_t>(data[1]) << 8);
            return String(v);
        }
        if (len == 4) {
            uint32_t v = data[0] | (static_cast<uint32_t>(data[1]) << 8)
                | (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
            return String(v);
        }

        String hex;
        for (size_t i = 0; i < len && i < 12; i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", data[i]);
            hex += buf;
        }
        return hex;
    }

    // Aggiorna (o crea) una riga da ascolto passivo, identificata dalla
    // label invece che da un puntatore a caratteristica (non esiste,
    // qui i dati vengono dal pacchetto di advertisement).
    void upsertBeaconRow(const String& label, const String& raw) {
        int idx = findCharRowByLabel(label);
        if (idx < 0) {
            if (charRowCount >= MAX_CHAR_ROWS) {
                return;
            }
            idx = charRowCount++;
            charRows[idx].characteristic = nullptr;
            charRows[idx].label = label;
            charRows[idx].notifying = true; // si aggiorna da solo ad ogni advertisement, stesso significato "notifying" della UI
        }
        charRows[idx].value = formatValue(reinterpret_cast<const uint8_t*>(raw.c_str()), raw.length());
        valuesChanged = true;
    }

    // Riceve OGNI advertisement visto durante la scansione (non solo il
    // primo per dispositivo, vedi wantDuplicates=true in startListening())
    // e aggiorna le righe SOLO per il dispositivo che si sta ascoltando.
    class BeaconListener : public BLEAdvertisedDeviceCallbacks {
    public:
        void onResult(BLEAdvertisedDevice advertisedDevice) override {
            if (!listening || advertisedDevice.getAddress().toString() != listenAddress) {
                return;
            }
            if (advertisedDevice.haveManufacturerData()) {
                upsertBeaconRow("ADV/MANUFACTURER", advertisedDevice.getManufacturerData());
            }
            int svcDataCount = advertisedDevice.getServiceDataCount();
            for (int i = 0; i < svcDataCount; i++) {
                String label = "ADV/" + shortUuid(advertisedDevice.getServiceDataUUID(i));
                upsertBeaconRow(label, advertisedDevice.getServiceData(i));
            }
        }
    };
    BeaconListener beaconListener;

    void startListening(const String& address) {
        listening = true;
        listenAddress = address;
        charRowCount = 0;
        scan->setAdvertisedDeviceCallbacks(&beaconListener, true /* wantDuplicates */, true);
        scan->start(0, nullptr, false); // durata 0 = scansione continua (vedi BLEScan::start())
    }

    void stopListening() {
        if (listening) {
            scan->stop();
            listening = false;
        }
    }

    int findCharRow(BLERemoteCharacteristic* ch) {
        for (int i = 0; i < charRowCount; i++) {
            if (charRows[i].characteristic == ch) {
                return i;
            }
        }
        return -1;
    }

    void onNotify(BLERemoteCharacteristic* ch, uint8_t* data, size_t length, bool isNotify) {
        (void)isNotify;
        int idx = findCharRow(ch);
        if (idx < 0) {
            return;
        }
        charRows[idx].value = formatValue(data, length);
        valuesChanged = true;
    }

    // Scopre TUTTI i servizi/caratteristiche esposti dal dispositivo
    // appena connesso (richiesto 26/09: dinamico, non servizi "noti"
    // predefiniti) - legge subito le leggibili e si iscrive alle
    // notificabili, cosi' il loro valore si aggiorna da solo.
    void discoverServices() {
        charRowCount = 0;

        std::map<std::string, BLERemoteService*>* services = client->getServices();
        if (services == nullptr) {
            return;
        }

        for (auto& svcEntry : *services) {
            if (charRowCount >= MAX_CHAR_ROWS) {
                break;
            }

            BLERemoteService* service = svcEntry.second;
            String svcShort = shortUuid(service->getUUID());

            std::map<std::string, BLERemoteCharacteristic*>* chars = service->getCharacteristics();
            if (chars == nullptr) {
                continue;
            }

            for (auto& chEntry : *chars) {
                if (charRowCount >= MAX_CHAR_ROWS) {
                    break;
                }

                BLERemoteCharacteristic* ch = chEntry.second;
                bool writable = ch->canWrite() || ch->canWriteNoResponse();
                if (!ch->canRead() && !ch->canNotify() && !writable) {
                    continue;
                }

                CharRowInternal& row = charRows[charRowCount];
                row.characteristic = ch;
                row.label = svcShort + "/" + shortUuid(ch->getUUID());
                row.notifying = false;
                row.canWrite = writable;

                if (ch->canRead()) {
                    String raw = ch->readValue();
                    row.value = formatValue(reinterpret_cast<const uint8_t*>(raw.c_str()), raw.length());
                } else if (ch->canNotify()) {
                    row.value = "(in attesa...)";
                } else {
                    // Solo scrittura (29/09: es. registro di comando di
                    // un sensore) - "in attesa" sarebbe fuorviante, qui
                    // non arrivera' mai nulla da solo.
                    row.value = "(solo scrittura)";
                }

                if (ch->canNotify()) {
                    ch->subscribe(true, onNotify);
                    row.notifying = true;
                }

                charRowCount++;
            }
        }

        valuesChanged = true;
    }

} // namespace


namespace BleGatt {

    void begin() {
        ensureNvsOpen();
        if (runtimeEnabled) {
            ensureStackInit();
            bleReady = true;
        }
    }

    void update() {
        // Niente da guidare qui ad ogni ciclo: a differenza di NetGuard
        // (giro periodico automatico), scan/connessione qui partono solo
        // su azione esplicita dell'utente (vedi BleGattUI).
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
            ensureStackInit();
            bleReady = true;
        } else {
            if (client != nullptr && client->isConnected()) {
                client->disconnect();
            }
            if (scan != nullptr) {
                scan->stop();
            }
            bleReady = false;
        }
    }

    bool isReady() {
        return bleReady;
    }

    String getMacAddress() {
        if (!stackInitialized) {
            return "";
        }
        return BLEDevice::getAddress().toString();
    }

    void startScan() {
        if (!bleReady || scan == nullptr) {
            return;
        }
        foundDeviceCount = 0;
        scanInProgress = true;
        scan->start(SCAN_DURATION_S, onScanComplete, false);
    }

    bool isScanning() {
        return scanInProgress;
    }

    void stopScan() {
        if (scan != nullptr) {
            scan->stop();
        }
        scanInProgress = false;
    }

    int getFoundDeviceCount() {
        return foundDeviceCount;
    }

    FoundDevice getFoundDevice(int index) {
        FoundDevice dev;
        if (index < 0 || index >= foundDeviceCount) {
            return dev;
        }
        BLEAdvertisedDevice& d = foundDevices[index];
        dev.name = d.haveName() ? d.getName() : "";
        dev.address = d.getAddress().toString();
        dev.rssi = d.getRSSI();
        return dev;
    }

    void connectToDevice(int foundIndex) {
        if (foundIndex < 0 || foundIndex >= foundDeviceCount) {
            return;
        }

        connState = ConnectionState::Connecting;
        String address = foundDevices[foundIndex].getAddress().toString();

        if (client == nullptr) {
            client = BLEDevice::createClient();
        } else if (client->isConnected()) {
            client->disconnect();
        }

        // connect() e' bloccante: la libreria non offre una versione
        // davvero asincrona per il pairing. Il chiamante (BleGattUI)
        // mostra prima un messaggio "connessione in corso" e richiama
        // questa funzione subito dopo - blocca solo per la durata reale
        // del pairing (di norma 1-3s, non l'intera sessione).
        bool ok = client->connect(&foundDevices[foundIndex]);
        if (ok) {
            connState = ConnectionState::DiscoveringServices;
            discoverServices();
        }

        if (!ok || charRowCount == 0) {
            // Connessione GATT fallita, o riuscita ma senza nessuna
            // caratteristica leggibile/notificabile: molti sensori
            // economici (es. monitor batteria BM6) sono beacon a sola
            // trasmissione, i dati viaggiano nel pacchetto di advertisement
            // - vedi il commento su startListening() sopra. Si passa
            // all'ascolto passivo invece di arrendersi.
            if (client->isConnected()) {
                client->disconnect();
            }
            startListening(address);
        }

        connState = ConnectionState::Connected;
        Sound::playBleGattConnected();
    }

    ConnectionState getConnectionState() {
        return connState;
    }

    bool isListening() {
        return listening;
    }

    void disconnect() {
        bool wasConnected = listening || (client != nullptr && client->isConnected());
        stopListening();
        if (client != nullptr && client->isConnected()) {
            client->disconnect();
        }
        charRowCount = 0;
        connState = ConnectionState::Idle;

        if (wasConnected) {
            Sound::playBleGattDisconnected();
        }
    }

    int getCharRowCount() {
        return charRowCount;
    }

    CharRow getCharRow(int index) {
        CharRow row;
        row.notifying = false;
        row.canWrite = false;
        if (index < 0 || index >= charRowCount) {
            return row;
        }
        row.label = charRows[index].label;
        row.value = charRows[index].value;
        row.notifying = charRows[index].notifying;
        row.canWrite = charRows[index].canWrite;
        return row;
    }

    bool writeCharRow(int index, const uint8_t* data, size_t length) {
        if (index < 0 || index >= charRowCount) {
            return false;
        }
        BLERemoteCharacteristic* ch = charRows[index].characteristic;
        if (ch == nullptr) {
            return false;
        }
        // La firma di writeValue() vuole uint8_t* non-const - il buffer
        // del chiamante (bytes hex appena decodificati) non viene mai
        // modificato da qui dentro, il const_cast e' sicuro.
        uint8_t* mutableData = const_cast<uint8_t*>(data);
        if (ch->canWrite()) {
            return ch->writeValue(mutableData, length, true);
        }
        if (ch->canWriteNoResponse()) {
            return ch->writeValue(mutableData, length, false);
        }
        return false;
    }

    bool consumeValuesChanged() {
        bool v = valuesChanged;
        valuesChanged = false;
        return v;
    }
}
