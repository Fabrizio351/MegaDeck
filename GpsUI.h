#pragma once

// Schermata GPS: coordinate in testa, poi nazione, poi comune/via (da
// reverse geocoding, richiede WiFi). Gestisce da sola l'input finche' e'
// attiva, come le altre schermate (WifiUI/SettingsUI/NetScanUI).
namespace GpsUI {
    void open();
    void update();
    bool isActive();

    // Torna a mostrare la schermata GPS (senza rifare il fix/i dati):
    // usata da WeatherUI/ParkUI per il loro "Indietro", cosi' si risale
    // di un livello alla volta invece di saltare dritti al menu principale.
    void showMain();
}
