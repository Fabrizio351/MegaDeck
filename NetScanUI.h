#pragma once

// Schermata "NetScan" (dentro Net Tools): scansiona la rete WiFi a cui
// si e' connessi e mostra IP + MAC address degli host trovati. Gestisce
// da sola l'input finche' e' attiva, come WifiUI/SettingsUI.
namespace NetScanUI {
    void open();
    void update();
    bool isActive();
}
