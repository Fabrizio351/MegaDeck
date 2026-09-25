#pragma once

#include <SPI.h>

namespace Board {
    void begin();
    void update();

    void restart();

    // Retroilluminazione schermo a PWM (0-100%). Il valore viene salvato
    // in NVS (memoria non volatile) ad ogni modifica e ricaricato
    // all'avvio, cosi' si riparte da dove si era lasciata.
    void setBacklightPercent(int percent);
    int getBacklightPercent();

    // Come sopra ma SENZA scrivere in NVS ne' toccare il valore
    // "logico" restituito da getBacklightPercent(): per spegnimenti/
    // riaccensioni temporanei e potenzialmente frequenti (es. blocco
    // schermo, vedi ScreenLock.h) che non devono consumare cicli di
    // scrittura flash ad ogni blocco/sblocco.
    void setBacklightPercentTransient(int percent);

    // Retroilluminazione della tastiera fisica, comandata via I2C verso
    // il suo coprocessore. NB: il comando esatto non e' documentato
    // ufficialmente per questo modulo, uso quello piu' diffuso nella
    // community (registro 0x01 + valore 0-255) - va verificato sul
    // dispositivo reale, potrebbe non avere alcun effetto se il firmware
    // della tastiera si aspetta un protocollo diverso. Salvata in NVS
    // come quella dello schermo.
    void setKeyboardBacklightPercent(int percent);
    int getKeyboardBacklightPercent();

    // Come sopra ma SENZA scrivere in NVS: stesso motivo di
    // setBacklightPercentTransient().
    void setKeyboardBacklightPercentTransient(int percent);

    // Percentuale batteria stimata (0-100) dalla tensione letta su
    // BOARD_BAT_ADC, in base al partitore/range configurati in Config.h.
    int getBatteryPercent();

    // Bus SPI condiviso da display, LoRa e SD (stessi pin fisici, CS diversi)
    SPIClass& getSharedSPI();
}
