#pragma once

// Schermata Impostazioni: per ora solo la luminosita' del display,
// regolabile con trackball sinistra/destra o touch (+/-). Gestisce da
// sola l'input finche' e' attiva, come WifiUI.
namespace SettingsUI {
    void open();
    void update();
    bool isActive();
}
