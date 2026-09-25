#pragma once

#include <stdint.h>

// Rilevamento tap generico per l'interfaccia disegnata a mano (Fase 1
// della riscrittura senza LVGL, vedi project_lvgl_removal_rewrite in
// memoria): prima LVGL faceva l'hit-test sui widget da solo, qui va
// rifatto a mano. Un solo elenco di "zone toccabili" attivo alla volta -
// ogni schermata registra le proprie zone quando si disegna, poi il
// loop principale interroga pollTap() per sapere se una di quelle zone
// e' stata toccata (pressione + rilascio senza spostarsi troppo, per
// distinguere un tap da uno scroll/trascinamento).
namespace TouchUI {
    constexpr int MAX_ZONES = 16;

    // Da chiamare a inizio ridisegno di una schermata, prima di
    // registrare le sue zone con addZone() - altrimenti le zone di una
    // schermata precedente resterebbero attive insieme alle nuove.
    void clearZones();

    // Registra un rettangolo toccabile (coordinate schermo, stesse usate
    // per disegnare il widget corrispondente). Ritorna l'indice della
    // zona (l'ordine di registrazione = l'id restituito da pollTap()),
    // o -1 se sono gia' state registrate MAX_ZONES zone.
    int addZone(int16_t x, int16_t y, int16_t w, int16_t h);

    // Da chiamare una volta per ciclo del loop principale, DOPO
    // Input::update() (serve lo stato touch gia' aggiornato). Ritorna
    // l'indice della zona appena toccata con un tap valido, o -1 se
    // nessun tap e' stato completato in questo ciclo.
    int pollTap();
}
