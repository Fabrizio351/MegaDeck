#include "Input.h"
#include "Config.h"

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Driver touch: SensorLib (lewisxhe), non piu' TouchLib.
// Installato in Documenti/Arduino/libraries/SensorLib. Questo header e'
// segnato come "deprecato" (redirige a touch/TouchDrvGT911.hpp), ma nella
// tua installazione e' l'unico percorso che il preprocessore risolve
// correttamente -> lo usiamo cosi', l'avviso e' innocuo e non blocca la
// compilazione.
#include "TouchDrvGT911.hpp"

// Se il tap cade nel punto sbagliato dello schermo, il problema e' quasi
// sempre l'orientamento: il GT911 da' coordinate nel sistema NATIVO del
// pannello (240x320 verticale), ma lo schermo e' ruotato in landscape
// (setRotation(1) -> 320x240). Prova a invertire questi due flag finche'
// il tap non cade nel punto giusto (guarda i valori stampati su Serial).
#define TOUCH_SWAP_XY   1   // scambia X e Y raw
#define TOUCH_INVERT_X  0   // inverte X dopo l'eventuale swap
#define TOUCH_INVERT_Y  1   // inverte Y dopo l'eventuale swap

// Risoluzione nativa del pannello (PRIMA della rotazione) -> serve per
// invertire correttamente gli assi. Deve combaciare con le dimensioni
// passate al costruttore di Arduino_ST7789 in Display.cpp (240, 320).
#define TOUCH_PANEL_W 240
#define TOUCH_PANEL_H 320

namespace {
    // Il trackball di questo T-Deck NON si comporta come due encoder
    // X/Y in quadratura: ogni pin G01-G04 e' un sensore indipendente
    // legato a UNA direzione cardinale precisa. Mapping dedotto dai
    // test fisici: G01=destra, G02=su. G03/G04 da confermare.
    volatile int cntRight = 0;
    volatile int cntUp = 0;
    volatile int cntLeft = 0;
    volatile int cntDown = 0;

    InputEvent currentEvent = InputEvent::None;
    char currentKey = 0;

    bool previousButtonState = HIGH;
    bool buttonSelectFired = false;
    unsigned long lastInputTime = 0;
    unsigned long lastMovementTime = 0;

    // --- Touch GT911 (SensorLib) ---
    // Nessun pin RST/INT dedicato sul T-Deck Plus -> polling puro via I2C,
    // setPins(-1, -1). L'indirizzo (0x5D o 0x14) va scoperto a runtime
    // come prima, perche' dipende dallo stato del pin INT del GT911
    // durante il power-on, che non controlliamo via software.
    TouchDrvGT911 touch;
    bool touchAvailable = false;
    uint8_t touchAddr = 0; // salvato per poter ricreare il driver dopo un blocco

    bool previousTouchState = false;
    int16_t touchX = 0;
    int16_t touchY = 0;

    // Debounce del tap: richiede letture "touched" stabili consecutive
    // prima di considerare il tocco reale, altrimenti il rumore sul bus
    // I2C condiviso con la tastiera genera falsi tap.
    constexpr uint8_t TOUCH_STABLE_READS = 3;
    uint8_t touchStableCount = 0;
    bool touchConfirmed = false;
    bool tapFlag = false;

    // --- Task dedicato al touch, isolato dal loop principale ---
    // Rete di sicurezza aggiuntiva: anche con un driver piu' affidabile,
    // se per qualsiasi motivo (bus I2C, alimentazione, bug residuo)
    // touch.getTouchPoints() dovesse bloccarsi, isolarlo in un task
    // separato impedisce che tastiera/trackball/display si congelino.
    // Il "guardiano" in Input::update() rileva il blocco e ricrea il
    // driver da zero.
    TaskHandle_t touchTaskHandle = nullptr;
    portMUX_TYPE touchMux = portMUX_INITIALIZER_UNLOCKED;

    volatile bool sharedTouched = false;
    volatile int16_t sharedRawX = 0;
    volatile int16_t sharedRawY = 0;
    volatile uint32_t touchReadStartMs = 0;
    volatile bool touchReadInProgress = false;

    constexpr uint32_t TOUCH_TASK_TIMEOUT_MS = 300;
    constexpr uint32_t TOUCH_TASK_POLL_MS = 15;

    void IRAM_ATTR trackballG01() {
        //cntRight++;
        cntUp++;
    }

    void IRAM_ATTR trackballG02() {
        cntRight++;
    }

    void IRAM_ATTR trackballG03() {
        //cntLeft++;
        cntDown++;
    }

    void IRAM_ATTR trackballG04() {
        cntLeft++;
    }

    void translateKey(char key) {
        currentKey = key;

        switch (key) {
            case 'w':
            case 'W':
                currentEvent = InputEvent::Up;
                break;

            case 's':
            case 'S':
                currentEvent = InputEvent::Down;
                break;

            case 'a':
            case 'A':
                currentEvent = InputEvent::Left;
                break;

            case 'd':
            case 'D':
                currentEvent = InputEvent::Right;
                break;

            case '\n':
            case '\r':
            case 'e':
            case 'E':
                currentEvent = InputEvent::Select;
                break;

            case 'b':
            case 'B':
                currentEvent = InputEvent::Back;
                break;

            default:
                currentEvent = InputEvent::Key;
                break;
        }
    }

    // Ritorna l'indirizzo I2C su cui risponde il GT911 (0x5D o 0x14),
    // 0 se non risponde a nessuno dei due.
    uint8_t detectTouchAddr() {
        const uint8_t candidates[2] = { 0x5D, 0x14 };

        for (uint8_t addr : candidates) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                return addr;
            }
        }

        return 0;
    }

    // Applica swap/invert per mappare le coordinate raw del pannello
    // (native, verticale) allo spazio dello schermo ruotato (landscape).
    void applyTouchRotation(int16_t rawX, int16_t rawY, int16_t &outX, int16_t &outY) {
        int16_t x = rawX;
        int16_t y = rawY;

#if TOUCH_SWAP_XY
        int16_t tmp = x;
        x = y;
        y = tmp;
#endif

#if TOUCH_INVERT_X
        x = TOUCH_PANEL_H - x;   // dopo lo swap l'asse X copre la dimensione lunga del pannello
#endif

#if TOUCH_INVERT_Y
        y = TOUCH_PANEL_W - y;   // dopo lo swap l'asse Y copre la dimensione corta del pannello
#endif

        outX = x;
        outY = y;
    }

    // Inizializza (o re-inizializza) il driver touch SensorLib.
    // Ritorna true se il touch e' stato trovato e inizializzato.
    bool initTouchDriver() {
        touch.setPins(-1, -1); // nessun pin RST/INT dedicato sul T-Deck Plus
        return touch.begin(Wire, touchAddr, BOARD_I2C_SDA, BOARD_I2C_SCL);
    }

    // Corpo del task dedicato: chiama touch.getTouchPoints() in loop,
    // isolato dal resto dell'app. Se questa chiamata si blocca, si blocca
    // solo questo task (gira su core 0, separato dal loop() Arduino su core 1).
    void touchTaskFunc(void*) {
        for (;;) {
            portENTER_CRITICAL(&touchMux);
            touchReadStartMs = millis();
            touchReadInProgress = true;
            portEXIT_CRITICAL(&touchMux);

            TouchPoints points = touch.getTouchPoints();
            bool touched = points.hasPoints();
            int16_t rx = 0;
            int16_t ry = 0;

            if (touched) {
                const TouchPoint &p = points.getPoint(0);
                rx = p.x;
                ry = p.y;
            }

            portENTER_CRITICAL(&touchMux);
            touchReadInProgress = false;
            sharedTouched = touched;
            if (touched) {
                sharedRawX = rx;
                sharedRawY = ry;
            }
            portEXIT_CRITICAL(&touchMux);

            vTaskDelay(pdMS_TO_TICKS(TOUCH_TASK_POLL_MS));
        }
    }

    void startTouchTask() {
        if (touchTaskHandle != nullptr) {
            return;
        }

        xTaskCreatePinnedToCore(
            touchTaskFunc,
            "touchTask",
            4096,
            nullptr,
            1,   // priorita' bassa: non deve rubare CPU al loop principale
            &touchTaskHandle,
            0    // core 0 (il loop() Arduino gira di default su core 1)
        );
    }

    // Chiamata dal "guardiano" quando il task touch resta bloccato oltre
    // TOUCH_TASK_TIMEOUT_MS: il task viene ucciso (vTaskDelete, gia' fatto
    // dal chiamante) e qui ricreiamo I2C e il driver touch da zero.
    void recreateTouchDriver() {
        Serial.println("[INPUT] touch bloccato oltre il timeout, riavvio driver touch...");

        Wire.end();
        delay(5);
        Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
        Wire.setTimeOut(50);

        touchAvailable = initTouchDriver();

        portENTER_CRITICAL(&touchMux);
        touchReadInProgress = false;
        sharedTouched = false;
        portEXIT_CRITICAL(&touchMux);

        if (!touchAvailable) {
            Serial.println("[INPUT] riavvio driver touch fallito");
        }
    }
}

namespace Input {

    void begin() {
        currentEvent = InputEvent::None;
        currentKey = 0;

        // Alimentazione e I2C sono gia' inizializzati da Board::begin(),
        // chiamato prima di Input::begin() (vedi App::begin()).

        // Interrupt tastiera
        pinMode(BOARD_KEYBOARD_INT, INPUT_PULLUP);

        // Trackball: segnali open-drain attivi-bassi -> pull-up interno
        // obbligatorio, altrimenti la linea fluttua e genera rumore.
        pinMode(BOARD_TBOX_G01, INPUT_PULLUP);
        pinMode(BOARD_TBOX_G02, INPUT_PULLUP);
        pinMode(BOARD_TBOX_G03, INPUT_PULLUP);
        pinMode(BOARD_TBOX_G04, INPUT_PULLUP);

        // FALLING invece di CHANGE: ogni impulso genera un solo fronte
        // di discesa. Con CHANGE ogni click veniva contato due volte
        // (salita + discesa), causando movimento a scatti/impreciso.
        attachInterrupt(
            digitalPinToInterrupt(BOARD_TBOX_G01),
            trackballG01,
            FALLING
        );

        attachInterrupt(
            digitalPinToInterrupt(BOARD_TBOX_G02),
            trackballG02,
            FALLING
        );

        attachInterrupt(
            digitalPinToInterrupt(BOARD_TBOX_G03),
            trackballG03,
            FALLING
        );

        attachInterrupt(
            digitalPinToInterrupt(BOARD_TBOX_G04),
            trackballG04,
            FALLING
        );

        // Pulsante centrale trackball
        pinMode(BOARD_TRACKBALL_BTN, INPUT_PULLUP);
        previousButtonState = digitalRead(BOARD_TRACKBALL_BTN);

        // Touch GT911: condivide il bus I2C gia' inizializzato da
        // Board::begin() (stesso bus della tastiera).
        Wire.setTimeOut(50); // ms

        touchAddr = detectTouchAddr();

        if (touchAddr != 0) {
            touchAvailable = initTouchDriver();

            if (touchAvailable) {
                Serial.print("[INPUT] touch GT911 (SensorLib) trovato a 0x");
                Serial.println(touchAddr, HEX);

                // La lettura vera e propria gira in un task separato,
                // cosi' un eventuale blocco non congela il resto del
                // dispositivo (vedi touchTaskFunc sopra).
                startTouchTask();
            } else {
                Serial.println("[INPUT] touch GT911 rilevato su I2C ma begin() fallita");
            }
        } else {
            Serial.println("[INPUT] touch GT911 non risponde su I2C (0x5D/0x14)");
        }
    }

    void update() {
        currentEvent = InputEvent::None;
        currentKey = 0;
        tapFlag = false;

        if (millis() - lastInputTime < 20) {
            return;
        }

        lastInputTime = millis();

        // Test da Monitor Seriale
        if (Serial.available()) {
            char key = static_cast<char>(Serial.read());
            translateKey(key);
            return;
        }

        // Tastiera fisica
        if (digitalRead(BOARD_KEYBOARD_INT) == LOW) {
            Wire.requestFrom(
                static_cast<uint8_t>(BOARD_KEYBOARD_ADDR),
                static_cast<uint8_t>(1)
            );

            if (Wire.available()) {
                char key = static_cast<char>(Wire.read());

                if (key != 0) {
                    translateKey(key);
                    return;
                }
            }
        }

        // Movimento trackball (4 sensori cardinali indipendenti)
        if (cntRight > 0) {
            cntRight = 0;
            lastMovementTime = millis();
            currentEvent = InputEvent::Right;
            return;
        }

        if (cntLeft > 0) {
            cntLeft = 0;
            lastMovementTime = millis();
            currentEvent = InputEvent::Left;
            return;
        }

        if (cntDown > 0) {
            cntDown = 0;
            lastMovementTime = millis();
            currentEvent = InputEvent::Down;
            return;
        }

        if (cntUp > 0) {
            cntUp = 0;
            lastMovementTime = millis();
            currentEvent = InputEvent::Up;
            return;
        }

        // Pressione centrale della trackball, con debounce a 2 letture
        // LOW consecutive e una finestra di silenzio di 150ms dopo
        // l'ultimo movimento: lo scroll veloce genera rumore elettrico
        // che puo' far leggere falsi LOW sul pin del pulsante, dato che
        // sono sullo stesso modulo fisico.
        bool buttonState = digitalRead(BOARD_TRACKBALL_BTN);
        bool movementCooldownActive = (millis() - lastMovementTime) < 150;

        if (buttonState == LOW && !movementCooldownActive) {
            if (previousButtonState == LOW && !buttonSelectFired) {
                buttonSelectFired = true;
                currentEvent = InputEvent::Select;
                previousButtonState = buttonState;
                return;
            }
        } else if (buttonState == HIGH) {
            buttonSelectFired = false;
        }

        previousButtonState = buttonState;

        // Touch GT911: la lettura vera avviene nel task dedicato
        // (touchTaskFunc). Qui: (1) controlliamo il "guardiano" -> se il
        // task e' fermo in lettura da troppo tempo, lo uccidiamo e
        // ricreiamo il driver; (2) altrimenti leggiamo lo stato condiviso
        // e applichiamo lo stesso debounce/tap di prima.
        if (touchAvailable) {
            bool inProgress;
            uint32_t startedAt;

            portENTER_CRITICAL(&touchMux);
            inProgress = touchReadInProgress;
            startedAt = touchReadStartMs;
            portEXIT_CRITICAL(&touchMux);

            if (inProgress && (millis() - startedAt > TOUCH_TASK_TIMEOUT_MS)) {
                if (touchTaskHandle != nullptr) {
                    vTaskDelete(touchTaskHandle);
                    touchTaskHandle = nullptr;
                }

                recreateTouchDriver();

                if (touchAvailable) {
                    startTouchTask();
                }
            } else {
                bool touched;
                int16_t rawX;
                int16_t rawY;

                portENTER_CRITICAL(&touchMux);
                touched = sharedTouched;
                rawX = sharedRawX;
                rawY = sharedRawY;
                portEXIT_CRITICAL(&touchMux);

                if (touched) {
                    applyTouchRotation(rawX, rawY, touchX, touchY);

                    if (touchStableCount < TOUCH_STABLE_READS) {
                        touchStableCount++;
                    }

                    if (touchStableCount >= TOUCH_STABLE_READS) {
                        touchConfirmed = true;
                    }

                    // Decommenta per calibrare l'orientamento (vedi flag
                    // TOUCH_SWAP_XY / TOUCH_INVERT_X / TOUCH_INVERT_Y sopra):
                    // Serial.printf("[TOUCH] raw=(%d,%d) mappato=(%d,%d)\n", rawX, rawY, touchX, touchY);
                } else {
                    if (touchConfirmed) {
                        tapFlag = true;
                    }

                    touchStableCount = 0;
                    touchConfirmed = false;
                }

                previousTouchState = touched;
            }
        }
    }

    InputEvent getEvent() {
        return currentEvent;
    }

    char getKey() {
        return currentKey;
    }

    bool available() {
        return currentEvent != InputEvent::None;
    }

    bool isTouched() {
        return previousTouchState;
    }

    int16_t getTouchX() {
        return touchX;
    }

    int16_t getTouchY() {
        return touchY;
    }

    bool wasTapped() {
        return tapFlag;
    }
}
