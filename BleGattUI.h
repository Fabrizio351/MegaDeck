#pragma once

// Schermata BLE GATT: scansiona le periferiche nelle vicinanze, ne
// mostra l'elenco, si connette a quella scelta e ne legge le
// caratteristiche - vedi BleGatt.h per la parte radio.
namespace BleGattUI {
    void open();
    void update();
    bool isActive();
}
