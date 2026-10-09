#include "zl38063_audio_codec.h"

#include "b220_audio_share.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <driver/i2s_common.h>

#include <cstdint>

#define TAG "Zl38063AudioCodec"

/*
 * The wire format of the one I2S0 link this codec lives on.  Both directions are
 * 48 kHz / 16-bit / stereo, and both numbers are FIXED by the board bring-up:
 * B220_I2sUpBeforeDspBoot() initialises the port with these parameters before
 * the ZL38063 is reset and never reconfigures it afterwards (the DSP is an I2S
 * slave that needs BCLK/WS running from before its boot, so the format cannot be
 * changed later).  These two constants therefore only guard the calls below -
 * the actual configuration is the board's (config.h AUDIO_*_SAMPLE_RATE and
 * B220_I2S_SAMPLE_RATE_HZ).
 */
#define B220_CODEC_I2S_RATE_HZ   48000
#define B220_CODEC_I2S_SLOTS     2

/* One stereo frame = one L + one R 16-bit sample (4 bytes). */
#define B220_CODEC_FRAME_BYTES   (B220_CODEC_I2S_SLOTS * sizeof(int16_t))

/* 240 stereo frames = exactly one DMA block of the port's channel config
 * (dma_frame_num = 240 in B220_I2sUpBeforeDspBoot()), i.e. 5 ms of audio.  Reads
 * and writes are chunked to that size so a short/partial DMA transfer can never
 * split a stereo frame. */
#define B220_CODEC_BLOCK_FRAMES  240
#define B220_CODEC_BLOCK_BYTES   (B220_CODEC_BLOCK_FRAMES * B220_CODEC_FRAME_BYTES)

/* Bounded timeouts: one I2S call never blocks longer than its own timeout, and
 * one Read()/Write() call never blocks longer than its budget.  The audio input
 * and output tasks call these from a task that must stay responsive (a stalled
 * read must not wedge the audio service), while 5 ms of audio per DMA block
 * means 100 ms is already twenty times the data that can accumulate. */
#define B220_CODEC_READ_TIMEOUT_MS    100u
#define B220_CODEC_READ_BUDGET_US     200000   /* 200 ms per Read()  */
#define B220_CODEC_WRITE_TIMEOUT_MS   200u
#define B220_CODEC_WRITE_BUDGET_US    1000000  /* 1 s per Write()    */

Zl38063AudioCodec::Zl38063AudioCodec(int input_sample_rate, int output_sample_rate)
    : tx_handle_(b220_audio_i2s_tx()), rx_handle_(b220_audio_i2s_rx()) {
    /* One full-duplex port: the same I2S0 pair carries the DSP's uplink (mic
     * audio in) and downlink (speaker audio out). */
    duplex_ = true;
    input_reference_ = false;
    /* The I2S link is stereo, and the ZL38063 drives the SAME sample into both
     * slots (hardware-proven: the loopback log's RX hex line reads as repeating
     * pairs, "FFED FFED FFF2 FFF2 ...", and the 1 kHz tone the board wrote was
     * duplicated into L and R as well).  The service's stereo path - it feeds
     * the left channel to the wake word / encoder - is therefore lossless here,
     * which is why the declared input format is the honest interleaved stereo
     * instead of a de-interleaved mono. */
    input_channels_ = B220_CODEC_I2S_SLOTS;
    /* The other direction is mono: the Opus decoder is opened with
     * ESP_AUDIO_MONO at this codec's output rate, so Write() receives mono PCM
     * and duplicates it into both I2S slots (see Write() below). */
    output_channels_ = 1;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    ESP_LOGI(TAG, "ZL38063 audio codec: %d Hz in / %d Hz out, %d-bit, %d slot(s), "
                  "I2S0 borrowed from the board bring-up (bck 5 / ws 25 / dout 26 / "
                  "din 35)",
             input_sample_rate_, output_sample_rate_, (int)(8 * sizeof(int16_t)),
             B220_CODEC_I2S_SLOTS);
    if ((input_sample_rate_ != B220_CODEC_I2S_RATE_HZ) ||
        (output_sample_rate_ != B220_CODEC_I2S_RATE_HZ)) {
        ESP_LOGW(TAG,
                 "the codec was told %d/%d Hz but the I2S0 port runs at %d Hz "
                 "(B220_I2S_SAMPLE_RATE_HZ) - the audio service will resample, and "
                 "playback/capture would be pitch-shifted if it did not",
                 input_sample_rate_, output_sample_rate_, B220_CODEC_I2S_RATE_HZ);
    }
    if ((input_sample_rate_ != output_sample_rate_)) {
        ESP_LOGW(TAG, "asymmetric rates %d/%d Hz: the ZL38063 link is one shared "
                      "clock, both directions run at the same rate",
                 input_sample_rate_, output_sample_rate_);
    }

    ReportBringupOnce();
}

Zl38063AudioCodec::~Zl38063AudioCodec() {
    /* Deliberately empty.  The I2S0 channel pair belongs to the board bring-up -
     * and, more importantly, BCLK/WS must keep running for the ZL38063 until
     * power-off: the DSP is an I2S slave with no MCLK of its own, and stopping
     * the clock would drop its application.  Nothing here disables, deletes or
     * re-inits a channel. */
    ESP_LOGI(TAG, "Zl38063AudioCodec destroyed; the I2S0 channels are left enabled "
                  "(the ZL38063 needs BCLK/WS until power-off)");
}

void Zl38063AudioCodec::ReportBringupOnce() {
    if (reported_) {
        return;
    }
    reported_ = true;

    if ((tx_handle_ == nullptr) || (rx_handle_ == nullptr)) {
        ESP_LOGE(TAG,
                 "audio bring-up did NOT leave a usable I2S0 pair (tx=%p rx=%p): "
                 "B220_I2sUpBeforeDspBoot() failed or never ran.  This codec does "
                 "NOT create a channel of its own (a second i2s_new_channel() on "
                 "I2S0 would fail), so audio input returns silence (-1/0) and "
                 "playback is dropped; the rest of the firmware keeps running.",
                 (void*)tx_handle_, (void*)rx_handle_);
    } else {
        ESP_LOGI(TAG, "audio bring-up: I2S0 tx/rx handles are up (tx=%p rx=%p)",
                 (void*)tx_handle_, (void*)rx_handle_);
    }

    /* Never a reason to refuse to run - only worth a line in the log.  The real
     * ready indicator is reg 0x0014 == 0x200; see b220_audio_share.h. */
    if (!b220_audio_dsp_ready()) {
        ESP_LOGW(TAG,
                 "the ZL38063 application is not proven ready (reg 0x0014 != 0x0200 "
                 "or the read did not answer): the I2S link still runs, but the "
                 "uplink may carry the DSP's idle pattern and playback may be "
                 "inaudible - check the boot log's DSP release lines");
    }
}

void Zl38063AudioCodec::Start() {
    /* The base class reads the persisted volume and logs "Audio codec started". */
    AudioCodec::Start();
    ReportBringupOnce();
    ESP_LOGI(TAG, "ZL38063 codec started: %d Hz / %d-bit / %d slot(s); the DSP "
                  "application and its config record were brought up by the board "
                  "at boot, this class only moves PCM over I2S0",
             input_sample_rate_, (int)(8 * sizeof(int16_t)), B220_CODEC_I2S_SLOTS);
}

void Zl38063AudioCodec::SetOutputVolume(int volume) {
    /* Stored and persisted by the base class (Settings/"audio"/output_volume), so
     * the value survives a reboot and is visible in the UI - but it is NOT
     * applied to the audio path: the ZL38063's own output level lives in the DSP
     * (regs 0x238/0x23A, -90 dB + vol) and is written by the 1042-entry config
     * record / zl38063_dsp_set_volume() at boot, not by anything this codec can
     * reach over I2S.  A software volume here would additionally mean scaling
     * every playback sample on the ESP32, which the proven path does not do. */
    AudioCodec::SetOutputVolume(volume);
    ESP_LOGW(TAG,
             "SetOutputVolume(%d): stored, but NOT applied by this codec - the "
             "ZL38063 output volume is set in the DSP (regs 0x238/0x23A) by the "
             "config record loaded during the board's boot bring-up",
             volume);
}

void Zl38063AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    AudioCodec::EnableInput(enable);
    /* No i2s_channel_disable()/enable() here on purpose: the RX channel shares
     * BCLK/WS with TX, and the ZL38063 loses its application if the clock stops.
     * The channel stays enabled and Read() simply returns whatever the DSP is
     * driving; the flag above is all the audio service needs to know. */
    ESP_LOGI(TAG, "Set input enable to %s (the I2S0 RX channel is left ENABLED: its "
                  "BCLK/WS keep the ZL38063 clocked)",
             enable ? "true" : "false");
}

void Zl38063AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    AudioCodec::EnableOutput(enable);

    if (!enable) {
        /* Off is a no-op on the hardware.  The only way this board can switch the
         * speaker amplifier off is another write to AW9106B reg 0x02, and every
         * 0 -> 1 transition on bit 1 is a NEW SHDN rising edge that moves the
         * AW8737S out of mode 1 (and more than four edges kill the output
         * entirely).  Silence therefore comes from the DSP not being fed audio,
         * not from the PA - the proven sequence is one enable per boot. */
        ESP_LOGI(TAG, "Set output enable to false: the AW8737S PA is left ON "
                      "(a second SHDN edge would change its mode) - playback silence "
                      "comes from the DSP no longer being fed audio");
        return;
    }

    /* The one authorised entry point: the five-step SHDN sequence, latched after
     * the first call, so this is a logged no-op whenever the board's boot got
     * there first (which it does - both call sites are safe). */
    const esp_err_t pa = b220_audio_pa_enable_once("Zl38063AudioCodec::EnableOutput(true)");
    if (pa == ESP_OK) {
        ESP_LOGI(TAG, "Set output enable to true: the ZL38063 -> AW8737S PA -> speaker "
                      "leg is live (PA enable returned ESP_OK)");
    } else {
        ESP_LOGW(TAG, "Set output enable to true, but the AW8737S PA enable did not "
                      "complete cleanly (%s); the DSP link keeps running, the speaker "
                      "may stay silent - see the boot log's pa: lines",
                 esp_err_to_name(pa));
    }
}

int Zl38063AudioCodec::Read(int16_t* dest, int samples) {
    if ((dest == nullptr) || (samples <= 0)) {
        return 0;
    }
    if (rx_handle_ == nullptr) {
        ReportBringupOnce();
        return -1;   /* no RX channel: never touch a null handle */
    }

    /* A partial stereo frame would rotate L/R from here on, so the odd tail
     * sample of an odd request is dropped rather than written with a partner
     * that was never read.  The audio service always asks for whole frames
     * (samples * channels), so this is a guard, not a normal path. */
    if (samples & 1) {
        --samples;
    }

    int filled = 0;   /* int16 samples written to dest (interleaved L/R) */
    const int64_t deadline_us = esp_timer_get_time() + B220_CODEC_READ_BUDGET_US;

    while (filled < samples) {
        size_t want = (size_t)(samples - filled) * sizeof(int16_t);
        if (want > B220_CODEC_BLOCK_BYTES) {
            want = B220_CODEC_BLOCK_BYTES;   /* at most one DMA block per call */
        }
        size_t got = 0;
        const esp_err_t err = i2s_channel_read(
            rx_handle_, (uint8_t*)dest + ((size_t)filled * sizeof(int16_t)), want, &got,
            B220_CODEC_READ_TIMEOUT_MS);
        if (err == ESP_ERR_TIMEOUT) {
            /* No data inside the timeout.  Not an error: the RX DMA is clocked by
             * TX, so this means the channel is not running (or the DSP is not
             * driving DIN at all).  Return whatever was collected. */
            break;
        }
        if (err != ESP_OK) {
            if (filled == 0) {
                if (!rx_warned_) {
                    rx_warned_ = true;
                    ESP_LOGE(TAG, "Read: i2s_channel_read failed (%s); audio input "
                                  "returns silence until this clears",
                             esp_err_to_name(err));
                }
                return -1;
            }
            break;
        }
        if (got < B220_CODEC_FRAME_BYTES) {
            break;   /* not even one whole stereo frame */
        }
        filled += (int)(got / sizeof(int16_t)) & ~1;   /* whole frames only */
        if (got < want) {
            break;   /* short block: stop instead of spinning for the rest */
        }
        if (esp_timer_get_time() >= deadline_us) {
            break;   /* bounded: hand back what we have, the task must stay live */
        }
    }

    if ((filled == 0) && !rx_warned_) {
        rx_warned_ = true;
        ESP_LOGW(TAG, "Read: no RX data within %u ms (the DSP is not driving the "
                      "uplink, or the RX channel is not enabled) - returning silence",
                 (unsigned)(B220_CODEC_READ_BUDGET_US / 1000));
    }
    return filled;
}

int Zl38063AudioCodec::Write(const int16_t* data, int samples) {
    if ((data == nullptr) || (samples <= 0)) {
        return 0;
    }
    if (tx_handle_ == nullptr) {
        ReportBringupOnce();
        return -1;   /* no TX channel: never touch a null handle */
    }

    int consumed = 0;   /* mono input samples accepted from the caller */
    const int64_t deadline_us = esp_timer_get_time() + B220_CODEC_WRITE_BUDGET_US;

    while (consumed < samples) {
        int chunk = samples - consumed;
        if (chunk > kTxChunkFrames) {
            chunk = kTxChunkFrames;
        }
        /* The DSP's downlink is stereo and the decoder's PCM is mono: the same
         * sample goes into both slots.  That is exactly what the proven 1 kHz
         * tone test wrote (B220_ToneRenderBlock(), "the same sample, no channel
         * swap"), so this cannot be the difference between sound and no sound. */
        for (int i = 0; i < chunk; ++i) {
            const int16_t s = data[consumed + i];
            tx_block_[2 * i] = s;
            tx_block_[2 * i + 1] = s;
        }

        const size_t want = (size_t)chunk * B220_CODEC_FRAME_BYTES;
        size_t wrote = 0;
        const esp_err_t err =
            i2s_channel_write(tx_handle_, tx_block_, want, &wrote, B220_CODEC_WRITE_TIMEOUT_MS);
        if (err != ESP_OK) {
            if (consumed == 0) {
                if (!tx_warned_) {
                    tx_warned_ = true;
                    ESP_LOGE(TAG, "Write: i2s_channel_write failed (%s); playback is "
                                  "dropped until this clears",
                             esp_err_to_name(err));
                }
                return -1;
            }
            break;
        }
        if (wrote < B220_CODEC_FRAME_BYTES) {
            break;   /* nothing whole reached the DMA */
        }
        consumed += (int)(wrote / B220_CODEC_FRAME_BYTES);
        if (wrote < want) {
            break;   /* short block: do not spin for the remainder */
        }
        if (esp_timer_get_time() >= deadline_us) {
            break;   /* bounded: the output task must not block for ever */
        }
    }

    return consumed;
}
