#include "ParkLookup.h"
#include "Config.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>

namespace {
    constexpr double SEARCH_RADIUS_METERS = 5000.0;
    constexpr int MAX_RESULTS = 20;
    constexpr double EARTH_RADIUS_METERS = 6371000.0;

    double haversineMeters(double lat1, double lon1, double lat2, double lon2) {
        double dLat = radians(lat2 - lat1);
        double dLon = radians(lon2 - lon1);

        double a = sin(dLat / 2) * sin(dLat / 2)
            + cos(radians(lat1)) * cos(radians(lat2)) * sin(dLon / 2) * sin(dLon / 2);
        double c = 2 * atan2(sqrt(a), sqrt(1 - a));

        return EARTH_RADIUS_METERS * c;
    }

    // Rilevamento iniziale (bearing) da (lat1,lon1) verso (lat2,lon2),
    // in gradi 0-360 (0=Nord, 90=Est, 180=Sud, 270=Ovest). Formula
    // standard di navigazione sferica.
    double bearingDegrees(double lat1, double lon1, double lat2, double lon2) {
        double phi1 = radians(lat1);
        double phi2 = radians(lat2);
        double deltaLambda = radians(lon2 - lon1);

        double y = sin(deltaLambda) * cos(phi2);
        double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(deltaLambda);
        double theta = atan2(y, x);

        double degrees = fmod((theta * 180.0 / PI) + 360.0, 360.0);
        return degrees;
    }

    // Scandisce la risposta Overpass cercando le coppie "lat"/"lon" in
    // sequenza e calcolando la distanza minima dal punto di partenza.
    // Con "out center" i nodi hanno "lat"/"lon" diretti, mentre le aree
    // (way/relation, es. i parchi) li hanno annidati dentro un oggetto
    // "center":{...} - ma essendo una scansione a sottostringa, non
    // JSON vero, la profondita' di annidamento non conta: trova comunque
    // la prossima coppia "lat"/"lon" in entrambi i casi. Niente libreria
    // JSON (stesso approccio di GeoLookup/WeatherLookup).
    bool findNearest(
        const String& json, double fromLat, double fromLon,
        double* outDistance, double* outLat, double* outLon
    ) {
        bool found = false;
        double minDistance = 0.0;
        double bestLat = 0.0;
        double bestLon = 0.0;

        int pos = 0;
        while (true) {
            int latPos = json.indexOf("\"lat\":", pos);
            if (latPos < 0) {
                break;
            }
            latPos += 6;

            int latEnd = json.indexOf(',', latPos);
            if (latEnd < 0) {
                break;
            }

            int lonPos = json.indexOf("\"lon\":", latEnd);
            if (lonPos < 0) {
                break;
            }
            lonPos += 6;

            int lonEnd = json.indexOf(',', lonPos);
            int lonEndBrace = json.indexOf('}', lonPos);
            if (lonEnd < 0 || (lonEndBrace >= 0 && lonEndBrace < lonEnd)) {
                lonEnd = lonEndBrace;
            }
            if (lonEnd < 0) {
                break;
            }

            double nodeLat = json.substring(latPos, latEnd).toDouble();
            double nodeLon = json.substring(lonPos, lonEnd).toDouble();

            double distance = haversineMeters(fromLat, fromLon, nodeLat, nodeLon);
            if (!found || distance < minDistance) {
                minDistance = distance;
                bestLat = nodeLat;
                bestLon = nodeLon;
                found = true;
            }

            pos = lonEnd;
        }

        if (found) {
            *outDistance = minDistance;
            *outLat = bestLat;
            *outLon = bestLon;
        }
        return found;
    }
}

namespace ParkLookup {

    Result findNearestPark(double lat, double lon) {
        Result result;
        result.valid = false;
        result.distanceMeters = 0.0;
        result.bearingDegrees = 0.0;
        result.lat = 0.0;
        result.lon = 0.0;

        if (WiFi.status() != WL_CONNECTED) {
            return result;
        }

        WiFiClientSecure client;
        // Overpass e' un servizio pubblico gratuito (dati OpenStreetMap):
        // setInsecure() salta la verifica del certificato TLS per non
        // incorporare un bundle di CA root nel firmware, come per
        // GeoLookup/WeatherLookup.
        client.setInsecure();

        HTTPClient http;
        char url[500];
        // Query Overpass QL con "[" "]" e spazi percent-encoded (%5B %5D
        // %20): gli altri caratteri (: ; ( ) , =) sono tollerati non
        // codificati in una query string. Unione di due filtri (parco +
        // area pic-nic) tramite "nwr" (node/way/relation, non solo nodi
        // come per l'acqua: i parchi sono spesso poligoni) e
        // "out center" per avere comunque un punto utilizzabile.
        snprintf(
            url, sizeof(url),
            "https://overpass-api.de/api/interpreter?data="
            "%%5Bout:json%%5D;(nwr(around:%.0f,%.6f,%.6f)%%5Bleisure=park%%5D;"
            "nwr(around:%.0f,%.6f,%.6f)%%5Btourism=picnic_site%%5D;);"
            "out%%20center%%20qt%%20%d;",
            SEARCH_RADIUS_METERS, lat, lon,
            SEARCH_RADIUS_METERS, lat, lon,
            MAX_RESULTS
        );

        if (!http.begin(client, url)) {
            return result;
        }

        http.setUserAgent("TDeckFW/" APP_VERSION);
        // Overpass e' tipicamente piu' lento di Nominatim/Open-Meteo.
        http.setTimeout(12000);

        int httpCode = http.GET();
        if (httpCode != 200) {
            http.end();
            return result;
        }

        String payload = http.getString();
        http.end();

        double distance = 0.0;
        double nodeLat = 0.0;
        double nodeLon = 0.0;
        if (!findNearest(payload, lat, lon, &distance, &nodeLat, &nodeLon)) {
            return result;
        }

        result.valid = true;
        result.distanceMeters = distance;
        result.lat = nodeLat;
        result.lon = nodeLon;
        result.bearingDegrees = bearingDegrees(lat, lon, nodeLat, nodeLon);
        return result;
    }
}
