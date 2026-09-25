#pragma once

#include "ArpSweep.h"

// Scansione ARP della sottorete locale (quella a cui si e' connessi via
// WiFi): trova gli host attivi e il loro indirizzo MAC. Non e' un vero
// "port/ping scan" - si limita a forzare una risoluzione ARP per ogni
// host possibile e leggere il risultato dalla tabella ARP di lwIP, che
// e' un'operazione leggera e non richiede permessi speciali sulla rete.
//
// L'ultima lista trovata resta in memoria finche' non si avvia una
// nuova scansione (start() la azzera): NetScanUI la ripropone
// all'apertura invece di rifare sempre una scansione da zero, ed e'
// anche la "whitelist" di riferimento per NetGuard (vedi NetGuard.h).
namespace NetScan {

    using Device = ArpSweep::Device;

    // Avvia una scansione asincrona (non blocca). Richiede di essere
    // connessi al WiFi: se non lo si e', non fa nulla.
    void start();

    bool isScanning();

    // 0-254 mentre isScanning() e' vera; non significativo altrimenti.
    int getProgressPercent();

    int getDeviceCount();
    Device getDevice(int index);

    // Da chiamare ad ogni ciclo (es. da App::update() o dalla UI che la
    // mostra): avanza la scansione di un host alla volta, senza mai
    // bloccare il loop principale.
    void update();
}
