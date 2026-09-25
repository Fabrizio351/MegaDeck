#pragma once

// Chat via Bluetooth LE: stesso schema della chat LoRa (broadcast a
// singolo salto, nessun account/connessione) ma a corto raggio (decine
// di metri). Gestisce da sola l'input finche' e' attiva, come le altre
// schermate.
namespace BleChatUI {
    void open();
    void update();
    bool isActive();

    // Drena i messaggi ricevuti nella cronologia e suona la notifica:
    // va chiamata ad OGNI ciclo, indipendentemente da isActive() (cosi'
    // un messaggio arrivato mentre si e' in un altro menu non va perso).
    void poll();
}
