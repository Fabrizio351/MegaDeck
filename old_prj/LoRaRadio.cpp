#include "LoRaRadio.h"
#include "Config.h"
#include "Board.h"

#include <RadioLib.h>

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

    void IRAM_ATTR onRadioAction() {
        packetFlag = true;
    }

    void startListening() {
        radio.startReceive();
    }
}

namespace LoRaRadio {

    bool begin() {
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
            Serial.print("[LORA] init fallito, codice: ");
            Serial.println(state);
            return false;
        }

        radio.setDio1Action(onRadioAction);
        startListening();

        Serial.println("[LORA] pronto, in ascolto");
        return true;
    }

    void update() {
        if (!packetFlag) {
            return;
        }

        packetFlag = false;

        String payload;
        int state = radio.readData(payload);

        if (state == RADIOLIB_ERR_NONE) {
            lastMessage = payload;
            lastRssiValue = radio.getRSSI();
            lastSnrValue = radio.getSNR();
        } else {
            Serial.print("[LORA] errore ricezione: ");
            Serial.println(state);
        }

        startListening();
    }

    bool send(const String& text) {
        String payload = text; // RadioLib vuole una String& non-const

        int state = radio.transmit(payload);

        // Torna sempre in ascolto dopo una TX
        startListening();

        if (state != RADIOLIB_ERR_NONE) {
            Serial.print("[LORA] errore invio: ");
            Serial.println(state);
            return false;
        }

        return true;
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
}
