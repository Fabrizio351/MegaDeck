#include "VpnConfig.h"
#include "Config.h"
#include "Board.h"

#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

    bool sdMounted = false;

    // Oggetto SPI proprio, come LoRaRadio.cpp - NON il bus condiviso di
    // Board::getSharedSPI(): un tentativo di richiamarne begin() (sia a
    // boot in Board::begin(), sia qui dentro il task) ha prodotto un
    // blocco totale silenzioso ancora peggiore (nessun log seriale,
    // schermo nero, niente watchdog) riproducibile anche a SD rimossa e
    // flash appena cancellata - non era quindi la SD la causa di quel
    // blocco specifico. Vedi project_wifi_connect_freeze_and_vpn_bug in
    // memoria per la storia completa.
    SPIClass sdSPI(HSPI);

    // STORIA (26/09): SD.begin() si e' bloccato per davvero dentro
    // spiTransferByteNL() (SD/sd_diskio.cpp:511, poi esp32-hal-spi.c) sia
    // su core 0 che su core 1. Un task dedicato con timeout duro resta
    // l'unica variante che si e' sempre autorecuperata (watchdog di
    // sistema, riavvio automatico) - la causa di fondo (probabile
    // problema hardware/wiring della SD condivisa sul bus HSPI) resta da
    // indagare separatamente, con la massima cautela vista la fragilita'
    // dimostrata di quel bus condiviso.
    volatile bool sdTaskDone = false;
    volatile bool sdTaskResult = false;

    void sdMountTaskFn(void*) {
        // Tentativo (27/09, trovato cercando su GitHub esempi ufficiali
        // LilyGO per questo stesso bus condiviso): l'esempio ufficiale
        // del T-Deck, prima di QUALUNQUE operazione SPI, porta HIGH
        // (deselezionati) TUTTI E TRE i CS del bus condiviso - SD, RADIO
        // *E* IL DISPLAY (BOARD_TFT_CS). Il nostro codice deseleziona
        // sempre SD e radio (vedi Board::begin()) ma MAI il display: se
        // Arduino_ESP32SPI lascia anche solo per un istante il proprio CS
        // basso fra una redraw e l'altra, il chip del display potrebbe
        // rispondere insieme alla SD sullo stesso MISO/SCK/MOSI - un vero
        // conflitto elettrico che spiegherebbe perche' spiTransferByteNL
        // (un trasferimento "NL" = senza lock, quindi senza alcuna
        // protezione da questo tipo di conflitto) non completa mai il
        // bit atteso. Mai provato finora - i tentativi precedenti
        // (core diverso, clock piu' lento) escludevano gia' cause di
        // tempistica/velocita', non di conflitto elettrico multi-device.
        pinMode(BOARD_TFT_CS, OUTPUT);
        digitalWrite(BOARD_TFT_CS, HIGH);
        pinMode(RADIO_CS_PIN, OUTPUT);
        digitalWrite(RADIO_CS_PIN, HIGH);
        pinMode(BOARD_SDCARD_CS, OUTPUT);
        digitalWrite(BOARD_SDCARD_CS, HIGH);

        sdSPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI, BOARD_SDCARD_CS);
        bool ok = SD.begin(BOARD_SDCARD_CS, sdSPI, 400000);
        sdTaskResult = ok;
        sdTaskDone = true;
        vTaskDelete(nullptr);
    }

    bool ensureSdMounted() {
        if (sdMounted) {
            return true;
        }

        constexpr uint32_t SD_MOUNT_TIMEOUT_MS = 8000;

        sdTaskDone = false;
        sdTaskResult = false;

        TaskHandle_t handle = nullptr;
        xTaskCreatePinnedToCore(sdMountTaskFn, "sdMount", 4096, nullptr, 1, &handle, 0);

        DEBUG_PRINTLN("[VPNCONFIG] diagnostica: task SD.begin() avviato, attesa con timeout 8s");
        uint32_t startMs = millis();
        while (!sdTaskDone && millis() - startMs < SD_MOUNT_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (!sdTaskDone) {
            DEBUG_PRINTLN("[VPNCONFIG] SD.begin() oltre il timeout (8s): si procede senza SD, task lasciato orfano in background");
            return false;
        }

        if (!sdTaskResult) {
            DEBUG_PRINTLN("[VPNCONFIG] SD card non trovata/non leggibile");
            return false;
        }

        sdMounted = true;
        DEBUG_PRINTLN("[VPNCONFIG] SD card montata");
        return true;
    }

    String trimmed(const String& s) {
        String t = s;
        t.trim();
        return t;
    }

    // Rimuove un eventuale suffisso "/NN" (notazione CIDR) da un
    // indirizzo IP: la libreria WireGuard vuole il solo IP.
    String stripCidr(const String& s) {
        int slash = s.indexOf('/');
        return (slash == -1) ? s : s.substring(0, slash);
    }

    // Copia interna, NVS: vedi VpnConfig::importFromSd()/loadSaved() -
    // richiesto 26/09 per non dover lasciare la chiave privata su una SD
    // estraibile.
    Preferences vpnConfigPrefs;
    constexpr const char* CFG_PREFS_NAMESPACE = "tdeckvpncfg";
    constexpr const char* KEY_VALID = "valid";
    constexpr const char* KEY_LOCAL_ADDR = "addr";
    constexpr const char* KEY_DNS = "dns";
    constexpr const char* KEY_PRIVATE_KEY = "priv";
    constexpr const char* KEY_ENDPOINT_HOST = "host";
    constexpr const char* KEY_ENDPOINT_PORT = "port";
    constexpr const char* KEY_PEER_PUBLIC_KEY = "pub";
    constexpr const char* KEY_PRESHARED_KEY = "psk";
    constexpr const char* KEY_ALLOWED_IPS = "aips";

    // Parser condiviso del formato wg-quick (.conf), estratto da load()
    // per essere riusato identico sia leggendo da SD sia da un blocco di
    // testo ricevuto via USB seriale (vedi importFromSerial()) - stesso
    // identico file, due sole strade per farlo arrivare al T-Deck.
    VpnConfig::Config parseWireguardConfText(const String& text) {
        VpnConfig::Config cfg;

        enum class Section { None, Interface, Peer };
        Section section = Section::None;

        int pos = 0;
        int len = text.length();
        while (pos < len) {
            int nl = text.indexOf('\n', pos);
            String rawLine = (nl == -1) ? text.substring(pos) : text.substring(pos, nl);
            pos = (nl == -1) ? len : nl + 1;
            String line = trimmed(rawLine);

            if (line.length() == 0 || line.startsWith("#") || line.startsWith(";")) {
                continue;
            }

            if (line.equalsIgnoreCase("[Interface]")) {
                section = Section::Interface;
                continue;
            }
            if (line.equalsIgnoreCase("[Peer]")) {
                section = Section::Peer;
                continue;
            }

            int eq = line.indexOf('=');
            if (eq == -1) {
                continue;
            }
            String key = trimmed(line.substring(0, eq));
            String value = trimmed(line.substring(eq + 1));

            if (section == Section::Interface) {
                if (key.equalsIgnoreCase("PrivateKey")) {
                    cfg.privateKey = value;
                } else if (key.equalsIgnoreCase("Address")) {
                    cfg.localAddress = stripCidr(value);
                } else if (key.equalsIgnoreCase("DNS")) {
                    cfg.dns = value;
                }
            } else if (section == Section::Peer) {
                if (key.equalsIgnoreCase("PublicKey")) {
                    cfg.peerPublicKey = value;
                } else if (key.equalsIgnoreCase("PresharedKey")) {
                    cfg.presharedKey = value;
                } else if (key.equalsIgnoreCase("AllowedIPs")) {
                    cfg.allowedIps = value;
                } else if (key.equalsIgnoreCase("Endpoint")) {
                    int colon = value.lastIndexOf(':');
                    if (colon != -1) {
                        cfg.endpointHost = value.substring(0, colon);
                        cfg.endpointPort = value.substring(colon + 1).toInt();
                    }
                }
            }
        }

        cfg.valid = cfg.privateKey.length() > 0
            && cfg.localAddress.length() > 0
            && cfg.peerPublicKey.length() > 0
            && cfg.endpointHost.length() > 0
            && cfg.endpointPort > 0;

        return cfg;
    }

    void writeToNvs(const VpnConfig::Config& cfg) {
        vpnConfigPrefs.begin(CFG_PREFS_NAMESPACE, false);
        vpnConfigPrefs.putString(KEY_LOCAL_ADDR, cfg.localAddress);
        vpnConfigPrefs.putString(KEY_DNS, cfg.dns);
        vpnConfigPrefs.putString(KEY_PRIVATE_KEY, cfg.privateKey);
        vpnConfigPrefs.putString(KEY_ENDPOINT_HOST, cfg.endpointHost);
        vpnConfigPrefs.putInt(KEY_ENDPOINT_PORT, cfg.endpointPort);
        vpnConfigPrefs.putString(KEY_PEER_PUBLIC_KEY, cfg.peerPublicKey);
        vpnConfigPrefs.putString(KEY_PRESHARED_KEY, cfg.presharedKey);
        vpnConfigPrefs.putString(KEY_ALLOWED_IPS, cfg.allowedIps);
        vpnConfigPrefs.putBool(KEY_VALID, true);
        vpnConfigPrefs.end();
    }

}

namespace VpnConfig {

    Config load() {
        Config cfg;
        DEBUG_PRINTLN("[VPNCONFIG] diagnostica: VpnConfig::load() chiamata");

        if (!ensureSdMounted()) {
            return cfg;
        }

        DEBUG_PRINTLN("[VPNCONFIG] diagnostica: prima di SD.open()");
        File file = SD.open("/wireguard.conf", FILE_READ);
        DEBUG_PRINTLN("[VPNCONFIG] diagnostica: dopo SD.open()");
        if (!file) {
            DEBUG_PRINTLN("[VPNCONFIG] /wireguard.conf non trovato sulla SD");
            return cfg;
        }

        String text;
        text.reserve(2048);
        while (file.available()) {
            text += static_cast<char>(file.read());
        }
        file.close();

        cfg = parseWireguardConfText(text);

        // Diagnostica: SOLO campi non sensibili, mai chiavi. Serve a
        // confermare dal vivo che il file e' stato letto/interpretato
        // correttamente senza rischiare di stampare un segreto sulla
        // seriale (che chiunque con un cavo USB potrebbe leggere).
        DEBUG_PRINTF(
            "[VPNCONFIG] valido=%d indirizzo=%s endpoint=%s:%d dns=%s\n",
            cfg.valid ? 1 : 0, cfg.localAddress.c_str(),
            cfg.endpointHost.c_str(), cfg.endpointPort, cfg.dns.c_str()
        );

        return cfg;
    }

    bool hasSavedConfig() {
        vpnConfigPrefs.begin(CFG_PREFS_NAMESPACE, true);
        bool valid = vpnConfigPrefs.getBool(KEY_VALID, false);
        vpnConfigPrefs.end();
        return valid;
    }

    bool importFromSd() {
        Config cfg = load();
        if (!cfg.valid) {
            return false;
        }

        writeToNvs(cfg);
        DEBUG_PRINTLN("[VPNCONFIG] configurazione importata da SD in memoria interna (NVS)");
        return true;
    }

    bool importFromSerial() {
        DEBUG_PRINTLN("[VPNCONFIG] in ascolto sulla USB seriale per wireguard.conf...");

        constexpr uint32_t OVERALL_TIMEOUT_MS = 15000;
        constexpr uint32_t IDLE_END_MS = 1200; // silenzio dopo l'ultimo byte = trasferimento concluso

        String text;
        text.reserve(2048);
        uint32_t startMs = millis();
        uint32_t lastByteAtMs = 0;
        bool gotAny = false;

        while (millis() - startMs < OVERALL_TIMEOUT_MS) {
            if (Serial.available()) {
                while (Serial.available()) {
                    text += static_cast<char>(Serial.read());
                }
                gotAny = true;
                lastByteAtMs = millis();
            } else if (gotAny && millis() - lastByteAtMs > IDLE_END_MS) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        if (!gotAny) {
            DEBUG_PRINTLN("[VPNCONFIG] nessun dato ricevuto via seriale (timeout 15s)");
            return false;
        }

        Config cfg = parseWireguardConfText(text);
        if (!cfg.valid) {
            DEBUG_PRINTLN("[VPNCONFIG] dati ricevuti via seriale non validi/incompleti");
            return false;
        }

        writeToNvs(cfg);
        DEBUG_PRINTLN("[VPNCONFIG] configurazione ricevuta via seriale e salvata in memoria interna (NVS)");
        return true;
    }

    Config loadSaved() {
        Config cfg;

        vpnConfigPrefs.begin(CFG_PREFS_NAMESPACE, true);
        cfg.valid = vpnConfigPrefs.getBool(KEY_VALID, false);
        if (cfg.valid) {
            cfg.localAddress = vpnConfigPrefs.getString(KEY_LOCAL_ADDR, "");
            cfg.dns = vpnConfigPrefs.getString(KEY_DNS, "");
            cfg.privateKey = vpnConfigPrefs.getString(KEY_PRIVATE_KEY, "");
            cfg.endpointHost = vpnConfigPrefs.getString(KEY_ENDPOINT_HOST, "");
            cfg.endpointPort = vpnConfigPrefs.getInt(KEY_ENDPOINT_PORT, 0);
            cfg.peerPublicKey = vpnConfigPrefs.getString(KEY_PEER_PUBLIC_KEY, "");
            cfg.presharedKey = vpnConfigPrefs.getString(KEY_PRESHARED_KEY, "");
            cfg.allowedIps = vpnConfigPrefs.getString(KEY_ALLOWED_IPS, "");
        }
        vpnConfigPrefs.end();

        return cfg;
    }

    void clearSaved() {
        vpnConfigPrefs.begin(CFG_PREFS_NAMESPACE, false);
        vpnConfigPrefs.clear();
        vpnConfigPrefs.end();
        DEBUG_PRINTLN("[VPNCONFIG] configurazione salvata cancellata da NVS");
    }
}
