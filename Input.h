#pragma once

#include <stdint.h>

enum class InputEvent {
    None,
    Up,
    Down,
    Left,
    Right,
    Select,
    Back,
    Key
};

namespace Input {
    void begin();
    void update();

    // Eventi "logici" da tastiera fisica e trackball, usati da Menu.cpp
    // per navigare la griglia a mano (LVGL non gestisce qui il focus:
    // la navigazione 2D a griglia resta pilotata direttamente).
    InputEvent getEvent();
    char getKey();
    bool available();

    // Il touch invece e' registrato come vero lv_indev_t (pointer) dentro
    // Input::begin(): i tap sui bottoni del menu arrivano a LVGL tramite
    // gli eventi LV_EVENT_CLICKED sui widget, non serve piu' hit-test
    // manuale ne' interrogare queste funzioni dal Menu.
    bool isTouched();
    int16_t getTouchX();
    int16_t getTouchY();

    // Normalmente 'w','a','s','d','e','b' sulla tastiera fisica sono
    // scorciatoie di navigazione (Up/Down/Left/Right/Select/Back). In un
    // campo di testo libero (es. password WiFi) queste stesse lettere
    // devono poter essere digitate normalmente: quando il modo e'
    // attivo, ogni tasto (comprese quelle lettere) arriva come
    // InputEvent::Key col carattere originale; solo Invio resta Select
    // (conferma) cosi' non serve reinventare un modo per confermare.
    void setTextEntryMode(bool enabled);
    bool isTextEntryMode();

    // Sospende la lettura dell'indev touch (vedi Input::update()): serve
    // a ScreenLock per impedire che un tocco sulla schermata "congelata"
    // durante il blocco raggiunga comunque i bottoni LVGL sottostanti -
    // l'elaborazione del touch (compreso il dispatch dei click) avviene
    // dentro lv_indev_read(), chiamata da questo modulo PRIMA che il
    // resto dell'app possa intervenire, quindi va fermata qui.
    // Tastiera/trackball restano invariati (servono per la combinazione
    // di sblocco).
    void setTouchSuppressed(bool suppressed);
}
