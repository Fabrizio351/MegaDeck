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

    // Stato touch "grezzo" gia' debounced (vedi Input.cpp): letto da
    // TouchUI per il proprio hit-test manuale sulle zone registrate da
    // ogni schermata.
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

    // Sospende la lettura touch (vedi Input::update()): serve a
    // ScreenLock per impedire che un tocco sulla schermata "congelata"
    // durante il blocco raggiunga comunque le zone toccabili sottostanti.
    // Tastiera/trackball restano invariati (servono per la combinazione
    // di sblocco).
    void setTouchSuppressed(bool suppressed);
}
