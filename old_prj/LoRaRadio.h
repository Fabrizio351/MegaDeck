#pragma once

#include <Arduino.h>

namespace LoRaRadio {
    bool begin();
    void update();  // da chiamare ad ogni loop, non bloccante

    bool send(const String& text);

    bool available();     // true se c'e' un messaggio ricevuto da leggere
    String read();         // consuma il messaggio disponibile

    float lastRSSI();
    float lastSNR();
}
