#include "WifiManager.h"
#include "Sound.h"

#include <WiFi.h>
#include <Preferences.h>

namespace {
    bool scanning = false;
    int scanResultCount = -1; // -1 = nessuno scan ancora completato

    WifiManager::ConnectionState connectionState = WifiManager::ConnectionState::Idle;
    uint32_t connectStartedMs = 0;
    constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

    // Credenziali salvate: una singola stringa in NVS con un formato
    // minimale "ssid1\x01password1\x02ssid2\x01password2\x02...".
    // Niente libreria JSON (spazio flash gia' scarso): SSID/password
    // WiFi validi non contengono caratteri di controllo, quindi \x01/\x02
    // come separatori sono sicuri.
    Preferences wifiPrefs;
    constexpr const char* WIFI_PREFS_NAMESPACE = "tdeckwifi";
    constexpr const char* WIFI_PREFS_KEY = "creds";
    constexpr const char* WIFI_PREFS_KEY_RADIO_EN = "radio_en";
    constexpr char FIELD_SEP = '\x01';
    constexpr char RECORD_SEP = '\x02';
    constexpr int MAX_SAVED_NETWORKS = 10;

    bool nvsOpened = false;
    bool radioEnabled = true;

    // Traccia lo stato WiFi.status()==WL_CONNECTED del giro precedente:
    // confrontandolo ad ogni update() rileviamo QUALSIASI transizione
    // connesso<->non connesso, sia causata da un'azione esplicita
    // (bottone Disconnetti, interruttore in Impostazioni) sia da una
    // perdita di segnale spontanea (utile proprio per accorgersi di
    // essere usciti dal campo senza dover guardare lo schermo).
    bool lastConnectedState = false;

    // Apre l'NVS una sola volta (condiviso da credenziali e interruttore
    // radio, che vivono nello stesso namespace): chiamate multiple a
    // Preferences::begin() sullo stesso oggetto sono da evitare.
    void ensureNvsOpen() {
        if (nvsOpened) {
            return;
        }

        wifiPrefs.begin(WIFI_PREFS_NAMESPACE, false);
        radioEnabled = wifiPrefs.getBool(WIFI_PREFS_KEY_RADIO_EN, true);
        nvsOpened = true;
    }

    bool credentialsLoaded = false;
    String credentialsBlob;

    void ensureCredentialsLoaded() {
        if (credentialsLoaded) {
            return;
        }

        ensureNvsOpen();
        credentialsBlob = wifiPrefs.getString(WIFI_PREFS_KEY, "");
        credentialsLoaded = true;
    }

    // Cerca ssid nel blob; se trovato ritorna true e (se richiesta)
    // popola outPassword con la password salvata.
    bool findCredential(const String& ssid, String* outPassword) {
        int pos = 0;
        int len = credentialsBlob.length();

        while (pos < len) {
            int recordEnd = credentialsBlob.indexOf(RECORD_SEP, pos);
            if (recordEnd < 0) {
                recordEnd = len;
            }

            String record = credentialsBlob.substring(pos, recordEnd);
            int fieldSep = record.indexOf(FIELD_SEP);

            if (fieldSep >= 0 && record.substring(0, fieldSep) == ssid) {
                if (outPassword != nullptr) {
                    *outPassword = record.substring(fieldSep + 1);
                }
                return true;
            }

            pos = recordEnd + 1;
        }

        return false;
    }

    // Ricostruisce credentialsBlob togliendo la voce per ssid, se c'e'.
    // Ritorna il numero di voci rimaste (serve al limite MAX_SAVED_NETWORKS
    // in saveCredentials). Non scrive su NVS: il chiamante decide se e
    // quando farlo.
    int removeCredentialFromBlob(const String& ssid) {
        String rebuilt;
        int recordCount = 0;
        int pos = 0;
        int len = credentialsBlob.length();

        while (pos < len) {
            int recordEnd = credentialsBlob.indexOf(RECORD_SEP, pos);
            if (recordEnd < 0) {
                recordEnd = len;
            }

            String record = credentialsBlob.substring(pos, recordEnd);
            int fieldSep = record.indexOf(FIELD_SEP);
            String recordSsid = (fieldSep >= 0) ? record.substring(0, fieldSep) : record;

            if (record.length() > 0 && recordSsid != ssid) {
                if (rebuilt.length() > 0) {
                    rebuilt += RECORD_SEP;
                }
                rebuilt += record;
                recordCount++;
            }

            pos = recordEnd + 1;
        }

        credentialsBlob = rebuilt;
        return recordCount;
    }
}

namespace WifiManager {

    void startScan() {
        if (scanning) {
            return;
        }

        // true = asincrono (non blocca il loop), false = non nasconde le
        // reti con SSID nascosto (le vogliamo comunque in lista).
        WiFi.scanNetworks(true, false);
        scanning = true;
        scanResultCount = -1;
    }

    bool isScanning() {
        return scanning;
    }

    int getNetworkCount() {
        return (scanResultCount < 0) ? 0 : scanResultCount;
    }

    Network getNetwork(int index) {
        Network net;
        net.ssid = "";
        net.rssi = 0;
        net.open = true;

        if (index < 0 || index >= getNetworkCount()) {
            return net;
        }

        net.ssid = WiFi.SSID(index);
        net.rssi = WiFi.RSSI(index);
        net.open = (WiFi.encryptionType(index) == WIFI_AUTH_OPEN);
        return net;
    }

    void connect(const String& ssid, const String& password) {
        if (!isRadioEnabled()) {
            // Rete di sicurezza: WifiUI dovrebbe gia' impedire di
            // arrivare qui col radio disattivato, ma meglio non tentare
            // comunque una connessione col radio spento.
            connectionState = ConnectionState::Failed;
            return;
        }

        WiFi.begin(ssid.c_str(), password.length() > 0 ? password.c_str() : nullptr);
        connectionState = ConnectionState::Connecting;
        connectStartedMs = millis();
    }

    ConnectionState getConnectionState() {
        return connectionState;
    }

    void disconnect() {
        // true, true = spegne anche il radio WiFi (non solo la
        // connessione), cosi' si risparmia batteria per davvero mentre
        // non serve, non solo si esce dalla rete.
        WiFi.disconnect(true, true);
        connectionState = ConnectionState::Idle;
    }

    String getConnectedSsid() {
        return WiFi.SSID();
    }

    String getLocalIP() {
        return WiFi.localIP().toString();
    }

    void update() {
        if (scanning) {
            int result = WiFi.scanComplete();

            // >=0: numero di reti trovate. WIFI_SCAN_RUNNING (-1) vuol
            // dire ancora in corso, WIFI_SCAN_FAILED (-2) fallita.
            if (result >= 0) {
                scanResultCount = result;
                scanning = false;
            } else if (result == WIFI_SCAN_FAILED) {
                scanResultCount = 0;
                scanning = false;
            }
        }

        if (connectionState == ConnectionState::Connecting) {
            wl_status_t status = WiFi.status();

            if (status == WL_CONNECTED) {
                connectionState = ConnectionState::Connected;
            } else if (millis() - connectStartedMs > CONNECT_TIMEOUT_MS) {
                connectionState = ConnectionState::Failed;
                WiFi.disconnect();
            }
            // Altri stati (WL_IDLE_STATUS, WL_DISCONNECTED durante il
            // tentativo) sono normali transitori: si continua ad
            // aspettare finche' non scade il timeout sopra.
        }

        bool nowConnected = (WiFi.status() == WL_CONNECTED);
        if (nowConnected != lastConnectedState) {
            if (nowConnected) {
                Sound::playWifiConnected();
            } else {
                Sound::playWifiDisconnected();
            }
            lastConnectedState = nowConnected;
        }
    }

    bool hasSavedPassword(const String& ssid) {
        ensureCredentialsLoaded();
        return findCredential(ssid, nullptr);
    }

    String getSavedPassword(const String& ssid) {
        ensureCredentialsLoaded();

        String password;
        findCredential(ssid, &password);
        return password;
    }

    void saveCredentials(const String& ssid, const String& password) {
        ensureCredentialsLoaded();

        // Togli un'eventuale voce precedente per la stessa rete (es.
        // password cambiata), cosi' non si accumulano doppioni.
        int recordCount = removeCredentialFromBlob(ssid);

        // Limite di sicurezza: se si superano le reti massime salvate,
        // si scarta la piu' vecchia (la prima rimasta nella lista).
        while (recordCount >= MAX_SAVED_NETWORKS) {
            int sep = credentialsBlob.indexOf(RECORD_SEP);
            if (sep < 0) {
                credentialsBlob = "";
                recordCount = 0;
                break;
            }
            credentialsBlob = credentialsBlob.substring(sep + 1);
            recordCount--;
        }

        if (credentialsBlob.length() > 0) {
            credentialsBlob += RECORD_SEP;
        }
        credentialsBlob += ssid + FIELD_SEP + password;

        wifiPrefs.putString(WIFI_PREFS_KEY, credentialsBlob);
    }

    void forgetPassword(const String& ssid) {
        ensureCredentialsLoaded();
        removeCredentialFromBlob(ssid);
        wifiPrefs.putString(WIFI_PREFS_KEY, credentialsBlob);
    }

    bool isRadioEnabled() {
        ensureNvsOpen();
        return radioEnabled;
    }

    void setRadioEnabled(bool enabled) {
        ensureNvsOpen();

        if (radioEnabled == enabled) {
            return;
        }

        radioEnabled = enabled;
        wifiPrefs.putBool(WIFI_PREFS_KEY_RADIO_EN, enabled);

        if (!enabled) {
            // true, true = spegne anche il radio (non solo la
            // connessione corrente), per il risparmio energetico voluto.
            WiFi.disconnect(true, true);
            connectionState = ConnectionState::Idle;
        }
    }
}
