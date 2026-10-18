#pragma once

#include <Arduino.h>

// User agent SIP minimo (08/10, ultimo capitolo del progetto: VoIP) - SIP
// su UDP puro (RFC 3261), autenticazione digest (MD5), pensato per
// funzionare con QUALSIASI provider SIP standard (requisito esplicito
// dell'utente: poter cambiare provider in futuro senza ricompilare).
// Scritto a mano come tutti gli altri protocolli di questo progetto
// (Meshtastic, WireGuard...) - non esiste una libreria Arduino completa
// per questo, vedi project VoIP in memoria per la ricerca fatta.
//
// v1 (fetta minima, vedi il piano approvato): REGISTER + re-registrazione
// periodica + keepalive NAT + UNA chiamata in uscita (INVITE/ACK/BYE/
// CANCEL) con offerta SDP G.711 (PCMU+PCMA). Niente INVITE in arrivo
// ancora (v3 del piano).
namespace SipClient {

    enum class CallState {
        Idle,       // nessuna chiamata in corso
        Calling,    // INVITE inviato, attesa risposta
        InCall,     // 200 OK + ACK scambiati, chiamata attiva
        Ending      // BYE/CANCEL inviato, attesa conferma
    };

    // Configurazione account - tutti i parametri richiesti dall'utente
    // per poter cambiare provider senza ricompilare, persistiti su NVS
    // come HermesClient/WebRadio.
    //
    // SERVER/PORTA = destinazione di rete reale dei pacchetti UDP (quasi
    // sempre il registrar, ma alcuni provider - es. sip2sip.info - usano
    // un "outbound proxy" separato, scoperto dal vivo il 09/10: i
    // pacchetti vanno mandati li', non al dominio). DOMINIO = usato
    // dentro gli URI SIP (Request-URI/From/To, autenticazione digest) -
    // se lasciato vuoto, si usa SERVER anche come dominio (caso comune,
    // provider senza proxy separato).
    void setConfig(
        const String& server, int port, const String& domain,
        const String& user, const String& password,
        const String& displayName
    );
    String getServer();
    int getPort();
    String getDomain();
    // Dominio effettivo usato dentro gli URI SIP (vedi setConfig()): se
    // getDomain() e' vuoto, ritorna getServer() - utile alla UI per
    // mostrare l'identita' account reale senza duplicare la logica.
    String getEffectiveDomain();
    String getUser();
    String getPassword();
    String getDisplayName();
    bool isConfigured();

    // Apre il socket UDP SIP e tenta la prima REGISTER. Sicuro da
    // richiamare piu' volte: se il socket e' gia' aperto non fa nulla
    // (vedi isStarted()); se la PRIMA chiamata fallisce perche' il WiFi
    // non e' ancora connesso, richiamarla di nuovo in seguito riprova
    // sul serio - bug reale trovato dal vivo l'11/10: il chiamante
    // (VoipUI) teneva un proprio flag "gia' tentato" SEPARATO che non
    // rifletteva se begin() fosse davvero riuscito, quindi se VOIP
    // veniva aperta prima che il WiFi finisse di connettersi, non si
    // riprovava mai piu' nemmeno dopo la connessione.
    void begin();

    // true se il socket UDP e' davvero aperto (begin() e' andata a buon
    // fine) - il chiamante lo usa per decidere se richiamare begin() di
    // nuovo, invece di tenere un proprio flag "ci ho gia' provato" che
    // non dice se e' andata bene.
    bool isStarted();

    // Da richiamare ad ogni ciclo (sia a schermo aperto che in
    // background): elabora pacchetti UDP in arrivo (risposte SIP),
    // ritrasmissioni, re-registrazione periodica, keepalive NAT.
    void update();

    bool isRegistered();
    // Testo breve per la UI ("registrato", "errore: ...", ecc.)
    String getStatusText();

    // Chiamata in uscita verso 'number' (user part del SIP URI, es.
    // "0612345678" o un sip:user@host completo se il provider lo richiede
    // - lasciato libero, non validato qui). Fallisce silenziosamente
    // (resta Idle) se non registrato o se una chiamata e' gia' in corso.
    // Ritorna false se la chiamata NON e' nemmeno partita (non
    // registrato, numero vuoto, chiamata gia' in corso) - il chiamante
    // (VoipUI) deve controllarlo prima di passare alla schermata
    // "CHIAMATA", altrimenti ci si ritrova bloccati su quella schermata
    // senza nessuna chiamata reale da chiudere (bug reale trovato dal
    // vivo il 10/10: CHIUDI CHIAMATA non faceva nulla perche' non
    // c'era alcuno stato da cui uscire).
    bool call(const String& number);
    void hangup(); // BYE se InCall, CANCEL se ancora Calling

    CallState getCallState();
    // Durante InCall: indirizzo/porta RTP del provider, presi dall'SDP
    // della risposta 200 OK all'INVITE - RtpAudio li usa per sapere dove
    // mandare i pacchetti (vedi RtpAudio::start()).
    String getRemoteRtpHost();
    int getRemoteRtpPort();
    // Porta RTP locale che ABBIAMO offerto nel nostro SDP (SipClient la
    // sceglie una sola volta per chiamata) - RtpAudio apre il suo socket
    // su questa stessa porta.
    int getLocalRtpPort();

    // Messaggio di stato/errore piu' recente, consumato una sola volta
    // (stesso pattern di BleGatt::consumeValuesChanged) - usato da VoipUI
    // per mostrare "occupato", "numero non valido", errori di rete, ecc.
    bool hasStatusMessage();
    String consumeStatusMessage();

    // Ultimo messaggio di stato/errore, SENZA consumarlo (a differenza di
    // consumeStatusMessage()) - per mostrarlo sempre a schermo (es. nella
    // schermata di composizione) senza dover intercettare esattamente il
    // momento in cui cambia. Copre sia gli stati normali ("Registrato su
    // ...") sia gli errori reali (WiFi assente, credenziali errate,
    // nessuna risposta dal server) - tutti passano da setStatus().
    String getLastStatusText();
}
