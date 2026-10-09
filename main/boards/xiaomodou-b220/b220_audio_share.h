#ifndef _B220_AUDIO_SHARE_H
#define _B220_AUDIO_SHARE_H

/*
 * The one interface between the board bring-up (xiaomodou_b220.cc) and the
 * production audio codec (zl38063_audio_codec.cc/.h) of this board.
 *
 * WHY IT EXISTS: the board owns the single full-duplex I2S0 channel of this
 * design.  B220_I2sUpBeforeDspBoot() (xiaomodou_b220.cc) creates it BEFORE the
 * ZL38063 is reset, because the DSP is an I2S slave that takes its clock from
 * this port's BCLK and answers nothing over HBI until BCLK/WS are running.  The
 * codec must therefore NOT create a channel of its own - i2s_new_channel() can
 * only succeed once per port - so the board hands the already-created handles
 * out through the accessors below and the codec borrows them for its whole life.
 *
 * The accessors are also what makes the codec safe when the bring-up failed:
 * both handle getters return NULL in that case (and in any build with
 * B220_I2S_BEFORE_DSP 0, where no channel is ever created), and the codec then
 * logs once and returns silence instead of touching a null handle.
 *
 * Declared in a C-compatible header on purpose: b220_audio_share.h is included
 * by the board file (which defines the accessors) and by the C++ codec (which
 * calls them), so a signature drift is a compile error rather than a link-time
 * surprise.
 */

#include <stdbool.h>

#include <driver/i2s_std.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The already-created I2S0 channel handles of B220_I2sUpBeforeDspBoot():
 * 48 kHz, 16-bit, stereo, both channels left ENABLED so BCLK/WS keep running
 * for the ZL38063 (the DSP needs them from before its reset until power-off).
 *
 * The codec must never delete, disable or re-init them; it only reads and
 * writes through them.  Either one is NULL when the I2S bring-up did not run or
 * did not succeed (the board's RX enable is deliberately non-fatal, so a NULL
 * rx handle with a valid tx handle is a legal state: TX alone keeps the clock).
 */
i2s_chan_handle_t b220_audio_i2s_tx(void);
i2s_chan_handle_t b220_audio_i2s_rx(void);

/*
 * Did the ZL38063 application come up?  Set once per boot by the board, right
 * after the DSP bring-up, from a READ-ONLY HBI read of reg 0x0014: that is the
 * register the stock firmware's own volume/route routine gates its route writes
 * on (it proceeds only when 0x0014 reads 0x200), while reg 0x0030 ("app
 * status") is never updated by this product's application and says nothing.
 *
 * False means "not proven", not "known dead": the HBI read may simply never
 * have been possible (no reset/boot ran, or the HBI device was never attached).
 * The codec treats it as a warning, never as a reason to refuse to run - the
 * I2S handles above are the only thing it really needs.
 */
bool b220_audio_dsp_ready(void);

/*
 * The ONE AW8737S SHDN enable sequence of this boot (b220_pa_enable_one_pulse()
 * in xiaomodou_b220.cc): 0x02 <- 0x00, hold >= 2 ms, 0x02 <- 0x02 = exactly one
 * rising edge, which selects amplifier mode 1 (1.2 W).
 *
 * Idempotent by construction - the board latches the sequence - so the board's
 * own boot-time call and the codec's EnableOutput(true) can both ask for it and
 * the second caller only gets the logged "already ran this boot" warning plus
 * the first call's result.  It must never be replaced by a bare
 * "reg 0x02 <- 0x02": a second 0 -> 1 edge moves the amplifier out of mode 1.
 */
esp_err_t b220_audio_pa_enable_once(const char* tag);

#ifdef __cplusplus
}
#endif

#endif  // _B220_AUDIO_SHARE_H
