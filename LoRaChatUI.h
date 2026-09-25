#pragma once

// Chat via radio LoRa: broadcast con inoltro multi-hop (mesh a flood,
// TTL 3 - vedi LoRaRadio::send()), non solo i dispositivi nel raggio
// radio diretto. Gestisce da sola l'input finche' e' attiva, come le
// altre schermate.
namespace LoRaChatUI {
    void open();
    void update();
    bool isActive();

    // Drena i messaggi ricevuti nella cronologia e suona la notifica:
    // va chiamata ad OGNI ciclo, indipendentemente da isActive() (cosi'
    // un messaggio arrivato mentre si e' in un altro menu non va perso -
    // vedi il commento in LoRaChatUI.cpp).
    void poll();

    // true dal momento in cui arriva un messaggio non ancora visto
    // (mentre la chat non era aperta) fino a quando si riapre la chat:
    // usato per colorare di giallo l'icona LoRa in alto, cosi' si nota
    // un messaggio in arrivo anche da un'altra schermata.
    bool hasUnreadMessages();
}
