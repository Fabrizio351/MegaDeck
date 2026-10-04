#pragma once

// Verifica di raggiungibilita' TCP su un host (30/09) - "telnet" nel
// senso "controlla se queste porte sono aperte", non un vero terminale
// interattivo: IP scelto a mano o dalla lista di NetScan (la stessa
// usata come whitelist da NetGuard, vedi NetScan.h), porte classiche
// 80/443/22 attivabili singolarmente + un campo porte extra separate da
// virgola - il risultato e' l'unione delle due. Terza voce del
// sottomenu NET TOOLS (vedi NetToolsMenu.cpp).
namespace TelnetUI {
    void open();
    void update();
    bool isActive();
}
