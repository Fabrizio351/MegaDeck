#pragma once

#include <Arduino.h>

// Legge la configurazione WireGuard da un file di testo su SD card
// (formato .conf standard di wg-quick, [Interface]/[Peer]) invece che
// dal firmware: le chiavi private non devono MAI finire nel codice
// sorgente (che e' su git) - vanno lette da un supporto esterno che
// resta sul dispositivo dell'utente. Copiare il file wg-quick esistente
// su una SD card, senza modificarlo, e' la via piu' semplice per non
// dover ridigitare chiavi lunghe sulla tastierina fisica.
namespace VpnConfig {

    struct Config {
        bool valid = false;

        String localAddress;  // solo l'IP, es. "192.168.27.67" (senza /32)
        String dns;
        String privateKey;

        String endpointHost;
        int endpointPort = 0;
        String peerPublicKey;
        String presharedKey;  // vuoto se il peer non ne usa una

        // AllowedIPs non e' usato per instradare selettivamente (questa
        // libreria WireGuard per ESP32 non lo supporta, vedi VpnManager):
        // tenuto solo per mostrarlo in UI a scopo informativo.
        String allowedIps;
    };

    // Monta la SD card (se non gia' montata) e legge/analizza
    // "/wireguard.conf" dalla radice. Config.valid resta false se la SD
    // non e' leggibile, il file non esiste, o mancano i campi minimi
    // indispensabili (PrivateKey, Endpoint, PublicKey del peer).
    // Usata SOLO da importFromSd() qui sotto - non richiamare per l'uso
    // normale, vedi loadSaved().
    Config load();

    // true se una configurazione e' gia' stata importata (vedi
    // importFromSd()) ed e' quindi disponibile senza toccare la SD.
    bool hasSavedConfig();

    // Copia UNA VOLTA la configurazione dalla SD alla memoria interna
    // (NVS) del T-Deck, poi la SD non serve piu' per l'uso normale -
    // richiesto 26/09: lasciare la chiave privata su una SD estraibile
    // e' un rischio reale (chiunque la trovi puo' leggerla su qualunque
    // lettore), oltre a evitare di dover rimontare la SD (con relativo
    // rischio di blocco, vedi project_wifi_connect_freeze_and_vpn_bug
    // in memoria) ad ogni singola connessione. La SD puo' essere estratta
    // e messa via non appena questa funzione ritorna true.
    // NOTA SICUREZZA: NVS su questo progetto non e' cifrata (servirebbe
    // abilitare la cifratura flash di ESP-IDF, un cambiamento piu' grande
    // dello schema partizioni) - la chiave e' quindi al sicuro da chi
    // trova solo la SD, non da chi ha accesso fisico diretto al T-Deck
    // stesso con gli strumenti giusti (esptool). Un miglioramento reale
    // ma non una cifratura vera e propria.
    bool importFromSd();

    // Legge la configurazione gia' importata da NVS (nessun accesso SD).
    // Config.valid resta false se non e' mai stata importata nulla.
    Config loadSaved();

    // Cancella la configurazione salvata da NVS (per re-importare da
    // zero, o se l'utente vuole rimuovere le credenziali dal T-Deck).
    void clearSaved();

    // Importa "wireguard.conf" ricevuto via USB seriale invece che dalla
    // SD (30/09) - stesso identico formato/parser di load(), il file non
    // va ridigitato: sul PC basta "cat wireguard.conf > /dev/ttyACM0" (o
    // incollarne il contenuto in un terminale seriale) subito dopo aver
    // avviato l'importazione dal T-Deck. Evita del tutto l'hang della SD
    // (vedi project_sd_spi_boot_hang_critical in memoria) - nessuna
    // operazione SPI/SD e' coinvolta, solo lettura da Serial. Bloccante
    // con un timeout duro (15s totali, chiusura anticipata dopo 1.2s di
    // silenzio una volta iniziato a ricevere dati) - il chiamante mostra
    // un messaggio "in attesa" prima di richiamarla.
    bool importFromSerial();
}
