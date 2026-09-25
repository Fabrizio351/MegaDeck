#include "ConnStatus.h"
#include "BleChat.h"
#include "LoRaRadio.h"
#include "LoRaChatUI.h"
#include "NetGuard.h"

#include <WiFi.h>

namespace {
    bool vpnConnected = false;
}

namespace ConnStatus {

    bool isWifiConnected() {
        return WiFi.status() == WL_CONNECTED;
    }

    bool isVpnConnected() {
        return vpnConnected;
    }

    void setVpnConnected(bool connected) {
        vpnConnected = connected;
    }

    bool isBluetoothActive() {
        return BleChat::isReady() && BleChat::isEnabled();
    }

    bool isLoraActive() {
        return LoRaRadio::isReady() && LoRaRadio::isEnabled();
    }

    bool isLoraTransmitting() {
        return LoRaRadio::isReady() && LoRaRadio::isTransmitting();
    }

    bool isNetGuardActive() {
        return NetGuard::isEnabled();
    }

    bool isNetGuardWaitingForWhitelist() {
        return NetGuard::isWaitingForWhitelist();
    }

    bool isNetGuardScanning() {
        return NetGuard::isScanning();
    }

    bool hasUnreadLoraMessages() {
        return LoRaChatUI::hasUnreadMessages();
    }

    bool hasNetGuardPendingAlert() {
        return NetGuard::isEnabled() && NetGuard::hasPendingAlert();
    }
}
