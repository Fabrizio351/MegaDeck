#include "Sound.h"
#include "Config.h"

#include <Arduino.h>
#include <ESP_I2S.h>
#include <Preferences.h>

namespace {
    // Pin I2S dell'altoparlante integrato - ora in Config.h (BOARD_I2S_*,
    // spostati il 28/09) perche' condivisi anche con WebRadioUI.cpp.
    constexpr int8_t I2S_BCLK_PIN = BOARD_I2S_BCLK;
    constexpr int8_t I2S_WS_PIN = BOARD_I2S_WS;
    constexpr int8_t I2S_DOUT_PIN = BOARD_I2S_DOUT;

    constexpr uint32_t SAMPLE_RATE = 16000;
    constexpr int16_t MAX_AMPLITUDE = 6000;

    I2SClass i2s;
    bool ready = false;

    Preferences soundPrefs;
    constexpr const char* SOUND_PREFS_NAMESPACE = "tdecksound";
    constexpr const char* SOUND_PREFS_KEY_VOLUME = "volume";
    bool nvsOpened = false;
    int volumePercent = 100;

    void ensureNvsOpen() {
        if (nvsOpened) {
            return;
        }

        soundPrefs.begin(SOUND_PREFS_NAMESPACE, false);
        volumePercent = soundPrefs.getInt(SOUND_PREFS_KEY_VOLUME, 100);
        nvsOpened = true;
    }

    // Onda quadra generata a mano (stesso approccio dell'esempio
    // ufficiale ESP_I2S/Simple_tone): niente file audio da incorporare
    // in flash, solo qualche centinaio di campioni per un bip breve.
    void playTone(int frequencyHz, int durationMs) {
        ensureNvsOpen();

        if (!ready || frequencyHz <= 0 || volumePercent <= 0) {
            return;
        }

        int halfWavelength = SAMPLE_RATE / frequencyHz / 2;
        if (halfWavelength <= 0) {
            return;
        }

        int16_t amplitude = static_cast<int16_t>((MAX_AMPLITUDE * volumePercent) / 100);

        int totalSamples = (SAMPLE_RATE * durationMs) / 1000;
        int16_t sample = amplitude;

        for (int i = 0; i < totalSamples; i++) {
            if (i % halfWavelength == 0) {
                sample = static_cast<int16_t>(-sample);
            }

            int16_t left = sample;
            int16_t right = sample;
            i2s.write(&left, sizeof(left));
            i2s.write(&right, sizeof(right));
        }
    }
}

namespace Sound {

    void begin() {
        ensureNvsOpen();

        i2s.setPins(I2S_BCLK_PIN, I2S_WS_PIN, I2S_DOUT_PIN);
        ready = i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);

        if (!ready) {
            DEBUG_PRINTLN("[SOUND] Inizializzazione I2S fallita, suoni disattivati");
        }
    }

    int getVolumePercent() {
        ensureNvsOpen();
        return volumePercent;
    }

    void setVolumePercent(int percent) {
        ensureNvsOpen();

        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;

        volumePercent = percent;
        soundPrefs.putInt(SOUND_PREFS_KEY_VOLUME, percent);
    }

    void pauseForExternalUse() {
        if (!ready) {
            return; // gia' non inizializzato, niente da rilasciare
        }
        i2s.end();
        ready = false;
    }

    void resumeForOwnUse() {
        i2s.setPins(I2S_BCLK_PIN, I2S_WS_PIN, I2S_DOUT_PIN);
        ready = i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        if (!ready) {
            DEBUG_PRINTLN("[SOUND] resumeForOwnUse: reinizializzazione I2S fallita");
        }
    }

    void playRawSamples(const int16_t* samples, size_t sampleCount) {
        if (!ready || samples == nullptr) {
            return;
        }
        for (size_t i = 0; i < sampleCount; i++) {
            int16_t left = samples[i];
            int16_t right = samples[i];
            i2s.write(&left, sizeof(left));
            i2s.write(&right, sizeof(right));
        }
    }

    // Ogni suono qui sotto e' pensato per essere riconoscibile dal solo
    // RITMO (numero di note, pause si'/no, sali/scendi/alterna), non
    // solo dall'altezza: due suoni con lo stesso "disegno" (es. due note
    // che scendono) si confondevano facilmente a orecchio anche con
    // tonalita' diverse. Riepilogo dei disegni usati, tutti diversi:
    //   WiFi connesso     -> 2 note che salgono, senza pausa
    //   WiFi disconnesso  -> 1 sola nota bassa e lunga
    //   LoRa ricevuto     -> 2 bip acuti IDENTICI con una pausa vera
    //   BLE GATT connesso    -> 2 note che salgono, CON pausa (vs WiFi: stesse note, niente pausa)
    //   BLE GATT disconnesso -> 2 note che scendono, CON pausa
    //   Blocco schermo    -> 3 note a scaletta che SCENDONO
    //   Sblocco schermo   -> 3 note a scaletta che SALGONO
    //   Allarme NetGuard  -> 4 note che ALTERNANO tra due toni (sirena)

    void playWifiConnected() {
        playTone(1200, 70);
        playTone(1800, 70);
    }

    void playWifiDisconnected() {
        playTone(450, 220);
    }

    void playLoRaMessageReceived() {
        playTone(1900, 50);
        delay(40);
        playTone(1900, 50);
    }

    void playBleGattConnected() {
        playTone(900, 60);
        delay(30);
        playTone(1400, 60);
    }

    void playBleGattDisconnected() {
        playTone(1400, 60);
        delay(30);
        playTone(900, 60);
    }

    void playScreenLocked() {
        playTone(1600, 55);
        playTone(1200, 55);
        playTone(800, 55);
    }

    void playScreenUnlocked() {
        playTone(800, 55);
        playTone(1200, 55);
        playTone(1600, 55);
    }

    void playNetGuardAlert() {
        for (int i = 0; i < 2; i++) {
            playTone(2200, 90);
            playTone(1500, 90);
        }
    }
}
