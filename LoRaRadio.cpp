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

    bool radioReady = false;
}

namespace LoRaRadio {

    bool begin() {
#if !LORA_ENABLED
        DEBUG_PRINTLN("[LORA] disabilitata (LORA_ENABLED=0), skip totale (bus SPI non toccato)");
        return false;
#else
        radioSPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI, RADIO_CS_PIN);

        // Parametri di partenza qualsiasi solo per accendere il chip -
        // vedi il commento in LoRaRadio.h, chi prende in mano la radio
        // dopo la riconfigura sempre per conto proprio.
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

        radioReady = true;
        radio.standby(); // nessuno ascolta di default (02/10, vedi LoRaRadio.h)
        DEBUG_PRINTLN("[LORA] pronta, in standby (nessun ascolto di base)");
        return true;
#endif
    }

    bool isReady() {
        return radioReady;
    }

    void pauseForExternalUse() {
        if (!radioReady) {
            return;
        }
        radio.standby();
    }

    void resumeAfterExternalUse() {
        // Vedi il commento in LoRaRadio.h - lasciata volutamente in
        // standby, nessun "proprietario di base" a cui restituirla.
        pauseForExternalUse();
    }

    SX1262& getRadioForExternalUse() {
        return radio;
    }
}
