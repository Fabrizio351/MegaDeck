#pragma once

#include "Input.h"

// Una voce per ogni bottone della griglia principale (vedi mainItems[] in
// Menu.cpp): mappatura diretta 1:1, niente piu' "FirstItem/SecondItem..."
// generici che si disallineavano ogni volta che la lista veniva cambiata.
enum class MenuAction {
    None,
    VoIP,
    Wifi,
    Bluetooth,
    LoRa,
    Gps,
    NetTools,
    Vpn,
    OpenClaw,
    HomeAssistant,
    Frigate,
    Drone,
    Settings
};

namespace Menu {

    using ActionCallback = void (*)(MenuAction action);

    void begin(ActionCallback callback);
    void update();

    void showMain();
    void showCurrentScreen();
    void back();

    bool isMainMenu();
}
