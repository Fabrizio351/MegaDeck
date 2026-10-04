#pragma once

// LoRaWAN (29/09) - nuova voce dentro il sottomenu LORA TOOLS, insieme
// a LoRa Chat e LoRa Scan (vedi LoRaToolsMenu.cpp), per usi IoT verso un
// vero network server (TTN, ChirpStack, ecc.), separato dalla chat LoRa
// punto-punto gia' esistente (LoRaChatUI.cpp).
// RadioLib ha gia' il protocollo pronto (protocols/LoRaWAN) - qui
// serve "solo" l'interfaccia: credenziali OTAA (Join EUI/Dev EUI/App
// Key) + join + un test di invio. Condivide la STESSA radio SX1262
// della chat LoRa (mai due istanze contemporanee) tramite lo stesso
// schema pausa/ripresa gia' usato da LoRaScanUI.cpp (LoRaRadio::
// pauseForExternalUse()/getRadioForExternalUse()/resumeAfterExternalUse()).
namespace LoRaWanUI {
    void open();
    void update();
    bool isActive();
}
