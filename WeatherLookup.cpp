#include "WeatherLookup.h"
#include "Config.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace {
    // Estrae l'elemento all'indice "index" di un array JSON numerico
    // "chiave":[v0,v1,v2,...]. Stessa filosofia di extractJsonStringField
    // in GeoLookup.cpp (niente libreria JSON), qui per un array invece
    // che per un campo singolo.
    int extractJsonIntArrayElement(const String& json, const char* key, int index) {
        String pattern = String("\"") + key + "\":[";
        int start = json.indexOf(pattern);
        if (start < 0) {
            return -1;
        }

        start += pattern.length();
        int end = json.indexOf(']', start);
        if (end < 0) {
            return -1;
        }

        String array = json.substring(start, end);

        int pos = 0;
        for (int i = 0; i < index; i++) {
            pos = array.indexOf(',', pos);
            if (pos < 0) {
                return -1;
            }
            pos++;
        }

        int nextComma = array.indexOf(',', pos);
        String valueStr = (nextComma < 0) ? array.substring(pos) : array.substring(pos, nextComma);
        valueStr.trim();

        if (valueStr.length() == 0) {
            return -1;
        }

        return valueStr.toInt();
    }

    // Codici meteo WMO (usati da Open-Meteo) raggruppati nelle 3
    // condizioni che vogliamo mostrare: non serve distinguere ogni
    // sfumatura (nebbia, neve, temporale...), solo dare un'indicazione
    // rapida "che tempo mi aspetta nella prossima ora".
    WeatherLookup::Condition classifyWeatherCode(int code) {
        if (code < 0) {
            return WeatherLookup::Condition::Sconosciuto;
        }
        if (code == 0) {
            return WeatherLookup::Condition::Sereno;
        }
        if (code <= 48) {
            // 1-3 parzialmente/coperto, 45/48 nebbia: nessuna pioggia,
            // ma nemmeno cielo sereno.
            return WeatherLookup::Condition::Nuvoloso;
        }
        // 51-67 pioviggine/pioggia, 71-77 neve, 80-99 rovesci/temporali:
        // tutte precipitazioni, mostrate come "pioggia" (non abbiamo
        // un'icona dedicata alla neve).
        return WeatherLookup::Condition::Pioggia;
    }

    // 0=Sereno, 1=Nuvoloso, 2=Pioggia: per confrontare "meglio/peggio" fra
    // due condizioni nella tendenza. Sconosciuto non e' comparabile.
    int severity(WeatherLookup::Condition c) {
        switch (c) {
            case WeatherLookup::Condition::Sereno:   return 0;
            case WeatherLookup::Condition::Nuvoloso: return 1;
            case WeatherLookup::Condition::Pioggia:  return 2;
            default:                                 return -1;
        }
    }

    const char* conditionLabel(WeatherLookup::Condition c) {
        switch (c) {
            case WeatherLookup::Condition::Sereno:   return "sereno";
            case WeatherLookup::Condition::Nuvoloso: return "nuvoloso";
            case WeatherLookup::Condition::Pioggia:  return "pioggia";
            default:                                 return "?";
        }
    }

    // Frase pronta sulla tendenza, confrontando la condizione di adesso
    // con quella fra ~4 ore (richiesto dall'utente: "capire come butta",
    // non solo l'ora immediatamente successiva).
    String buildTrend(WeatherLookup::Condition now, WeatherLookup::Condition later) {
        int sNow = severity(now);
        int sLater = severity(later);
        if (sNow < 0 || sLater < 0) {
            return "";
        }

        if (sLater == sNow) {
            return String("Stabile nelle prossime ore (") + conditionLabel(later) + ").";
        }
        if (sLater > sNow) {
            return String("In peggioramento: ") + conditionLabel(later) + " tra circa 4 ore.";
        }
        return String("In miglioramento: ") + conditionLabel(later) + " tra circa 4 ore.";
    }
}

namespace WeatherLookup {

    Result fetchNextHour(double lat, double lon, int utcHour) {
        Result result;
        result.valid = false;
        result.condition = Condition::Sconosciuto;

        if (WiFi.status() != WL_CONNECTED || utcHour < 0) {
            return result;
        }

        WiFiClientSecure client;
        // Open-Meteo e' un servizio pubblico gratuito senza dati
        // sensibili: setInsecure() salta la verifica del certificato TLS
        // per non incorporare un bundle di CA root nel firmware (stessa
        // scelta gia' fatta in GeoLookup).
        client.setInsecure();

        HTTPClient http;
        char url[220];
        // forecast_days=2 copre oggi+domani in UTC (indici 0-47): con
        // "prossima ora" = utcHour+1 non si esce mai da questo intervallo,
        // nemmeno a ridosso della mezzanotte.
        snprintf(
            url, sizeof(url),
            "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f"
            "&hourly=weathercode&forecast_days=2&timezone=UTC",
            lat, lon
        );

        if (!http.begin(client, url)) {
            return result;
        }

        http.setUserAgent("TDeckFW/" APP_VERSION);
        http.setTimeout(8000);

        int httpCode = http.GET();
        if (httpCode != 200) {
            http.end();
            return result;
        }

        String payload = http.getString();
        http.end();

        int code = extractJsonIntArrayElement(payload, "weathercode", utcHour + 1);
        if (code < 0) {
            return result;
        }

        result.condition = classifyWeatherCode(code);
        result.valid = true;

        // Tendenza: stesso payload gia' scaricato, nessuna richiesta di
        // rete aggiuntiva - legge solo un altro indice dello stesso array.
        int laterCode = extractJsonIntArrayElement(payload, "weathercode", utcHour + 4);
        if (laterCode >= 0) {
            result.trend = buildTrend(result.condition, classifyWeatherCode(laterCode));
        }

        return result;
    }
}
