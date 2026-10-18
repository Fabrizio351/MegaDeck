#pragma once

#include <Arduino.h>

// Client HTTP verso il server API di Hermes Agent (Nous Research) sulla
// rete locale - sostituisce sia il vecchio OpenClawClient (backend
// diverso) SIA il tentativo via Telegram Bot API (07/10, abbandonato lo
// stesso giorno: Telegram non permette a un bot di "parlare come se
// fosse l'utente", quindi mandare messaggi col token del BOT non
// attivava mai la logica di Hermes - vedi project_telegram_bot_chat_built
// in memoria per il resoconto completo di perche' non funzionava).
//
// Endpoint REALE (confermato dalla documentazione ufficiale di Hermes
// Agent, hermes-agent.nousresearch.com/docs - niente di indovinato):
// formato "OpenAI-compatible", lo stesso standard de-facto usato da
// Ollama/LM Studio/vLLM/text-generation-webui e tanti altri backend
// self-hosted - scelto apposta dall'utente per restare piu' a prova di
// futuro rispetto a un contratto proprietario.
//   POST http://<host>:<porta>/v1/chat/completions
//   Header: Authorization: Bearer <token>  (API_SERVER_KEY di Hermes)
//   Header: Content-Type: application/json
//   Corpo:    {"model":"hermes-agent","messages":[{"role":"user",
//              "content":"..."}],"stream":false}
//   Risposta: {"choices":[{"message":{"role":"assistant",
//              "content":"..."}}], ...}
// Risposta SINCRONA nella stessa chiamata (a differenza del tentativo
// Telegram) - stesso identico schema del vecchio OpenClaw pre-A2A,
// solo endpoint/corpo diversi.
//
// Timeout configurabile dal dispositivo (stesso motivo di OpenClaw:
// Hermes e' anch'esso un agente AI self-hosted, puo' essere lento a
// seconda del modello/hardware dietro) - implementato su un socket
// grezzo (WiFiClient, non HTTPClient): HTTPClient::setTimeout() accetta
// solo un uint16_t, non puo' rappresentare piu' di ~65s, troppo poco
// per un tuning che deve poter arrivare anche a diversi minuti. HTTP
// semplice, non HTTPS: rete locale di casa, stesso livello di fiducia
// gia' assunto da NetScan/NetGuard/il vecchio OpenClaw in questo
// progetto.
namespace HermesClient {

    struct Result {
        bool valid;
        String reply;
        String error; // messaggio leggibile se valid==false
    };

    // Chiamata BLOCCANTE (richiesta HTTP - puo' richiedere qualche
    // secondo se il modello e' lento a rispondere): va richiamata solo
    // su azione esplicita dell'utente. Richiede WiFi connesso e host/
    // porta gia' configurati (vedi isConfigured()).
    Result sendMessage(const String& message);

    bool isConfigured();
    String getHost();
    int getPort();
    String getToken(); // vuoto se non impostato (header omesso in quel caso)
    int getTimeoutSeconds();

    // Persistiti in NVS (Preferences), sopravvivono al riavvio.
    void setConfig(const String& host, int port, const String& token, int timeoutSeconds);
}
