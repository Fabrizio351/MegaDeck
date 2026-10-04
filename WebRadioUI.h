#pragma once

// Web Radio (27/09 interfaccia, 28/09 streaming vero, 29/09 riproduzione
// in background) - schermata con musicassetta anni '90 animata (bobine
// che girano durante il "play"), controlli Play/Stop, 5 stazioni (link
// persistenti su NVS, modificabili dal dispositivo) e streaming audio
// reale via libreria ESP32-audioI2S.
namespace WebRadioUI {
    void open();

    // Apre direttamente la schermata "modifica link" delle 5 stazioni,
    // saltando la schermata di riproduzione - usata da SettingsUI.cpp
    // (29/09, richiesto: raggiungibile anche da Impostazioni per tenere
    // tutto ordinato). Indietro/FATTO da qui tornano a Impostazioni, non
    // alla riproduzione. Non interrompe la riproduzione in corso.
    void openEditLinks();

    void update();
    bool isActive();

    // Da richiamare SEMPRE ad ogni ciclo (App.cpp), indipendentemente da
    // quale schermata sia attiva - stesso principio gia' usato per GPS::
    // update()/LoRaRadio::update()/NetGuard::update() - cosi' lo stream
    // continua a scaricare/decodificare anche navigando altrove nel
    // menu, invece di fermarsi appena si esce da questa schermata.
    void backgroundUpdate();
}
