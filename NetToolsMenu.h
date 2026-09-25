#pragma once

// Sottomenu a griglia di "Net Tools" (stesso stile/comportamento del menu
// principale: griglia 2 colonne, navigazione trackball, tasto Indietro
// touch). Contiene le singole utility di rete (per ora solo NetScan);
// nuove voci si aggiungono nell'array items[] in NetToolsMenu.cpp.
namespace NetToolsMenu {

    void open();
    void update();
    bool isActive();

    // Torna a mostrare la griglia (senza reimpostare la selezione):
    // usata dalle schermate figlie (es. NetScanUI) per il loro "Indietro",
    // cosi' si risale di un livello alla volta invece di saltare dritti
    // al menu principale.
    void showMain();
}
