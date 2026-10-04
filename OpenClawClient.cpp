#include "OpenClawClient.h"
#include "Config.h"

#include <WiFi.h>
#include <Preferences.h>

namespace {

    Preferences prefs;
    constexpr const char* PREFS_NAMESPACE = "tdeckopenclaw";
    constexpr const char* KEY_HOST = "host";
    constexpr const char* KEY_PORT = "port";
    constexpr const char* KEY_TOKEN = "token";
    constexpr const char* KEY_TIMEOUT = "timeoutSec";

    // 60s (28/09): valore di default "ragionevole" per una macchina con
    // risposte normali - ora CONFIGURABILE dal dispositivo (richiesto
    // dall'utente per fare tuning: il suo hardware attuale, un modello
    // locale minicpm5-2b-32k su Ollama con CPU al 98% durante
    // l'inferenza, ha impiegato oltre 2 minuti in un test dal vivo).
    constexpr int DEFAULT_TIMEOUT_SEC = 60;

    // Valori di default - LASCIATI VUOTI apposta in questo repository
    // pubblico: host/porta/token sono personali di ciascuna installazione
    // OpenClaw, non vanno mai pubblicati. Restano comunque
    // sovrascrivibili via NVS (setConfig()), cioe' dalla schermata
    // Impostazioni > OPENCLAW sul dispositivo stesso - ogni utente
    // inserisce li' i propri dati, nessuna modifica al codice necessaria.
    constexpr const char* DEFAULT_HOST = "";
    constexpr int DEFAULT_PORT = 18789;
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

    // Estrae un campo stringa da un JSON semplice ({"reply":"..."}),
    // gestendo gli escape piu' comuni (\", \\, \n, \r, \t) - a differenza
    // dei lookup piu' vecchi di questo progetto (GeoLookup/WeatherLookup,
    // valori senza escape), qui serve: una risposta di un LLM e' testo
    // libero, puo' contenere virgolette o andare a capo. Stessa filosofia
    // "niente libreria JSON pesante", solo un parser leggermente piu'
    // robusto.
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
                    case 'r': break; // scartato, non serve su questo schermo
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

    // Simmetrico a extractJsonStringField(): scrive il messaggio
    // dell'utente dentro il corpo della richiesta con lo stesso escape
    // minimo (") (\) e newline.
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

namespace OpenClawClient {

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
            result.error = "Configura prima indirizzo e porta di OpenClaw.";
            return result;
        }

        // Socket grezzo (WiFiClient), NIENTE HTTPClient (a differenza
        // degli altri lookup del progetto): HTTPClient::setTimeout()
        // accetta solo un uint16_t, quindi non puo' rappresentare piu' di
        // ~65s - troppo poco per il timeout configurabile dall'utente,
        // che deve poter arrivare a diversi minuti per fare tuning su un
        // modello locale lento. L'attesa qui sotto usa un proprio
        // conteggio in millis(), senza quel limite.
        WiFiClient client;
        constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;
        if (!client.connect(host.c_str(), port, CONNECT_TIMEOUT_MS)) {
            result.error = "Impossibile contattare " + host + ":" + String(port);
            return result;
        }

        // Endpoint A2A (02/10, protocollo REALE confermato con un test
        // curl riuscito dal PC dell'utente - il primo tentativo con
        // "/api/a2a/message" e un corpo REST semplice era SBAGLIATO,
        // rispondeva 404 sia dal firmware che da curl: il vero endpoint
        // e' "/a2a/v1", protocollo A2A standard su JSON-RPC 2.0, metodo
        // "SendMessage"). messageId generato ad ogni invio (esp_random(),
        // non serve coordinamento - l'id JSON-RPC esterno resta fisso,
        // le richieste sono sempre sincrone una alla volta, non serve
        // correlarle).
        char messageId[24];
        snprintf(messageId, sizeof(messageId), "tdeck-%08x", static_cast<unsigned>(esp_random()));

        String body = "{\"jsonrpc\":\"2.0\",\"id\":\"tdeck-req\",\"method\":\"SendMessage\",\"params\":{\"message\":{\"messageId\":\""
            + String(messageId) + "\",\"role\":\"ROLE_USER\",\"parts\":[{\"text\":\""
            + jsonEscape(message) + "\"}]}}}";

        String request = "POST /a2a/v1 HTTP/1.1\r\n";
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
            result.error = "Nessuna risposta da OpenClaw entro " + String(timeoutSeconds) + "s (timeout).";
            return result;
        }

        int bodyStart = raw.indexOf("\r\n\r\n");
        if (bodyStart < 0) {
            result.error = "Risposta non valida da OpenClaw (header malformati).";
            return result;
        }
        String statusLine = raw.substring(0, raw.indexOf("\r\n"));
        String payload = raw.substring(bodyStart + 4);

        // Codice di stato dalla status line ("HTTP/1.1 200 OK" -> 200).
        int firstSpace = statusLine.indexOf(' ');
        int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
        int httpCode = (firstSpace >= 0 && secondSpace > firstSpace)
            ? statusLine.substring(firstSpace + 1, secondSpace).toInt()
            : 0;

        if (httpCode != 200) {
            result.error = "OpenClaw ha risposto con errore (codice " + String(httpCode) + ")";
            return result;
        }

        // Risposta A2A reale (02/10, confermata con un test curl riuscito):
        // {"jsonrpc":"2.0","id":"...","result":{"task":{...,
        // "artifacts":[{"artifactId":"...","parts":[{"text":"..."}]}],
        // "history":[]}}} - extractJsonStringField cerca il primo
        // "text":"..." nel testo grezzo, senza un vero parser JSON (stesso
        // approccio gia' in uso nel progetto): funziona perche' "text"
        // compare una sola volta, dentro il primo artifact/part.
        String reply = extractJsonStringField(payload, "text");
        if (reply.length() == 0) {
            result.error = "Risposta non valida da OpenClaw (campo \"text\" mancante).";
            return result;
        }

        result.valid = true;
        result.reply = reply;
        return result;
    }
}
