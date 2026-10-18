#pragma once

// Interfaccia VoIP (08/10, ultimo capitolo del progetto) - schermata
// inattiva/comporre numero, chiamata in corso, e la CONFIG a 5 campi
// (SERVER/PORTA/UTENTE/PASSWORD/NOME) per poter cambiare provider SIP
// senza mai ricompilare (richiesta esplicita dell'utente). Vedi
// SipClient.h per il protocollo e RtpAudio.h per l'audio.
//
// v1+v2 (vedi il piano approvato): REGISTER + UNA chiamata in uscita o
// in arrivo, audio full-duplex (microfono->rete E rete->altoparlante,
// vedi RtpAudio.h). Manca ancora v3 (squillo per le chiamate in arrivo).
namespace VoipUI {
    void open();

    // Apre direttamente la CONFIG, saltando la schermata inattiva - usata
    // da SettingsUI.cpp. Indietro/FATTO da qui tornano a Impostazioni.
    void openConfig();

    void update();
    bool isActive();

    // Per l'icona VOIP nella barra in alto (10/10) - vero se la
    // schermata VOIP e' aperta IN PRIMO PIANO oppure se il secondo
    // piano e' acceso (vedi backgroundEnabled) - stesso significato di
    // "attivo" usato da MeshtasticUI::isListening() per la sua icona
    // LORA, non "schermata effettivamente aperta in questo istante"
    // come isActive() (quello serve solo al dispatch di App.cpp).
    bool isListening();

    // Da richiamare SEMPRE da App.cpp, indipendentemente dalla schermata
    // attiva (stesso principio di WebRadioUI::backgroundUpdate()/
    // MeshtasticUI::backgroundUpdate()) - gestisce SipClient::update()
    // cosi' la registrazione/chiamata restano vive anche a schermo
    // chiuso, ma SOLO se l'utente ha acceso il secondo piano con
    // l'apposito bottone (stesso flag RAM-only di MeshtasticUI, mai
    // persistito, sempre spento al riavvio).
    void backgroundUpdate();
}
