#pragma once

#include <Arduino.h>

// "Guardiano" della rete locale: quando attivo, ripete periodicamente in
// background una scansione della rete (riusando lo STESSO motore
// condiviso di NetScan, vedi NetScan.h - mai un motore proprio, mai due
// scansioni attive in parallelo, l'unico approccio dimostratosi stabile
// su questo hardware dopo alcuni crash in prova) e confronta il
// risultato con la whitelist di prima del giro: qualunque dispositivo
// nuovo comparso fa scattare un allarme a schermo + un suono.
//
// Se l'utente avvia una scansione manuale da NetScan mentre NetGuard e'
// attivo, NetGuard si disattiva da solo (vedi NetScanUI.cpp): due
// richieste di scansione sullo stesso motore condiviso nello stesso
// momento non sono mai state provate ed e' meglio non rischiare.
//
// Lo stato acceso/spento NON sopravvive al riavvio (parte sempre
// spento) e si spegne da solo se cade la connessione WiFi: e' sempre
// l'utente a doverlo riaccendere esplicitamente.
namespace NetGuard {

    void begin();

    // Da chiamare ad ogni ciclo, SEMPRE (come GPS::update()/LoRaRadio::
    // update()): guida il ciclo di scansione periodico e si spegne da
    // solo se cade il WiFi.
    void update();

    bool isEnabled();

    // Accende/spegne il monitoraggio. Ritorna false (e NON attiva nulla)
    // se si chiede di accendere senza che esista ancora una whitelist
    // (NetScan::getDeviceCount() == 0): il chiamante (NetGuardUI) lo usa
    // per spiegare all'utente che serve prima un NetScan.
    bool setEnabled(bool enabled);

    // true se il monitoraggio e' acceso ma non esiste ancora nessuna
    // whitelist da controllare: utile per la UI, per mostrare "serve un
    // NetScan" invece di far credere che il monitoraggio sia gia' attivo.
    bool isWaitingForWhitelist();

    // true mentre e' in corso il giro di scansione periodico (riusa
    // NetScan::isScanning()/getProgressPercent() per il dettaglio):
    // utile per la UI, per mostrare l'avanzamento invece di "attivo" e
    // basta.
    bool isScanning();

    // Numero di dispositivi nella whitelist attuale (comodo per la UI,
    // equivalente a NetScan::getDeviceCount()).
    int getWhitelistCount();

    // Dettagli dell'ultimo allarme generato (vuoto se nessuno finora in
    // questa sessione).
    String getLastAlertSummary();

    // true dal momento in cui scatta un allarme fino a quando lo si
    // chiude (bottone Indietro/tap sulla schermata dell'allarme): usato
    // per colorare di rosso l'icona NetGuard in alto finche' non lo si
    // "acknowledge".
    bool hasPendingAlert();
}
