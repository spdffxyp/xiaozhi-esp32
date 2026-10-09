#ifndef _ZL38063_AUDIO_CODEC_H
#define _ZL38063_AUDIO_CODEC_H

#include "audio_codec.h"

/*
 * The production audio codec of the XiaoMoDou B220: it is the ESP32 side of the
 * one full-duplex I2S0 link to the ZL38063 AcuEdge DSP.
 *
 *   ESP32 (I2S master, 48 kHz / 16-bit / stereo, bck 5 / ws 25 / dout 26 /
 *   din 35) <-> ZL38063 (I2S slave, cross-point + internal DAC) -> AW8737S PA
 *   -> speaker.
 *
 * There is no codec chip on I2C here: the ZL38063 IS the codec.  Its
 * application image, the 1042-entry config record and its volume/mute
 * registers are loaded by the board's boot bring-up (zl38063_dsp.c), which is
 * also the only thing that can talk to it over HBI.  This class is therefore
 * deliberately thin - it moves PCM over the I2S link the bring-up already
 * created and switches the speaker amplifier on:
 *
 *   - it never calls i2s_new_channel(): I2S0 is created ONCE, by
 *     B220_I2sUpBeforeDspBoot() in xiaomodou_b220.cc, BEFORE the DSP reset,
 *     because the DSP takes its clock from that port's BCLK.  The handles are
 *     borrowed through b220_audio_share.h (b220_audio_i2s_tx/rx()).
 *   - it never disables the channels either: BCLK/WS must run continuously, and
 *     a stopped clock drops the DSP.  EnableInput()/EnableOutput() therefore
 *     only track the service's state; the hardware stays up.
 *   - Read()  = i2s_channel_read()  on RX, interleaved L/R at 48 kHz.
 *   - Write() = i2s_channel_write() on TX, the mono PCM the audio service
 *     decodes duplicated into both slots (the DSP drives DIN with the same
 *     sample in L and R, and the proven 1 kHz tone test wrote it the same way).
 *   - EnableOutput(true) issues the one AW8737S SHDN enable sequence of the
 *     boot through b220_audio_pa_enable_once() (idempotent; the board's boot
 *     normally got there first).  EnableOutput(false) deliberately leaves the
 *     amplifier alone - see the .cc.
 *
 * The class is safe with a failed bring-up: the handle getters return NULL and
 * Read()/Write() then log once and return silence (0 / -1) without touching
 * anything.
 */
class Zl38063AudioCodec : public AudioCodec {
public:
    Zl38063AudioCodec(int input_sample_rate, int output_sample_rate);
    virtual ~Zl38063AudioCodec();

    virtual void Start() override;
    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;

private:
    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

    /* One log line per boot about what the board bring-up left behind. */
    void ReportBringupOnce();

    /* Borrowed from the board bring-up; never created, deleted or disabled here. */
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    /* Mono samples of the caller's stream per I2S chunk: 240 stereo frames =
     * one DMA block of the port's configuration (dma_frame_num = 240), so every
     * i2s_channel_write() hands the driver exactly one descriptor's worth.
     * The staging buffer is a fixed member, not a per-call allocation: the
     * audio output path must not allocate on every 60 ms frame. */
    static constexpr int kTxChunkFrames = 240;
    int16_t tx_block_[kTxChunkFrames * 2];

    bool reported_ = false;   /* bring-up verdict already logged  */
    bool rx_warned_ = false;  /* the "no RX data" warning, once   */
    bool tx_warned_ = false;  /* the "TX write failed" error, once */
};

#endif  // _ZL38063_AUDIO_CODEC_H
