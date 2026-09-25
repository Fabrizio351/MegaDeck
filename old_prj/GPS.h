#pragma once

#include <Arduino.h>

namespace GPS {
    void begin();
    void update();  // da chiamare ad ogni loop, non bloccante

    bool hasFix();
    double getLatitude();
    double getLongitude();
    double getAltitudeMeters();
    double getSpeedKmh();
    int getSatellites();

    // "HH:MM:SS" UTC, stringa vuota se non ancora disponibile
    String getTimeUTC();
}
