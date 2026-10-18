#include "HermesClient.h"
#include "Config.h"

#include <WiFi.h>
#include <Preferences.h>

namespace {

    Preferences prefs;
    constexpr const char* PREFS_NAMESPACE = "tdeckhermes";
    constexpr const char* KEY_HOST = "host";
    constexpr const char* KEY_PORT = "port";
    constexpr const char* KEY_TOKEN = "token";
    constexpr const char* KEY_TIMEOUT = "timeoutSec";

    // Stesso default "ragionevole" gia' confermato dall'utente per
    // OpenClaw (60s) - Hermes e' anch'esso un agente AI self-hosted,
    // stesso tipo di incertezza sui tempi di risposta a seconda del
    // modello/hardware dietro.
    constexpr int DEFAULT_TIMEOUT_SEC = 60;

    // Versione pubblica: nessun host/token precompilato - vuoti, da
    // impostare dalla schermata CONFIG (host/porta/token del PROPRIO
    // server Hermes). 8642 e' la porta reale di default documentata da
    // Hermes stesso (non personale, comodo punto di partenza - vedi
    // commit precedenti per come e' stata confermata dal vivo).
    constexpr const char* DEFAULT_HOST = "";
    constexpr int DEFAULT_PORT = 8642;
    constexpr const char* DEFAULT_TOKEN = "";

    bool nvsOpened = false;
    String host;
    int port = 0;
    String token;
    int timeoutSeconds = DEFAULT_TIMEOUT_SEC;

    void ensureNvsOpen() {
        if (nvsOpened) {
            return;
        }
        prefs.begin(PREFS_NAMESPACE, false);
        host = prefs.getString(KEY_HOST, DEFAULT_HOST);
        port = prefs.getInt(KEY_PORT, DEFAULT_PORT);
        token = prefs.getString(KEY_TOKEN, DEFAULT_TOKEN);
        timeoutSeconds = prefs.getInt(KEY_TIMEOUT, DEFAULT_TIMEOUT_SEC);
        nvsOpened = true;
    }

    // Stesso parser "niente libreria JSON" gia' usato nel progetto
    // (ultima volta in OpenClawClient.cpp) - qui per il campo
    // "content" dentro choices[0].message, formato OpenAI-compatible.
    // Funziona perche' "content" compare una sola volta nella risposta
    // reale di Hermes (vedi HermesClient.h), senza bisogno di un vero
    // parser JSON strutturato.
    String extractJsonStringField(const String& json, const char* key) {
        String pattern = String("\"") + key + "\":\"";
        int start = json.indexOf(pattern);
        if (start < 0) {
            return "";
        }
        start += pattern.length();

        String out;
        int len = json.length();
        for (int i = start; i < len; i++) {
            char c = json.charAt(i);
            if (c == '\\' && i + 1 < len) {
                char next = json.charAt(i + 1);
                switch (next) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    default: out += next; break;
                }
                i++;
                continue;
            }
            if (c == '"') {
                break;
            }
            out += c;
        }
        return out;
    }

    String jsonEscape(const String& text) {
        String out;
        int len = text.length();
        for (int i = 0; i < len; i++) {
            char c = text.charAt(i);
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (c == '\n') {
                out += "\\n";
            } else {
                out += c;
            }
        }
        return out;
    }
}

namespace HermesClient {

    bool isConfigured() {
        ensureNvsOpen();
        return host.length() > 0 && port > 0;
    }

    String getHost() {
        ensureNvsOpen();
        return host;
    }

    int getPort() {
        ensureNvsOpen();
        return port;
    }

    String getToken() {
        ensureNvsOpen();
        return token;
    }

    int getTimeoutSeconds() {
        ensureNvsOpen();
        return timeoutSeconds;
    }

    void setConfig(const String& newHost, int newPort, const String& newToken, int newTimeoutSeconds) {
        ensureNvsOpen();
        host = newHost;
        port = newPort;
        token = newToken;
        timeoutSeconds = (newTimeoutSeconds > 0) ? newTimeoutSeconds : DEFAULT_TIMEOUT_SEC;
        prefs.putString(KEY_HOST, host);
        prefs.putInt(KEY_PORT, port);
        prefs.putString(KEY_TOKEN, token);
        prefs.putInt(KEY_TIMEOUT, timeoutSeconds);
    }

    Result sendMessage(const String& message) {
        Result result;
        result.valid = false;

        ensureNvsOpen();

        if (WiFi.status() != WL_CONNECTED) {
            result.error = "Serve una rete WiFi connessa.";
            return result;
        }
        if (!isConfigured()) {
            result.error = "Configura prima indirizzo e porta di Hermes.";
            return result;
        }

        // Socket grezzo (WiFiClient), NIENTE HTTPClient - vedi
        // HermesClient.h sul perche' (timeout configurabile oltre i
        // ~65s che HTTPClient::setTimeout() puo' rappresentare).
        WiFiClient client;
        constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;
        if (!client.connect(host.c_str(), port, CONNECT_TIMEOUT_MS)) {
            result.error = "Impossibile contattare " + host + ":" + String(port);
            return result;
        }

        // Formato OpenAI-compatible reale di Hermes (vedi HermesClient.h,
        // confermato dalla documentazione ufficiale) - "model" e'
        // richiesto dal contratto ma Hermes ignora quale modello dici,
        // usa sempre il suo agente configurato.
        String body = "{\"model\":\"hermes-agent\",\"messages\":[{\"role\":\"user\",\"content\":\""
            + jsonEscape(message) + "\"}],\"stream\":false}";

        String request = "POST /v1/chat/completions HTTP/1.1\r\n";
        request += "Host: " + host + "\r\n";
        request += "Content-Type: application/json\r\n";
        if (token.length() > 0) {
            request += "Authorization: Bearer " + token + "\r\n";
        }
        request += "User-Agent: TDeckFW/" APP_VERSION "\r\n";
        request += "Content-Length: " + String(body.length()) + "\r\n";
        request += "Connection: close\r\n";
        request += "\r\n";
        request += body;

        client.print(request);

        // Attesa manuale con scadenza in millis() (vedi commento sopra) -
        // finche' non arriva almeno un byte E la connessione si chiude
        // (server con "Connection: close" onorato), o scade il timeout.
        uint32_t deadline = millis() + static_cast<uint32_t>(timeoutSeconds) * 1000;
        String raw;
        while (static_cast<int32_t>(deadline - millis()) > 0) {
            while (client.available()) {
                raw += static_cast<char>(client.read());
            }
            if (!client.connected() && !client.available()) {
                break;
            }
            delay(20);
        }
        client.stop();

        if (raw.length() == 0) {
            result.error = "Nessuna risposta da Hermes entro " + String(timeoutSeconds) + "s (timeout).";
            return result;
        }

        int bodyStart = raw.indexOf("\r\n\r\n");
        if (bodyStart < 0) {
            result.error = "Risposta non valida da Hermes (header malformati).";
            return result;
        }
        String statusLine = raw.substring(0, raw.indexOf("\r\n"));
        String headers = raw.substring(0, bodyStart);
        String payload = raw.substring(bodyStart + 4);

        int firstSpace = statusLine.indexOf(' ');
        int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
        int httpCode = (firstSpace >= 0 && secondSpace > firstSpace)
            ? statusLine.substring(firstSpace + 1, secondSpace).toInt()
            : 0;

        // Header diagnostico REALE di Hermes (07/10, trovato dal vivo -
        // non in documentazione, nell'header della risposta): anche con
        // HTTP 200 "X-Hermes-Error: ..." indica un problema lato server
        // (es. modello configurato non disponibile sul catalogo/
        // provider) - molto piu' chiaro del generico "content mancante"
        // che si vedeva prima, controllato SEMPRE prima del resto cosi'
        // l'utente legge l'errore vero direttamente in chat.
        int errHeaderStart = headers.indexOf("X-Hermes-Error:");
        if (errHeaderStart >= 0) {
            errHeaderStart += String("X-Hermes-Error:").length();
            int errHeaderEnd = headers.indexOf("\r\n", errHeaderStart);
            String hermesError = headers.substring(errHeaderStart, errHeaderEnd < 0 ? headers.length() : errHeaderEnd);
            hermesError.trim();
            if (hermesError.length() > 0) {
                result.error = "Hermes: " + hermesError;
                return result;
            }
        }

        if (httpCode != 200) {
            result.error = "Hermes ha risposto con errore (codice " + String(httpCode) + ")";
            return result;
        }

        // Risposta OpenAI-compatible reale: choices[0].message.content -
        // extractJsonStringField cerca il primo "content":"..." nel testo
        // grezzo, senza un vero parser JSON (stesso approccio gia' in uso
        // nel progetto): funziona perche' "content" compare una sola
        // volta, dentro il primo choice/message.
        String reply = extractJsonStringField(payload, "content");
        if (reply.length() == 0) {
            result.error = "Risposta non valida da Hermes (campo \"content\" mancante).";
            return result;
        }

        result.valid = true;
        result.reply = reply;
        return result;
    }
}
