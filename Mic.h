#pragma once

#include <Arduino.h>

// Microfono integrato (29/09) - verifica hardware iniziale, NON ancora
// la vera dettatura: serve solo a confermare che il microfono cattura
// audio vero, primo passo condiviso da dettatura OpenClaw e VoIP (vedi
// project_mic_pinout_researched in memoria). Usa il chip codec ES7210
// (I2C) + un canale I2S dedicato in RICEZIONE su I2S_NUM_1 - bus fisico
// diverso da quello dell'altoparlante (I2S_NUM_0/AUTO in Sound.cpp),
// quindi puo' girare senza il pausa/ripresa che serve invece per
// condividere l'altoparlante con Web Radio.
namespace Mic {

    // Inizializza il codec ES7210 + il canale I2S RX. Va richiamato una
    // sola volta. Ritorna false se l'inizializzazione fallisce (es. il
    // codec non risponde su I2C).
    bool begin();

    // Cattura BLOCCANTE per 'seconds' secondi a 16kHz mono 16 bit,
    // dentro il buffer fornito (deve contenere almeno seconds*16000
    // campioni, es. 3s -> 48000 int16_t -> 96000 byte). Ritorna il
    // numero di campioni realmente catturati (puo' essere meno del
    // richiesto se il buffer e' piu' piccolo o la lettura I2S va in
    // timeout).
    size_t captureSeconds(int16_t* buffer, size_t bufferCapacitySamples, int seconds);

    // Lettura "a pezzi" per lo streaming (08/10, per VoIP) - riusa lo
    // stesso canale I2S RX gia' inizializzato da begin(), ma legge un
    // singolo blocco di 'samples' campioni invece di un ciclo lungo
    // secondi: pensata per essere richiamata a cadenza fissa (20ms) dal
    // task audio di RtpAudio.cpp. Ritorna il numero di campioni letti
    // davvero (0 se non pronto o timeout).
    size_t readChunk(int16_t* buffer, size_t samples, uint32_t timeoutMs);
}
