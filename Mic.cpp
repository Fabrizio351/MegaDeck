#include "Mic.h"
#include "Config.h"
#include "Input.h"

#include <Wire.h>
#include <driver/i2s_std.h>
#include <es7210.h>

namespace {

    constexpr uint32_t SAMPLE_RATE = 16000;
    constexpr i2s_port_t MIC_I2S_PORT = I2S_NUM_1; // porta diversa da quella dell'altoparlante (I2S_NUM_0/AUTO)
    constexpr uint32_t I2C_LOCK_TIMEOUT_MS = 200;

    i2s_chan_handle_t rxHandle = nullptr;
    bool ready = false;

    // Presa del mutex I2C condiviso con tastiera/touch (29/09, vedi
    // Input::lockI2C()) - causa sospetta di un blocco intermittente
    // osservato dal vivo: senza questo lock, una transazione I2C verso
    // l'ES7210 poteva sovrapporsi a una lettura tastiera/touch in corso
    // su un altro task e incastrare il bus (nessuna delle due parti
    // rilascia mai SDA/SCL). Solo l'INIZIALIZZAZIONE tocca I2C (la
    // cattura vera e propria e' I2S puro, bus diverso) quindi il lock si
    // prende una volta sola qui, non ad ogni campione catturato.
    bool initEs7210() {
        if (!Input::lockI2C(I2C_LOCK_TIMEOUT_MS)) {
            DEBUG_PRINTLN("[MIC] impossibile ottenere il mutex I2C (bus occupato da tastiera/touch)");
            return false;
        }

        audio_hal_codec_config_t cfg = {};
        cfg.adc_input = AUDIO_HAL_ADC_INPUT_ALL;
        cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
        cfg.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;
        cfg.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
        cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
        cfg.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_16BITS;

        uint32_t err = ESP_OK;
        err |= es7210_adc_init(&Wire, &cfg);
        err |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
        err |= es7210_adc_set_gain(
            static_cast<es7210_input_mics_t>(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2),
            GAIN_0DB
        );
        err |= es7210_adc_set_gain(
            static_cast<es7210_input_mics_t>(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4),
            GAIN_37_5DB
        );
        err |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);

        Input::unlockI2C();

        if (err != ESP_OK) {
            DEBUG_PRINTLN("[MIC] es7210_adc_* fallito - il codec risponde su I2C?");
            return false;
        }
        return true;
    }

    bool initI2sRx() {
        i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
        if (i2s_new_channel(&chanCfg, nullptr, &rxHandle) != ESP_OK) {
            DEBUG_PRINTLN("[MIC] i2s_new_channel (RX) fallito");
            return false;
        }

        i2s_std_config_t stdCfg = {};
        stdCfg.clk_cfg.sample_rate_hz = SAMPLE_RATE;
        stdCfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
        stdCfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
        stdCfg.gpio_cfg.mclk = static_cast<gpio_num_t>(BOARD_ES7210_MCLK);
        stdCfg.gpio_cfg.bclk = static_cast<gpio_num_t>(BOARD_ES7210_SCK);
        stdCfg.gpio_cfg.ws = static_cast<gpio_num_t>(BOARD_ES7210_LRCK);
        stdCfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
        stdCfg.gpio_cfg.din = static_cast<gpio_num_t>(BOARD_ES7210_DIN);

        if (i2s_channel_init_std_mode(rxHandle, &stdCfg) != ESP_OK) {
            DEBUG_PRINTLN("[MIC] i2s_channel_init_std_mode (RX) fallito");
            return false;
        }
        if (i2s_channel_enable(rxHandle) != ESP_OK) {
            DEBUG_PRINTLN("[MIC] i2s_channel_enable (RX) fallito");
            return false;
        }
        return true;
    }

}

namespace Mic {

    bool begin() {
        if (ready) {
            return true;
        }

        Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);

        if (!initEs7210()) {
            return false;
        }
        if (!initI2sRx()) {
            return false;
        }

        ready = true;
        DEBUG_PRINTLN("[MIC] inizializzato (ES7210 + I2S RX su I2S_NUM_1)");
        return true;
    }

    size_t captureSeconds(int16_t* buffer, size_t bufferCapacitySamples, int seconds) {
        if (!ready || buffer == nullptr || seconds <= 0) {
            return 0;
        }

        size_t wantedSamples = static_cast<size_t>(seconds) * SAMPLE_RATE;
        size_t targetSamples = min(wantedSamples, bufferCapacitySamples);
        size_t captured = 0;

        while (captured < targetSamples) {
            size_t bytesToRead = (targetSamples - captured) * sizeof(int16_t);
            size_t bytesRead = 0;
            esp_err_t err = i2s_channel_read(rxHandle, buffer + captured, bytesToRead, &bytesRead, 1000);
            if (err != ESP_OK) {
                DEBUG_PRINTF("[MIC] i2s_channel_read errore=%d dopo %u campioni\n", err, static_cast<unsigned>(captured));
                break;
            }
            captured += bytesRead / sizeof(int16_t);
        }

        return captured;
    }
}
