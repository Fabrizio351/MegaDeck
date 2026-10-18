#include "SipClient.h"
#include "Config.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <mbedtls/md5.h>

// User agent SIP scritto a mano (08/10) - vedi SipClient.h. Niente vera
// macchina a stati Timer A/B di RFC3261: due "transazioni pendenti"
// semplici (una per REGISTER, una per la chiamata corrente) con
// ritrasmissione a tentativi fissi - sufficiente per parlare con UN
// provider reale noto, non con whole internet (stessa scelta dichiarata
// nel piano approvato).
namespace {

    Preferences prefs;
    constexpr const char* PREFS_NAMESPACE = "tdecksip";
    constexpr const char* KEY_SERVER = "server";
    constexpr const char* KEY_PORT = "port";
    constexpr const char* KEY_DOMAIN = "domain";
    constexpr const char* KEY_USER = "user";
    constexpr const char* KEY_PASSWORD = "password";
    constexpr const char* KEY_DISPNAME = "dispname";

    bool nvsOpened = false;
    String server;   // destinazione di rete reale (registrar o outbound proxy)
    int port = 5060;
    String domain;   // dominio/realm per gli URI SIP - vuoto = usa "server"
    String user;
    String password;
    String displayName;

    // Account reale di questa installazione - LOCALE, NON committato
    // (vedi la versione pubblica su git: vuoti li', stesso trattamento
    // gia' usato per HermesClient.cpp). sip2sip.info (09/10): servizio
    // SIP-to-SIP gratuito usato per il primo test end-to-end del
    // protocollo scritto a mano, prima di passare a un provider pagante
    // con uscita verso la rete telefonica tradizionale (VoIP.ms).
    // IMPORTANTE (scoperto dal vivo il 09/10, zero risposta dal server
    // finche' non l'ho capito): sip2sip.info usa un outbound proxy
    // SEPARATO dal dominio (confermato sia dalla SIP account info
    // dell'utente sia dal DNS SRV _sip._udp.sip2sip.info) - i pacchetti
    // UDP vanno mandati a proxy.sipthor.net, NON a sip2sip.info, mentre
    // gli URI SIP (Request-URI/From/To, digest) restano sul dominio
    // sip2sip.info.
    // Versione pubblica: nessun account precompilato - vuoti, da
    // impostare dalla schermata CONFIG di VOIP (stesso schema gia' usato
    // per Hermes/OpenClaw). La porta standard SIP (5060) resta come
    // comodo punto di partenza, non e' un dato personale.
    constexpr const char* DEFAULT_SERVER = "";
    constexpr int DEFAULT_PORT = 5060;
    constexpr const char* DEFAULT_DOMAIN = "";
    constexpr const char* DEFAULT_USER = "";
    constexpr const char* DEFAULT_PASSWORD = "";
    constexpr const char* DEFAULT_DISPLAYNAME = "";

    void ensureNvsOpen() {
        if (nvsOpened) {
            return;
        }
        prefs.begin(PREFS_NAMESPACE, false);
        server = prefs.getString(KEY_SERVER, DEFAULT_SERVER);
        port = prefs.getInt(KEY_PORT, DEFAULT_PORT);
        domain = prefs.getString(KEY_DOMAIN, DEFAULT_DOMAIN);
        user = prefs.getString(KEY_USER, DEFAULT_USER);
        password = prefs.getString(KEY_PASSWORD, DEFAULT_PASSWORD);
        displayName = prefs.getString(KEY_DISPNAME, DEFAULT_DISPLAYNAME);
        nvsOpened = true;
    }

    // Dominio effettivo da usare dentro gli URI SIP: se l'utente non ha
    // impostato un dominio separato, "server" fa doppio uso (caso
    // comune, provider senza outbound proxy dedicato).
    String effectiveDomain() {
        return (domain.length() > 0) ? domain : server;
    }

    constexpr uint16_t LOCAL_SIP_PORT = 5060;
    // Porta RTP locale fissa: una sola chiamata alla volta in questo
    // progetto (niente multi-linea), nessun rischio di conflitto.
    constexpr uint16_t RTP_LOCAL_PORT = 40000;
    constexpr int REGISTER_EXPIRE_SEC = 3600;
    // NAT dei router di casa: la mappatura UDP scade tipicamente dopo
    // 30-60s di silenzio, molto piu' spesso della ri-registrazione -
    // vedi il piano, punto NAT.
    constexpr uint32_t KEEPALIVE_INTERVAL_MS = 20000;
    constexpr uint32_t RETRANSMIT_MS = 2000;
    constexpr int MAX_ATTEMPTS = 3;

    WiFiUDP udp;
    bool socketOpen = false;

    String pendingStatusText;
    bool pendingStatus = false;

    void setStatus(const String& text) {
        pendingStatusText = text;
        pendingStatus = true;
        DEBUG_PRINTLN("[SIP] " + text);
    }

    // --- Utilita' generiche (random, MD5, parsing header testuali) ---

    String randomHexString(int bytes) {
        String out;
        for (int i = 0; i < bytes; i++) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned>(random(0, 256)));
            out += buf;
        }
        return out;
    }

    String makeBranch() {
        return "z9hG4bK" + randomHexString(8);
    }

    String makeTag() {
        return randomHexString(8);
    }

    String makeCallId() {
        return randomHexString(12) + "@tdeckfw";
    }

    String md5Hex(const String& input) {
        unsigned char digest[16];
        mbedtls_md5_context ctx;
        mbedtls_md5_init(&ctx);
        mbedtls_md5_starts(&ctx);
        mbedtls_md5_update(&ctx, reinterpret_cast<const unsigned char*>(input.c_str()), input.length());
        mbedtls_md5_finish(&ctx, digest);
        mbedtls_md5_free(&ctx);

        char hex[33];
        for (int i = 0; i < 16; i++) {
            snprintf(hex + i * 2, 3, "%02x", digest[i]);
        }
        hex[32] = 0;
        return String(hex);
    }

    // Cerca "Nome: valore" (case-insensitive sul nome) e ritorna il
    // valore fino a fine riga, senza spazi. Stesso "niente libreria"
    // dell'approccio gia' usato per JSON in HermesClient.cpp, qui per
    // header SIP testuali.
    String getHeaderValue(const String& msg, const char* name) {
        String pattern = String("\r\n") + name + ":";
        String lowerMsg = msg;
        lowerMsg.toLowerCase();
        String lowerPattern = pattern;
        lowerPattern.toLowerCase();

        int start = lowerMsg.indexOf(lowerPattern);
        if (start < 0) {
            return "";
        }
        start += pattern.length();
        int end = msg.indexOf("\r\n", start);
        if (end < 0) {
            end = msg.length();
        }
        String value = msg.substring(start, end);
        value.trim();
        return value;
    }

    int getStatusCode(const String& msg) {
        if (!msg.startsWith("SIP/2.0")) {
            return 0;
        }
        int firstSpace = msg.indexOf(' ');
        int secondSpace = msg.indexOf(' ', firstSpace + 1);
        if (firstSpace < 0 || secondSpace < 0) {
            return 0;
        }
        return msg.substring(firstSpace + 1, secondSpace).toInt();
    }

    // Estrae un parametro "nome=valore" o "nome="valore"" da un valore
    // di header con piu' parametri separati da virgola (WWW-Authenticate,
    // To con ;tag=...). Ignora corrispondenze che sono in realta' la coda
    // di un altro nome (es. "qop" dentro "xqop").
    String getHeaderParam(const String& headerValue, const char* paramName) {
        String pattern = String(paramName) + "=";
        int idx = 0;
        while (true) {
            idx = headerValue.indexOf(pattern, idx);
            if (idx < 0) {
                return "";
            }
            if (idx > 0 && isAlphaNumeric(headerValue.charAt(idx - 1))) {
                idx += pattern.length();
                continue;
            }
            int valueStart = idx + pattern.length();
            if (valueStart < static_cast<int>(headerValue.length()) && headerValue.charAt(valueStart) == '"') {
                int endQuote = headerValue.indexOf('"', valueStart + 1);
                if (endQuote < 0) {
                    return "";
                }
                return headerValue.substring(valueStart + 1, endQuote);
            }
            int endIdx = headerValue.indexOf(',', valueStart);
            String val = (endIdx < 0) ? headerValue.substring(valueStart) : headerValue.substring(valueStart, endIdx);
            val.trim();
            return val;
        }
    }

    String extractSdpLine(const String& body, const char* prefix) {
        int prefixLen = strlen(prefix);
        int idx = 0;
        while (idx < static_cast<int>(body.length())) {
            int lineEnd = body.indexOf('\n', idx);
            if (lineEnd < 0) {
                lineEnd = body.length();
            }
            String line = body.substring(idx, lineEnd);
            line.trim();
            if (line.startsWith(prefix)) {
                return line.substring(prefixLen);
            }
            idx = lineEnd + 1;
        }
        return "";
    }

    void sendRaw(const String& msg) {
        udp.beginPacket(server.c_str(), port);
        udp.write(reinterpret_cast<const uint8_t*>(msg.c_str()), msg.length());
        udp.endPacket();
    }

    // --- Autenticazione digest (RFC 2617/3261), usata sia da REGISTER
    // sia dall'INVITE: ogni provider SIP reale la richiede, e' il modo
    // standard per cui funziona con "qualsiasi" provider. ---

    String lastRealm, lastNonce, lastQop, lastOpaque;

    // Il nostro indirizzo pubblico reale, imparato dal parametro
    // "received=" del Via di ogni risposta (vedi processIncoming()) -
    // usato nell'SDP delle INVITE al posto dell'IP privato
    // (WiFi.localIP()), altrimenti il server non sa dove mandarci
    // l'audio in arrivo. Vuoto finche' non arriva almeno una risposta.
    String lastKnownPublicIp;
    // Nome dell'header da usare nella richiesta autenticata - "Authorization"
    // per una sfida 401 (WWW-Authenticate, livello registrar/UAS), ma
    // "Proxy-Authorization" per una sfida 407 (Proxy-Authenticate, livello
    // proxy) - RFC3261 22.3/RFC2617: i due NON sono intercambiabili. Bug
    // reale trovato dal vivo il 10/10: l'INVITE verso sip2sip.info viene
    // sfidato con 407 (confermato: "Server: SIP Thor on OpenSIPS XS 3.2"),
    // ma il codice rimandava sempre "Authorization" anche in quel caso -
    // il proxy continuava a rifiutare (nuova sfida 407) perche' non vedeva
    // mai l'header che si aspettava davvero. REGISTER funzionava perche'
    // la sua sfida e' un 401 (WWW-Authenticate), dove "Authorization" e'
    // effettivamente quello giusto.
    String lastAuthHeaderName = "Authorization";

    void parseAuthChallenge(const String& response) {
        String header = getHeaderValue(response, "WWW-Authenticate");
        if (header.length() > 0) {
            lastAuthHeaderName = "Authorization";
        } else {
            header = getHeaderValue(response, "Proxy-Authenticate");
            lastAuthHeaderName = "Proxy-Authorization";
        }
        lastRealm = getHeaderParam(header, "realm");
        lastNonce = getHeaderParam(header, "nonce");
        lastOpaque = getHeaderParam(header, "opaque");

        // Il campo "qop" della sfida puo' offrire PIU' opzioni separate
        // da virgola dentro le stesse virgolette (es. qop="auth,auth-int"
        // - visto dal vivo il 09/10 con sip2sip.info/OpenSIPS) - vanno
        // SCELTE, non echeggiate intere: mandare "qop=auth,auth-int" come
        // valore singolo nella risposta e' sintatticamente invalido e il
        // server lo rifiuta con 400 Bad Request (causa esatta di questo
        // bug). Supportiamo solo "auth" (non "auth-int", che richiede
        // hashare anche il corpo del messaggio): se offerto, selezioniamo
        // sempre quello.
        String rawQop = getHeaderParam(header, "qop");
        lastQop = (rawQop.indexOf("auth") >= 0) ? "auth" : rawQop;
    }

    String buildAuthHeader(const String& method, const String& uri) {
        String ha1 = md5Hex(user + ":" + lastRealm + ":" + password);
        String ha2 = md5Hex(method + ":" + uri);
        String nc = "00000001";
        String cnonce = randomHexString(8);
        String response;
        if (lastQop.length() > 0) {
            response = md5Hex(ha1 + ":" + lastNonce + ":" + nc + ":" + cnonce + ":" + lastQop + ":" + ha2);
        } else {
            response = md5Hex(ha1 + ":" + lastNonce + ":" + ha2);
        }

        String header = "Digest username=\"" + user + "\", realm=\"" + lastRealm
            + "\", nonce=\"" + lastNonce + "\", uri=\"" + uri
            + "\", response=\"" + response + "\", algorithm=MD5";
        if (lastQop.length() > 0) {
            header += ", qop=" + lastQop + ", nc=" + nc + ", cnonce=\"" + cnonce + "\"";
        }
        if (lastOpaque.length() > 0) {
            header += ", opaque=\"" + lastOpaque + "\"";
        }
        return header;
    }

    // --- REGISTER (+ re-registrazione periodica) ---

    String regCallId, regFromTag;
    int regCseq = 0;
    bool registered = false;
    bool regAuthRetried = false;
    uint32_t registerDeadlineMs = 0;
    uint32_t lastRegisterAttemptMs = 0;
    uint32_t lastKeepaliveMs = 0;

    bool regPending = false;
    String regRawMessage;
    uint32_t regSentAtMs = 0;
    int regAttempts = 0;

    String buildRegisterMessage(bool withAuth) {
        String branch = makeBranch();
        String localIp = WiFi.localIP().toString();
        String dom = effectiveDomain();
        String uri = "sip:" + dom;

        String msg;
        msg += "REGISTER sip:" + dom + " SIP/2.0\r\n";
        msg += "Via: SIP/2.0/UDP " + localIp + ":" + String(LOCAL_SIP_PORT) + ";branch=" + branch + ";rport\r\n";
        msg += "Max-Forwards: 70\r\n";
        msg += "From: \"" + displayName + "\" <sip:" + user + "@" + dom + ">;tag=" + regFromTag + "\r\n";
        msg += "To: \"" + displayName + "\" <sip:" + user + "@" + dom + ">\r\n";
        msg += "Call-ID: " + regCallId + "\r\n";
        msg += "CSeq: " + String(regCseq) + " REGISTER\r\n";
        msg += "Contact: <sip:" + user + "@" + localIp + ":" + String(LOCAL_SIP_PORT) + ">\r\n";
        msg += "Expires: " + String(REGISTER_EXPIRE_SEC) + "\r\n";
        msg += "User-Agent: TDeckFW/" APP_VERSION "\r\n";
        if (withAuth) {
            msg += lastAuthHeaderName + ": " + buildAuthHeader("REGISTER", uri) + "\r\n";
        }
        msg += "Content-Length: 0\r\n\r\n";
        return msg;
    }

    void startRegister(bool withAuth) {
        regCseq++;
        String req = buildRegisterMessage(withAuth);
        sendRaw(req);
        regRawMessage = req;
        regPending = true;
        regSentAtMs = millis();
        regAttempts = 0;
    }

    void handleRegisterResponse(int code, const String& msg) {
        regPending = false;

        if (code == 401 || code == 407) {
            parseAuthChallenge(msg);
            if (regAuthRetried) {
                regAuthRetried = false;
                registered = false;
                setStatus("Registrazione SIP fallita: credenziali errate");
                return;
            }
            regAuthRetried = true;
            startRegister(true);
            return;
        }

        regAuthRetried = false;
        if (code == 200) {
            registered = true;
            String expiresHeader = getHeaderValue(msg, "Expires");
            int expireSec = (expiresHeader.length() > 0) ? expiresHeader.toInt() : REGISTER_EXPIRE_SEC;
            if (expireSec <= 0) {
                expireSec = REGISTER_EXPIRE_SEC;
            }
            registerDeadlineMs = millis() + static_cast<uint32_t>(expireSec) * 1000UL;
            setStatus("Registrato su " + effectiveDomain());
            return;
        }

        registered = false;
        setStatus("Registrazione SIP rifiutata (codice " + String(code) + ")");
    }

    void updateRegistration() {
        uint32_t now = millis();

        if (regPending) {
            if (now - regSentAtMs > RETRANSMIT_MS) {
                if (regAttempts >= MAX_ATTEMPTS) {
                    regPending = false;
                    registered = false;
                    setStatus("Registrazione SIP: nessuna risposta dal server");
                } else {
                    sendRaw(regRawMessage);
                    regSentAtMs = now;
                    regAttempts++;
                }
            }
            return;
        }

        if (!registered && now - lastRegisterAttemptMs > 5000) {
            lastRegisterAttemptMs = now;
            startRegister(false);
        } else if (registered && static_cast<int32_t>(registerDeadlineMs - now) < 5000) {
            startRegister(false);
        }
    }

    void updateNatKeepalive() {
        uint32_t now = millis();
        if (now - lastKeepaliveMs < KEEPALIVE_INTERVAL_MS) {
            return;
        }
        lastKeepaliveMs = now;
        if (server.length() == 0) {
            return;
        }
        // Datagramma vuoto (solo CRLF, convenzione comune del "SIP OPTIONS
        // ping" semplificato): serve solo a generare traffico sul socket
        // per rinnovare la mappatura NAT, la risposta (se arriva) viene
        // ignorata da processIncoming() come qualunque altro pacchetto non
        // riconosciuto.
        udp.beginPacket(server.c_str(), port);
        udp.write(reinterpret_cast<const uint8_t*>("\r\n"), 2);
        udp.endPacket();
    }

    // --- Chiamata in uscita (INVITE/ACK/BYE/CANCEL) ---

    SipClient::CallState callState = SipClient::CallState::Idle;
    String callCallId, callFromTag, callToTag, callRequestUri;
    int callCseq = 0;
    bool callAuthRetried = false;
    uint32_t callSdpId = 0;
    uint16_t callLocalRtpPort = 0;
    String callLastInviteBranch;
    int callLastInviteCseq = 0;
    String remoteRtpHost;
    int remoteRtpPort = 0;

    bool callPending = false;
    String callRawMessage;
    uint32_t callSentAtMs = 0;
    int callAttempts = 0;

    // Timeout di sicurezza per CallState::Ending (09/10, bug reale
    // trovato dal vivo): se la risposta finale del server non arriva
    // mai (es. un CANCEL/BYE che il server ignora), la chiamata restava
    // bloccata in "Ending" per sempre - nessun ramo gestiva un secondo
    // tocco su CHIUDI CHIAMATA in quello stato, e non c'era nessun
    // timeout a liberarla da sola. Vedi hangup()/updateCallTransaction().
    uint32_t callEndingSinceMs = 0;
    constexpr uint32_t CALL_ENDING_TIMEOUT_MS = 5000;

    String buildInviteMessage(bool withAuth, const String& branch) {
        String localIp = WiFi.localIP().toString();
        // SDP: usa l'IP PUBBLICO imparato dal Via (vedi
        // lastKnownPublicIp sopra) quando disponibile - mai l'IP
        // privato, altrimenti il server non puo' mandarci l'audio in
        // arrivo (bug reale dell'11/10). Il Via/Contact SIP qui sotto
        // restano invece sull'IP privato: la segnalazione gestisce il
        // NAT da sola via rport (RFC3581), e cambiarli non e' mai stato
        // il problema.
        String sdpAddr = (lastKnownPublicIp.length() > 0) ? lastKnownPublicIp : localIp;

        String sdp;
        sdp += "v=0\r\n";
        sdp += "o=tdeckfw " + String(callSdpId) + " " + String(callSdpId) + " IN IP4 " + sdpAddr + "\r\n";
        sdp += "s=TDeckFW VoIP\r\n";
        sdp += "c=IN IP4 " + sdpAddr + "\r\n";
        sdp += "t=0 0\r\n";
        sdp += "m=audio " + String(callLocalRtpPort) + " RTP/AVP 0 8\r\n";
        sdp += "a=rtpmap:0 PCMU/8000\r\n";
        sdp += "a=rtpmap:8 PCMA/8000\r\n";
        sdp += "a=sendrecv\r\n";

        String msg;
        msg += "INVITE " + callRequestUri + " SIP/2.0\r\n";
        msg += "Via: SIP/2.0/UDP " + localIp + ":" + String(LOCAL_SIP_PORT) + ";branch=" + branch + ";rport\r\n";
        msg += "Max-Forwards: 70\r\n";
        msg += "From: \"" + displayName + "\" <sip:" + user + "@" + effectiveDomain() + ">;tag=" + callFromTag + "\r\n";
        msg += "To: <" + callRequestUri + ">\r\n";
        msg += "Call-ID: " + callCallId + "\r\n";
        msg += "CSeq: " + String(callCseq) + " INVITE\r\n";
        msg += "Contact: <sip:" + user + "@" + localIp + ":" + String(LOCAL_SIP_PORT) + ">\r\n";
        msg += "User-Agent: TDeckFW/" APP_VERSION "\r\n";
        if (withAuth) {
            msg += lastAuthHeaderName + ": " + buildAuthHeader("INVITE", callRequestUri) + "\r\n";
        }
        msg += "Content-Type: application/sdp\r\n";
        msg += "Content-Length: " + String(sdp.length()) + "\r\n";
        msg += "\r\n";
        msg += sdp;
        return msg;
    }

    String buildAckMessage() {
        String branch = makeBranch();
        String localIp = WiFi.localIP().toString();

        String msg;
        msg += "ACK " + callRequestUri + " SIP/2.0\r\n";
        msg += "Via: SIP/2.0/UDP " + localIp + ":" + String(LOCAL_SIP_PORT) + ";branch=" + branch + ";rport\r\n";
        msg += "Max-Forwards: 70\r\n";
        msg += "From: \"" + displayName + "\" <sip:" + user + "@" + effectiveDomain() + ">;tag=" + callFromTag + "\r\n";
        msg += "To: <" + callRequestUri + ">;tag=" + callToTag + "\r\n";
        msg += "Call-ID: " + callCallId + "\r\n";
        msg += "CSeq: " + String(callCseq) + " ACK\r\n";
        msg += "Content-Length: 0\r\n\r\n";
        return msg;
    }

    String buildByeMessage() {
        callCseq++;
        String branch = makeBranch();
        String localIp = WiFi.localIP().toString();

        String msg;
        msg += "BYE " + callRequestUri + " SIP/2.0\r\n";
        msg += "Via: SIP/2.0/UDP " + localIp + ":" + String(LOCAL_SIP_PORT) + ";branch=" + branch + ";rport\r\n";
        msg += "Max-Forwards: 70\r\n";
        msg += "From: \"" + displayName + "\" <sip:" + user + "@" + effectiveDomain() + ">;tag=" + callFromTag + "\r\n";
        msg += "To: <" + callRequestUri + ">;tag=" + callToTag + "\r\n";
        msg += "Call-ID: " + callCallId + "\r\n";
        msg += "CSeq: " + String(callCseq) + " BYE\r\n";
        msg += "Content-Length: 0\r\n\r\n";
        return msg;
    }

    String buildCancelMessage() {
        String localIp = WiFi.localIP().toString();

        String msg;
        msg += "CANCEL " + callRequestUri + " SIP/2.0\r\n";
        // Stesso branch e stesso CSeq dell'INVITE che si sta annullando -
        // obbligatorio per RFC3261 (16.10), non un nuovo numero.
        msg += "Via: SIP/2.0/UDP " + localIp + ":" + String(LOCAL_SIP_PORT) + ";branch=" + callLastInviteBranch + ";rport\r\n";
        msg += "Max-Forwards: 70\r\n";
        msg += "From: \"" + displayName + "\" <sip:" + user + "@" + effectiveDomain() + ">;tag=" + callFromTag + "\r\n";
        msg += "To: <" + callRequestUri + ">\r\n";
        msg += "Call-ID: " + callCallId + "\r\n";
        msg += "CSeq: " + String(callLastInviteCseq) + " CANCEL\r\n";
        msg += "Content-Length: 0\r\n\r\n";
        return msg;
    }

    String extractTagFromTo(const String& msg) {
        String toHeader = getHeaderValue(msg, "To");
        return getHeaderParam(toHeader, "tag");
    }

    void parseRemoteSdp(const String& msg) {
        int bodyStart = msg.indexOf("\r\n\r\n");
        if (bodyStart < 0) {
            return;
        }
        String body = msg.substring(bodyStart + 4);

        String cValue = extractSdpLine(body, "c=IN IP4 ");
        if (cValue.length() > 0) {
            cValue.trim();
            remoteRtpHost = cValue;
        }

        String mValue = extractSdpLine(body, "m=audio ");
        if (mValue.length() > 0) {
            int sp = mValue.indexOf(' ');
            String portStr = (sp > 0) ? mValue.substring(0, sp) : mValue;
            remoteRtpPort = portStr.toInt();
        }
    }

    void handleCallResponse(int code, const String& msg, const String& respMethod) {
        if (respMethod == "INVITE") {
            callPending = false;

            if (code == 100) {
                return;
            }
            if (code == 180 || code == 183) {
                setStatus("Squilla...");
                return;
            }
            if (code == 401 || code == 407) {
                if (callAuthRetried) {
                    callAuthRetried = false;
                    callState = SipClient::CallState::Idle;
                    setStatus("Chiamata fallita: credenziali errate");
                    return;
                }
                callAuthRetried = true;
                parseAuthChallenge(msg);
                callCseq++;
                String branch = makeBranch();
                callLastInviteBranch = branch;
                callLastInviteCseq = callCseq;
                String req = buildInviteMessage(true, branch);
                sendRaw(req);
                callRawMessage = req;
                callPending = true;
                callSentAtMs = millis();
                callAttempts = 0;
                return;
            }
            if (code == 200) {
                callAuthRetried = false;
                callToTag = extractTagFromTo(msg);
                parseRemoteSdp(msg);
                sendRaw(buildAckMessage());
                callState = SipClient::CallState::InCall;
                setStatus("Chiamata attiva");
                return;
            }
            if (code >= 300) {
                callState = SipClient::CallState::Idle;
                setStatus("Chiamata non riuscita (codice " + String(code) + ")");
                return;
            }
        } else if (respMethod == "BYE") {
            callPending = false;
            callState = SipClient::CallState::Idle;
            setStatus("Chiamata terminata");
        }
        // respMethod == "CANCEL": solo il 200 OK alla CANCEL stessa - la
        // chiamata si considera chiusa quando arriva la risposta finale
        // (tipicamente 487) per l'INVITE originale, gestita sopra.
    }

    void updateCallTransaction() {
        if (!callPending) {
            return;
        }
        uint32_t now = millis();
        if (now - callSentAtMs <= RETRANSMIT_MS) {
            return;
        }
        if (callAttempts >= MAX_ATTEMPTS) {
            callPending = false;
            if (callState != SipClient::CallState::Idle) {
                callState = SipClient::CallState::Idle;
                setStatus("Chiamata: nessuna risposta dal server");
            }
            return;
        }
        sendRaw(callRawMessage);
        callSentAtMs = now;
        callAttempts++;
    }

    // Rete di sicurezza (09/10, vedi il commento su CALL_ENDING_TIMEOUT_MS):
    // se la risposta finale non arriva mai (es. un provider che ignora
    // la CANCEL), la chiamata si libera da sola dopo qualche secondo
    // invece di restare bloccata su "Ending" per sempre, anche SENZA un
    // secondo tocco su CHIUDI CHIAMATA.
    void updateCallEndingTimeout() {
        if (callState != SipClient::CallState::Ending) {
            return;
        }
        if (millis() - callEndingSinceMs > CALL_ENDING_TIMEOUT_MS) {
            callPending = false;
            callState = SipClient::CallState::Idle;
            setStatus("Chiamata chiusa (timeout)");
        }
    }

    // --- Smistamento pacchetti in arrivo ---

    void processIncoming() {
        int packetSize = udp.parsePacket();
        if (packetSize <= 0) {
            return;
        }

        static char buf[1536];
        int len = udp.read(reinterpret_cast<uint8_t*>(buf), min(packetSize, static_cast<int>(sizeof(buf) - 1)));
        if (len <= 0) {
            return;
        }
        buf[len] = 0;
        String msg(buf);

        // Risposte attese iniziano con la status-line "SIP/2.0 <codice> ...".
        // Richieste in arrivo (es. un futuro INVITE per chiamate ricevute,
        // v3 del piano) non sono ancora gestite in questa fetta v1 - un
        // keepalive di risposta o rumore di rete qualunque viene scartato
        // allo stesso modo.
        if (!msg.startsWith("SIP/2.0")) {
            return;
        }

        // Impara il nostro indirizzo pubblico REALE dal parametro
        // "received=" che il server aggiunge all'header Via di OGNI
        // risposta (RFC 3581) - bug reale trovato dal vivo l'11/10:
        // l'SDP delle nostre INVITE dichiarava l'IP PRIVATO
        // (WiFi.localIP(), es. 172.20.10.2 su un hotspot) come indirizzo
        // dove mandarci l'audio - un indirizzo non raggiungibile da
        // internet, quindi il server non riusciva MAI a farci arrivare
        // un pacchetto RTP (ring RX sempre vuoto, confermato in
        // diagnostica). Vedi buildInviteMessage() sotto, dove questo
        // indirizzo sostituisce WiFi.localIP() nell'SDP.
        String viaHeader = getHeaderValue(msg, "Via");
        String received = getHeaderParam(viaHeader, "received");
        if (received.length() > 0) {
            lastKnownPublicIp = received;
        }

        int code = getStatusCode(msg);
        String cseqHeader = getHeaderValue(msg, "CSeq");
        int sp = cseqHeader.indexOf(' ');
        int respCseq = (sp > 0) ? cseqHeader.substring(0, sp).toInt() : -1;
        String respMethod = (sp > 0) ? cseqHeader.substring(sp + 1) : "";
        respMethod.trim();

        String respCallId = getHeaderValue(msg, "Call-ID");
        respCallId.trim();

        if (respCallId.length() > 0 && respCallId == regCallId && respMethod == "REGISTER") {
            handleRegisterResponse(code, msg);
            return;
        }
        if (respCallId.length() > 0 && respCallId == callCallId) {
            handleCallResponse(code, msg, respMethod);
            return;
        }
        (void)respCseq; // usato solo per isolare il valore dal metodo, non confrontato oltre (un solo account/una sola chiamata alla volta)
    }
}

namespace SipClient {

    void setConfig(
        const String& newServer, int newPort, const String& newDomain,
        const String& newUser, const String& newPassword,
        const String& newDisplayName
    ) {
        ensureNvsOpen();
        server = newServer;
        port = (newPort > 0) ? newPort : 5060;
        domain = newDomain;
        user = newUser;
        password = newPassword;
        displayName = (newDisplayName.length() > 0) ? newDisplayName : newUser;

        prefs.putString(KEY_SERVER, server);
        prefs.putInt(KEY_PORT, port);
        prefs.putString(KEY_DOMAIN, domain);
        prefs.putString(KEY_USER, user);
        prefs.putString(KEY_PASSWORD, password);
        prefs.putString(KEY_DISPNAME, displayName);

        // Account cambiato (es. nuovo provider): via le credenziali
        // digest residue del vecchio server e forza una registrazione
        // da zero con un Call-ID/tag nuovi - requisito esplicito
        // dell'utente, "cambiare provider senza ricompilare".
        registered = false;
        regPending = false;
        regAuthRetried = false;
        lastRealm = ""; lastNonce = ""; lastQop = ""; lastOpaque = "";
        if (socketOpen) {
            regCallId = makeCallId();
            regFromTag = makeTag();
            regCseq = 0;
            lastRegisterAttemptMs = 0;
        }
    }

    String getServer() { ensureNvsOpen(); return server; }
    int getPort() { ensureNvsOpen(); return port; }
    String getDomain() { ensureNvsOpen(); return domain; }
    String getEffectiveDomain() { ensureNvsOpen(); return effectiveDomain(); }
    String getUser() { ensureNvsOpen(); return user; }
    String getPassword() { ensureNvsOpen(); return password; }
    String getDisplayName() { ensureNvsOpen(); return displayName; }

    bool isConfigured() {
        ensureNvsOpen();
        return server.length() > 0 && port > 0 && user.length() > 0;
    }

    void begin() {
        ensureNvsOpen();
        if (socketOpen) {
            return;
        }
        if (!isConfigured()) {
            setStatus("VoIP non configurato - vai su Impostazioni");
            return;
        }
        if (WiFi.status() != WL_CONNECTED) {
            setStatus("Serve una rete WiFi connessa");
            return;
        }

        udp.begin(LOCAL_SIP_PORT);
        socketOpen = true;

        regCallId = makeCallId();
        regFromTag = makeTag();
        regCseq = 0;
        registered = false;
        lastRegisterAttemptMs = 0; // forza il primo tentativo al prossimo update()
        lastKeepaliveMs = millis();
        setStatus("Connessione al server SIP...");
    }

    bool isStarted() {
        return socketOpen;
    }

    void update() {
        if (!socketOpen) {
            return;
        }
        processIncoming();
        updateRegistration();
        updateCallTransaction();
        updateCallEndingTimeout();
        updateNatKeepalive();
    }

    bool isRegistered() {
        return registered;
    }

    String getStatusText() {
        if (!socketOpen) {
            return isConfigured() ? "Non connesso" : "Non configurato";
        }
        return registered ? ("Registrato su " + effectiveDomain()) : "Registrazione in corso...";
    }

    bool call(const String& number) {
        if (!registered) {
            setStatus("Non registrato: impossibile chiamare");
            return false;
        }
        if (callState != CallState::Idle) {
            setStatus("Una chiamata e' gia' in corso");
            return false;
        }

        String trimmedNum = number;
        trimmedNum.trim();
        if (trimmedNum.length() == 0) {
            setStatus("Numero vuoto");
            return false;
        }

        callCallId = makeCallId();
        callFromTag = makeTag();
        callToTag = "";
        callCseq = 1;
        callAuthRetried = false;
        callSdpId = millis();
        callLocalRtpPort = RTP_LOCAL_PORT;
        remoteRtpHost = "";
        remoteRtpPort = 0;
        callRequestUri = trimmedNum.startsWith("sip:") ? trimmedNum : ("sip:" + trimmedNum + "@" + effectiveDomain());

        String branch = makeBranch();
        callLastInviteBranch = branch;
        callLastInviteCseq = callCseq;

        String req = buildInviteMessage(false, branch);
        sendRaw(req);
        callRawMessage = req;
        callPending = true;
        callSentAtMs = millis();
        callAttempts = 0;
        callState = CallState::Calling;
        setStatus("Chiamata in corso verso " + trimmedNum);
        return true;
    }

    void hangup() {
        if (callState == CallState::Calling) {
            sendRaw(buildCancelMessage());
            callPending = false; // CANCEL non ritrasmessa in v1 (semplificazione dichiarata nel piano)
            callState = CallState::Ending;
            callEndingSinceMs = millis();
            setStatus("Annullamento chiamata...");
        } else if (callState == CallState::InCall) {
            String req = buildByeMessage();
            sendRaw(req);
            callRawMessage = req;
            callPending = true;
            callSentAtMs = millis();
            callAttempts = 0;
            callState = CallState::Ending;
            callEndingSinceMs = millis();
            setStatus("Chiusura chiamata...");
        } else if (callState == CallState::Ending) {
            // Secondo tocco su CHIUDI CHIAMATA mentre si aspetta ancora
            // la risposta finale del server: rinuncia subito invece di
            // restare bloccati (vedi il timeout gemello in
            // updateCallTransaction() per il caso senza un secondo tocco).
            callPending = false;
            callState = CallState::Idle;
            setStatus("Chiamata chiusa (forzata)");
        }
    }

    CallState getCallState() {
        return callState;
    }

    String getRemoteRtpHost() {
        return remoteRtpHost;
    }

    int getRemoteRtpPort() {
        return remoteRtpPort;
    }

    int getLocalRtpPort() {
        return callLocalRtpPort;
    }

    bool hasStatusMessage() {
        return pendingStatus;
    }

    String consumeStatusMessage() {
        pendingStatus = false;
        return pendingStatusText;
    }

    String getLastStatusText() {
        return pendingStatusText;
    }
}
