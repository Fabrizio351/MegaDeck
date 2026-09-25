#pragma once

// Schermata "Park" raggiunta dal GPS: mostra distanza, direzione e (se
// disponibile) il nome della via/localita' del parco o dell'area
// pic-nic piu' vicina rispetto alle coordinate GPS correnti - utile per
// sapere dove fermarsi a riposare in viaggio. Gestisce da sola l'input
// finche' e' attiva, come le altre schermate (WifiUI/SettingsUI/
// NetScanUI/GpsUI/WeatherUI).
namespace ParkUI {
    void open();
    void update();
    bool isActive();
}
