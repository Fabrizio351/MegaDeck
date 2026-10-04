#include "LoRaScanUI.h"
#include "Display.h"
#include "Input.h"
#include "TouchUI.h"
#include "LoRaToolsMenu.h"
#include "LoRaRadio.h"
#include "Config.h"

#include <Arduino.h>
#include <RadioLib.h>

// Scansione RSSI grezza della banda 868MHz (26/09, richiesta dall'utente:
// "vediamo cosa esce" prima di provare a costruire qualunque cattura
// vera) - NON decodifica nulla, misura solo quanta energia radio c'e' ad
// ogni frequenza (ampiezza del segnale, non il contenuto), passo dopo
// passo su tutta la banda. Serve a scoprire DOVE c'e' attivita' (es. "ogni
// volta che premo il telecomando del cancello, vedo un picco a X MHz")
// prima di investire tempo nella parte molto piu' complessa di cattura/
// decodifica vera e propria.
//
// La radio SX1262 di questo T-Deck lavora normalmente in modulazione
// LoRa per la chat (vedi LoRaRadio.cpp) - qui viene temporaneamente
// riconfigurata in modulazione FSK a banda larga (solo per leggere
// l'RSSI, non per demodulare nulla) tramite LoRaRadio::pauseForExternalUse()/
// getRadioForExternalUse()/resumeAfterExternalUse(), che la rimettono in
// modalita' LoRa normale alla chiusura di questa schermata - la chat
// resta quindi inutilizzabile mentre questa schermata e' aperta.
//
// Aggiunta il 26/09 una seconda modalita', "Monitor": fissa la radio
// sulla frequenza del picco trovato dalla scansione e mostra l'energia
// nel TEMPO invece che per frequenza, per capire il "comportamento" di
// un segnale sconosciuto (periodico da solo vs solo su pressione,
// durata/ripetizioni delle raffiche) - vedi CONFERMATO CON L'UTENTE
// (26/09): il chip SX1262 di questo T-Deck NON supporta l'uscita del
// flusso di bit grezzo in ricezione (RadioLib: "SX126x is unable to
// output received data directly", solo il TX bit-bang lo supporta) -
// quindi questo e' il massimo di precisione ottenibile con QUESTO
// hardware, non decodifica mai il contenuto di un segnale, solo la sua
// presenza/durata nel tempo. Una cattura vera richiederebbe un ricevitore
// OOK/ASK dedicato su un GPIO libero (fuori dallo scopo di questo modulo).
namespace {

    constexpr float SCAN_FREQ_START = 863.0f; // MHz, inizio banda SRD860 EU
    constexpr float SCAN_FREQ_STEP = 0.2f;    // MHz, vedi nota RadioLib: <= larghezza banda RX
    constexpr int SCAN_BUCKET_COUNT = 35;     // (870.0 - 863.0) / 0.2

    constexpr float FSK_BIT_RATE = 4.8f;      // kbps, non rilevante per la sola lettura RSSI
    constexpr float FSK_FREQ_DEV = 5.0f;      // kHz
    constexpr float FSK_RX_BANDWIDTH = 234.3f; // kHz, la piu' larga disponibile - vedi SX126x.h

    // Range RSSI (dBm) mappato sull'altezza delle barre: sotto RSSI_MIN
    // barra vuota, sopra RSSI_MAX barra piena. Puramente indicativo,
    // andra' probabilmente ritoccato osservando i valori reali sul campo.
    constexpr float RSSI_MIN = -120.0f;
    constexpr float RSSI_MAX = -40.0f;

    enum class Mode {
        Closed,
        Scanning,
        Monitor
    };

    Mode mode = Mode::Closed;
    bool radioConfigured = false;
    bool scanRunning = false;
    int currentBucket = 0;
    float rssiValues[SCAN_BUCKET_COUNT];
    int peakBucket = -1;

    // Ritmo del giro di scansione (26/09, corretto su segnalazione
    // dell'utente): un giro completo impiegava solo ~200ms (35 frequenze
    // x 6ms), quindi il grafico si riempiva di verde a ripetizione
    // talmente veloce da sembrare un lampeggio illeggibile ("tutto verde
    // senza capire nulla"). Ora un giro completo dura circa un secondo,
    // con una pausa alla fine per poterlo osservare fermo prima che
    // riparta - un "fotogramma" leggibile al secondo invece di un
    // flusso continuo.
    constexpr uint32_t SWEEP_DURATION_MS = 1000;
    constexpr uint32_t STEP_INTERVAL_MS = SWEEP_DURATION_MS / SCAN_BUCKET_COUNT;
    constexpr uint32_t SWEEP_PAUSE_MS = 600;
    uint32_t nextStepMs = 0;

    constexpr int16_t BTN_X = 8;
    constexpr int16_t BTN_W = 304;
    constexpr int16_t BTN_H = 26;
    constexpr int16_t SCAN_BTN_Y = Display::DD_HEADER_H + 24;
    constexpr int16_t MONITOR_BTN_Y = SCAN_BTN_Y + BTN_H + 4;
    constexpr int16_t INFO_Y = MONITOR_BTN_Y + BTN_H + 8;
    constexpr int16_t GRAPH_TOP = INFO_Y + 16;
    constexpr int16_t GRAPH_BOTTOM = 236;
    constexpr int16_t GRAPH_H = GRAPH_BOTTOM - GRAPH_TOP;
    constexpr int16_t BAR_W = 320 / SCAN_BUCKET_COUNT;

    int toggleZone = -1;
    int monitorZone = -1;

    // Bottone a fuoco per la navigazione da trackball in Mode::Scanning
    // (29/09, richiesto: audit generale - prima Select andava sempre e
    // solo su SCAN/STOP, il bottone MONITOR era raggiungibile solo col
    // touch). Rilevante solo quando monitorZone e' disponibile
    // (peakBucket >= 0); altrimenti resta forzato su Toggle.
    enum class ScanFocus {
        Toggle,
        Monitor
    };
    ScanFocus scanFocus = ScanFocus::Toggle;

    void closeAndReturnToMenu() {
        mode = Mode::Closed;
        scanRunning = false;
        if (radioConfigured) {
            LoRaRadio::resumeAfterExternalUse();
            radioConfigured = false;
        }
        LoRaToolsMenu::showMain();
    }

    void drawHeaderTitle(const char* subtitle) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 8);
        gfx->print(subtitle);

        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(228, Display::DD_HEADER_H + 8);
        gfx->print("[B] INDIETRO");
    }

    // fillRect prima di ristampare: senza, un'etichetta piu' corta che ne
    // sostituisce una piu' lunga lascia lettere vecchie sovrapposte -
    // stesso errore gia' preso e corretto altrove in questa sessione
    // (NetGuardUI/NetScanUI), preso anche qui la prima volta (bottone
    // "sempre su avvia", segnalato dall'utente).
    void drawButton(int16_t y, const char* label, bool focused = true) {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(BTN_X + 1, y + 1, BTN_W - 2, BTN_H - 2, Display::DD_BG);
        gfx->drawRect(BTN_X, y, BTN_W, BTN_H, focused ? Display::DD_ACCENT : Display::DD_ACCENT_DIM);
        gfx->setTextColor(focused ? Display::DD_ACCENT : Display::DD_TEXT_DIM);
        gfx->setCursor(BTN_X + 12, y + BTN_H / 2 - 3);
        gfx->print(label);
    }

    // Disegna ENTRAMBI i bottoni e ri-registra SEMPRE le loro zone tap
    // insieme (clearZones + riaggiunta di tutte quelle di questa
    // schermata, mai una registrazione parziale) - stesso principio gia'
    // applicato a VpnUI/NetGuardUI in questa sessione. Va richiamata ogni
    // volta che peakBucket cambia (non solo all'apertura schermata):
    // prima la zona di MONITOR veniva registrata una sola volta, quando
    // la schermata si apriva e peakBucket era ancora -1, quindi restava
    // per sempre non toccabile anche dopo che un picco veniva trovato
    // (bug reale, segnalato dall'utente: "il tasto monitor non fa nulla").
    void drawScanButtons() {
        if (monitorZone < 0) {
            scanFocus = ScanFocus::Toggle; // nessun MONITOR disponibile, il fuoco non puo' restarci sopra
        }

        drawButton(SCAN_BTN_Y, scanRunning ? "[ STOP ] FERMA SCANSIONE" : "[ SCAN ] AVVIA SCANSIONE 863-870 MHZ",
            scanFocus == ScanFocus::Toggle);

        if (peakBucket >= 0) {
            float peakFreq = SCAN_FREQ_START + peakBucket * SCAN_FREQ_STEP;
            char label[40];
            snprintf(label, sizeof(label), "[ MONITOR ] OSSERVA %.1f MHZ NEL TEMPO", peakFreq);
            drawButton(MONITOR_BTN_Y, label, scanFocus == ScanFocus::Monitor);
        } else {
            Arduino_GFX* gfx = Display::getGfx();
            gfx->fillRect(BTN_X, MONITOR_BTN_Y, BTN_W, BTN_H, Display::DD_BG);
            gfx->drawRect(BTN_X, MONITOR_BTN_Y, BTN_W, BTN_H, Display::DD_ACCENT_DIM);
            gfx->setTextColor(Display::DD_TEXT_DIM);
            gfx->setCursor(BTN_X + 12, MONITOR_BTN_Y + BTN_H / 2 - 3);
            gfx->print("Fai prima un giro di scansione");
        }

        TouchUI::clearZones();
        toggleZone = TouchUI::addZone(BTN_X, SCAN_BTN_Y, BTN_W, BTN_H);
        monitorZone = (peakBucket >= 0) ? TouchUI::addZone(BTN_X, MONITOR_BTN_Y, BTN_W, BTN_H) : -1;
    }

    void doToggleScan();
    void enterMonitorMode();

    // Sposta il fuoco fra SCAN/STOP e MONITOR (Su/Giu' da trackball,
    // solo quando MONITOR e' disponibile) e attiva quello a fuoco
    // (Select) - stesso schema gia' applicato oggi a VpnUI.cpp.
    void moveScanFocus(ScanFocus f) {
        if (scanFocus == f || (f == ScanFocus::Monitor && monitorZone < 0)) {
            return;
        }
        scanFocus = f;
        drawScanButtons();
    }

    void activateScanFocused() {
        if (scanFocus == ScanFocus::Monitor && monitorZone >= 0) {
            enterMonitorMode();
        } else {
            doToggleScan();
        }
    }

    void drawInfoLine() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, INFO_Y, 320, 12, Display::DD_BG);
        gfx->setTextColor(Display::DD_TEXT_DIM);
        gfx->setCursor(8, INFO_Y);

        if (!scanRunning && peakBucket < 0) {
            gfx->print("Misura solo energia radio (RSSI), non decodifica nulla.");
            return;
        }

        char line[64];
        if (peakBucket >= 0) {
            float peakFreq = SCAN_FREQ_START + peakBucket * SCAN_FREQ_STEP;
            snprintf(line, sizeof(line), "Picco: %.1f MHz (%.0f dBm)", peakFreq, rssiValues[peakBucket]);
        } else {
            snprintf(line, sizeof(line), "In corso...");
        }
        gfx->print(line);
    }

    int16_t barHeight(float rssi) {
        if (rssi <= RSSI_MIN) {
            return 0;
        }
        if (rssi >= RSSI_MAX) {
            return GRAPH_H;
        }
        return static_cast<int16_t>((rssi - RSSI_MIN) / (RSSI_MAX - RSSI_MIN) * GRAPH_H);
    }

    void drawBar(int bucket) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t x = bucket * BAR_W;

        gfx->fillRect(x, GRAPH_TOP, BAR_W - 1, GRAPH_H, Display::DD_BG);

        int16_t h = barHeight(rssiValues[bucket]);
        if (h > 0) {
            uint16_t color = (bucket == peakBucket) ? Display::DD_DANGER : Display::DD_ACCENT;
            gfx->fillRect(x, GRAPH_BOTTOM - h, BAR_W - 1, h, color);
        }
    }

    void drawGraphAxis() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->drawFastHLine(0, GRAPH_BOTTOM, 320, Display::DD_ACCENT_DIM);
    }

    void drawScanScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("LORA SCAN");
        drawScanButtons();
        drawInfoLine();
        drawGraphAxis();

        for (int i = 0; i < SCAN_BUCKET_COUNT; i++) {
            drawBar(i);
        }
        // Zone tap gia' registrate da drawScanButtons() sopra (clearZones
        // + entrambi i bottoni insieme) - niente da aggiungere qui.
    }

    bool ensureRadioConfigured() {
        if (radioConfigured) {
            return true;
        }

        LoRaRadio::pauseForExternalUse();
        SX1262& radio = LoRaRadio::getRadioForExternalUse();

        int state = radio.beginFSK(SCAN_FREQ_START, FSK_BIT_RATE, FSK_FREQ_DEV, FSK_RX_BANDWIDTH);
        if (state != RADIOLIB_ERR_NONE) {
            DEBUG_PRINTF("[LORASCAN] beginFSK fallito, codice %d\n", state);
            LoRaRadio::resumeAfterExternalUse();
            return false;
        }

        radio.setRxBandwidth(FSK_RX_BANDWIDTH);
        radio.setDataShaping(RADIOLIB_SHAPING_NONE);
        radio.startReceive();

        radioConfigured = true;
        return true;
    }

    void doToggleScan() {
        if (scanRunning) {
            scanRunning = false;
            drawScanButtons();
            return;
        }

        if (!ensureRadioConfigured()) {
            Display::showMessage(
                "LoRa Scan",
                "Impossibile configurare la radio per la scansione (radio non pronta o inizializzazione FSK fallita).",
                []() { LoRaScanUI::open(); }
            );
            return;
        }

        currentBucket = 0;
        peakBucket = -1;
        nextStepMs = 0;
        for (int i = 0; i < SCAN_BUCKET_COUNT; i++) {
            rssiValues[i] = RSSI_MIN;
        }
        scanRunning = true;
        drawScanButtons();
    }

    // Avanza di UNA frequenza per ciclo (mai un ciclo bloccante che
    // scansiona tutta la banda in un colpo solo, stesso principio "non
    // bloccare mai il loop principale" di NetScan/ArpSweep): ogni
    // chiamata imposta una frequenza, aspetta che l'RSSI si stabilizzi
    // e la legge, poi ridisegna solo la barra cambiata.
    void advanceScanStep() {
        if (millis() < nextStepMs) {
            return;
        }

        SX1262& radio = LoRaRadio::getRadioForExternalUse();

        // Cambiare frequenza (SetRfFrequency, un semplice registro) non
        // ha effetto reale se la radio e' gia' in ricezione continua -
        // serve rientrare in RX perche' il nuovo valore sia davvero
        // applicato. standby() prima per una transizione di stato pulita.
        float freq = SCAN_FREQ_START + currentBucket * SCAN_FREQ_STEP;
        radio.standby();
        radio.setFrequency(freq);
        radio.startReceive();
        delay(6); // tempo minimo perche' l'RSSI si aggiorni dopo il cambio frequenza

        // getRSSI(false) = valore CORRENTE/istantaneo, non quello
        // dell'ultimo pacchetto ricevuto (mai, dato che non decodifichiamo
        // nulla qui) - getRSSI() senza parametri leggerebbe quest'ultimo,
        // causa del bug "tutte le barre sempre al massimo" osservato.
        float rssi = radio.getRSSI(false);
        rssiValues[currentBucket] = rssi;

        if (peakBucket < 0 || rssi > rssiValues[peakBucket]) {
            peakBucket = currentBucket;
        }

        drawBar(currentBucket);

        currentBucket++;
        if (currentBucket >= SCAN_BUCKET_COUNT) {
            currentBucket = 0;
            drawInfoLine(); // mostra il picco del giro appena completato
            // peakBucket puo' essere appena diventato valido (o cambiato
            // frequenza) - ridisegna ENTRAMBI i bottoni e ri-registra le
            // zone tap di conseguenza, altrimenti "MONITOR" resta per
            // sempre non toccabile anche quando appare un picco.
            drawScanButtons();
            nextStepMs = millis() + SWEEP_PAUSE_MS; // ferma il quadro completo un attimo prima di ripartire
        } else {
            nextStepMs = millis() + STEP_INTERVAL_MS;
        }
    }

    // --- Modalita' Monitor: energia nel tempo su UNA frequenza fissa ---
    //
    // Massima precisione possibile con QUESTO chip (vedi commento in
    // testa al file): nessuna pausa fra un campione e l'altro (a
    // differenza della scansione, qui vogliamo il campionamento piu'
    // veloce possibile, limitato solo dal tempo reale di una transazione
    // SPI), soglia adattiva sopra il rumore di fondo per riconoscere
    // "raffiche" (attivita' vera) dal semplice rumore.

    constexpr int16_t MON_COL_W = 2;
    constexpr int MON_COL_COUNT = 320 / MON_COL_W; // 160 colonne
    constexpr float BURST_THRESHOLD_DELTA_DB = 6.0f; // sopra il rumore di fondo = raffica

    float monitorFreq = 0;
    int monCol = 0;
    float monHistory[MON_COL_COUNT];
    float noiseFloor = RSSI_MIN;
    bool noiseFloorInit = false;
    bool burstActive = false;
    uint32_t burstStartMs = 0;
    uint32_t lastBurstDurationMs = 0;
    uint32_t lastBurstEndMs = 0;
    int burstCount = 0;

    constexpr int16_t MON_GRAPH_TOP = Display::DD_HEADER_H + 60;
    constexpr int16_t MON_GRAPH_BOTTOM = 200;
    constexpr int16_t MON_GRAPH_H = MON_GRAPH_BOTTOM - MON_GRAPH_TOP;
    constexpr int16_t MON_STOP_BTN_Y = 206;

    int monStopZone = -1;

    void drawMonitorHeaderInfo() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillRect(0, Display::DD_HEADER_H + 24, 320, 34, Display::DD_BG);

        gfx->setFont(nullptr);
        gfx->setTextSize(1);
        gfx->setTextWrap(false);

        char line1[48];
        snprintf(line1, sizeof(line1), "Frequenza: %.1f MHz   Rumore: %.0f dBm", monitorFreq, noiseFloor);
        gfx->setTextColor(Display::DD_TEXT);
        gfx->setCursor(8, Display::DD_HEADER_H + 26);
        gfx->print(line1);

        char line2[64];
        uint32_t silenceMs = burstActive ? 0 : (millis() - lastBurstEndMs);
        snprintf(line2, sizeof(line2), "Raffiche: %d   Ultima durata: %lums   Silenzio: %lus",
            burstCount, static_cast<unsigned long>(lastBurstDurationMs), static_cast<unsigned long>(silenceMs / 1000));
        gfx->setTextColor(Display::DD_ACCENT);
        gfx->setCursor(8, Display::DD_HEADER_H + 40);
        gfx->print(line2);
    }

    void drawMonitorColumn(int col) {
        Arduino_GFX* gfx = Display::getGfx();
        int16_t x = col * MON_COL_W;

        gfx->fillRect(x, MON_GRAPH_TOP, MON_COL_W, MON_GRAPH_H, Display::DD_BG);

        int16_t h = barHeight(monHistory[col]);
        // Ridotto alla scala del grafico Monitor (altezza diversa dal
        // grafico di scansione) - barHeight() lavora in percentuale
        // implicita (0..GRAPH_H per lo scan), qui riscalata.
        h = static_cast<int16_t>((static_cast<float>(h) / GRAPH_H) * MON_GRAPH_H);

        if (h > 0) {
            bool wasBurstHere = monHistory[col] >= noiseFloor + BURST_THRESHOLD_DELTA_DB;
            uint16_t color = wasBurstHere ? Display::DD_DANGER : Display::DD_ACCENT_DIM;
            gfx->fillRect(x, MON_GRAPH_BOTTOM - h, MON_COL_W, h, color);
        }
    }

    void drawMonitorScreen() {
        Arduino_GFX* gfx = Display::getGfx();
        gfx->fillScreen(Display::DD_BG);
        Display::drawHeaderBar();
        drawHeaderTitle("LORA MONITOR");
        drawMonitorHeaderInfo();

        gfx->drawFastHLine(0, MON_GRAPH_BOTTOM, 320, Display::DD_ACCENT_DIM);
        for (int i = 0; i < MON_COL_COUNT; i++) {
            drawMonitorColumn(i);
        }

        drawButton(MON_STOP_BTN_Y, "[ B ] TORNA ALLA SCANSIONE");

        TouchUI::clearZones();
        monStopZone = TouchUI::addZone(BTN_X, MON_STOP_BTN_Y, BTN_W, BTN_H);
    }

    void enterMonitorMode() {
        monitorFreq = SCAN_FREQ_START + peakBucket * SCAN_FREQ_STEP;

        SX1262& radio = LoRaRadio::getRadioForExternalUse();
        radio.standby();
        radio.setFrequency(monitorFreq);
        radio.startReceive();

        monCol = 0;
        noiseFloor = RSSI_MIN;
        noiseFloorInit = false;
        burstActive = false;
        burstCount = 0;
        lastBurstDurationMs = 0;
        lastBurstEndMs = millis();
        for (int i = 0; i < MON_COL_COUNT; i++) {
            monHistory[i] = RSSI_MIN;
        }

        mode = Mode::Monitor;
        drawMonitorScreen();
    }

    void exitMonitorMode() {
        mode = Mode::Scanning;
        scanFocus = ScanFocus::Toggle;
        // Torna alla frequenza di partenza della scansione (la radio
        // resta configurata in FSK, non serve reinizializzarla).
        SX1262& radio = LoRaRadio::getRadioForExternalUse();
        radio.standby();
        radio.setFrequency(SCAN_FREQ_START);
        radio.startReceive();
        drawScanScreen();
    }

    // Chiamata ad ogni ciclo mentre in Monitor: NESSUNA pausa fra un
    // campione e l'altro (a differenza della scansione), il ritmo reale
    // e' dettato solo dal tempo di una transazione SPI - il massimo
    // ottenibile con questo chip (vedi commento in testa al file).
    void advanceMonitorStep() {
        SX1262& radio = LoRaRadio::getRadioForExternalUse();
        float rssi = radio.getRSSI(false);

        // Rumore di fondo: media mobile lenta, aggiornata SOLO fuori da
        // una raffica (altrimenti un segnale forte e prolungato
        // alzerebbe la propria stessa soglia di rilevamento).
        if (!noiseFloorInit) {
            noiseFloor = rssi;
            noiseFloorInit = true;
        } else if (!burstActive) {
            noiseFloor = noiseFloor * 0.98f + rssi * 0.02f;
        }

        bool active = rssi >= noiseFloor + BURST_THRESHOLD_DELTA_DB;
        if (active && !burstActive) {
            burstActive = true;
            burstStartMs = millis();
        } else if (!active && burstActive) {
            burstActive = false;
            lastBurstDurationMs = millis() - burstStartMs;
            lastBurstEndMs = millis();
            burstCount++;
        }

        monHistory[monCol] = rssi;
        drawMonitorColumn(monCol);

        monCol++;
        if (monCol >= MON_COL_COUNT) {
            monCol = 0;
        }

        // Le statistiche (raffiche/durata/silenzio) cambiano di continuo:
        // ridisegnate ogni giro di colonna intero per restare leggibili
        // senza ridisegnare tutto ad ogni singolo campione.
        static uint32_t lastStatsMs = 0;
        if (millis() - lastStatsMs > 300) {
            lastStatsMs = millis();
            drawMonitorHeaderInfo();
        }
    }

} // namespace

namespace LoRaScanUI {

    void open() {
        mode = Mode::Scanning;
        scanRunning = false;
        radioConfigured = false;
        peakBucket = -1;
        scanFocus = ScanFocus::Toggle;
        for (int i = 0; i < SCAN_BUCKET_COUNT; i++) {
            rssiValues[i] = RSSI_MIN;
        }
        Display::setDirectDrawMode(true);
        drawScanScreen();
    }

    bool isActive() {
        return mode != Mode::Closed;
    }

    void update() {
        if (mode == Mode::Closed) {
            return;
        }

        if (mode == Mode::Monitor) {
            advanceMonitorStep();

            int tapped = TouchUI::pollTap();
            if (tapped == monStopZone) {
                exitMonitorMode();
                return;
            }

            InputEvent event = Input::getEvent();
            if (event == InputEvent::Back || event == InputEvent::Select) {
                exitMonitorMode();
            }
            return;
        }

        // Mode::Scanning
        if (scanRunning) {
            advanceScanStep();
        }

        int tapped = TouchUI::pollTap();
        if (tapped == toggleZone) {
            moveScanFocus(ScanFocus::Toggle);
            doToggleScan();
            return;
        }
        if (tapped == monitorZone && monitorZone >= 0) {
            moveScanFocus(ScanFocus::Monitor);
            enterMonitorMode();
            return;
        }

        InputEvent event = Input::getEvent();
        switch (event) {
            case InputEvent::Up:
                moveScanFocus(ScanFocus::Toggle);
                break;

            case InputEvent::Down:
                moveScanFocus(ScanFocus::Monitor);
                break;

            case InputEvent::Select:
                activateScanFocused();
                break;

            case InputEvent::Back:
                closeAndReturnToMenu();
                break;

            default:
                break;
        }
    }
}
