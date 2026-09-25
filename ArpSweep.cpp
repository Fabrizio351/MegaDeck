#include "ArpSweep.h"

#include <WiFi.h>

extern "C" {
    #include "lwip/etharp.h"
    #include "lwip/netif.h"
    #include "lwip/tcpip.h"
}
#include "esp_netif.h"
#include "esp_netif_net_stack.h"

namespace {
    struct netif* getStaNetif() {
        esp_netif_t* espNetif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (espNetif == nullptr) {
            return nullptr;
        }
        return static_cast<struct netif*>(esp_netif_get_netif_impl(espNetif));
    }
}

void ArpSweep::sendArpRequest(uint8_t lastOctet) {
    struct netif* n = getStaNetif();
    if (n == nullptr) {
        return;
    }

    ip4_addr_t target;
    IP4_ADDR(&target, baseOctet0_, baseOctet1_, baseOctet2_, lastOctet);

    // etharp_request tocca le strutture interne dello stack lwIP: va
    // eseguita con il lock del core lwIP, altrimenti puo' correre in
    // parallelo col task TCP/IP (es. mentre elabora una risposta ARP
    // reale) e corrompere la tabella ARP - causa di un crash e della
    // perdita del WiFi osservati in passato senza questo lock.
    LOCK_TCPIP_CORE();
    etharp_request(n, &target);
    UNLOCK_TCPIP_CORE();
}

bool ArpSweep::checkArpReply(uint8_t lastOctet, uint8_t macOut[6]) {
    struct netif* n = getStaNetif();
    if (n == nullptr) {
        return false;
    }

    ip4_addr_t target;
    IP4_ADDR(&target, baseOctet0_, baseOctet1_, baseOctet2_, lastOctet);

    struct eth_addr* ethRet = nullptr;
    const ip4_addr_t* ipRet = nullptr;

    LOCK_TCPIP_CORE();
    ssize_t idx = etharp_find_addr(n, &target, &ethRet, &ipRet);
    bool found = (idx >= 0 && ethRet != nullptr);
    if (found) {
        memcpy(macOut, ethRet->addr, 6);
    }
    UNLOCK_TCPIP_CORE();

    return found;
}

void ArpSweep::recordDevice(uint8_t lastOctet, const uint8_t mac[6]) {
    if (deviceCount_ >= MAX_DEVICES) {
        return;
    }

    char macStr[18];
    snprintf(
        macStr, sizeof(macStr),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );

    IPAddress ip(baseOctet0_, baseOctet1_, baseOctet2_, lastOctet);

    devices_[deviceCount_].ip = ip.toString();
    devices_[deviceCount_].mac = String(macStr);
    deviceCount_++;
}

void ArpSweep::advanceToNextHost() {
    currentHost_++;
    waitingReply_ = false;

    if (currentHost_ > 254) {
        scanning_ = false;
    }
}

void ArpSweep::start() {
    if (WiFi.status() != WL_CONNECTED) {
        scanning_ = false;
        return;
    }

    // Assume una sottorete /24 (maschera 255.255.255.0), la piu' comune
    // nelle reti domestiche/ufficio: scansiona gli host 1-254 dell'ultimo
    // ottetto. Reti con maschere diverse (raro in ambito domestico) non
    // vengono gestite per restare semplici.
    IPAddress localIp = WiFi.localIP();
    baseOctet0_ = localIp[0];
    baseOctet1_ = localIp[1];
    baseOctet2_ = localIp[2];
    ownLastOctet_ = localIp[3];

    deviceCount_ = 0;
    currentHost_ = 1;
    waitingReply_ = false;
    scanning_ = true;
}

bool ArpSweep::isScanning() const {
    return scanning_;
}

int ArpSweep::getProgressPercent() const {
    if (!scanning_) {
        return 100;
    }
    return (currentHost_ * 100) / 254;
}

int ArpSweep::getDeviceCount() const {
    return deviceCount_;
}

ArpSweep::Device ArpSweep::getDevice(int index) const {
    if (index < 0 || index >= deviceCount_) {
        return Device{ "", "" };
    }
    return devices_[index];
}

void ArpSweep::update() {
    if (!scanning_) {
        return;
    }

    // Se la connessione cade a meta' scansione, la fermiamo subito
    // invece di continuare a inviare richieste ARP su un'interfaccia non
    // piu' connessa: non serve a nulla e tiene occupata inutilmente la
    // rete/radio.
    if (WiFi.status() != WL_CONNECTED) {
        scanning_ = false;
        return;
    }

    // Il proprio host non serve arp-arlo: e' il T-Deck stesso.
    if (currentHost_ == ownLastOctet_) {
        advanceToNextHost();
        if (!scanning_) {
            return;
        }
    }

    uint32_t now = millis();

    if (!waitingReply_) {
        sendArpRequest(static_cast<uint8_t>(currentHost_));
        requestSentMs_ = now;
        waitingReply_ = true;
        return;
    }

    if (now - requestSentMs_ < ARP_WAIT_MS) {
        return;
    }

    uint8_t mac[6];
    if (checkArpReply(static_cast<uint8_t>(currentHost_), mac)) {
        recordDevice(static_cast<uint8_t>(currentHost_), mac);
    }

    advanceToNextHost();
}
