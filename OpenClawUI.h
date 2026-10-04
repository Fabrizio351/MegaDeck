#pragma once

// Chat testuale con la macchina "OpenClaw" sulla rete locale (hub
// personale: LLM locale + gestione automazioni Home Assistant/Frigate,
// vedi OpenClawClient.h per il contratto HTTP) - a differenza di
// LoRaChatUI l'invio e' BLOCCANTE (una richiesta HTTP con risposta
// diretta, non un broadcast radio asincrono): l'interfaccia mostra "in
// corso..." per la durata della richiesta.
namespace OpenClawUI {
    void open();

    // Apre direttamente la schermata di configurazione (host/porta/
    // token/timeout), saltando la chat - usata da SettingsUI.cpp (29/09,
    // richiesto: raggiungibile anche da Impostazioni per tenere tutto
    // ordinato). Indietro/FATTO da qui tornano a Impostazioni, non alla
    // chat.
    void openConfig();

    void update();
    bool isActive();
}
