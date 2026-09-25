#include "ScreenLock.h"
#include "Input.h"
#include "Board.h"
#include "Sound.h"
#include "Config.h"

#include <Arduino.h>
#include <ctype.h>

namespace {
    // Quanto resta accesa la sola retroilluminazione tastiera come
    // conferma "sto ricevendo input" quando si preme un tasto a schermo
    // bloccato, prima di rispegnersi da sola (lo schermo invece resta
    // sempre spento finche' non si sblocca).
    constexpr uint32_t KEYBOARD_FEEDBACK_MS = 3000;

    // Livello minimo a cui accendere la tastiera per il feedback: se
    // l'utente tiene normalmente la retroilluminazione tastiera spenta
    // (0%) premere un tasto non darebbe altrimenti alcun riscontro
    // visibile.
    constexpr int KEYBOARD_FEEDBACK_MIN_PERCENT = 50;

    // Promemoria periodico "sono bloccato, non spento": un flash breve
    // della sola retroilluminazione tastiera ogni minuto, senza bisogno
    // di premere alcun tasto.
    constexpr uint32_t REMINDER_FLASH_INTERVAL_MS = 60000;
    constexpr uint32_t REMINDER_FLASH_DURATION_MS = 200;
    constexpr int REMINDER_FLASH_PERCENT = 60;

    uint32_t nextReminderFlashMs = 0;

    // Finestra massima fra 'q' e Barra Spaziatrice perche' conti come
    // combinazione: e' la stessa sia per bloccare che per sbloccare.
    // CONFERMATO con la diagnostica dal vivo (2026-09-23): con 700ms la
    // combinazione non si completava MAI in pratica, nemmeno provando piu'
    // volte di seguito - i tempi reali fra 'q' e barra spaziatrice erano
    // sistematicamente 700-1300ms (tempo di reazione umano normale per
    // raggiungere due tasti separati), sempre appena sopra la soglia.
    // Uno schermo bloccato con una combinazione di sblocco fisicamente
    // impossibile da eseguire in tempo equivale a un dispositivo
    // inutilizzabile - alzata a un valore che lascia margine reale.
    constexpr uint32_t COMBO_WINDOW_MS = 1500;

    bool locked = false;

    // Luminosita' di schermo/tastiera salvate al momento del blocco, da
    // ripristinare esattamente com'erano allo sblocco.
    int savedScreenBrightness = 0;
    int savedKeyboardBrightness = 0;

    // Mentre bloccato, retroilluminazione tastiera "a tempo" accesa da
    // una pressione qualsiasi: 0 = spenta in questo momento.
    uint32_t keyboardFeedbackUntilMs = 0;

    // Rileva la sequenza "q" poi " " (Barra Spaziatrice) entro
    // COMBO_WINDOW_MS: e' la stessa combinazione sia per bloccare che
    // per sbloccare.
    char lastKey = 0;
    uint32_t lastKeyMs = 0;

    bool feedComboDetector(char key) {
        uint32_t now = millis();
        char lower = static_cast<char>(tolower(static_cast<unsigned char>(key)));

        bool completed = (lower == ' ' && lastKey == 'q' && (now - lastKeyMs) < COMBO_WINDOW_MS);

        lastKey = lower;
        lastKeyMs = now;
        return completed;
    }

    void lockNow() {
        locked = true;
        savedScreenBrightness = Board::getBacklightPercent();
        savedKeyboardBrightness = Board::getKeyboardBacklightPercent();
        keyboardFeedbackUntilMs = 0;
        nextReminderFlashMs = millis() + REMINDER_FLASH_INTERVAL_MS;

        DEBUG_PRINTF(
            "[SCREENLOCK] BLOCCO: luminosita' schermo salvata=%d%% tastiera salvata=%d%%\n",
            savedScreenBrightness, savedKeyboardBrightness
        );

        Board::setBacklightPercentTransient(0);
        Board::setKeyboardBacklightPercentTransient(0);

        // Sospende la lettura dell'indev touch: l'elaborazione del tocco
        // (compreso il dispatch dei click LVGL) avviene dentro
        // Input::update(), PRIMA che App.cpp possa intervenire - va
        // fermata li', non basterebbe ignorare lo stato qui.
        Input::setTouchSuppressed(true);

        Sound::playScreenLocked();
    }

    void unlockNow() {
        locked = false;
        keyboardFeedbackUntilMs = 0;

        DEBUG_PRINTF(
            "[SCREENLOCK] SBLOCCO: ripristino luminosita' schermo=%d%% tastiera=%d%%\n",
            savedScreenBrightness, savedKeyboardBrightness
        );

        Input::setTouchSuppressed(false);
        Board::setBacklightPercentTransient(savedScreenBrightness);
        Board::setKeyboardBacklightPercentTransient(savedKeyboardBrightness);

        Sound::playScreenUnlocked();
    }
}

namespace ScreenLock {

    void begin() {
        locked = false;
        lastKey = 0;
        keyboardFeedbackUntilMs = 0;
        nextReminderFlashMs = 0;
        Input::setTouchSuppressed(false);
    }

    bool isLocked() {
        return locked;
    }

    void update() {
        if (!locked) {
            // Da sbloccato, la combinazione va rilevata solo fuori dal
            // testo libero (password WiFi, messaggi chat...): altrimenti
            // scrivere una parola con la lettera 'q' seguita da uno
            // spazio bloccherebbe lo schermo per sbaglio mentre si sta
            // scrivendo.
            if (!Input::isTextEntryMode() && Input::getEvent() == InputEvent::Key) {
                if (feedComboDetector(Input::getKey())) {
                    lockNow();
                }
            }
            return;
        }

        // Bloccato: consumiamo l'input qui (App::update() salta del
        // tutto la normale gestione di menu/schermate mentre isLocked()
        // e' vero, vedi App.cpp), cosi' nessun tasto raggiunge le
        // schermate sottostanti. Il touch e' gia' sospeso a monte in
        // Input::update(), quindi qui basta gestire solo la tastiera.
        InputEvent event = Input::getEvent();

        if (event == InputEvent::Key && feedComboDetector(Input::getKey())) {
            unlockNow();
            return;
        }

        if (event != InputEvent::None) {
            // Qualsiasi altro tasto premuto: solo la retroilluminazione
            // tastiera si accende per qualche secondo come conferma "sto
            // ricevendo input", lo schermo resta spento e il dispositivo
            // resta bloccato.
            keyboardFeedbackUntilMs = millis() + KEYBOARD_FEEDBACK_MS;
            int feedbackPercent = savedKeyboardBrightness > KEYBOARD_FEEDBACK_MIN_PERCENT
                ? savedKeyboardBrightness
                : KEYBOARD_FEEDBACK_MIN_PERCENT;
            Board::setKeyboardBacklightPercentTransient(feedbackPercent);
        }

        if (keyboardFeedbackUntilMs != 0 && millis() > keyboardFeedbackUntilMs) {
            keyboardFeedbackUntilMs = 0;
            Board::setKeyboardBacklightPercentTransient(0);
        }

        // Promemoria periodico: solo se la tastiera non e' gia' accesa
        // per un altro motivo (feedback da tasto appena premuto), cosi'
        // non si accavallano due timer di spegnimento diversi.
        if (keyboardFeedbackUntilMs == 0 && millis() >= nextReminderFlashMs) {
            keyboardFeedbackUntilMs = millis() + REMINDER_FLASH_DURATION_MS;
            nextReminderFlashMs = millis() + REMINDER_FLASH_INTERVAL_MS;
            Board::setKeyboardBacklightPercentTransient(REMINDER_FLASH_PERCENT);
        }
    }
}
