#pragma once

// Sottomenu a griglia di "LoRa Tools" (stesso stile/comportamento di
// NetToolsMenu.h: griglia 2 colonne, navigazione trackball, tasto
// Indietro touch). Contiene LoRa Chat, LoRa Scan e LoRaWAN; nuove voci
// si aggiungono nell'array items[] in LoRaToolsMenu.cpp.
namespace LoRaToolsMenu {

    void open();
    void update();
    bool isActive();

    // Torna a mostrare la griglia (senza reimpostare la selezione):
    // usata dalle schermate figlie (es. LoRaScanUI) per il loro
    // "Indietro", cosi' si risale di un livello alla volta invece di
    // saltare dritti al menu principale.
    void showMain();
}
