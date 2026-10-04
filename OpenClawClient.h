#pragma once

#include <Arduino.h>

// Client HTTP verso la macchina "OpenClaw" sulla rete locale (hub
// personale: LLM locale tipo Qwen + gestione automazioni Home Assistant/
// Frigate) - il T-Deck fa solo da terminale chat testuale, tutta la
// logica vive sulla macchina.
//
// Endpoint A2A (02/10, protocollo REALE confermato con un test curl
// riuscito dal PC dell'utente - sostituisce sia il vecchio "/v1/chat/
// completions" in stile OpenAI, SIA un primo tentativo "/api/a2a/message"
// con corpo REST semplice che si e' rivelato sbagliato, 404 sia dal
// firmware che da curl). E' il protocollo A2A standard su JSON-RPC 2.0:
//   POST http://<host>:<porta>/a2a/v1
//   Header: Authorization: Bearer <token>  (se impostato)
//   Header: Content-Type: application/json
//   Corpo:    {"jsonrpc":"2.0","id":"tdeck-req","method":"SendMessage",
//              "params":{"message":{"messageId":"tdeck-<hex casuale>",
//              "role":"ROLE_USER","parts":[{"text":"..."}]}}}
//   Risposta: {"jsonrpc":"2.0","id":"...","result":{"task":{...,
//              "artifacts":[{"parts":[{"text":"..."}]}],"history":[]}}}
// messageId generato ad ogni invio (esp_random()); l'id JSON-RPC esterno
// resta fisso - le richieste sono sempre sincrone una alla volta, non
// serve correlarle. HTTP semplice, non HTTPS: rete locale di casa, stesso
// livello di fiducia gia' assunto da NetScan/NetGuard in questo progetto.
//
// Timeout ora CONFIGURABILE dal dispositivo (28/09, richiesto: "magari
// metti nei parametri anche il timeout cosi faccio poi tuning sulle
// risposte") - persistito in NVS come host/porta/token, default 60s
// (confermato dall'utente: "in linea di massima con un modello normale
// 60 sec vanno bene"). Sull'hardware ATTUALE dell'utente il modello
// locale e' pero' molto piu' lento (minicpm5-2b-32k su Ollama, CPU al
// 98%): un primo test end-to-end (28/09) ha confermato l'intera catena
// funzionante (WiFi -> gateway -> Ollama, CPU che si muove davvero alla
// richiesta) ma con timeout scaduto prima che il modello finisse. Per
// questo l'attesa della risposta e' implementata a mano su un socket
// grezzo (non HTTPClient - vedi OpenClawClient.cpp): HTTPClient::
// setTimeout() accetta solo un uint16_t, quindi non puo' rappresentare
// piu' di ~65s, troppo poco per un tuning che deve poter arrivare anche
// a diversi minuti.
namespace OpenClawClient {

    struct Result {
        bool valid;
        String reply;
        String error; // messaggio leggibile se valid==false
    };

    // Chiamata BLOCCANTE (richiesta HTTP - puo' richiedere qualche
    // secondo se il LLM locale e' lento a rispondere): va richiamata
    // solo su azione esplicita dell'utente. Richiede WiFi connesso e
    // host/porta gia' configurati (vedi isConfigured()).
    Result sendMessage(const String& message);

    bool isConfigured();
    String getHost();
    int getPort();
    String getToken(); // vuoto se non impostato (header omesso in quel caso)
    int getTimeoutSeconds();

    // Persistiti in NVS (Preferences), sopravvivono al riavvio - a
    // differenza di WiFi/VPN/NetGuard non c'e' un vero e proprio stato
    // "acceso/spento" da resettare ad ogni avvio, solo dati di
    // configurazione.
    void setConfig(const String& host, int port, const String& token, int timeoutSeconds);
}
