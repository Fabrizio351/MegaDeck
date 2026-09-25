#pragma once

// Stato di rete condiviso, mostrato come icone nella barra in alto
// (vedi Display.cpp). Il WiFi legge lo stato reale della libreria WiFi
// dell'ESP32; per la VPN nel progetto non esiste ancora un client VPN
// vero e proprio (la voce di menu "VPN" e' un placeholder), quindi qui
// c'e' solo uno stato che il futuro codice VPN dovra' aggiornare con
// ConnStatus::setVpnConnected() quando la funzione verra' implementata.
//
// NB: questo file NON si chiama "Network.h" apposta. Il core ESP32
// include una propria libreria chiamata "Network" (da cui dipende
// WiFi.h): un nostro Network.h nella cartella dello sketch avrebbe
// priorita' su quello vero nella risoluzione degli include di Arduino,
// rompendo la compilazione di WiFi.h in tutto il progetto.
namespace ConnStatus {
    bool isWifiConnected();

    bool isVpnConnected();
    void setVpnConnected(bool connected);

    // Bluetooth/LoRa non hanno un "connesso a" persistente come WiFi/VPN
    // (sono broadcast, non collegamenti punto a punto): qui "attivo"
    // significa radio pronta e non disattivata dalle Impostazioni, non
    // "in comunicazione con qualcuno in questo momento".
    bool isBluetoothActive();
    bool isLoraActive();

    // true dal momento in cui parte una trasmissione LoRa fino al suo
    // completamento: usato per colorare diversamente l'icona LoRa in
    // alto mentre e' effettivamente in corso un invio, cosi' si vede a
    // colpo d'occhio da qualunque schermata (non solo dalla chat).
    bool isLoraTransmitting();

    // NetGuard (vedi NetGuard.h) attivo/disattivo, per l'icona in alto.
    bool isNetGuardActive();

    // true se NetGuard e' acceso ma non esiste ancora una whitelist da
    // controllare (serve prima un NetScan): l'icona in alto la colora
    // diversamente, cosi' non sembra gia' "in guardia" quando in realta'
    // non ha nulla da confrontare.
    bool isNetGuardWaitingForWhitelist();

    // true mentre NetGuard sta facendo un giro di scansione periodico:
    // stessa colorazione "occupato" di isNetGuardWaitingForWhitelist().
    bool isNetGuardScanning();

    // Eventi in sospeso non ancora "acknowledged" dall'utente: l'icona
    // in alto passa di colore (giallo per LoRa, rosso per NetGuard)
    // finche' non si apre la chat / si chiude l'allarme.
    bool hasUnreadLoraMessages();
    bool hasNetGuardPendingAlert();
}
