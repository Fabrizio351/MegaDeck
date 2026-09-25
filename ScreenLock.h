#pragma once

// Blocco schermo stile "vecchi Nokia": la combinazione Q poi Spazio
// (in sequenza, entro una finestra breve) blocca l'input; dopo qualche
// secondo lo schermo si spegne da solo per risparmiare batteria pur
// restando acceso (GPS/LoRa/BT continuano a funzionare in background).
// Qualsiasi tasto/tocco riaccende lo schermo senza sbloccarlo; la
// stessa combinazione Q+Spazio sblocca.
//
// Mentre e' bloccato, App::update() salta del tutto la normale gestione
// di menu/schermate (vedi App.cpp): nessun input arriva alle schermate
// sottostanti finche' non si sblocca.
namespace ScreenLock {
    void begin();
    void update(); // da chiamare per PRIMA cosa in App::update(), ogni ciclo

    bool isLocked();
}
