#pragma once

#include <Arduino.h>

namespace GPS {
    void begin();
    void update();  // da chiamare ad ogni loop, non bloccante

    bool hasFix();
    double getLatitude();
    double getLongitude();
    double getAltitudeMeters();
    double getSpeedKmh();
    int getSatellites();

    // Ora UTC grezza (0-23) dal GPS, -1 se non ancora disponibile: serve
    // per indicizzare correttamente le previsioni orarie (vedi WeatherUI),
    // che sono allineate a UTC indipendentemente dalla stima del fuso di
    // getUtcOffsetHours().
    int getUtcHour();

    // Fuso orario: interruttore manuale GMT+-N impostato dall'utente in
    // Impostazioni (persistente, NVS). Sostituisce la vecchia stima
    // automatica dalla longitudine, che non poteva comunque tenere conto
    // dell'ora legale/solare (in Europa cambia fra +1 e +2 a seconda del
    // periodo) ne' dei confini politici dei fusi reali. Range valido
    // -12..+14 (offset UTC standard). Default 0 finche' l'utente non lo
    // imposta la prima volta.
    int getUtcOffsetHours();
    void setUtcOffsetHours(int hours);

    // "HH:MM" nel fuso impostato (vedi getUtcOffsetHours/setUtcOffsetHours),
    // stringa vuota se non ancora disponibile
    String getTimeLocal();

    // "gg.mm.aa" nel fuso impostato, stringa vuota se non ancora disponibile
    String getDateLocal();

    // Interruttore persistente (NVS) per il risparmio energetico: quando
    // disattivato, update() smette di leggere/interpretare la UART.
    // Nota: il modulo GPS resta comunque alimentato (non c'e' un pin di
    // potenza dedicato solo a lui su questa scheda, condivide
    // BOARD_POWERON con tastiera/SD/radio), quindi il risparmio
    // energetico reale e' minimo - questo interruttore disattiva
    // soprattutto la funzione software. Sopravvive al riavvio.
    bool isEnabled();
    void setEnabled(bool enabled);
}
