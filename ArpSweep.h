#pragma once

#include <Arduino.h>

// Motore di scansione ARP riutilizzabile: incapsula lo stesso identico
// meccanismo usato originariamente solo da NetScan (una richiesta ARP
// per ogni host 1-254 della sottorete /24 a cui si e' connessi via
// WiFi, con lock lwIP - vedi commenti in ArpSweep.cpp per il perche' del
// lock). Non e' un singleton: ogni "proprietario" (NetScan per la
// scansione manuale mostrata all'utente, NetGuard per il monitoraggio
// periodico in background) ne istanzia una copia propria con il proprio
// stato/buffer, cosi' le due scansioni possono coesistere senza
// interferire a vicenda.
class ArpSweep {
public:
    struct Device {
        String ip;
        String mac;
    };

    // Avvia una scansione asincrona (non blocca). Richiede WiFi connesso,
    // altrimenti non fa nulla (isScanning() resta/torna false).
    void start();

    bool isScanning() const;

    // 0-100 mentre isScanning() e' vera; non significativo altrimenti.
    int getProgressPercent() const;

    int getDeviceCount() const;
    Device getDevice(int index) const;

    // Da chiamare ad ogni ciclo (es. da App::update() o dalla UI che la
    // mostra): avanza la scansione di un host alla volta, senza mai
    // bloccare il loop principale.
    void update();

private:
    static constexpr int MAX_DEVICES = 60;
    static constexpr uint32_t ARP_WAIT_MS = 80;

    bool scanning_ = false;
    bool waitingReply_ = false;
    int currentHost_ = 1;      // ultimo ottetto, 1-254
    uint32_t requestSentMs_ = 0;

    uint8_t baseOctet0_ = 0;
    uint8_t baseOctet1_ = 0;
    uint8_t baseOctet2_ = 0;
    uint8_t ownLastOctet_ = 0;

    Device devices_[MAX_DEVICES];
    int deviceCount_ = 0;

    void sendArpRequest(uint8_t lastOctet);
    bool checkArpReply(uint8_t lastOctet, uint8_t macOut[6]);
    void recordDevice(uint8_t lastOctet, const uint8_t mac[6]);
    void advanceToNextHost();
};
