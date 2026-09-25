#pragma once

#include <Arduino.h>

// Previsione meteo per la prossima ora, via l'API pubblica gratuita
// Open-Meteo (nessuna chiave richiesta). Stessa filosofia di GeoLookup:
// niente libreria JSON, serve solo un campo dalla risposta.
namespace WeatherLookup {

    enum class Condition {
        Sereno,
        Nuvoloso,
        Pioggia,
        Sconosciuto
    };

    struct Result {
        bool valid;
        Condition condition;
    };

    // Chiamata BLOCCANTE (richiesta HTTPS, tipicamente 1-3 secondi): va
    // richiamata solo su azione esplicita dell'utente. Richiede una
    // connessione WiFi attiva e un'ora UTC valida dal GPS (per scegliere
    // l'ora giusta nell'array delle previsioni orarie).
    Result fetchNextHour(double lat, double lon, int utcHour);
}
