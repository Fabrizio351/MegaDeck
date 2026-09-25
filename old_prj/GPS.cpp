#include "GPS.h"
#include "Config.h"

#include <TinyGPS.h>

namespace {
    TinyGPSPlus gpsParser;
    HardwareSerial gpsSerial(1);
}

namespace GPS {

    void begin() {
        // Se dopo qualche minuto non ottieni mai un fix, prova a cambiare
        // GPS_BAUD_RATE in Config.h (alcuni MIA-M10Q escono di fabbrica a 38400).
        gpsSerial.begin(GPS_BAUD_RATE, SERIAL_8N1, BOARD_GPS_RX_PIN, BOARD_GPS_TX_PIN);

        Serial.println("[GPS] avvio...");
    }

    void update() {
        while (gpsSerial.available() > 0) {
            gpsParser.encode(gpsSerial.read());
        }
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

    String getTimeUTC() {
        if (!gpsParser.time.isValid()) {
            return "";
        }

        char buffer[9];
        snprintf(
            buffer,
            sizeof(buffer),
            "%02d:%02d:%02d",
            gpsParser.time.hour(),
            gpsParser.time.minute(),
            gpsParser.time.second()
        );

        return String(buffer);
    }
}
