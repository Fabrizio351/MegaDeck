#include "Input.h"
#include "Config.h"

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Driver touch: SensorLib (lewisxhe), non piu' TouchLib.
#include "TouchDrvGT911.hpp"

#define TOUCH_SWAP_XY   1
#define TOUCH_INVERT_X  0
#define TOUCH_INVERT_Y  1

#define TOUCH_PANEL_W 240
#define TOUCH_PANEL_H 320

namespace {
    // --- Trackball: 4 sensori cardinali indipendenti (vedi note originali) ---
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
    TouchDrvGT911 touch;
    bool touchAvailable = false;
    uint8_t touchAddr = 0;

    // Risoluzione nativa del pannello riportata dal chip stesso durante
    // l'init (registri X/Y_RESOLUTION). NON coincide necessariamente con
    // la risoluzione in pixel del pannello LCD (240x320): i moduli GT911
    // spesso riportano una risoluzione di calibrazione diversa. Usare
    // TOUCH_PANEL_W/H come se fossero gia' il range corretto (come si
    // faceva prima) produce coordinate "plausibili" ma abbastanza
    // imprecise da mancare sistematicamente i bottoni. Se il chip non
    // riporta nulla (0), si ricade sulle costanti come approssimazione.
    uint16_t touchResX = 0;
    uint16_t touchResY = 0;

    int16_t touchX = 0;
    int16_t touchY = 0;
    bool touchConfirmed = false;
    uint32_t lastTouchSeenMs = 0;

    // Il task dedicato pulisce il registro del GT911 ad ogni ciclo
    // (clearBuffer()): puo' quindi capitare che, a dito fermo, una singola
    // lettura risulti "non toccato" per un istante prima che il chip
    // ri-latchi i dati. Un debounce a N letture CONSECUTIVE positive prima
    // di iniziare il press (come si faceva prima) puo' non arrivare mai a
    // convalidarsi se questo rumore capita anche una sola volta ogni tanto.
    // Usiamo invece: press immediato alla prima lettura positiva, rilascio
    // solo dopo questa finestra di tolleranza senza altre letture positive.
    constexpr uint32_t TOUCH_RELEASE_GRACE_MS = 80;

    // --- Task dedicato al touch (isolato dal loop principale) ---
    TaskHandle_t touchTaskHandle = nullptr;
    portMUX_TYPE touchMux = portMUX_INITIALIZER_UNLOCKED;

    // Mutex FreeRTOS reale che serializza TUTTE le transazioni sul bus I2C
    // condiviso (touch GT911 su core 0 + tastiera I2C letta dal loop
    // principale su core 1). Il vecchio portMUX proteggeva solo le
    // variabili condivise, non le vere chiamate Wire.*: due core potevano
    // finire a parlare sul bus nello stesso istante, corrompendo sia le
    // letture touch sia quelle tastiera. Questo mutex risolve il problema.
    SemaphoreHandle_t i2cMutex = nullptr;
    constexpr TickType_t I2C_MUTEX_TIMEOUT_TOUCH = pdMS_TO_TICKS(50);
    constexpr TickType_t I2C_MUTEX_TIMEOUT_KEYBOARD = pdMS_TO_TICKS(20);

    volatile bool sharedTouched = false;
    volatile int16_t sharedRawX = 0;
    volatile int16_t sharedRawY = 0;
    volatile uint32_t touchReadStartMs = 0;
    volatile bool touchReadInProgress = false;

    constexpr uint32_t TOUCH_TASK_TIMEOUT_MS = 300;
    constexpr uint32_t TOUCH_TASK_POLL_MS = 15;

    void IRAM_ATTR trackballG01() { cntUp++; }
    void IRAM_ATTR trackballG02() { cntRight++; }
    void IRAM_ATTR trackballG03() { cntDown++; }
    void IRAM_ATTR trackballG04() { cntLeft++; }

    bool textEntryMode = false;
    bool touchSuppressed = false;

    void translateKey(char key) {
        currentKey = key;

        if (textEntryMode) {
            // In un campo di testo libero solo Invio ha un significato
            // speciale (conferma): tutto il resto, comprese le lettere
            // altrimenti usate per la navigazione, e' testo da digitare.
            if (key == '\n' || key == '\r') {
                currentEvent = InputEvent::Select;
            } else {
                currentEvent = InputEvent::Key;
            }
            return;
        }

        switch (key) {
            case 'w': case 'W': currentEvent = InputEvent::Up; break;
            case 's': case 'S': currentEvent = InputEvent::Down; break;
            case 'a': case 'A': currentEvent = InputEvent::Left; break;
            case 'd': case 'D': currentEvent = InputEvent::Right; break;
            case '\n': case '\r': case 'e': case 'E': currentEvent = InputEvent::Select; break;
            case 'b': case 'B': currentEvent = InputEvent::Back; break;
            default: currentEvent = InputEvent::Key; break;
        }
    }

    // Sequenza di reset hardware del GT911. Il chip decide il proprio
    // indirizzo I2C (0x5D o 0x14) leggendo lo stato del pin INT nel
    // momento in cui esce dal reset: se non lo piloti esplicitamente,
    // l'indirizzo che risulta sul bus dopo il power-on e' indeterminato
    // e detectTouchAddr() puo' non trovarlo (o trovarlo a intermittenza
    // da un boot all'altro).
    //
    // Convenzione SensorLib: tenendo INT LOW durante il reset si forza
    // l'indirizzo 0x5D; tenendolo HIGH si forza 0x14. Qui selezioniamo
    // sempre 0x5D perche' e' l'indirizzo piu' comune sui moduli T-Deck.
    void resetTouchController(uint8_t forcedAddr) {
        pinMode(BOARD_TOUCH_INT, OUTPUT);
        digitalWrite(BOARD_TOUCH_INT, forcedAddr == 0x5D ? LOW : HIGH);
        delay(10);

        // Il T-Deck non espone un pin RESET dedicato per il touch (e'
        // legato al reset generale scheda), quindi ci limitiamo a tenere
        // l'INT nello stato voluto per il tempo minimo richiesto dal
        // datasheet GT911 dopo il power-on, poi lo liberiamo come input.
        delay(50);

        pinMode(BOARD_TOUCH_INT, INPUT);
        delay(50);
    }

    uint8_t detectTouchAddr() {
        const uint8_t candidates[2] = { 0x5D, 0x14 };

        for (uint8_t addr : candidates) {
            resetTouchController(addr);

            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                return addr;
            }
        }

        return 0;
    }

    void applyTouchRotation(int16_t rawX, int16_t rawY, int16_t &outX, int16_t &outY) {
        // Risoluzione nativa da usare per la scala: quella riportata dal
        // chip se disponibile, altrimenti le costanti come fallback.
        uint16_t resX = (touchResX > 0) ? touchResX : TOUCH_PANEL_W;
        uint16_t resY = (touchResY > 0) ? touchResY : TOUCH_PANEL_H;

        int32_t x = rawX;
        int32_t y = rawY;

#if TOUCH_SWAP_XY
        int32_t tmp = x;
        x = y;
        y = tmp;

        // Dopo lo swap, x copre l'asse che nel pannello nativo era lungo
        // "resY" campioni, e y quello lungo "resX": scaliamo ciascuno
        // verso la dimensione EFFETTIVA dello schermo (in pixel), invece
        // di assumere che i valori grezzi siano gia' in quel range.
        x = x * DISPLAY_WIDTH / (resY > 0 ? resY : 1);
        y = y * DISPLAY_HEIGHT / (resX > 0 ? resX : 1);
#else
        x = x * DISPLAY_WIDTH / (resX > 0 ? resX : 1);
        y = y * DISPLAY_HEIGHT / (resY > 0 ? resY : 1);
#endif

#if TOUCH_INVERT_X
        x = DISPLAY_WIDTH - x;
#endif
#if TOUCH_INVERT_Y
        y = DISPLAY_HEIGHT - y;
#endif

        if (x < 0) x = 0;
        if (x > DISPLAY_WIDTH - 1) x = DISPLAY_WIDTH - 1;
        if (y < 0) y = 0;
        if (y > DISPLAY_HEIGHT - 1) y = DISPLAY_HEIGHT - 1;

        outX = static_cast<int16_t>(x);
        outY = static_cast<int16_t>(y);
    }

    bool initTouchDriver() {
        touch.setPins(-1, BOARD_TOUCH_INT);

        if (!touch.begin(Wire, touchAddr, BOARD_I2C_SDA, BOARD_I2C_SCL)) {
            return false;
        }

        touchResX = touch.getResolutionX();
        touchResY = touch.getResolutionY();
        DEBUG_PRINTF(
            "[INPUT] touch risoluzione nativa riportata dal chip: %u x %u\n",
            touchResX, touchResY
        );

        return true;
    }

    void touchTaskFunc(void*) {
        for (;;) {
            if (xSemaphoreTake(i2cMutex, I2C_MUTEX_TIMEOUT_TOUCH) == pdTRUE) {
                portENTER_CRITICAL(&touchMux);
                touchReadStartMs = millis();
                touchReadInProgress = true;
                portEXIT_CRITICAL(&touchMux);

                TouchPoints points = touch.getTouchPoints();
                bool touched = points.hasPoints();
                int16_t rx = 0, ry = 0;

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

                xSemaphoreGive(i2cMutex);
            }
            // Se il mutex non si libera entro il timeout, semplicemente
            // saltiamo questo ciclo di lettura: il guardiano in
            // Input::update() si occupa di rilevare un driver davvero
            // bloccato e di ricrearlo.

            vTaskDelay(pdMS_TO_TICKS(TOUCH_TASK_POLL_MS));
        }
    }

    void startTouchTask() {
        if (touchTaskHandle != nullptr) {
            return;
        }

        xTaskCreatePinnedToCore(
            touchTaskFunc, "touchTask", 4096, nullptr, 1, &touchTaskHandle, 0
        );
    }

    void recreateTouchDriver() {
        DEBUG_PRINTLN("[INPUT] touch bloccato oltre il timeout, riavvio driver touch...");

        // A questo punto il chiamante ha gia' fatto vTaskDelete() sul task
        // touch: se quel task era bloccato DENTRO touch.getTouchPoints()
        // stava tenendo i2cMutex, e vTaskDelete() non esegue alcun cleanup.
        // Il mutex resterebbe quindi "posseduto" per sempre da un task che
        // non esiste piu': ogni xSemaphoreTake successivo (sia qui sia
        // nella lettura tastiera nel loop principale) fallirebbe a vita,
        // bloccando touch E tastiera insieme anche dopo un singolo blocco
        // transitorio. Ricreiamo quindi il mutex da zero invece di provare
        // a prenderlo: cosi' il bus torna sempre utilizzabile.
        if (i2cMutex != nullptr) {
            vSemaphoreDelete(i2cMutex);
        }
        i2cMutex = xSemaphoreCreateMutex();

        Wire.end();
        delay(5);
        Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
        Wire.setClock(400000);
        Wire.setTimeOut(50);

        touchAvailable = initTouchDriver();

        portENTER_CRITICAL(&touchMux);
        touchReadInProgress = false;
        sharedTouched = false;
        portEXIT_CRITICAL(&touchMux);

        if (!touchAvailable) {
            DEBUG_PRINTLN("[INPUT] riavvio driver touch fallito");
        }
    }

    // Legge lo stato touch "grezzo" gia' debounced e aggiorna
    // touchX/touchY/touchConfirmed. Il debounce a letture stabili evita
    // che il rumore I2C (bus condiviso con la tastiera) generi tap
    // fantasma sulle zone toccabili (vedi TouchUI). Prima passava per un
    // lv_indev_t LVGL (rimosso il 26/09, vedi
    // project_lvgl_removal_rewrite in memoria) - stessa identica logica,
    // ora scrive direttamente sullo stato letto da TouchUI/isTouched().
    void updateTouchState() {
        bool touched;
        int16_t rawX, rawY;

        portENTER_CRITICAL(&touchMux);
        touched = sharedTouched;
        rawX = sharedRawX;
        rawY = sharedRawY;
        portEXIT_CRITICAL(&touchMux);

        uint32_t now = millis();

        if (touched) {
            applyTouchRotation(rawX, rawY, touchX, touchY);
            lastTouchSeenMs = now;
            touchConfirmed = true;
        } else if (touchConfirmed && (now - lastTouchSeenMs) < TOUCH_RELEASE_GRACE_MS) {
            // Buco di lettura momentaneo: manteniamo il press attivo
            // finche' non scade la finestra di tolleranza, altrimenti un
            // singolo ciclo "vuoto" durante un tocco reale genererebbe un
            // rilascio a meta' tap.
        } else {
            touchConfirmed = false;
        }

        static bool lastReportedState = false;
        if (touchConfirmed != lastReportedState) {
            DEBUG_PRINTF("[INPUT] touch confermato=%d a (%d,%d)\n", touchConfirmed, touchX, touchY);
            lastReportedState = touchConfirmed;
        }
    }
}

namespace Input {

    void begin() {
        currentEvent = InputEvent::None;
        currentKey = 0;

        i2cMutex = xSemaphoreCreateMutex();

        pinMode(BOARD_KEYBOARD_INT, INPUT_PULLUP);

        // Diagnostica temporanea: verifica se il coprocessore tastiera
        // risponde affatto sul bus I2C, indipendentemente da eventuali
        // pressioni di tasti. Se non risponde qui, il problema e'
        // hardware/alimentazione, non il nostro codice di lettura.
        Wire.beginTransmission(BOARD_KEYBOARD_ADDR);
        uint8_t kbdProbeResult = Wire.endTransmission();
        if (kbdProbeResult == 0) {
            DEBUG_PRINTF(
                "[INPUT] tastiera: risponde su I2C a 0x%02X\n",
                BOARD_KEYBOARD_ADDR
            );
        } else {
            DEBUG_PRINTF(
                "[INPUT] tastiera: NESSUNA risposta su I2C a 0x%02X (errore=%d)\n",
                BOARD_KEYBOARD_ADDR, kbdProbeResult
            );
        }

        pinMode(BOARD_TBOX_G01, INPUT_PULLUP);
        pinMode(BOARD_TBOX_G02, INPUT_PULLUP);
        pinMode(BOARD_TBOX_G03, INPUT_PULLUP);
        pinMode(BOARD_TBOX_G04, INPUT_PULLUP);

        attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_G01), trackballG01, FALLING);
        attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_G02), trackballG02, FALLING);
        attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_G03), trackballG03, FALLING);
        attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_G04), trackballG04, FALLING);

        pinMode(BOARD_TRACKBALL_BTN, INPUT_PULLUP);
        previousButtonState = digitalRead(BOARD_TRACKBALL_BTN);

        Wire.setTimeOut(50);

#if INPUT_TOUCH_ENABLED
        touchAddr = detectTouchAddr();

        if (touchAddr != 0) {
            touchAvailable = initTouchDriver();

            if (touchAvailable) {
                DEBUG_PRINT("[INPUT] touch GT911 (SensorLib) trovato a 0x");
                DEBUG_PRINTLN(touchAddr, HEX);

                startTouchTask();
            } else {
                DEBUG_PRINTLN("[INPUT] touch GT911 rilevato su I2C ma begin() fallita");
            }
        } else {
            DEBUG_PRINTLN("[INPUT] touch GT911 non risponde su I2C (0x5D/0x14)");
        }
#else
        DEBUG_PRINTLN("[INPUT] touch disabilitato (INPUT_TOUCH_ENABLED=0), skip totale");
#endif
    }

    void update() {
        if (touchAvailable && !touchSuppressed) {
            updateTouchState();
        }

        currentEvent = InputEvent::None;
        currentKey = 0;

        if (millis() - lastInputTime < 20) {
            return;
        }

        lastInputTime = millis();

        // RIMOSSO DEL TUTTO (09/10) - la USB CDC nativa serviva sia i log
        // di debug (DEBUG_PRINTLN/PRINTF) SIA, da qui, come un canale di
        // "tastiera" non autenticato: qualunque byte stampabile arrivato
        // sulla stessa porta (incluso - scoperto dal vivo - un frammento
        // leggibile del log di debug stesso, es. "disegno_diretto=1" o
        // "ccezione" da una riga di CRASHLOG) veniva preso per un tasto
        // vero e finiva dentro il buffer di testo attivo (es. il numero
        // da comporre su VOIP). Il filtro precedente (solo ASCII
        // stampabile, vedi project_serial_input_noise_bug in memoria)
        // bloccava il rumore binario ma non un frammento di testo
        // leggibile come questo. L'utente ha confermato di non usare il
        // trigger "VPNIMPORT" (vedi sotto, rimosso insieme) ne' di
        // digitare tasti dal monitor seriale per testare senza tastiera
        // fisica - quindi l'intero canale e' stato disattivato invece di
        // reso "piu' sicuro": la tastiera fisica vera (sotto, via I2C) e'
        // un canale completamente separato e non ne e' affetta.
        // L'import VPN resta comunque disponibile dal menu (VpnUI.cpp
        // chiama VpnManager::importConfigFromSerial() direttamente, un
        // percorso bloccante indipendente da questo ciclo).

        // Il pin INT non si e' mai dimostrato affidabile su questo modulo
        // (resta sempre alto anche premendo tasti, verificato via log):
        // invece di aspettarlo, interroghiamo il coprocessore tastiera
        // via I2C ad ogni ciclo (comunque gia' limitato dal debounce da
        // 20ms sopra) e lasciamo che sia lui a rispondere 0 quando non
        // c'e' nessun tasto premuto.
        if (xSemaphoreTake(i2cMutex, I2C_MUTEX_TIMEOUT_KEYBOARD) == pdTRUE) {
            Wire.requestFrom(static_cast<uint8_t>(BOARD_KEYBOARD_ADDR), static_cast<uint8_t>(1));

            if (Wire.available()) {
                char key = static_cast<char>(Wire.read());

                if (key != 0) {
                    xSemaphoreGive(i2cMutex);

                    translateKey(key);
                    DEBUG_PRINT("[INPUT] tastiera key='");
                    DEBUG_PRINT(key);
                    DEBUG_PRINTLN("'");
                    return;
                }
            }

            xSemaphoreGive(i2cMutex);
        }

        // I 4 sensori del trackball sono interruttori meccanici: un solo
        // scatto fisico puo' generare piu' impulsi elettrici ravvicinati
        // (rimbalzo). Finche' siamo dentro la finestra di debounce dopo
        // l'ultimo movimento accettato, ignoriamo qualunque nuovo impulso;
        // quando accettiamo un movimento azzeriamo TUTTI e 4 i contatori
        // (non solo quello letto), cosi' il rimbalzo residuo sugli altri
        // pin durante lo stesso scatto non genera un secondo movimento
        // spurio al giro successivo.
        bool trackballDebounceActive = (millis() - lastMovementTime) < TRACKBALL_DEBOUNCE_MS;

        if (!trackballDebounceActive) {
            if (cntRight > 0) { cntRight = cntLeft = cntDown = cntUp = 0; lastMovementTime = millis(); currentEvent = InputEvent::Right; DEBUG_PRINTLN("[INPUT] trackball RIGHT"); return; }
            if (cntLeft  > 0) { cntRight = cntLeft = cntDown = cntUp = 0; lastMovementTime = millis(); currentEvent = InputEvent::Left;  DEBUG_PRINTLN("[INPUT] trackball LEFT");  return; }
            if (cntDown  > 0) { cntRight = cntLeft = cntDown = cntUp = 0; lastMovementTime = millis(); currentEvent = InputEvent::Down;  DEBUG_PRINTLN("[INPUT] trackball DOWN");  return; }
            if (cntUp    > 0) { cntRight = cntLeft = cntDown = cntUp = 0; lastMovementTime = millis(); currentEvent = InputEvent::Up;    DEBUG_PRINTLN("[INPUT] trackball UP");    return; }
        } else {
            // Scarta comunque gli impulsi accumulati durante il cooldown,
            // altrimenti verrebbero processati tutti insieme non appena
            // il debounce scade.
            cntRight = cntLeft = cntDown = cntUp = 0;
        }

        bool buttonState = digitalRead(BOARD_TRACKBALL_BTN);
        bool movementCooldownActive = (millis() - lastMovementTime) < 150;

        if (buttonState == LOW && !movementCooldownActive) {
            if (previousButtonState == LOW && !buttonSelectFired) {
                buttonSelectFired = true;
                currentEvent = InputEvent::Select;
                previousButtonState = buttonState;
                DEBUG_PRINTLN("[INPUT] trackball SELECT (pulsante centrale)");
                return;
            }
        } else if (buttonState == HIGH) {
            buttonSelectFired = false;
        }

        previousButtonState = buttonState;

        // Guardiano task touch: se il driver resta bloccato in lettura
        // oltre il timeout, lo ricrea da zero.
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
        return touchConfirmed;
    }

    int16_t getTouchX() {
        return touchX;
    }

    int16_t getTouchY() {
        return touchY;
    }

    void setTextEntryMode(bool enabled) {
        textEntryMode = enabled;
    }

    bool isTextEntryMode() {
        return textEntryMode;
    }

    void setTouchSuppressed(bool suppressed) {
        touchSuppressed = suppressed;
    }

    bool lockI2C(uint32_t timeoutMs) {
        if (i2cMutex == nullptr) {
            return false;
        }
        return xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    }

    void unlockI2C() {
        if (i2cMutex != nullptr) {
            xSemaphoreGive(i2cMutex);
        }
    }
}
