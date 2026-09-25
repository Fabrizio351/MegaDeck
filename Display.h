#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>

namespace Display {

    // Accesso diretto al driver del pannello: serve ai moduli della
    // nuova interfaccia disegnata a mano (TouchUI e' su Input, non su
    // gfx; TextInputUI e i futuri moduli-schermata invece si', per
    // disegnare senza passare da LVGL). Vedi project_lvgl_removal_rewrite.
    Arduino_GFX* getGfx();

    // Fase 2: mentre una schermata disegnata a mano e' attiva, LVGL va
    // "messo in pausa" (update() salta lv_timer_handler()/lv_refr_now()
    // e l'header LVGL su lv_layer_top()), altrimenti risovrascriverebbe
    // il framebuffer sopra quello che abbiamo appena disegnato noi. Chi
    // apre una schermata cosi' chiama setDirectDrawMode(true) e la
    // richiude con setDirectDrawMode(false) prima di tornare al menu
    // (LVGL riprende da dove aveva lasciato, nessun redraw esplicito
    // necessario al ritorno).
    void setDirectDrawMode(bool active);

    // Diagnostica (25/09): NetScan segnalato "bloccato solo a schermo"
    // dopo diverse scansioni ripetute, nessun crash/riavvio nello
    // storico - probabile causa: qualcosa rimette in pausa il disegno
    // diretto (o lo lascia disattivo) senza che nessuno lo riattivi.
    // Esposto nel log periodico [BACKLIGHT] per vederlo subito al
    // prossimo blocco invece di continuare a indovinare.
    bool isDirectDrawMode();

    // Barra in alto condivisa per le schermate disegnate a mano - stessi
    // dati reali dell'header LVGL (ConnStatus, GPS, batteria) ma
    // disegnata direttamente. Da richiamare ad ogni apertura schermata e
    // ogni volta che si vuole aggiornarla (nessun redraw automatico:
    // niente ridisegno a tempo per non reintrodurre il problema gia'
    // risolto altrove, vedi project_lvgl_pool_exhaustion_fix).
    void drawHeaderBar();

    // Colori condivisi fra le schermate disegnate a mano - stessa
    // palette validata in Fase 1 (verde matrix). RGB565, non 24 bit.
    extern const uint16_t DD_BG;
    extern const uint16_t DD_ACCENT;
    extern const uint16_t DD_ACCENT_DIM;
    extern const uint16_t DD_TEXT;
    extern const uint16_t DD_TEXT_DIM;

    // Altezza della barra in alto disegnata a mano, in px: le schermate
    // la usano per sapere dove inizia il loro contenuto.
    constexpr int16_t DD_HEADER_H = 18;

    // Stampa testo multi-riga rispettando i "\n" gia' presenti nel testo
    // E spezzando le righe piu' larghe di w - il font classico e' a
    // larghezza fissa (6px/carattere a size 1), quindi basta un calcolo
    // diretto, niente getTextBounds(). Condivisa da tutte le schermate
    // disegnate a mano che mostrano testo libero (stato, risultati...).
    void drawWrappedText(int16_t x, int16_t y, int16_t w, const String& text, uint16_t color);

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

    // Fase 1 della riscrittura senza LVGL: disegna una schermata di prova
    // usando SOLO Arduino_GFX (nessun oggetto/widget LVGL), per
    // verificare dal vivo leggibilita' del font, resa dei colori e
    // rilevamento tap (via TouchUI) prima di costruire il resto sopra.
    // selectedIndex = voce evidenziata col rettangolino verticale.
    // Temporanea - vedi project_lvgl_removal_rewrite.
    void showDirectDrawTest(int selectedIndex = 0);

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
