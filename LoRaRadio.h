#pragma once

#include <Arduino.h>
#include <RadioLib.h>

// Proprietario della radio SX1262 condivisa (02/10: ridotto a questo -
// prima conteneva anche il protocollo della chat LoRa custom, rimosso
// in favore di Meshtastic, vedi project_lora_meshtastic_lorawan_plan
// in memoria). Oggi questo modulo fa SOLO da gestore del ciclo di vita
// dell'hardware condiviso: nessuno "ascolta di base" per conto proprio -
// ogni feature (MeshtasticUI, LoRaWanUI, LoRaScanUI) prende la radio con
// pauseForExternalUse()/getRadioForExternalUse(), la riconfigura per i
// propri scopi, e la rilascia con resumeAfterExternalUse() quando ha
// finito (che oggi la lascia semplicemente in standby - vedi il
// commento su resumeAfterExternalUse() piu' sotto per il limite noto
// accettato).
namespace LoRaRadio {
    // Inizializza l'hardware (bus SPI + radio.begin()) con parametri di
    // partenza qualsiasi solo per accendere il chip - chi prende in
    // mano la radio dopo (vedi getRadioForExternalUse()) la riconfigura
    // sempre per conto proprio. Non avvia nessun ascolto: la radio resta
    // in standby finche' una feature specifica non lo richiede.
    bool begin();

    // true se begin() e' andato a buon fine (radio inizializzata).
    bool isReady();

    // Sospende la radio (standby) per uso ESCLUSIVO da un altro modulo
    // che deve riconfigurarla per i propri scopi (frequenza/modulazione
    // diverse) - stessa radio fisica, un solo utilizzatore alla volta.
    // Richiamare SEMPRE resumeAfterExternalUse() al termine. Non fa
    // nulla se la radio non e' pronta.
    void pauseForExternalUse();

    // Rilascia la radio dopo un uso esclusivo (02/10: oggi la lascia
    // semplicemente in standby, NON la riconfigura piu' per nessuno in
    // particolare - prima del 02/10 la restituiva automaticamente alla
    // vecchia chat LoRa custom, che e' stata rimossa). Limite noto
    // accettato: se MeshtasticUI stava ascoltando in secondo piano,
    // usare LoRaWAN/LoRaScan nel frattempo interrompe quell'ascolto
    // finche' non si riapre la schermata Meshtastic (che riconfigura e
    // riavvia la ricezione da zero, come fa gia' per un avvio a freddo).
    void resumeAfterExternalUse();

    // Accesso diretto all'oggetto radio SX1262 condiviso - da usare SOLO
    // tra pauseForExternalUse() e resumeAfterExternalUse(), mai in altri
    // momenti.
    SX1262& getRadioForExternalUse();
}
