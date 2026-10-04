#pragma once

// Primo mattone verso la compatibilita' Meshtastic (30/09 sera) - vedi
// project_lora_meshtastic_lorawan_plan in memoria per il resoconto
// completo della ricerca sul repo ufficiale. Questo screen NON decodifica
// ancora nulla del protocollo Meshtastic (niente protobuf/crypto): si
// limita a riconfigurare la radio SX1262 condivisa con gli ESATTI
// parametri fisici del preset di default "LongFast" per la regione
// EU_868 (letti dal sorgente reale di meshtastic/firmware,
// src/mesh/RadioInterface.cpp + MeshRadio.h):
//   - frequenza 869.525 MHz (banda G3 869.4-869.65, NON la stessa
//     868.0 MHz usata dalla chat LoRa custom di questo progetto)
//   - banda 250 kHz, SF11, coding rate 4/5, sync word 0x2B, preambolo 8
// Se la radio riesce a ricevere pacchetti validi con questi parametri
// da dispositivi Meshtastic reali nell'area, e' la prova che il livello
// fisico e' compatibile - passo necessario PRIMA di affrontare il
// livello mesh vero (protobuf + AES-CTR + routing), che resta un lavoro
// separato e piu' grosso.
//
// Passo 2 (stessa sera): i 16 byte iniziali di ogni pacchetto ("struct
// PacketHeader" nel sorgente reale) NON sono protobuf - sono un formato
// fisso a byte allineati (to/from/id a 4 byte, poi flags/channel/
// next_hop/relay_node a 1 byte ciascuno) e viaggiano IN CHIARO
// sull'aria. Si possono quindi leggere subito (mittente, destinatario,
// hash del canale, hop limit) senza ancora nessuna libreria protobuf.
// Tutto cio' che segue l'header nel pacchetto resta cifrato (AES-CTR
// con la chiave del canale) e per ora viene solo mostrato in hex, non
// decifrato - quello e' un passo successivo.
namespace MeshtasticUI {
    void open();
    void update(); // primo piano: touch/trackball, SOLO quando la schermata e' quella attiva
    bool isActive(); // true se la schermata e' in primo piano (per il dispatch di App.cpp)

    // Secondo piano (02/10, vedi project_lora_meshtastic_lorawan_plan in
    // memoria) - chiamata SEMPRE da App.cpp, indipendentemente da quale
    // schermata e' in primo piano, per drenare i pacchetti radio anche
    // quando Meshtastic non e' quella visibile.
    void backgroundUpdate();

    // true se la radio sta ascoltando per Meshtastic adesso (primo piano
    // O secondo piano) - per colorare l'icona LORA nella barra in alto.
    bool isListening();

    // true da quando e' arrivato un messaggio mentre non si guardava la
    // chat, finche' non la si riapre - per colorare di rosso l'icona
    // LORA invece che verde.
    bool hasUnread();

    // Preset modem (02/10, richiesto: sempre piu' radioamatori in Europa
    // stanno passando da LongFast a MediumFast per avere piu' nodi vicini
    // compatibili - vedi SettingsUI.cpp, riga "PRESET LORA"). Cambia solo
    // lo spreading factor (11 vs 9): banda 250kHz, coding rate 4/5, sync
    // word e preambolo restano identici in entrambi i preset (veri valori
    // Meshtastic, non una scelta nostra - vedi il commento su
    // activeSpreadingFactor() in MeshtasticUI.cpp). Persistito in NVS,
    // default false = LongFast (il preset storico, invariato per chi non
    // tocca questa impostazione).
    bool isMediumFastPreset();
    void setMediumFastPreset(bool mediumFast);
}
