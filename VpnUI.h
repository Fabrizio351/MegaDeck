#pragma once

// Schermata VPN: stato del tunnel WireGuard + interruttore, stesso
// schema "dashboard" di NetGuardUI.h.
namespace VpnUI {
    void open();
    void update();
    bool isActive();
}
