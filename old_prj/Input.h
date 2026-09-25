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

    InputEvent getEvent();
    char getKey();

    bool available();

    // --- Touch GT911 ---
    // isTouched()/getTouchX()/getTouchY() riflettono lo stato raw del
    // touch (coordinate dell'ultimo punto toccato, valide anche a dito
    // sollevato: restano quelle dell'ultimo contatto).
    // wasTapped() e' true per UN SOLO ciclo di update(), al rilascio
    // di un tocco confermato (debounce a letture stabili) -> e' l'evento
    // da usare per selezionare una voce di menu al tocco.
    bool isTouched();
    int16_t getTouchX();
    int16_t getTouchY();
    bool wasTapped();
}
