#include "GeoLookup.h"
#include "Config.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace {
    // Estrae il valore di un campo JSON "chiave":"valore" con una ricerca
    // di stringa semplice (niente libreria JSON: risparmia spazio flash
    // e ci servono solo 3 campi ben noti dalla risposta di Nominatim,
    // non serve un parser completo).
    String extractJsonStringField(const String& json, const char* key) {
        String pattern = String("\"") + key + "\":\"";
        int start = json.indexOf(pattern);
        if (start < 0) {
            return "";
        }

        start += pattern.length();
        int end = json.indexOf('"', start);
        if (end < 0) {
            return "";
        }

        return json.substring(start, end);
    }
}

namespace GeoLookup {

    Result reverseGeocode(double lat, double lon) {
        Result result;
        result.valid = false;

        if (WiFi.status() != WL_CONNECTED) {
            return result;
        }

        WiFiClientSecure client;
        // Nominatim e' un servizio pubblico gratuito che restituisce solo
        // dati geografici pubblici (non sensibili): setInsecure() salta
        // la verifica del certificato TLS per non dover incorporare un
        // bundle di CA root nel firmware (risparmio di spazio flash).
        client.setInsecure();

        HTTPClient http;
        char url[160];
        snprintf(
            url, sizeof(url),
            "https://nominatim.openstreetmap.org/reverse?lat=%.6f&lon=%.6f&format=json&zoom=16",
            lat, lon
        );

        if (!http.begin(client, url)) {
            return result;
        }

        // Nominatim richiede un User-Agent identificativo per l'uso
        // corretto dell'API pubblica (policy del servizio).
        http.setUserAgent("TDeckFW/" APP_VERSION);
        http.setTimeout(8000);

        int httpCode = http.GET();
        if (httpCode != 200) {
            http.end();
            return result;
        }

        String payload = http.getString();
        http.end();

        result.country = extractJsonStringField(payload, "country");

        result.locality = extractJsonStringField(payload, "city");
        if (result.locality.length() == 0) {
            result.locality = extractJsonStringField(payload, "town");
        }
        if (result.locality.length() == 0) {
            result.locality = extractJsonStringField(payload, "village");
        }

        result.road = extractJsonStringField(payload, "road");

        result.valid = (result.country.length() > 0 || result.locality.length() > 0);
        return result;
    }
}
