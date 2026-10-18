#include "Board.h"
#include "Config.h"

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>

namespace {
    // Host HSPI dedicato al bus condiviso display + LoRa + SD.
    SPIClass sharedSPI(HSPI);

    constexpr int BACKLIGHT_PWM_FREQ_HZ = 5000;
    constexpr int BACKLIGHT_PWM_RESOLUTION_BITS = 8; // duty 0-255
    constexpr int BACKLIGHT_DEFAULT_PERCENT = 80;
    constexpr int KEYBOARD_BACKLIGHT_DEFAULT_PERCENT = 50;

    // NVS (memoria flash non volatile): le due luminosita' sopravvivono
    // al riavvio invece di ripartire sempre dal default.
    Preferences prefs;
    constexpr const char* PREFS_NAMESPACE = "tdeckfw";
    constexpr const char* PREFS_KEY_SCREEN_BRIGHT = "scr_bright";
    constexpr const char* PREFS_KEY_KEYBOARD_BRIGHT = "kbd_bright";

    int backlightPercent = BACKLIGHT_DEFAULT_PERCENT;
    int keyboardBacklightPercent = KEYBOARD_BACKLIGHT_DEFAULT_PERCENT;

    // Registro/comando non ufficiale, vedi nota in Board.h.
    constexpr uint8_t KEYBOARD_BACKLIGHT_REGISTER = 0x01;

    void sendKeyboardBacklightCommand(int percent) {
        uint8_t value = static_cast<uint8_t>((percent * 255) / 100);

        Wire.beginTransmission(BOARD_KEYBOARD_ADDR);
        Wire.write(KEYBOARD_BACKLIGHT_REGISTER);
        Wire.write(value);
        Wire.endTransmission();
    }
}

namespace Board {

    void begin() {
        Serial.begin(115200);
        delay(300);

        // Alimentazione periferiche: va portata HIGH prima di init
        // qualsiasi bus verso tastiera, radio, GPS, display.
        pinMode(BOARD_POWERON, OUTPUT);
        digitalWrite(BOARD_POWERON, HIGH);
        delay(100);

        prefs.begin(PREFS_NAMESPACE, false);
        backlightPercent = prefs.getInt(PREFS_KEY_SCREEN_BRIGHT, BACKLIGHT_DEFAULT_PERCENT);
        keyboardBacklightPercent = prefs.getInt(PREFS_KEY_KEYBOARD_BRIGHT, KEYBOARD_BACKLIGHT_DEFAULT_PERCENT);

        // Diagnostica temporanea: capire se la luminosita' schermo salvata
        // in NVS da un avvio precedente e' rimasta bloccata a un valore
        // troppo basso (illeggibile) - senza poter vedere lo schermo per
        // aprire le Impostazioni e alzarla, l'utente resterebbe bloccato
        // per sempre in quello stato.
        DEBUG_PRINTF(
            "[BOARD] luminosita' caricata da NVS: schermo=%d%% tastiera=%d%%\n",
            backlightPercent, keyboardBacklightPercent
        );

        // Un valore <10% e' sotto la soglia minima che l'interfaccia
        // Impostazioni permette di impostare volontariamente (vedi
        // SCREEN_BRIGHTNESS_MIN in SettingsUI.cpp) - se lo troviamo qui
        // comunque (NVS scritta da fuori quel percorso, o da una versione
        // precedente del firmware senza quel limite) lo correggiamo prima
        // di applicarlo, altrimenti il dispositivo riparte sempre con lo
        // schermo illeggibile e non c'e' modo di raggiungere Impostazioni
        // per sistemarlo senza prima vedere qualcosa.
        if (backlightPercent < 10) {
            DEBUG_PRINTF(
                "[BOARD] luminosita' schermo troppo bassa (%d%%), corretta a 80%%\n",
                backlightPercent
            );
            backlightPercent = 80;
        }

        // Retroilluminazione via PWM (invece di semplice on/off): permette
        // di regolare la luminosita' in modo graduale da Impostazioni.
        // Va accesa qui, prima di Display::begin(), cosi' il pannello non
        // resta al buio durante l'inizializzazione del controller ST7789.
        ledcAttach(BOARD_TFT_BACKLIGHT, BACKLIGHT_PWM_FREQ_HZ, BACKLIGHT_PWM_RESOLUTION_BITS);
        setBacklightPercent(backlightPercent);

        // I2C: unico punto di inizializzazione (tastiera + eventuale IO expander).
        Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
        Wire.setClock(400000);

        // La retroilluminazione tastiera richiede il bus I2C gia' attivo
        // (vedi Wire.begin() appena sopra), quindi va applicata solo qui.
        sendKeyboardBacklightCommand(keyboardBacklightPercent);

        // Deseleziona esplicitamente gli altri device sul bus condiviso
        // prima di qualunque comunicazione SPI, altrimenti possono interferire.
        pinMode(BOARD_SDCARD_CS, OUTPUT);
        digitalWrite(BOARD_SDCARD_CS, HIGH);

        pinMode(RADIO_CS_PIN, OUTPUT);
        digitalWrite(RADIO_CS_PIN, HIGH);

        // NON attivato qui apposta (26/09): un tentativo di farlo
        // partire ad OGNI avvio, prima di qualunque stampa seriale, ha
        // prodotto un blocco totale silenzioso (nessun log, schermo nero)
        // riproducibile anche a SD rimossa e flash appena cancellata -
        // vedi project_wifi_connect_freeze_and_vpn_bug. VpnConfig.cpp usa
        // Board::getSharedSPI() ma ne chiama begin() lui stesso, solo
        // quando serve davvero (al primo import da SD), non qui.
        //sharedSPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI, -1);

        DEBUG_PRINTLN();
        DEBUG_PRINTLN("================================");
        DEBUG_PRINTLN(APP_NAME);
        DEBUG_PRINT("Versione: ");
        DEBUG_PRINTLN(APP_VERSION);
        DEBUG_PRINTLN("================================");
    }

    void update() {
        // Riservato ad aggiornamenti hardware futuri
    }

    void restart() {
        ESP.restart();
    }

    void setBacklightPercent(int percent) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;

        backlightPercent = percent;

        uint32_t duty = (static_cast<uint32_t>(percent) * 255) / 100;
        ledcWrite(BOARD_TFT_BACKLIGHT, duty);

        prefs.putInt(PREFS_KEY_SCREEN_BRIGHT, percent);
    }

    int getBacklightPercent() {
        return backlightPercent;
    }

    void setBacklightPercentTransient(int percent) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;

        uint32_t duty = (static_cast<uint32_t>(percent) * 255) / 100;
        ledcWrite(BOARD_TFT_BACKLIGHT, duty);
    }

    void setKeyboardBacklightPercent(int percent) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;

        keyboardBacklightPercent = percent;
        sendKeyboardBacklightCommand(percent);

        prefs.putInt(PREFS_KEY_KEYBOARD_BRIGHT, percent);
    }

    void setKeyboardBacklightPercentTransient(int percent) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;

        sendKeyboardBacklightCommand(percent);
    }

    int getKeyboardBacklightPercent() {
        return keyboardBacklightPercent;
    }

    SPIClass& getSharedSPI() {
        return sharedSPI;
    }

    // Fattorizzata (04/10, seconda tornata di calibrazione - vedi
    // getBatteryCellVolts() in Board.h) - stessa media di 16 campioni
    // gia' usata da getBatteryPercent(), condivisa anche dalla
    // diagnostica temporanea cosi' non si duplica la logica di lettura.
    float sampleBatteryCellVolts() {
        // Una singola lettura ADC balla di qualche punto percentuale da
        // una chiamata all'altra per puro rumore elettrico, facendo
        // "ballare" la cifra in alto a destra. Media di piu' campioni
        // presi in rapida sequenza per stabilizzare il valore alla fonte,
        // invece di limitarsi ad aggiornare la scritta piu' di rado.
        constexpr int SAMPLES = 16;
        uint32_t sumMillivolts = 0;

        for (int i = 0; i < SAMPLES; i++) {
            sumMillivolts += analogReadMilliVolts(BOARD_BAT_ADC);
        }

        // analogReadMilliVolts() compensa in hardware la non linearita'
        // dell'ADC dell'ESP32 (molto piu' affidabile del semplice
        // analogRead() scalato a mano).
        float pinMillivolts = static_cast<float>(sumMillivolts) / SAMPLES;
        return (pinMillivolts / 1000.0f) * BATTERY_ADC_DIVIDER;
    }

    int getBatteryPercent() {
        float cellVolts = sampleBatteryCellVolts();

        float percent = (cellVolts - BATTERY_VOLT_EMPTY)
            / (BATTERY_VOLT_FULL - BATTERY_VOLT_EMPTY) * 100.0f;

        if (percent < 0.0f) percent = 0.0f;
        if (percent > 100.0f) percent = 100.0f;

        return static_cast<int>(percent + 0.5f);
    }

    float getBatteryCellVolts() {
        return sampleBatteryCellVolts();
    }
}
