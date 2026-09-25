#include "GPS.h"
#include "Config.h"

#include <TinyGPS.h>
#include <Preferences.h>
#include <time.h>

namespace {
    TinyGPSPlus gpsParser;
    HardwareSerial gpsSerial(1);

    Preferences gpsPrefs;
    constexpr const char* GPS_PREFS_NAMESPACE = "tdeckgps";
    constexpr const char* GPS_PREFS_KEY_ENABLED = "enabled";
    constexpr const char* GPS_PREFS_KEY_GMT_OFFSET = "gmt_offset";
    bool nvsOpened = false;
    bool runtimeEnabled = true;

    // Interruttore manuale GMT+-N (Impostazioni): rimpiazza la vecchia
    // stima dalla longitudine, che non poteva comunque sapere se in
    // quel momento e periodo si applica l'ora legale o solare (es.
    // Europa, +1 o +2 a seconda della stagione).
    int manualUtcOffsetHours = 0;

    void ensureNvsOpen() {
        if (nvsOpened) {
            return;
        }

        gpsPrefs.begin(GPS_PREFS_NAMESPACE, false);
        runtimeEnabled = gpsPrefs.getBool(GPS_PREFS_KEY_ENABLED, true);
        manualUtcOffsetHours = gpsPrefs.getInt(GPS_PREFS_KEY_GMT_OFFSET, 0);
        nvsOpened = true;
    }

    // Subito dopo l'accensione (prima di agganciare un orario reale dai
    // satelliti), il modulo GPS trasmette una data-sentinella fissa
    // ("30.11.99" nei nostri log, cioe' anno 2099) invece di lasciare il
    // campo vuoto: TinyGPS++ la marca comunque "valida" perche' il campo
    // e' presente nella frase NMEA, ma NON e' un orario reale. Un
    // controllo di plausibilita' sull'anno scarta questo placeholder
    // senza scartare date vere per i prossimi ~70 anni.
    bool isPlausibleGpsYear(int year) {
        return year >= 2020 && year <= 2090;
    }

    // Applica l'offset di fuso a data+ora GPS (UTC) gestendo correttamente
    // il cambio di giorno quando l'offset porta oltre mezzanotte.
    bool getLocalDateTime(struct tm& out) {
        if (!gpsParser.time.isValid() || !gpsParser.date.isValid()) {
            return false;
        }

        if (!isPlausibleGpsYear(gpsParser.date.year())) {
            return false;
        }

        struct tm t = {};
        t.tm_year = gpsParser.date.year() - 1900;
        t.tm_mon = gpsParser.date.month() - 1;
        t.tm_mday = gpsParser.date.day();
        t.tm_hour = gpsParser.time.hour();
        t.tm_min = gpsParser.time.minute();
        t.tm_sec = gpsParser.time.second();

        // Il core ESP32 di default gira in UTC (nessuna TZ impostata),
        // quindi mktime() qui tratta i campi come UTC, non come ora
        // locale del dispositivo.
        time_t utcTime = mktime(&t);
        utcTime += manualUtcOffsetHours * 3600L;

        gmtime_r(&utcTime, &out);
        return true;
    }
}

namespace GPS {

    void begin() {
#if !GPS_ENABLED
        DEBUG_PRINTLN("[GPS] disabilitato (GPS_ENABLED=0), skip totale");
        return;
#else
        // Se dopo qualche minuto non ottieni mai un fix, prova a cambiare
        // GPS_BAUD_RATE in Config.h (alcuni MIA-M10Q escono di fabbrica a 38400).
        gpsSerial.begin(GPS_BAUD_RATE, SERIAL_8N1, BOARD_GPS_RX_PIN, BOARD_GPS_TX_PIN);

        ensureNvsOpen();

        DEBUG_PRINTLN("[GPS] avvio...");
#endif
    }

    void update() {
#if !GPS_ENABLED
        return;
#else
        ensureNvsOpen();
        if (!runtimeEnabled) {
            return;
        }

        while (gpsSerial.available() > 0) {
            gpsParser.encode(gpsSerial.read());
        }
#endif
    }

    bool hasFix() {
        return gpsParser.location.isValid() && gpsParser.location.isUpdated();
    }

    double getLatitude() {
        return gpsParser.location.isValid() ? gpsParser.location.lat() : 0.0;
    }

    double getLongitude() {
        return gpsParser.location.isValid() ? gpsParser.location.lng() : 0.0;
    }

    double getAltitudeMeters() {
        return gpsParser.altitude.isValid() ? gpsParser.altitude.meters() : 0.0;
    }

    double getSpeedKmh() {
        return gpsParser.speed.isValid() ? gpsParser.speed.kmph() : 0.0;
    }

    int getSatellites() {
        return gpsParser.satellites.isValid() ? gpsParser.satellites.value() : 0;
    }

    int getUtcHour() {
        return gpsParser.time.isValid() ? static_cast<int>(gpsParser.time.hour()) : -1;
    }

    int getUtcOffsetHours() {
        ensureNvsOpen();
        return manualUtcOffsetHours;
    }

    void setUtcOffsetHours(int hours) {
        ensureNvsOpen();

        if (hours < -12) hours = -12;
        if (hours > 14) hours = 14;

        if (manualUtcOffsetHours == hours) {
            return;
        }

        manualUtcOffsetHours = hours;
        gpsPrefs.putInt(GPS_PREFS_KEY_GMT_OFFSET, hours);
    }

    String getTimeLocal() {
        struct tm local;
        if (!getLocalDateTime(local)) {
            return "";
        }

        // Solo ore:minuti, niente secondi: l'header in alto si aggiorna
        // solo ogni minuto (vedi HEADER_UPDATE_INTERVAL_MS in Display.cpp),
        // mostrare i secondi darebbe l'illusione di una precisione che
        // non c'e' (resterebbero fermi per un minuto intero).
        char buffer[6];
        snprintf(buffer, sizeof(buffer), "%02d:%02d", local.tm_hour, local.tm_min);

        return String(buffer);
    }

    String getDateLocal() {
        struct tm local;
        if (!getLocalDateTime(local)) {
            return "";
        }

        char buffer[9];
        snprintf(
            buffer,
            sizeof(buffer),
            "%02d.%02d.%02d",
            local.tm_mday,
            local.tm_mon + 1,
            (local.tm_year + 1900) % 100
        );

        return String(buffer);
    }

    bool isEnabled() {
        ensureNvsOpen();
        return runtimeEnabled;
    }

    void setEnabled(bool enabled) {
        ensureNvsOpen();

        if (runtimeEnabled == enabled) {
            return;
        }

        runtimeEnabled = enabled;
        gpsPrefs.putBool(GPS_PREFS_KEY_ENABLED, enabled);
    }
}
