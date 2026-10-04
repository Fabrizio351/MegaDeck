#pragma once

#include <Arduino.h>

// Distanza dal parco/area pic-nic, fontanella d'acqua potabile o
// distributore di benzina piu' vicino, via l'API pubblica gratuita
// Overpass (dati OpenStreetMap). Nessuna chiave richiesta. Come
// GeoLookup/WeatherLookup: la qualita' del risultato dipende dalla
// completezza della mappatura OSM nella zona.
//
// Estesa il 26/09 (prima cercava solo parchi/aree pic-nic, richiesto
// dall'utente per un "quadro completo" con piu' categorie sulla stessa
// schermata - vedi ParkUI.cpp) - tre funzioni separate invece di una
// query combinata: piu' lenta (tre richieste di rete invece di una) ma
// molto piu' semplice da tenere corretta, e ognuna riusa la stessa
// identica logica di parsing gia' in uso.
namespace ParkLookup {

    struct Result {
        bool valid;
        double distanceMeters;

        // Rilevamento iniziale (0=Nord, 90=Est, 180=Sud, 270=Ovest) dal
        // punto di partenza verso il punto trovato: la UI lo traduce in
        // un punto cardinale leggibile (es. "Nord-Est").
        double bearingDegrees;

        // Coordinate del punto trovato (centro dell'area se e' un
        // poligono): servono a chi chiama per un eventuale reverse
        // geocoding (nome via/localita').
        double lat;
        double lon;

        // Nome OSM del punto ("tags":{"name":...}), se mappato - vuoto
        // se il dato OSM non ha un nome (comune per fontanelle/bagni).
        String name;
    };

    // Chiamate BLOCCANTI (richiesta HTTPS, puo' richiedere qualche
    // secondo: Overpass e' piu' lento di Nominatim/Open-Meteo): vanno
    // richiamate solo su azione esplicita dell'utente. Richiedono una
    // connessione WiFi attiva. Cercano entro un raggio fisso di 5 km;
    // valid=false se non trovano nulla in quel raggio o se la richiesta
    // fallisce.
    Result findNearestPark(double lat, double lon);         // leisure=park, tourism=picnic_site
    Result findNearestWater(double lat, double lon);        // amenity=drinking_water
    Result findNearestFuel(double lat, double lon);         // amenity=fuel
    Result findNearestToilet(double lat, double lon);       // amenity=toilets
}
