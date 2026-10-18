#include "RtpAudio.h"
#include "Config.h"
#include "Mic.h"
#include "Sound.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESP_I2S.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

// Task audio dedicato per VoIP (08/10, v1; 11/10, v2: aggiunta la
// riproduzione in arrivo) - vedi RtpAudio.h e il piano approvato.
namespace {

    constexpr int FRAME_SAMPLES_16K = 320; // 20ms a 16kHz (Mic.cpp/Sound.cpp)
    constexpr int FRAME_SAMPLES_8K = 160;  // 20ms a 8kHz (G.711)
    constexpr uint32_t MIC_READ_TIMEOUT_MS = 100;
    constexpr uint32_t STOP_WAIT_TIMEOUT_MS = 2000;

    // Ring RX per assorbire il jitter di arrivo (vedi il piano: "~3
    // frame/60ms, silenzio sui vuoti, scarta il piu' vecchio sui
    // pieni"). File-scope (non sullo stack del task, che ha gia'
    // parecchi buffer suoi - vedi audioTaskFn) dato che esiste un solo
    // task alla volta.
    constexpr int RX_RING_SIZE = 3;
    int16_t rxRing[RX_RING_SIZE][FRAME_SAMPLES_16K];
    int rxRingCount = 0;
    int rxRingHead = 0;
    int rxRingTail = 0;

    void rxRingReset() {
        rxRingCount = 0;
        rxRingHead = 0;
        rxRingTail = 0;
    }

    struct State {
        volatile bool stopRequested = false;
        volatile bool taskRunning = false;
        String remoteHost;
        int remotePort = 0;
        uint16_t localPort = 0;
        volatile uint32_t loopMinMs = 0xFFFFFFFF;
        volatile uint32_t loopMaxMs = 0;
        // "underrun" = sia letture del microfono incomplete/in timeout
        // SIA il ring RX vuoto quando toccherebbe riprodurre un frame
        // (silenzio verso l'altoparlante); "overrun" = il ring RX era
        // pieno e si e' dovuto scartare il frame piu' vecchio per
        // farne entrare uno nuovo - vedi il piano, "strumentare da
        // subito invece di assumere".
        volatile uint32_t underrunCount = 0;
        volatile uint32_t overrunCount = 0;
        // Microfono muto (10/10) - manda silenzio al posto dell'audio
        // vero mentre attivo (vedi il TX nel ciclo del task sotto).
        volatile bool muted = false;
    };

    State state;
    TaskHandle_t taskHandle = nullptr;

    // Istanza I2S TUTTA DEL TASK per l'altoparlante (v2, 11/10) - stesso
    // schema di "prendi in prestito, usa, restituisci" gia' in uso in
    // WebRadioUI.cpp: Sound::playRawSamples() non si puo' riusare qui
    // perche' richiede Sound "pronto" (ready=true), che
    // pauseForExternalUse() mette apposta a false - serve quindi un
    // oggetto I2SClass separato, sugli stessi pin, inizializzato DOPO
    // che Sound ha rilasciato (vedi RtpAudio::start()) e chiuso PRIMA
    // che Sound lo riprenda (vedi RtpAudio::stop()).
    I2SClass outputI2s;
    bool outputI2sReady = false;

    // Media di coppie di campioni (NON "scarta uno su due" - vedi il
    // piano: scartare crea aliasing udibile, la media ha uno zero
    // proprio alla nuova frequenza di Nyquist ed e' gratis in CPU).
    void downsample16kTo8k(const int16_t* in, int16_t* out, int outSamples) {
        for (int i = 0; i < outSamples; i++) {
            int32_t sum = static_cast<int32_t>(in[2 * i]) + static_cast<int32_t>(in[2 * i + 1]);
            out[i] = static_cast<int16_t>(sum / 2);
        }
    }

    // Interpolazione lineare 8k->16k (NON duplicazione semplice - vedi
    // il piano, stesso motivo dell'aliasing per la media in downsample:
    // inserisce un campione intermedio vero tra due originali invece di
    // ripetere lo stesso valore due volte).
    void upsample8kTo16k(const int16_t* in, int16_t* out, int inSamples) {
        for (int i = 0; i < inSamples; i++) {
            int16_t a = in[i];
            int16_t b = (i + 1 < inSamples) ? in[i + 1] : a;
            out[2 * i] = a;
            out[2 * i + 1] = static_cast<int16_t>((static_cast<int32_t>(a) + b) / 2);
        }
    }

    void rxRingPush(const int16_t* frame) {
        if (rxRingCount == RX_RING_SIZE) {
            // Pieno: scarta il piu' vecchio per far posto al nuovo
            // (vedi il piano - mai lasciare crescere il ritardo).
            rxRingHead = (rxRingHead + 1) % RX_RING_SIZE;
            rxRingCount--;
            state.overrunCount++;
        }
        memcpy(rxRing[rxRingTail], frame, sizeof(int16_t) * FRAME_SAMPLES_16K);
        rxRingTail = (rxRingTail + 1) % RX_RING_SIZE;
        rxRingCount++;
    }

    bool rxRingPop(int16_t* outFrame) {
        if (rxRingCount == 0) {
            return false;
        }
        memcpy(outFrame, rxRing[rxRingHead], sizeof(int16_t) * FRAME_SAMPLES_16K);
        rxRingHead = (rxRingHead + 1) % RX_RING_SIZE;
        rxRingCount--;
        return true;
    }

    // G.711 mu-law (PCMU, payload type 0) - algoritmo standard CCITT/Sun
    // a segmenti logaritmici, calcolato (non una tabella da 256 voci
    // copiata a mano: stesso risultato, meno rischio di errore di
    // trascrizione).
    uint8_t linearToUlaw(int16_t pcm) {
        constexpr int BIAS = 0x84;
        constexpr int CLIP = 32635;

        int sign = (pcm < 0) ? 0x80 : 0x00;
        int magnitude = (pcm < 0) ? -static_cast<int>(pcm) : static_cast<int>(pcm);
        if (magnitude > CLIP) {
            magnitude = CLIP;
        }
        magnitude += BIAS;

        int seg = 7;
        for (int s = 0; s < 8; s++) {
            if ((magnitude >> (s + 8)) == 0) {
                seg = s;
                break;
            }
        }

        int mantissa = (magnitude >> (seg + 3)) & 0x0F;
        uint8_t ulawByte = static_cast<uint8_t>(~(sign | (seg << 4) | mantissa));
        if (ulawByte == 0) {
            ulawByte = 0x02; // ZEROTRAP: il byte 0x00 e' riservato dallo standard
        }
        return ulawByte;
    }

    // Decodifica mu-law (inversa dell'encode sopra) - formula standard
    // CCITT/Sun (tabella dei valori-base per segmento, non inventata:
    // ogni valore e' il doppio del precedente +132, lo stesso BIAS
    // usato in encode - verificato a mano prima di usarla, per essere
    // certi che faccia davvero "andata e ritorno" con linearToUlaw()).
    int16_t ulawToLinear(uint8_t ulawByte) {
        static const int16_t segmentBase[8] = {0, 132, 396, 924, 1980, 4092, 8316, 16764};
        uint8_t inverted = static_cast<uint8_t>(~ulawByte);
        int sign = inverted & 0x80;
        int exponent = (inverted >> 4) & 0x07;
        int mantissa = inverted & 0x0F;
        int sample = segmentBase[exponent] + (mantissa << (exponent + 3));
        return static_cast<int16_t>(sign ? -sample : sample);
    }

    // Decodifica A-law (PCMA, payload type 8) - l'altra variante G.711
    // offerta nel nostro SDP (vedi SipClient.cpp): mai codificata in TX
    // in questa fetta (si manda sempre mu-law), ma va comunque DECODIFICATA
    // in RX se il provider/l'altra parte sceglie di mandarcela in quel
    // formato, altrimenti si sentirebbe solo rumore.
    int16_t alawToLinear(uint8_t alawByte) {
        uint8_t toggled = static_cast<uint8_t>(alawByte ^ 0x55);
        int sign = toggled & 0x80;
        int exponent = (toggled >> 4) & 0x07;
        int mantissa = toggled & 0x0F;
        int sample;
        if (exponent == 0) {
            sample = (mantissa << 4) + 8;
        } else {
            sample = ((mantissa << 4) + 0x108) << (exponent - 1);
        }
        return static_cast<int16_t>(sign ? -sample : sample);
    }

    void audioTaskFn(void*) {
        WiFiUDP mediaUdp;
        mediaUdp.begin(state.localPort);

        outputI2s.setPins(BOARD_I2S_BCLK, BOARD_I2S_WS, BOARD_I2S_DOUT);
        outputI2sReady = outputI2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        if (!outputI2sReady) {
            DEBUG_PRINTLN("[RTPAUDIO] inizializzazione I2S altoparlante fallita - si procede senza riproduzione");
        }

        uint16_t seq = static_cast<uint16_t>(random(0, 65536));
        uint32_t timestamp = static_cast<uint32_t>(random(0, 2147483647));
        uint32_t ssrc = static_cast<uint32_t>(random(0, 2147483647));

        // Buffer TUTTI in RAM interna normale (stack del task o
        // file-scope, MAI PSRAM - vedi il piano: il framebuffer dello
        // schermo vive in PSRAM e un task a scadenza fissa sul core 0
        // che condividesse quel bus rischierebbe stalli sporadici
        // difficili da distinguere da bug del jitter buffer.
        int16_t micBuffer[FRAME_SAMPLES_16K];
        int16_t downBuffer[FRAME_SAMPLES_8K];
        uint8_t rtpPacket[12 + FRAME_SAMPLES_8K];
        uint8_t rtpInBuffer[512];
        int16_t decodeBuffer8k[FRAME_SAMPLES_8K];
        int16_t rxFrame[FRAME_SAMPLES_16K];

        rxRingReset();
        uint32_t lastLoopStartMs = millis();

        while (!state.stopRequested) {
            uint32_t loopStartMs = millis();
            uint32_t deltaMs = loopStartMs - lastLoopStartMs;
            lastLoopStartMs = loopStartMs;
            if (deltaMs < state.loopMinMs) {
                state.loopMinMs = deltaMs;
            }
            if (deltaMs > state.loopMaxMs) {
                state.loopMaxMs = deltaMs;
            }

            // (1a) Svuota TUTTI i pacchetti RTP disponibili in questo
            // giro (non solo uno: in caso di piccole raffiche di rete,
            // lasciarne in coda nel buffer del socket li farebbe solo
            // accumulare nei giri successivi) - decodifica G.711
            // (mu-law o A-law secondo il payload type del pacchetto,
            // vedi SipClient.cpp: offriamo entrambi nell'SDP) e
            // interpola a 16kHz prima di metterli nel ring.
            while (true) {
                int incomingSize = mediaUdp.parsePacket();
                if (incomingSize <= 0) {
                    break;
                }
                int len = mediaUdp.read(rtpInBuffer, min(incomingSize, static_cast<int>(sizeof(rtpInBuffer))));
                if (len < 12 + FRAME_SAMPLES_8K) {
                    continue; // pacchetto troppo corto per un frame G.711 completo, scarta
                }
                uint8_t payloadType = rtpInBuffer[1] & 0x7F;
                const uint8_t* payload = rtpInBuffer + 12;
                if (payloadType == 0) {
                    for (int i = 0; i < FRAME_SAMPLES_8K; i++) {
                        decodeBuffer8k[i] = ulawToLinear(payload[i]);
                    }
                } else if (payloadType == 8) {
                    for (int i = 0; i < FRAME_SAMPLES_8K; i++) {
                        decodeBuffer8k[i] = alawToLinear(payload[i]);
                    }
                } else {
                    continue; // codec non supportato in questa fetta, scarta
                }
                upsample8kTo16k(decodeBuffer8k, rxFrame, FRAME_SAMPLES_8K);
                rxRingPush(rxFrame);
            }

            // (1b) Riproduce UN frame dal ring verso l'altoparlante -
            // silenzio (nessuna scrittura) se il ring e' vuoto, invece
            // di inventare dati. BLOCCANTE (stesso ritmo reale dei 20ms
            // della lettura mic sotto) - vedi RtpAudio.h e la nota nel
            // piano sul non assumere che sia "rapido": strumentato con
            // loopMinMs/loopMaxMs per scoprirlo davvero sul dispositivo.
            if (outputI2sReady && rxRingPop(rxFrame)) {
                // Volume (10/10): riusa Sound::getVolumePercent() (vedi
                // RtpAudio.h) - lette una sola volta per frame, non per
                // campione, per non interrogare le NVS 320 volte a giro.
                int volumePercent = Sound::getVolumePercent();
                for (int i = 0; i < FRAME_SAMPLES_16K; i++) {
                    int16_t scaled = (volumePercent >= 100)
                        ? rxFrame[i]
                        : static_cast<int16_t>((static_cast<int32_t>(rxFrame[i]) * volumePercent) / 100);
                    int16_t left = scaled;
                    int16_t right = scaled;
                    outputI2s.write(&left, sizeof(left));
                    outputI2s.write(&right, sizeof(right));
                }
            } else {
                state.underrunCount++;
            }

            // (2) Lettura BLOCCANTE del microfono per un frame - scandisce
            // il ritmo reale dei 20ms di questo ciclo (vedi il piano).
            size_t got = Mic::readChunk(micBuffer, FRAME_SAMPLES_16K, MIC_READ_TIMEOUT_MS);
            if (got < static_cast<size_t>(FRAME_SAMPLES_16K)) {
                state.underrunCount++;
                memset(micBuffer, 0, sizeof(micBuffer)); // silenzio invece di dati parziali/spazzatura
            }
            if (state.muted) {
                // Microfono muto (10/10): manda silenzio al posto
                // dell'audio vero - il flusso RTP resta regolare
                // (nessun pacchetto saltato), cambia solo il contenuto.
                memset(micBuffer, 0, sizeof(micBuffer));
            }

            // (3) decima + codifica G.711 + spedisci.
            downsample16kTo8k(micBuffer, downBuffer, FRAME_SAMPLES_8K);

            rtpPacket[0] = 0x80; // V=2, P=0, X=0, CC=0
            rtpPacket[1] = 0x00; // M=0, PT=0 (PCMU)
            rtpPacket[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
            rtpPacket[3] = static_cast<uint8_t>(seq & 0xFF);
            rtpPacket[4] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
            rtpPacket[5] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
            rtpPacket[6] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
            rtpPacket[7] = static_cast<uint8_t>(timestamp & 0xFF);
            rtpPacket[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
            rtpPacket[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
            rtpPacket[10] = static_cast<uint8_t>((ssrc >> 8) & 0xFF);
            rtpPacket[11] = static_cast<uint8_t>(ssrc & 0xFF);
            for (int i = 0; i < FRAME_SAMPLES_8K; i++) {
                rtpPacket[12 + i] = linearToUlaw(downBuffer[i]);
            }

            mediaUdp.beginPacket(state.remoteHost.c_str(), state.remotePort);
            mediaUdp.write(rtpPacket, sizeof(rtpPacket));
            mediaUdp.endPacket();

            seq++;
            timestamp += FRAME_SAMPLES_8K;
        }

        mediaUdp.stop();
        if (outputI2sReady) {
            outputI2s.end();
            outputI2sReady = false;
        }
        state.taskRunning = false;
        vTaskDelete(nullptr);
    }
}

namespace RtpAudio {

    bool start(const String& remoteHost, int remotePort, int localPort) {
        if (state.taskRunning) {
            return false; // gia' attivo - il chiamante deve prima fermarlo
        }
        if (!Mic::begin()) {
            DEBUG_PRINTLN("[RTPAUDIO] Mic::begin() fallito, impossibile avviare il task audio");
            return false;
        }

        // Rilascia l'altoparlante PRIMA di avviare il task (v2, 11/10) -
        // stesso ordine obbligatorio gia' seguito da WebRadioUI.cpp:
        // il proprietario attuale deve liberare l'I2S prima che il
        // nuovo consumatore (qui, il task RTP) lo tocchi, altrimenti
        // i2s_new_channel() puo' trovarlo occupato e una libreria a
        // valle chiama ESP_ERROR_CHECK() su un handle invalido -> abort()
        // (vedi project_ble_deinit_crash in memoria per un crash reale
        // causato dall'ordine sbagliato in un contesto analogo).
        Sound::pauseForExternalUse();

        state.remoteHost = remoteHost;
        state.remotePort = remotePort;
        state.localPort = static_cast<uint16_t>(localPort);
        state.stopRequested = false;
        state.loopMinMs = 0xFFFFFFFF;
        state.loopMaxMs = 0;
        state.underrunCount = 0;
        state.overrunCount = 0;
        state.muted = false; // si azzera ad ogni nuova chiamata, mai persistito
        state.taskRunning = true;

        TaskHandle_t handle = nullptr;
        // Core 0 (come Input::touchTask/VpnConfig::sdMountTask - vedi il
        // piano): loop()/WiFi di Arduino girano su core 1 per via del
        // FQBN LoopCore=1,EventsCore=1. Stack a 8192 (non piu' 4096,
        // v2 11/10): il task ora tiene parecchi buffer in piu' (lettura
        // RTP in arrivo, decodifica, upsampling) oltre a quelli di v1.
        BaseType_t ok = xTaskCreatePinnedToCore(audioTaskFn, "rtpAudio", 8192, nullptr, 1, &handle, 0);
        if (ok != pdPASS) {
            state.taskRunning = false;
            Sound::resumeForOwnUse(); // il task non e' mai partito, restituisci subito l'altoparlante
            DEBUG_PRINTLN("[RTPAUDIO] xTaskCreatePinnedToCore fallito");
            return false;
        }
        taskHandle = handle;
        return true;
    }

    void stop() {
        if (!state.taskRunning) {
            return;
        }
        state.stopRequested = true;

        uint32_t startMs = millis();
        while (state.taskRunning && millis() - startMs < STOP_WAIT_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        taskHandle = nullptr;

        // Restituisce l'altoparlante a Sound SOLO dopo che il task si
        // e' davvero fermato (ha gia' chiuso la sua outputI2s da solo,
        // vedi audioTaskFn) - stesso ordine "prima il vecchio
        // proprietario rilascia" usato all'avvio, speculare.
        Sound::resumeForOwnUse();
    }

    bool isActive() {
        return state.taskRunning;
    }

    void setMuted(bool muted) {
        state.muted = muted;
    }

    bool isMuted() {
        return state.muted;
    }

    uint32_t getLoopMinMs() { return state.loopMinMs; }
    uint32_t getLoopMaxMs() { return state.loopMaxMs; }
    uint32_t getUnderrunCount() { return state.underrunCount; }
    uint32_t getOverrunCount() { return state.overrunCount; }
}
