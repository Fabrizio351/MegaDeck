#pragma once

#include <Arduino.h>

// Reverse geocoding (coordinate -> nazione/comune/via) tramite l'API
// pubblica gratuita di Nominatim (OpenStreetMap). Serve una connessione
// WiFi attiva con accesso a internet.
namespace GeoLookup {

    struct Result {
        bool valid;
        String country;
        String locality; // comune/citta'
        String road;
    };

    // Chiamata BLOCCANTE (tipicamente 1-3 secondi, fino al timeout se la
    // rete e' lenta): va richiamata solo su azione esplicita dell'utente
    // (es. un bottone "Aggiorna"), mai in un ciclo continuo. Ritorna
    // valid=false se il WiFi non e' connesso o la richiesta fallisce.
    Result reverseGeocode(double lat, double lon);
}
