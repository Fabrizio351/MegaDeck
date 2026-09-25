#pragma once

// Schermata "Meteo" raggiunta dal GPS: mostra un'icona (sole/nuvola/
// pioggia) per la condizione prevista nella prossima ora, nel punto delle
// coordinate GPS correnti. Gestisce da sola l'input finche' e' attiva,
// come le altre schermate (WifiUI/SettingsUI/NetScanUI/GpsUI).
namespace WeatherUI {
    void open();
    void update();
    bool isActive();
}
