#include "ConnStatus.h"
#include "BleGatt.h"
#include "MeshtasticUI.h"
#include "NetGuard.h"
#include "VoipUI.h"
#include "SipClient.h"

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
        return BleGatt::isReady() && BleGatt::isEnabled();
    }

    bool isLoraActive() {
        return MeshtasticUI::isListening();
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
        return MeshtasticUI::hasUnread();
    }

    bool hasNetGuardPendingAlert() {
        return NetGuard::isEnabled() && NetGuard::hasPendingAlert();
    }

    bool hasNetGuardActiveIntruder() {
        return NetGuard::isEnabled() && NetGuard::hasActiveIntruder();
    }

    bool isVoipActive() {
        return VoipUI::isListening();
    }

    bool isVoipCallActive() {
        return SipClient::getCallState() != SipClient::CallState::Idle;
    }
}
