#pragma once

#include <Arduino.h>

// Pacchettizzazione RTP + codifica/decodifica G.711 per VoIP (08/10 v1,
// 11/10 v2: full-duplex) - task FreeRTOS dedicato pinnato al core 0
// (stesso precedente di Input::touchTask e VpnConfig::sdMountTask), un
// solo task per tutto il ciclo audio della chiamata (non uno per TX e
// uno per RX: eviterebbe due cicli a 20ms indipendenti sullo stesso
// socket/periferiche I2S).
//
// v2: microfono->rete E rete->altoparlante, entrambi i versi. I pacchetti
// RTP in arrivo vengono decodificati (mu-law o A-law secondo il payload
// type) e interpolati a 16kHz dentro un piccolo ring (~3 frame) per
// assorbire il jitter di arrivo, poi riprodotti verso l'altoparlante -
// preso in prestito da Sound (Sound::pauseForExternalUse()/
// resumeForOwnUse(), stesso schema di WebRadioUI.cpp) con una propria
// istanza I2S, non tramite Sound::playRawSamples() (che richiede Sound
// "pronto", messo apposta a false durante il prestito).
//
// G.711 a 8kHz, Mic/Sound fissi a 16kHz: la conversione e' fatta qui
// dentro (media di coppie di campioni in downsampling, interpolazione
// lineare in upsampling - MAI "scarta/duplica uno su due", aliasing
// udibile in entrambi i casi).
//
// Volume in riproduzione (10/10): scala i campioni decodificati con
// Sound::getVolumePercent() prima di scriverli - RIUSA lo stesso valore
// persistito del resto del progetto (regolabile anche dai bottoni
// VOL+/VOL- della schermata CHIAMATA, vedi VoipUI.cpp), non un volume
// separato solo per le chiamate.
namespace RtpAudio {

    // Avvia il task audio: apre il socket UDP media su 'localPort' (la
    // stessa porta offerta nell'SDP da SipClient), e invia verso
    // remoteHost:remotePort (presi dall'SDP della risposta). Tutti i
    // buffer usati dal task vivono in RAM interna normale, mai PSRAM
    // (vedi piano: rischio di contesa bus col framebuffer schermo).
    bool start(const String& remoteHost, int remotePort, int localPort);

    // Segnala al task di fermarsi (flag volatile, letto a ogni giro) e
    // attende che si sia auto-cancellato (vTaskDelete su se stesso,
    // stesso schema di VpnConfig::sdMountTaskFn) prima di ritornare -
    // mai ucciso dall'esterno mentre potrebbe avere socket/I2S aperti.
    void stop();

    bool isActive();

    // Microfono muto (10/10, richiesto: poterlo attivare senza uscire
    // dalla schermata CHIAMATA) - manda silenzio al posto dell'audio
    // vero mentre attivo, invece di smettere di mandare pacchetti del
    // tutto (il flusso RTP resta regolare). Non persistito, si azzera
    // ad ogni nuova chiamata (vedi start()).
    void setMuted(bool muted);
    bool isMuted();

    // Diagnostica (vedi piano: da strumentare da subito, non assumere
    // che scrittura/lettura siano sempre rapide - mai provato prima in
    // questo progetto un ciclo audio a cadenza fissa).
    uint32_t getLoopMinMs();
    uint32_t getLoopMaxMs();
    uint32_t getUnderrunCount();
    uint32_t getOverrunCount();
}
