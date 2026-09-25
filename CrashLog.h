#pragma once

#include <Arduino.h>

// Registro leggero degli ultimi eventi prima di un riavvio anomalo, per
// capire "dove scoppia" anche senza monitor seriale collegato - utile
// apposta perche' i messaggi seriali possono essere disattivati per
// risparmiare batteria (vedi DEBUG_SERIAL_ENABLED in Config.h).
//
// Le "briciole di pane" della sessione corrente vivono in RAM RTC
// (RTC_NOINIT_ATTR): sopravvive a reset software/watchdog/panico, NON a
// un vero distacco di alimentazione, e costa scriverci quasi zero
// tempo/energia (nessun accesso flash). Se all'avvio il motivo del
// reset risulta anomalo, la traccia viene copiata in NVS (quella si',
// sopravvive anche al power-cycle successivo) prima di azzerarla per la
// nuova sessione - cosi' resta leggibile "a posteriori" anche molto
// dopo il crash vero e proprio.
namespace CrashLog {
    // Va chiamata il prima possibile in App::begin(), prima di
    // qualunque altro modulo (cosi' le sue breadcrumb() sono incluse
    // nella traccia).
    void begin();

    // Registra un evento breve (troncato a ~23 caratteri, tenerlo
    // corto): es. CrashLog::breadcrumb("menu:Wifi").
    void breadcrumb(const char* tag);

    // true se l'ultimo avvio e' partito da un reset anomalo (non
    // accensione normale ne' riavvio software volontario).
    bool hadCrash();

    // Motivo del reset anomalo piu' recente mai registrato (stringa
    // leggibile, es. "Watchdog task"), vuota se non c'e' mai stato nulla.
    String getLastCrashReason();

    // Le breadcrumb registrate prima dell'ultimo crash rilevato, una per
    // riga, dalla piu' vecchia alla piu' recente.
    String getLastCrashLog();

    // Riepilogo del coredump ESP-IDF salvato automaticamente in flash al
    // momento del crash (task, indirizzo dell'istruzione, causa
    // dell'eccezione, backtrace) - letto e stampato su seriale UNA VOLTA
    // sola in CrashLog::begin() (poi il coredump grezzo viene azzerato,
    // ma questo riepilogo testuale resta leggibile qui e nella schermata
    // di avvio anomalo anche molto dopo). Non serve nessun tool esterno
    // (espcoredump/IDF): usa esp_core_dump_get_summary(), disponibile
    // gia' nel firmware stesso. Vuoto se l'ultimo reset non ha lasciato
    // un coredump valido (es. un semplice watchdog senza eccezione CPU).
    String getLastCoreDumpSummary();

    // Valore grezzo di esp_reset_reason() per QUESTO avvio (diagnostica
    // temporanea: capire come viene classificato il reset via RTS usato
    // da arduino-cli per ogni caricamento). Il print diretto in begin()
    // si perde spesso con l'USB nativo (il terminale si ricollega dopo
    // che i primissimi byte del boot sono gia' passati, non c'e' un
    // buffer esterno che li trattiene) - questo getter permette di
    // leggerlo con calma dalla ristampa periodica in App.cpp, quando la
    // connessione seriale e' sicuramente gia' stabile.
    int getLastRawResetReason();

    // Cronologia rotante degli ultimi crash (non solo l'ultimo): fino a
    // 5, dal piu' recente (indice 0) al piu' vecchio ancora in memoria
    // (indice getCrashHistoryCount()-1). Ogni nuovo crash spinge fuori
    // il piu' vecchio quando la cronologia e' piena.
    int getCrashHistoryCount();
    String getCrashHistoryReason(int indexFromRecent);
    String getCrashHistoryLog(int indexFromRecent);
    String getCrashHistorySummary(int indexFromRecent);

    // Registra un guasto intercettato "in diretta" (prima di un riavvio
    // volontario) nella stessa cronologia rotante dei crash veri, con le
    // breadcrumb della sessione corrente. Va chiamata subito prima di
    // riavviare. Usata in passato dall'assert-handler di LVGL (rimosso
    // il 26/09, vedi project_lvgl_removal_rewrite in memoria); generica,
    // riutilizzabile per futuri guasti intercettati.
    void recordControlledFault(const char* reason);
}
