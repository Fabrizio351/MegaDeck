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
    // Cerca "name":"..." nei "tags" dell'elemento corrente, cercando
    // SOLO fino al prossimo elemento ("\"type\":\"" successivo, o la
    // fine della risposta) - il nome sta sempre dopo lat/lon sia per i
    // nodi singoli sia per le aree con "center" (verificato sul formato
    // reale "out center" di Overpass). Gestisce il raro caso di
    // virgolette interne sfuggite ("\\\"").
    String extractName(const String& json, int fromPos, int boundEnd) {
        int namePos = json.indexOf("\"name\":\"", fromPos);
        if (namePos < 0 || namePos >= boundEnd) {
            return String();
        }
        namePos += 8;

        int nameEnd = json.indexOf('"', namePos);
        while (nameEnd > 0 && nameEnd < boundEnd && json[nameEnd - 1] == '\\') {
            nameEnd = json.indexOf('"', nameEnd + 1);
        }
        if (nameEnd < 0 || nameEnd > boundEnd) {
            return String();
        }

        String raw = json.substring(namePos, nameEnd);
        raw.replace("\\\"", "\"");
        return raw;
    }

    bool findNearest(
        const String& json, double fromLat, double fromLon,
        double* outDistance, double* outLat, double* outLon, String* outName
    ) {
        bool found = false;
        double minDistance = 0.0;
        double bestLat = 0.0;
        double bestLon = 0.0;
        String bestName;

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

                int nextElementPos = json.indexOf("\"type\":\"", lonEnd);
                int boundEnd = (nextElementPos < 0) ? json.length() : nextElementPos;
                bestName = extractName(json, lonEnd, boundEnd);
            }

            pos = lonEnd;
        }

        if (found) {
            *outDistance = minDistance;
            *outLat = bestLat;
            *outLon = bestLon;
            if (outName != nullptr) {
                *outName = bestName;
            }
        }
        return found;
    }

    // Esegue una query Overpass QL gia' completamente formata
    // ("filterBody": una o piu' istruzioni tipo "nwr(around:R,lat,lon)
    // %5Btag=valore%5D;", "[" "]" gia' percent-encoded) e ne fa il
    // parsing col risultato piu' vicino. Fattorizza l'HTTP+parsing comune
    // alle tre categorie (park, acqua, benzina) - prima duplicato per
    // intero dentro findNearestPark(), ora condiviso.
    ParkLookup::Result runOverpassQuery(double lat, double lon, const String& filterBody) {
        ParkLookup::Result result;
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
        String url = "https://overpass-api.de/api/interpreter?data="
            "%5Bout:json%5D;" + filterBody + "out%20center%20qt%20" + String(MAX_RESULTS) + ";";

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
        String name;
        if (!findNearest(payload, lat, lon, &distance, &nodeLat, &nodeLon, &name)) {
            return result;
        }

        result.valid = true;
        result.distanceMeters = distance;
        result.lat = nodeLat;
        result.lon = nodeLon;
        result.name = name;
        result.bearingDegrees = bearingDegrees(lat, lon, nodeLat, nodeLon);
        return result;
    }

    // Un solo filtro tag=valore su un tipo OSM dato (es. "nwr" o "node"),
    // percent-encoded pronto per l'URL - usato dalle categorie con un
    // solo tag (acqua, benzina). Il parco ne usa due insieme, costruiti
    // direttamente in findNearestPark().
    String singleTagFilter(double lat, double lon, const char* osmType, const char* tag, const char* value) {
        char buf[160];
        snprintf(
            buf, sizeof(buf), "%s(around:%.0f,%.6f,%.6f)%%5B%s=%s%%5D;",
            osmType, SEARCH_RADIUS_METERS, lat, lon, tag, value
        );
        return String(buf);
    }
}

namespace ParkLookup {

    Result findNearestPark(double lat, double lon) {
        // "nwr" (node/way/relation), non solo nodi: i parchi sono spesso
        // mappati come poligoni (way/relation), non un singolo punto. Due
        // filtri uniti in un'unica query (parco + area pic-nic).
        String filterBody = "(" +
            singleTagFilter(lat, lon, "nwr", "leisure", "park") +
            singleTagFilter(lat, lon, "nwr", "tourism", "picnic_site") +
            ");";
        return runOverpassQuery(lat, lon, filterBody);
    }

    Result findNearestWater(double lat, double lon) {
        // Le fontanelle d'acqua potabile sono quasi sempre mappate come
        // singoli nodi (mai poligoni) - "node" invece di "nwr".
        return runOverpassQuery(lat, lon, singleTagFilter(lat, lon, "node", "amenity", "drinking_water"));
    }

    Result findNearestFuel(double lat, double lon) {
        // "nwr": alcuni distributori sono mappati come area (il piazzale),
        // non solo un nodo singolo.
        return runOverpassQuery(lat, lon, singleTagFilter(lat, lon, "nwr", "amenity", "fuel"));
    }

    Result findNearestToilet(double lat, double lon) {
        // Quasi sempre nodi singoli, ma "nwr" copre anche i rari casi di
        // blocchi bagni mappati come area.
        return runOverpassQuery(lat, lon, singleTagFilter(lat, lon, "nwr", "amenity", "toilets"));
    }
}
