#pragma once

#include <Arduino.h>

// "Guardiano" della rete locale: quando attivo, ripete periodicamente in
// background una scansione della rete (riusando lo STESSO motore
// condiviso di NetScan, vedi NetScan.h - mai un motore proprio, mai due
// scansioni attive in parallelo, l'unico approccio dimostratosi stabile
// su questo hardware dopo alcuni crash in prova) e confronta OGNI giro
// con la whitelist congelata al momento dell'attivazione (vedi
// setEnabled()) - MAI con l'ultimo giro, per non "spostare" pian piano
// cosa si considera fidato (bug reale corretto il 26/09, la whitelist
// restava agganciata al giro precedente invece che a quella iniziale).
// Un dispositivo nuovo (mai visto prima in questa sessione di
// sorveglianza) fa scattare un allarme a schermo + un suono; l'elenco
// completo dei dispositivi attualmente fuori whitelist (vedi
// getTrackedCount()/getTracked()) resta visibile nella UI: rosso se
// rilevato nell'ultimo giro, giallo per un giro se era rosso e non si
// e' piu' visto (utile per capire chi ha provato ad entrare ed e' poi
// scomparso).
//
// Se l'utente avvia una scansione manuale da NetScan mentre NetGuard e'
// attivo, NetGuard si disattiva da solo (vedi NetScanUI.cpp): due
// richieste di scansione sullo stesso motore condiviso nello stesso
// momento non sono mai state provate ed e' meglio non rischiare.
//
// Lo stato acceso/spento NON sopravvive al riavvio (parte sempre
// spento) e si spegne da solo se cade la connessione WiFi: e' sempre
// l'utente a doverlo riaccendere esplicitamente.
namespace NetGuard {

    void begin();

    // Da chiamare ad ogni ciclo, SEMPRE (come GPS::update()/LoRaRadio::
    // update()): guida il ciclo di scansione periodico e si spegne da
    // solo se cade il WiFi.
    void update();

    bool isEnabled();

    // Accende/spegne il monitoraggio. Ritorna false (e NON attiva nulla)
    // se si chiede di accendere senza che esista ancora una whitelist
    // (NetScan::getDeviceCount() == 0): il chiamante (NetGuardUI) lo usa
    // per spiegare all'utente che serve prima un NetScan.
    bool setEnabled(bool enabled);

    // true se il monitoraggio e' acceso ma non esiste ancora nessuna
    // whitelist da controllare: utile per la UI, per mostrare "serve un
    // NetScan" invece di far credere che il monitoraggio sia gia' attivo.
    bool isWaitingForWhitelist();

    // Da chiamare quando parte un NUOVO NetScan manuale (vedi
    // NetScanUI.cpp): la whitelist congelata (se esiste) non e' piu'
    // valida, il prossimo setEnabled(true) ne cattura una fresca invece
    // di riusare quella vecchia. Spegnere/riaccendere la sorveglianza
    // SENZA un nuovo NetScan di mezzo NON deve invalidarla - vedi il
    // commento su whitelistMacs in NetGuard.cpp.
    void invalidateWhitelist();

    // true mentre e' in corso il giro di scansione periodico (riusa
    // NetScan::isScanning()/getProgressPercent() per il dettaglio):
    // utile per la UI, per mostrare l'avanzamento invece di "attivo" e
    // basta.
    bool isScanning();

    // Secondi mancanti al prossimo giro di scansione periodico (0 se
    // gia' in corso o se il monitoraggio e' spento) - usato dalla UI per
    // mostrare un conto alla rovescia invece di un generico "ogni
    // minuto", cosi' si vede che il ciclo procede davvero.
    int getSecondsUntilNextScan();

    // Numero di dispositivi nella whitelist CONGELATA all'accensione
    // (NON l'ultimo giro di NetScan, che dopo il primo giro periodico
    // riflette gia' un'altra scansione - vedi il commento in testa).
    int getWhitelistCount();

    // Dettagli dell'ultimo allarme generato (vuoto se nessuno finora in
    // questa sessione).
    String getLastAlertSummary();

    // true dal momento in cui scatta un allarme fino a quando lo si
    // chiude (bottone Indietro/tap sulla schermata dell'allarme) - un
    // "flash" per l'evento appena rilevato, NON lo stato corrente della
    // rete (vedi hasActiveIntruder() per quello).
    bool hasPendingAlert();

    // true se ALMENO UN dispositivo fuori whitelist e' visto nell'ultimo
    // giro (stato Active, rosso in UI - non un JustLeft/giallo). A
    // differenza di hasPendingAlert() (che torna false appena si chiude
    // il popup, anche se l'intruso e' ancora collegato) resta vero finche'
    // resta collegato - usato per l'icona NetGuard in alto, che altrimenti
    // tornava verde ad ogni giro anche con un intruso ancora attivo
    // (segnalato dall'utente il 26/09).
    bool hasActiveIntruder();

    // Dispositivi attualmente fuori whitelist, per la UI (elenco live).
    struct TrackedDevice {
        String mac;
        String ip;
        bool justLeft; // false = visto nell'ultimo giro (rosso); true = visto nel giro precedente ma non in questo (giallo, un solo giro prima di essere tolto dall'elenco)
    };
    int getTrackedCount();
    TrackedDevice getTracked(int index);

    // Aggiunge manualmente un MAC alla whitelist congelata (richiesto
    // 26/09: capita che il primo NetScan non trovi subito tutti i
    // dispositivi di casa, quindi compaiono come "intrusi" anche se
    // legittimi - l'utente li marca lui invece di aspettare un nuovo
    // NetScan completo) e lo toglie subito dall'elenco tracciato (non
    // e' piu' "fuori whitelist"). Resta cosi' SOLO per questa sessione di
    // sorveglianza (whitelistMacs non e' persistita) - un nuovo NetScan
    // manuale o uno spegni/riaccendi dopo invalidateWhitelist() la
    // perderebbe, come per il resto della whitelist congelata. Ritorna
    // false se la whitelist e' piena (MAX_SNAPSHOT, molto improbabile).
    bool addToWhitelist(const String& mac);
}
