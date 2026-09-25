#pragma once

#include <SPI.h>

namespace Board {
    void begin();
    void update();

    void restart();
    void setBacklight(bool enabled);

    // Bus SPI condiviso da display, LoRa e SD (stessi pin fisici, CS diversi)
    SPIClass& getSharedSPI();
}
