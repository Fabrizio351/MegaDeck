#include "LoRaRadio.h"
#include "Config.h"
#include "Board.h"

#include <RadioLib.h>
#include <Preferences.h>
#include <esp_random.h>
#include <stdlib.h>

namespace {
    // Stesso bus SPI fisico del display (SCK/MOSI/MISO condivisi),
    // differenziato dal CS. Vedi nota in Board.h/Board.cpp.
    SPIClass radioSPI(HSPI);

    SX1262 radio = new Module(
        RADIO_CS_PIN,
        RADIO_DIO1_PIN,
        RADIO_RST_PIN,
        RADIO_BUSY_PIN,
        radioSPI
    );

    volatile bool packetFlag = false;
    String lastMessage;
    float lastRssiValue = 0;
    float lastSnrValue = 0;

    bool radioReady = false;

    // Macchina a stati minimale (come fa Meshtastic con RadioLib): DIO1
    // segnala eventi diversi a seconda della modalita' in cui si trovava
    // la radio quando l'interrupt e' scattato, quindi va tracciata per
    // interpretare correttamente il flag in update().
    enum class RadioState {
        Idle,          // radio in standby (LoRa disattivato dalle Impostazioni)
        Receiving,
        Transmitting
    };
    RadioState radioState = RadioState::Idle;

    // Piccola coda FIFO per i messaggi da inviare mentre una
    // trasmissione precedente e' ancora in corso: evita di dover
    // bloccare il chiamante (send()) in attesa che il canale si liberi.
    constexpr int TX_QUEUE_SIZE = 5;
    String txQueue[TX_QUEUE_SIZE];
    int txQueueHead = 0;
    int txQueueCount = 0;

    bool enqueueTx(const String& text) {
        if (txQueueCount >= TX_QUEUE_SIZE) {
            return false;
        }
        int tail = (txQueueHead + txQueueCount) % TX_QUEUE_SIZE;
        txQueue[tail] = text;
        txQueueCount++;
        return true;
    }

    bool dequeueTx(String& out) {
        if (txQueueCount == 0) {
            return false;
        }
        out = txQueue[txQueueHead];
        txQueueHead = (txQueueHead + 1) % TX_QUEUE_SIZE;
        txQueueCount--;
        return true;
    }

    // --- Mesh a piu' salti (flood con TTL) ---
    //
    // Ogni pacchetto porta un piccolo header testuale davanti al testo
    // vero e proprio: "<id 8 esadecimali><ttl>|<testo>". L'id identifica
    // in modo univoco un messaggio (generato con l'RNG hardware, non
    // serve coordinamento fra i nodi); il TTL e' quante volte il
    // messaggio puo' ancora essere rilanciato prima di fermarsi.
    //
    // Quando un nodo riceve un pacchetto con un id MAI visto prima, lo
    // mostra in chat e - se il TTL residuo e' maggiore di zero - lo
    // ritrasmette con lo stesso id e TTL-1, cosi' altri nodi fuori
    // portata diretta dal mittente originale possono comunque riceverlo.
    // Un id gia' visto (perche' l'abbiamo mandato noi, o perche' un
    // altro nodo l'ha gia' rilanciato fin qui) viene ignorato del tutto:
    // e' cosi' che si evitano echi ed inoltri all'infinito in una mesh
    // dove piu' nodi sentono lo stesso pacchetto.
    constexpr int DEFAULT_TTL = 3;
    constexpr int SEEN_CACHE_SIZE = 20;

    uint32_t seenIds[SEEN_CACHE_SIZE];
    int seenCount = 0;
    int seenNextSlot = 0;

    bool wasSeen(uint32_t msgId) {
        for (int i = 0; i < seenCount; i++) {
            if (seenIds[i] == msgId) {
                return true;
            }
        }
        return false;
    }

    void markSeen(uint32_t msgId) {
        seenIds[seenNextSlot] = msgId;
        seenNextSlot = (seenNextSlot + 1) % SEEN_CACHE_SIZE;
        if (seenCount < SEEN_CACHE_SIZE) {
            seenCount++;
        }
    }

    String encodePacket(uint32_t msgId, int ttl, const String& text) {
        char header[16];
        snprintf(header, sizeof(header), "%08lX%d|", static_cast<unsigned long>(msgId), ttl);
        return String(header) + text;
    }

    // Cerca il separatore '|' invece di assumere una posizione fissa:
    // cosi' funziona anche se in futuro il TTL massimo superasse le 9
    // unita' (piu' di una cifra).
    bool decodePacket(const String& raw, uint32_t* outMsgId, int* outTtl, String* outText) {
        int sep = raw.indexOf('|');
        if (sep < 9) {
            return false;
        }

        *outMsgId = strtoul(raw.substring(0, 8).c_str(), nullptr, 16);
        *outTtl = raw.substring(8, sep).toInt();
        *outText = raw.substring(sep + 1);
        return true;
    }

    Preferences loraPrefs;
    constexpr const char* LORA_PREFS_NAMESPACE = "tdecklora";
    constexpr const char* LORA_PREFS_KEY_ENABLED = "enabled";
    bool nvsOpened = false;
    bool runtimeEnabled = true;

    void ensureNvsOpen() {
        if (nvsOpened) {
            return;
        }

        loraPrefs.begin(LORA_PREFS_NAMESPACE, false);
        runtimeEnabled = loraPrefs.getBool(LORA_PREFS_KEY_ENABLED, true);
        nvsOpened = true;
    }

    void IRAM_ATTR onRadioAction() {
        packetFlag = true;
    }

    void startListening() {
        radio.startReceive();
        radioState = RadioState::Receiving;
    }

    // Avvia una trasmissione asincrona: ritorna subito, il completamento
    // arriva dall'interrupt DIO1 gestito in update() (stesso meccanismo
    // usato da Meshtastic per non bloccare mai il loop principale).
    bool startTransmitNow(const String& text) {
        String payload = text; // RadioLib vuole una String& non-const
        int state = radio.startTransmit(payload);

        if (state != RADIOLIB_ERR_NONE) {
            DEBUG_PRINT("[LORA] errore avvio invio: ");
            DEBUG_PRINTLN(state);
            return false;
        }

        radioState = RadioState::Transmitting;
        return true;
    }

    // Punto unico usato sia per i messaggi nuovi dell'utente (send())
    // sia per i rilanci della mesh: se un invio e' gia' in corso accoda
    // invece di aspettare, esattamente come gia' faceva send() da solo.
    bool transmitEncoded(const String& encoded) {
        if (radioState == RadioState::Transmitting) {
            return enqueueTx(encoded);
        }
        return startTransmitNow(encoded);
    }
}

namespace LoRaRadio {

    bool begin() {
#if !LORA_ENABLED
        DEBUG_PRINTLN("[LORA] disabilitata (LORA_ENABLED=0), skip totale (bus SPI non toccato)");
        return false;
#else
        radioSPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI, RADIO_CS_PIN);

        int state = radio.begin(
            RADIO_FREQ,
            RADIO_BANDWIDTH,
            RADIO_SF,
            RADIO_CR,
            RADIO_SYNC_WORD,
            RADIO_TX_POWER
        );

        if (state != RADIOLIB_ERR_NONE) {
            DEBUG_PRINT("[LORA] init fallito, codice: ");
            DEBUG_PRINTLN(state);
            return false;
        }

        radio.setDio1Action(onRadioAction);
        radioReady = true;

        ensureNvsOpen();
        if (runtimeEnabled) {
            startListening();
            DEBUG_PRINTLN("[LORA] pronto, in ascolto");
        } else {
            radio.standby();
            radioState = RadioState::Idle;
            DEBUG_PRINTLN("[LORA] pronto, ma disattivata dalle Impostazioni (standby)");
        }

        return true;
#endif
    }

    void update() {
#if !LORA_ENABLED
        return;
#else
        if (!radioReady || !packetFlag) {
            return;
        }

        packetFlag = false;

        if (radioState == RadioState::Transmitting) {
            // Invio precedente completato (o fallito): finishTransmit()
            // va sempre chiamato per rimettere la radio in uno stato
            // pulito, come richiesto da RadioLib dopo startTransmit().
            radio.finishTransmit();

            String next;
            if (dequeueTx(next)) {
                // C'era un altro messaggio in coda: parte subito, senza
                // tornare in ascolto nel frattempo.
                startTransmitNow(next);
            } else if (runtimeEnabled) {
                startListening();
            } else {
                // Disattivato mentre l'invio era in corso: lasciato
                // finire per non interromperlo a meta', ora si spegne.
                radio.standby();
                radioState = RadioState::Idle;
            }
            return;
        }

        if (radioState == RadioState::Receiving) {
            String payload;
            int state = radio.readData(payload);

            // Se il rilancio (sotto) avvia davvero una trasmissione,
            // radioState passa a Transmitting: NON bisogna richiamare
            // startListening() in quel caso, altrimenti interromperebbe
            // la trasmissione appena avviata invece di lasciarla finire.
            bool relaying = false;

            if (state == RADIOLIB_ERR_NONE) {
                uint32_t msgId = 0;
                int ttl = 0;
                String text;

                if (decodePacket(payload, &msgId, &ttl, &text) && !wasSeen(msgId)) {
                    markSeen(msgId);

                    lastMessage = text;
                    lastRssiValue = radio.getRSSI();
                    lastSnrValue = radio.getSNR();

                    if (ttl > 0) {
                        // Rilancia per estendere la portata oltre il
                        // salto diretto dal mittente originale: stesso
                        // id (per farlo riconoscere come duplicato dagli
                        // altri nodi), TTL decrementato di uno.
                        relaying = transmitEncoded(encodePacket(msgId, ttl - 1, text));
                    }
                }
                // Pacchetto malformato (non nostro) o id gia' visto
                // (duplicato/eco della mesh): ignorato silenziosamente,
                // ne' mostrato ne' rilanciato di nuovo.
            } else {
                DEBUG_PRINT("[LORA] errore ricezione: ");
                DEBUG_PRINTLN(state);
            }

            if (!relaying) {
                startListening();
            }
        }
#endif
    }

    bool send(const String& text) {
        if (!radioReady || !runtimeEnabled) {
            return false;
        }

        // Nuovo messaggio "originale": id casuale mai usato prima e TTL
        // pieno (DEFAULT_TTL salti disponibili). Segnato subito come
        // "gia' visto" cosi', se dovesse tornare indietro rilanciato da
        // un altro nodo della mesh, non venga rimostrato/rilanciato di
        // nuovo come se fosse un messaggio ricevuto.
        uint32_t msgId = esp_random();
        markSeen(msgId);

        return transmitEncoded(encodePacket(msgId, DEFAULT_TTL, text));
    }

    bool available() {
        return lastMessage.length() > 0;
    }

    String read() {
        String msg = lastMessage;
        lastMessage = "";
        return msg;
    }

    float lastRSSI() {
        return lastRssiValue;
    }

    float lastSNR() {
        return lastSnrValue;
    }

    bool isReady() {
        return radioReady;
    }

    bool isTransmitting() {
        return radioState == RadioState::Transmitting;
    }

    int pendingCount() {
        return txQueueCount;
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
        loraPrefs.putBool(LORA_PREFS_KEY_ENABLED, enabled);

        if (!radioReady || radioState == RadioState::Transmitting) {
            // Se c'e' un invio in corso lo lasciamo finire senza
            // interromperlo a meta': ci pensera' update() ad applicare
            // il nuovo stato (ascolto o standby) al termine della TX.
            return;
        }

        if (enabled) {
            startListening();
        } else {
            radio.standby();
            radioState = RadioState::Idle;
        }
    }
}
