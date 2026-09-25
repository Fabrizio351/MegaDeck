#pragma once

// Interfaccia WiFi: scansione reti, selezione, richiesta password se
// necessaria, connessione. Gestisce da sola l'input (trackball/tastiera/
// touch) finche' e' attiva: App::update() le passa il controllo al posto
// di Menu::update() mentre isActive() e' vera.
namespace WifiUI {
    void open();
    void update();
    bool isActive();
}
