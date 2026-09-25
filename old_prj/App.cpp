#include "App.h"
#include <Arduino.h>
#include "Board.h"
#include "Display.h"
#include "Input.h"
#include "Menu.h"
#include "GPS.h"
#include "LoRaRadio.h"

namespace {

    void handleMenuAction(MenuAction action) {
        switch (action) {
            case MenuAction::FirstItem:
                Display::showMessage(
                    "Gestione Chiamate e Messaggi VoIP",
                    "Inserisci qui la tua funzione"
                );
                break;

            case MenuAction::SecondItem:
                Display::showMessage(
                    "Connessione e Gestione reti WiFi",
                    "Inserisci qui la tua funzione"
                );
                break;

            case MenuAction::ThirdItem:
                Display::showMessage(
                    "Gestioni dati GPS",
                    "Inserisci qui la tua funzione"
                );
                break;

            case MenuAction::Settings:
                Display::showMessage(
                    "Impostazioni",
                    "Inserisci qui le impostazioni"
                );
                break;

            case MenuAction::About:
                Display::showMessage(
                    "Informazioni",
                    "TDeckFW 0.1.0"
                );
                break;

            default:
                break;
        }
    }
}

namespace App {

    void begin() {
        Board::begin();
        Display::begin();
        Input::begin();
        GPS::begin();
        LoRaRadio::begin();

        Display::showBootScreen();

        Menu::begin(handleMenuAction);
    }

    void update() {
        Board::update();

        Input::update();
        Menu::update();

        GPS::update();
        LoRaRadio::update();

        Display::update();

        delay(5);
    }
}
