#include "Board.h"
#include "Config.h"

#include <Arduino.h>
#include <Wire.h>

namespace {
    // Host HSPI dedicato al bus condiviso display + LoRa + SD.
    SPIClass sharedSPI(HSPI);
}

namespace Board {

    void begin() {
        Serial.begin(115200);
        delay(300);

        // Alimentazione periferiche: va portata HIGH prima di init
        // qualsiasi bus verso tastiera, radio, GPS, display.
        pinMode(BOARD_POWERON, OUTPUT);
        digitalWrite(BOARD_POWERON, HIGH);
        delay(100);

        // I2C: unico punto di inizializzazione (tastiera + eventuale IO expander).
        Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
        Wire.setClock(400000);

        // Deseleziona esplicitamente gli altri device sul bus condiviso
        // prima di qualunque comunicazione SPI, altrimenti possono interferire.
        pinMode(BOARD_SDCARD_CS, OUTPUT);
        digitalWrite(BOARD_SDCARD_CS, HIGH);

        pinMode(RADIO_CS_PIN, OUTPUT);
        digitalWrite(RADIO_CS_PIN, HIGH);

        //sharedSPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI, -1);

        Serial.println();
        Serial.println("================================");
        Serial.println(APP_NAME);
        Serial.print("Versione: ");
        Serial.println(APP_VERSION);
        Serial.println("================================");
    }

    void update() {
        // Riservato ad aggiornamenti hardware futuri
    }

    void restart() {
        ESP.restart();
    }

    void setBacklight(bool enabled) {
        digitalWrite(BOARD_TFT_BACKLIGHT, enabled ? HIGH : LOW);
    }

    SPIClass& getSharedSPI() {
        return sharedSPI;
    }
}
