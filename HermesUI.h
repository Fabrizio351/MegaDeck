#pragma once

// Chat testuale con l'agente Hermes sulla rete locale (hub personale -
// vedi HermesClient.h per il contratto HTTP) - sostituisce sia il
// vecchio OpenClawUI SIA il tentativo via Telegram Bot API (07/10,
// abbandonato lo stesso giorno - vedi project_telegram_bot_chat_built
// in memoria). L'invio e' BLOCCANTE (una richiesta HTTP con risposta
// diretta, non un broadcast radio asincrono): l'interfaccia mostra "in
// corso..." per la durata della richiesta.
namespace HermesUI {
    void open();

    // Apre direttamente la schermata di configurazione (host/porta/
    // token/timeout), saltando la chat - usata da SettingsUI.cpp.
    // Indietro/FATTO da qui tornano a Impostazioni, non alla chat.
    void openConfig();

    void update();
    bool isActive();
}
