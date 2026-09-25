#pragma once

// Schermata NetGuard: stato del monitoraggio, bottone per
// attivarlo/disattivarlo e bottone per aggiornare la whitelist (rilancia
// un NetScan). La logica vera e propria vive in NetGuard.h/.cpp, gia'
// attiva in background indipendentemente da questa UI.
namespace NetGuardUI {
    void open();
    bool isActive();
    void update();
}
