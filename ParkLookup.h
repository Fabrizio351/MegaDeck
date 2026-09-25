#pragma once

#include <Arduino.h>

// Distanza dal parco/area pic-nic piu' vicino, via l'API pubblica
// gratuita Overpass (dati OpenStreetMap, tag leisure=park e
// tourism=picnic_site). Nessuna chiave richiesta. Come GeoLookup/
// WeatherLookup/(l'ormai ex WaterLookup): la qualita' del risultato
// dipende dalla completezza della mappatura OSM nella zona.
//
// A differenza delle fontane d'acqua (sempre singoli punti), i parchi
// sono spesso mappati come aree (way/relation, non un singolo nodo):
// la query usa "nwr" (node/way/relation) + "out center" per ottenere
// comunque un punto di riferimento utilizzabile per ciascun risultato.
namespace ParkLookup {

    struct Result {
        bool valid;
        double distanceMeters;

        // Rilevamento iniziale (0=Nord, 90=Est, 180=Sud, 270=Ovest) dal
        // punto di partenza verso il parco trovato: la UI lo traduce in
        // un punto cardinale leggibile (es. "Nord-Est").
        double bearingDegrees;

        // Coordinate del parco trovato (centro dell'area se e' un
        // poligono): servono a chi chiama per un eventuale reverse
        // geocoding (nome via/localita').
        double lat;
        double lon;
    };

    // Chiamata BLOCCANTE (richiesta HTTPS, puo' richiedere qualche
    // secondo in piu' di GeoLookup/WeatherLookup: Overpass e' piu' lento
    // di Nominatim/Open-Meteo): va richiamata solo su azione esplicita
    // dell'utente. Richiede una connessione WiFi attiva. Cerca entro un
    // raggio fisso di 5 km; valid=false se non trova nulla in quel raggio
    // o se la richiesta fallisce.
    Result findNearestPark(double lat, double lon);
}
