#include "NetScan.h"

namespace {
    ArpSweep engine;
}

namespace NetScan {

    void start() {
        engine.start();
    }

    bool isScanning() {
        return engine.isScanning();
    }

    int getProgressPercent() {
        return engine.getProgressPercent();
    }

    int getDeviceCount() {
        return engine.getDeviceCount();
    }

    Device getDevice(int index) {
        return engine.getDevice(index);
    }

    void update() {
        engine.update();
    }
}
