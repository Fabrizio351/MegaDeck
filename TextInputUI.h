#pragma once

#include <Arduino.h>
#include <stdint.h>

// Campo di testo per l'interfaccia disegnata a mano (Fase 1 della
// riscrittura senza LVGL, vedi project_lvgl_removal_rewrite in memoria):
// prima lv_textarea faceva tutto da solo (buffer, cursore, disegno), qui
// va rifatto a mano in due pezzi separati - raccolta tasti (gia' esisteva
// in forma identica dentro WifiUI.cpp per la password WiFi, qui resa
// riutilizzabile) e disegno (nuovo, solo Arduino_GFX).
namespace TextInputUI {

    enum class Result {
        None,       // nessun tasto rilevante in questo ciclo
        Changed,    // il buffer e' cambiato (carattere aggiunto/tolto) - ridisegnare
        Confirmed   // Invio/Select: chi chiama decide cosa fare (es. connettersi)
    };

    // Da chiamare una volta per ciclo mentre il campo ha il focus (serve
    // Input::setTextEntryMode(true) gia' attivo lato chiamante, stesso
    // requisito di prima). Aggiorna buffer sul posto.
    Result handleKey(String& buffer, uint16_t maxLength);

    // Disegna il campo: rettangolo con bordo, testo (mostra la CODA del
    // buffer se piu' largo del box, cosi' si vede sempre l'ultimo
    // carattere digitato) e un cursore statico alla fine - niente
    // lampeggio a tempo: andrebbe ridisegnato ad ogni ciclo anche senza
    // che il testo cambi, lo stesso errore gia' corretto altrove questa
    // settimana (vedi project_lvgl_pool_exhaustion_fix). Da richiamare
    // solo quando handleKey() ritorna Changed (o al primo disegno).
    void draw(
        int16_t x, int16_t y, int16_t w, int16_t h,
        const String& buffer, bool maskAsPassword = false
    );
}
