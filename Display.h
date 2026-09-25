#pragma once

#include <stdint.h>
#include <lvgl.h>

namespace Display {

    // Palette condivisa (Menu.cpp la riusa per stilizzare i bottoni)
    extern const lv_color_t COLOR_BG;       // nero pieno
    extern const lv_color_t COLOR_ACCENT;   // azzurro cyberpunk
    extern const lv_color_t COLOR_TEXT;     // bianco

    void begin();
    void update();  // chiama lv_timer_handler(), va invocata ogni ciclo

    // Diagnostica schermo nero (2026-09-24): true se l'allocazione dei
    // buffer di disegno LVGL in PSRAM (dentro begin()) e' andata a buon
    // fine. Se false, lo schermo resta "retroilluminato ma nero" perche'
    // LVGL non ha mai avuto un buffer valido da cui disegnare - il vecchio
    // print d'errore ("[DISPLAY] ERRORE: allocazione buffer LVGL in PSRAM
    // fallita") e' troppo presto nel boot per catturarlo dal vivo in modo
    // affidabile (vedi project_crash_diagnosis_workflow sulla
    // ri-enumerazione USB), questo getter permette di leggerlo con calma
    // dal log periodico invece.
    bool areBuffersOk();

    // Schermata di boot
    void showBootScreen();

    // Schermata "teschio stile Matrix" mostrata per qualche secondo dopo
    // un riavvio anomalo, al posto del vecchio riepilogo testuale: i
    // dettagli veri (motivo, breadcrumb, coredump) continuano comunque a
    // essere salvati da CrashLog e leggibili in seguito (seriale/NVS),
    // qui e' solo un avviso visivo rapido "qualcosa e' crashato".
    void showCrashSkull();

    // Schermata generica titolo + messaggio (usata dalle voci non ancora implementate).
    // onBack (opzionale): se passato, mostra un bottone touch "Indietro"
    // che lo richiama al tap. Senza, si puo' uscire solo da tastiera/
    // trackball (Indietro/Select) - va bene per schermate raggiungibili
    // solo da menu principale, ma ogni sottomenu dovrebbe passarlo.
    void showMessage(const char* title, const char* message, void (*onBack)() = nullptr);

    // Crea uno schermo LVGL vuoto con sfondo nero e titolo in alto,
    // pronto per essere popolato da chi chiama. Ritorna lo screen e,
    // tramite outContent, un container vuoto sotto al titolo dove
    // il chiamante puo' aggiungere i propri widget.
    // onBack (opzionale): aggiunge un bottone touch "Indietro" in alto a
    // destra, per garantire che ogni sottomenu abbia un modo touch di
    // tornare indietro, non solo tastiera/trackball.
    lv_obj_t* createScreen(const char* title, lv_obj_t** outContent, void (*onBack)() = nullptr);
}
