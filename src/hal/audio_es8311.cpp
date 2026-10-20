// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Andres Leroi
#include "audio_if.h"

#include <LilyGoLib.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>

#include <ft8_decoder.h>

#include "bsp_codec/esp_codec.h"

namespace {

constexpr int kCodecRate = 48000;
constexpr int kFactor = kCodecRate / ft8::kSampleRate;  // 4
constexpr int kTaps = 48;

// Both codecs run at 48 kHz, a rate every ES8311 clock configuration supports;
// the FT8 engine works at 12 kHz. One low-pass filter serves both directions.
class Resampled48k : public AudioIo {
public:
    bool read(int16_t *samples, size_t count) override
    {
        if (!_ok) {
            return false;
        }
        size_t done = 0;
        while (done < count) {
            size_t n = min(count - done, (size_t)kChunk);
            if (!readRaw(_io, n * kFactor)) {
                return false;
            }
            for (size_t i = 0; i < n; ++i) {
                // Shift four new input samples into the history, then filter once
                memmove(_rx_hist, _rx_hist + kFactor, (kTaps - kFactor) * sizeof(float));
                for (int k = 0; k < kFactor; ++k) {
                    _rx_hist[kTaps - kFactor + k] = _io[i * kFactor + k];
                }
                float acc = 0;
                for (int t = 0; t < kTaps; ++t) {
                    acc += _fir[t] * _rx_hist[t];
                }
                samples[done + i] = (int16_t)constrain(acc, -32768.0f, 32767.0f);
            }
            done += n;
        }
        return true;
    }

    bool write(const int16_t *samples, size_t count) override
    {
        if (!_ok) {
            return false;
        }
        size_t done = 0;
        while (done < count) {
            size_t n = min(count - done, (size_t)kChunk);
            for (size_t i = 0; i < n; ++i) {
                // Zero-stuffing interpolation: one input sample, three zeros, filtered
                for (int k = 0; k < kFactor; ++k) {
                    memmove(_tx_hist, _tx_hist + 1, (kTaps - 1) * sizeof(float));
                    _tx_hist[kTaps - 1] = (k == 0) ? samples[done + i] * (float)kFactor : 0.0f;
                    float acc = 0;
                    for (int t = 0; t < kTaps; ++t) {
                        acc += _fir[t] * _tx_hist[t];
                    }
                    _io[i * kFactor + k] = (int16_t)constrain(acc, -32768.0f, 32767.0f);
                }
            }
            if (!writeRaw(_io, n * kFactor)) {
                return false;
            }
            done += n;
        }
        return true;
    }

    // I2S DMA buffering plus the filter delay; refined by comparing DT against WSJT-X
    int outputLatencyMs() const override { return 40; }

protected:
    void makeFilter()
    {
        // Windowed-sinc low-pass, cutoff 5 kHz at 48 kHz
        const float fc = 5000.0f / kCodecRate;
        float sum = 0;
        for (int i = 0; i < kTaps; ++i) {
            float x = i - (kTaps - 1) / 2.0f;
            float sinc = (x == 0) ? 2 * fc : sinf(2 * PI * fc * x) / (PI * x);
            float window = 0.54f - 0.46f * cosf(2 * PI * i / (kTaps - 1));
            _fir[i] = sinc * window;
            sum += _fir[i];
        }
        for (float &c : _fir) {
            c /= sum;
        }
    }

    /// Moves count samples at 48 kHz between the codec and buf.
    virtual bool readRaw(int16_t *buf, size_t count) = 0;
    virtual bool writeRaw(const int16_t *buf, size_t count) = 0;

    bool _ok = false;

private:
    static constexpr int kChunk = 240;  // 20 ms at 12 kHz
    float _fir[kTaps];
    float _rx_hist[kTaps] = {0};
    float _tx_hist[kTaps] = {0};
    int16_t _io[kChunk * kFactor];
};

// --- built-in codec ---------------------------------------------------------------------

class Es8311Audio : public Resampled48k {
public:
    bool begin() override
    {
        makeFilter();
        _ok = instance.codec.open(16, 1, kCodecRate) == 0;
        setOutputEnabled(false);
        return _ok;
    }

    void setOutputEnabled(bool on) override
    {
        // The codec keeps the speaker amplifier on while the stream is open;
        // it is only wanted while transmitting.
        instance.io.digitalWrite(EXPANDS_AMP_EN, on ? HIGH : LOW);
    }

    void setInputGain(float db) override { instance.codec.setGain(db); }
    void setOutputVolume(int percent) override { instance.codec.setVolume(constrain(percent, 0, 100)); }

protected:
    bool readRaw(int16_t *buf, size_t count) override
    {
        return instance.codec.read((uint8_t *)buf, count * sizeof(int16_t)) == 0;
    }

    bool writeRaw(const int16_t *buf, size_t count) override
    {
        return instance.codec.write((uint8_t *)buf, count * sizeof(int16_t)) == 0;
    }
};

// --- interface board on the 12-pin port -------------------------------------------------

// The board carries its own ES8311 at 0x19 on the shared I2C bus. It has no
// MCLK and takes its clock from BCLK. The port offers a single pin for audio
// data, so an analogue switch on the board connects it to the ADC output while
// receiving and to the DAC input while the PTT line (XL9555 P11, pin 10) is set.
// Here the pin is an input while receiving and carries the I2S output only
// while transmitting; the engine keys the PTT before it enables the output and
// unkeys after it disabled it, so the pin and the codec never drive each other.
constexpr uint8_t kBoardAddr = 0x19;
constexpr gpio_num_t kBclk = GPIO_NUM_43;  // port pin 3
constexpr gpio_num_t kLrck = GPIO_NUM_44;  // port pin 4
constexpr gpio_num_t kData = GPIO_NUM_9;   // port pin 8

class InterfaceAudio : public Resampled48k {
public:
    bool begin() override
    {
        makeFilter();
        Wire.beginTransmission(kBoardAddr);
        if (Wire.endTransmission() != 0) {
            return false;
        }

        // I2S1; the built-in codec has I2S0. TX and RX share the clocks, which run all the time.
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true;  // silence when nothing is written
        if (i2s_new_channel(&chan_cfg, &_tx, &_rx) != ESP_OK) {
            return false;
        }
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kCodecRate),
            .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = kBclk,
                .ws = kLrck,
                .dout = I2S_GPIO_UNUSED,  // routed to the data pin only while transmitting
                .din = kData,
                .invert_flags = {false, false, false},
            },
        };
        if (i2s_channel_init_std_mode(_tx, &std_cfg) != ESP_OK || i2s_channel_init_std_mode(_rx, &std_cfg) != ESP_OK) {
            return false;
        }

        audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_1, .rx_handle = _rx, .tx_handle = _tx};
        const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
        audio_codec_i2c_cfg_t i2c_cfg = {.port = 0, .addr = kBoardAddr, .bus_handle = (void *)&Wire};
        const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
        const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
        if (data_if == nullptr || ctrl_if == nullptr || gpio_if == nullptr) {
            return false;
        }
        es8311_codec_cfg_t es_cfg = {};
        es_cfg.ctrl_if = ctrl_if;
        es_cfg.gpio_if = gpio_if;
        es_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
        es_cfg.pa_pin = -1;          // line output, no amplifier
        es_cfg.master_mode = false;
        es_cfg.use_mclk = false;     // clock from BCLK, multiplied inside the codec
        es_cfg.hw_gain.pa_voltage = 3.3f;
        es_cfg.hw_gain.codec_dac_voltage = 3.3f;
        const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
        if (codec_if == nullptr) {
            return false;
        }
        esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = codec_if, .data_if = data_if};
        _dev = esp_codec_dev_new(&dev_cfg);
        if (_dev == nullptr) {
            return false;
        }
        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16;
        fs.channel = 1;
        fs.sample_rate = kCodecRate;
        if (esp_codec_dev_open(_dev, &fs) != ESP_CODEC_DEV_OK) {
            return false;
        }
        setOutputEnabled(false);
        _ok = true;
        return true;
    }

    void setOutputEnabled(bool on) override
    {
        if (on) {
            gpio_set_direction(kData, GPIO_MODE_INPUT_OUTPUT);
            esp_rom_gpio_connect_out_signal(kData, I2S1O_SD_OUT_IDX, false, false);
            _tx_on = true;
            _tx_peak = 0;
        } else {
            if (_tx_on && _tx_peak == 0) {
                Serial.println("[if] transmitted, but the data pin carried no audio");
            }
            _tx_on = false;
            // Also detaches the I2S output from the pin; the input path stays connected
            gpio_set_direction(kData, GPIO_MODE_INPUT);
        }
    }

    void setInputGain(float db) override
    {
        if (_dev != nullptr) {
            esp_codec_dev_set_in_gain(_dev, db);
        }
    }

    void setOutputVolume(int percent) override
    {
        if (_dev != nullptr) {
            esp_codec_dev_set_out_vol(_dev, constrain(percent, 0, 100));
        }
    }

protected:
    bool readRaw(int16_t *buf, size_t count) override
    {
        return esp_codec_dev_read(_dev, buf, count * sizeof(int16_t)) == ESP_CODEC_DEV_OK;
    }

    bool writeRaw(const int16_t *buf, size_t count) override
    {
        bool ok = esp_codec_dev_write(_dev, (void *)buf, count * sizeof(int16_t)) == ESP_CODEC_DEV_OK;
        if (_tx_on) {
            // While transmitting, the input still listens on the data pin: what it reads
            // is what the pin carries, which shows that the output reaches it
            static int16_t probe[256];
            size_t got = 0;
            if (i2s_channel_read(_rx, probe, sizeof(probe), &got, 0) == ESP_OK) {
                for (size_t i = 0; i < got / sizeof(int16_t); ++i) {
                    _tx_peak = max(_tx_peak, abs((int)probe[i]));
                }
            }
        }
        return ok;
    }

private:
    bool _tx_on = false;
    int _tx_peak = 0;
    i2s_chan_handle_t _tx = nullptr, _rx = nullptr;
    esp_codec_dev_handle_t _dev = nullptr;
};

Es8311Audio audio;
InterfaceAudio board_audio;

}  // namespace

bool audio_on_board = false;

AudioIo &es8311Audio()
{
    return audio;
}

bool interfaceBoardPresent()
{
    Wire.beginTransmission(kBoardAddr);
    return Wire.endTransmission() == 0;
}

AudioIo &interfaceAudio()
{
    return board_audio;
}
