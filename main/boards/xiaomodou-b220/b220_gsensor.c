/*
 * b220_gsensor.c - the B220's accelerometer (SC7A20H / LIS3DH compatible), read on demand.
 * See b220_gsensor.h for what this is, what it deliberately does not do, and the units.
 */

#include "b220_gsensor.h"

/* The board publishes its I2C0 bus through the LED module's accessor, which is defined
 * in EVERY build (the button-discovery one exists only in discovery builds).  Resolved
 * lazily, so this module does not depend on the backlight bring-up having run first. */
#include "b220_led.h"

#include <stdio.h>
#include <string.h>

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char* TAG = "b220-gs";

/* ------------------------------------------------------------------ registers ---
 * All from the reference port (components/modou_drivers/modou_gsensor.c, .h), which names the
 * part and the values; the register meanings are the LIS3DH family's.
 */
#define B220_GS_ADDR 0x19
#define B220_GS_WHO_AM_I 0x0f
#define B220_GS_WHO_AM_I_VALUE 0x11
#define B220_GS_CTRL_REG1 0x20 /* 0x47 = 100 Hz, X/Y/Z enabled */
#define B220_GS_CTRL_REG1_VALUE 0x47
/*
 * CTRL_REG4 = 0x08: +-2g full scale, which is what the reference port writes and what this
 * unit answers with real numbers (it produced a clean +-1000 mg on one axis at a time).
 *
 * DO NOT ADD BDU HERE.  The LIS3DH datasheet puts block-data-update in bit 7, so this file
 * briefly wrote 0x88 to stop a six-byte read from spanning two samples.  On THIS part - an
 * SC7A20H, register-compatible but not an LIS3DH - that made every read come back near zero:
 * with 0x88 the magnitude never reached the 500 mg floor and {BASE+ACCEL} answered
 * "moving (or no valid sample)" for every attempt.  Whatever bit 7 does here, it is not the
 * datasheet's BDU, and a value the reference port never wrote is not the place to experiment.
 * The reference's 0x08 goes back, and any sample-coherency handling has to be done in software
 * (the status flag), not by trusting a bit that behaves differently on this clone.
 */
#define B220_GS_CTRL_REG4 0x23
#define B220_GS_CTRL_REG4_VALUE 0x08
#define B220_GS_STATUS 0x27 /* bit3 (ZYXDA) = a new sample is ready */
#define B220_GS_STATUS_ZYXDA 0x08
/* Output registers start at X_L (0x28); bit 7 asks the LIS3DH family for auto-increment, so
 * one six-byte read returns X, Y and Z in order. */
#define B220_GS_OUT_AUTO_INC 0xa8
#define B220_GS_OUT_BYTES 6

/* 100 kHz, the speed the AW9106B on the same bus is registered at. */
/*
 * COUNTS PER G, AND WHY IT IS STILL TAKEN FROM EACH SAMPLE.
 *
 * The bench readings that drove this: the same 1 g, with the unit held still in different
 * orientations, came back as raw 18096, 15984, 16432, 6784 and 16000 - always on whichever axis
 * pointed down.
 *
 * THE DATASHEET (Silan SC7A20H, v0.7) then settled the format: the part outputs 12-BIT data, and
 * its own worked example gives 0x4000 = 1024 counts = 1.0 g at FS=10 (+-8g) / 4.0 g at FS=00
 * (+-2g).  The twelve significant bits therefore sit in the HIGH half of the 16-bit register pair
 * and the low four are discarded - this file does that shift first - after which the bench
 * measured 998 and 1020 counts for 1 g, against the datasheet's 1024 for the +-8g range.
 *
 * WHAT THE DATASHEET DOES NOT EXPLAIN, and why the scale is still taken from the sample: this
 * firmware writes CTRL_REG4 = 0x08, which the datasheet reads as FS[1:0]=00 i.e. +-2g (and 0x08
 * is what the reference port for this toy writes too), yet the measured sensitivity is the one
 * the datasheet gives for +-8g.  Adding the datasheet's BDU bit (0x88) was tried and made every
 * reading collapse to near zero, which the datasheet does not predict either.  Rather than pick
 * a scale from a register this part does not appear to honour, the scale comes from the reading:
 * the largest axis of a resting sample IS 1 g, by definition of gravity.
 *
 * The window below only has to reject a reading that is obviously not a resting 1 g - a part
 * that has not produced a sample yet, or a unit being waved around - and within it the exact
 * scale does not matter for the orientation, which is a comparison between axes.
 */
#define B220_GS_COUNTS_PER_G_MIN 500
#define B220_GS_COUNTS_PER_G_MAX 8000
#define B220_GS_TYPICAL_COUNTS_PER_G 1024 /* the datasheet's 12-bit figure for 1 g */

#define B220_GS_SCL_HZ 100000
#define B220_GS_XFER_TIMEOUT_MS 100

/* --------------------------------------------------------------------- state --- */
static i2c_master_dev_handle_t s_dev;
static bool s_ready;
static uint8_t s_who_am_i;
/* CTRL_REG4 as it reads back.  Reported for the record: the datasheet picks the mg-per-count
 * scale from this register's FS[1:0], and what this part actually produces does not match what
 * was written to it, so the read-back belongs in the log. */
static uint8_t s_ctrl4 = 0x08;
static SemaphoreHandle_t s_lock;

/*
 * The bus is shared with the AW9106B (LCD backlight, audio PA SHDN) and with the LED driver.
 * The ESP-IDF I2C master driver makes each individual transaction atomic, but that is not the
 * same as making a read-modify-write safe, and it also does not stop this module's probe from
 * interleaving with the PA sequence at boot.  One mutex around everything this file does is
 * cheap and removes the question.
 */
static bool lock_take(void) {
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == nullptr) {
            return false;
        }
    }
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE;
}

static void lock_give(void) {
    if (s_lock != nullptr) {
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t write_reg(uint8_t reg, uint8_t value) {
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), B220_GS_XFER_TIMEOUT_MS);
}

static esp_err_t read_regs(uint8_t reg, uint8_t* out, size_t len) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, B220_GS_XFER_TIMEOUT_MS);
}

esp_err_t b220_gsensor_init(void) {
    if (s_ready) {
        return ESP_OK;
    }
    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    i2c_master_bus_handle_t bus = b220_led_i2c_bus();
    if (bus == nullptr) {
        ESP_LOGW(TAG, "no I2C bus yet (the backlight bring-up publishes it); no motion data");
        lock_give();
        return ESP_ERR_INVALID_STATE;
    }

    if (s_dev == nullptr) {
        /* Probe before adding: a board without the part must not leave a device handle behind
         * that every later transaction then times out on. */
        err = i2c_master_probe(bus, B220_GS_ADDR, B220_GS_XFER_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nothing answers at 0x%02X (%s); no motion data on this unit",
                     B220_GS_ADDR, esp_err_to_name(err));
            lock_give();
            return ESP_ERR_NOT_FOUND;
        }
        i2c_device_config_t cfg = {};
        cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        cfg.device_address = B220_GS_ADDR;
        cfg.scl_speed_hz = B220_GS_SCL_HZ;
        err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "0x%02X answered but could not be added (%s)", B220_GS_ADDR,
                     esp_err_to_name(err));
            s_dev = nullptr;
            lock_give();
            return err;
        }
    }

    err = read_regs(B220_GS_WHO_AM_I, &s_who_am_i, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WHO_AM_I read failed (%s)", esp_err_to_name(err));
        lock_give();
        return err;
    }

    /* Configure, but keep going even if a write is refused: a wrong id is worth reporting and
     * still worth reading, because what comes back says more than the id does. */
    const esp_err_t e1 = write_reg(B220_GS_CTRL_REG1, B220_GS_CTRL_REG1_VALUE);
    const esp_err_t e4 = write_reg(B220_GS_CTRL_REG4, B220_GS_CTRL_REG4_VALUE);
    if (e1 != ESP_OK || e4 != ESP_OK) {
        ESP_LOGW(TAG, "configuration write failed (CTRL_REG1=%s CTRL_REG4=%s); reading anyway",
                 esp_err_to_name(e1), esp_err_to_name(e4));
    }

    /* Read CTRL_REG4 back: the full-scale setting decides the mg-per-count scale, and the
     * point of printing it is that no one has to assume which one the part accepted. */
    uint8_t ctrl4_readback = 0;
    const esp_err_t rb = read_regs(B220_GS_CTRL_REG4, &ctrl4_readback, 1);
    if (rb == ESP_OK) {
        s_ctrl4 = ctrl4_readback;
    }

    s_ready = true;
    if (s_who_am_i == B220_GS_WHO_AM_I_VALUE) {
        ESP_LOGI(TAG,
                 "SC7A20H at 0x%02X: WHO_AM_I 0x%02X as expected, CTRL_REG1=0x%02X "
                 "(100 Hz, XYZ), CTRL_REG4 wrote 0x%02X and reads back 0x%02X; the mg scale "
                 "comes from each sample (largest axis of a resting reading = 1g), read on "
                 "demand only",
                 B220_GS_ADDR, s_who_am_i, B220_GS_CTRL_REG1_VALUE, B220_GS_CTRL_REG4_VALUE,
                 ctrl4_readback);
    } else {
        ESP_LOGW(TAG,
                 "something answers at 0x%02X but WHO_AM_I is 0x%02X, not the expected "
                 "0x%02X - values are reported, but treat them as unverified",
                 B220_GS_ADDR, s_who_am_i, B220_GS_WHO_AM_I_VALUE);
    }
    lock_give();
    return ESP_OK;
}

/*
 * Which way is up, in the words the owner of this toy uses: it is described like a figure lying
 * down - which way its FACE points, and where its HEAD is - rather than in parts of the screen.
 *
 * THE AXIS MAPPING IS MEASURED, not taken from the chip's own axes.  A stationary unit was held
 * in each orientation and the dominant count landed within a few percent of the datasheet's 1024
 * every time:
 *
 *   screen up, flat -> z = +1000   => Z is normal to the screen (the face)
 *   right edge low  -> y = -1000   => Y runs along the head-to-feet axis, head at +y
 *   top edge low    -> x = +1000   => X runs across the body, +x towards the top edge
 *
 * SO THE SIGNS READ AS "that edge is UP", NOT "that edge is down".  With Y running from the feet
 * up to the head, gravity pointing away from the head (y = -1000) means the HEAD is the high end.
 * An earlier version of these strings said "right edge down (y down)", which contradicts itself
 * when read literally, and a language model reading it duly reported the tilt backwards; the
 * axis signs now stay in this comment and out of the sentence a model reads out.
 *
 * The face/head names for the two flat cases and for head-up were given by the owner and are the
 * ones to keep.  The sideways cases are named after them by symmetry: with the face up the toy is
 * lying on its back, face down means on its face, and standing on an edge means on its side -
 * which would make "lying on its left side" the case where the left edge is the low one, and
 * "head down" the opposite of the measured head-up case.
 */
static const char* orientation_of(int mg_x, int mg_y, int mg_z) {
    const int ax = mg_x < 0 ? -mg_x : mg_x;
    const int ay = mg_y < 0 ? -mg_y : mg_y;
    const int az = mg_z < 0 ? -mg_z : mg_z;

    if (az >= ax && az >= ay) {
        return mg_z > 0 ? "face up" : "face down";
    }
    if (ax >= ay) {
        /*
         * X runs across the body, so +x means the top edge is the high one - which is the case
         * measured on the bench, and the owner calls it "lying on its side".  The opposite sign
         * is the other side, and WHICH SIDE is which has not been measured on this unit; the
         * names only have to stay distinguishable until someone checks.
         */
        return mg_x > 0 ? "lying on its side" : "lying on its other side";
    }
    /* Y runs from the feet to the head, so -y means the head is the high end. */
    return mg_y > 0 ? "head down" : "head up";
}

esp_err_t b220_gsensor_read(b220_gsensor_state_t* state) {
    if (state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(state, 0, sizeof(*state));

    const esp_err_t init_err = b220_gsensor_init();
    if (init_err != ESP_OK) {
        state->orientation = "no accelerometer";
        return init_err;
    }
    if (!lock_take()) {
        state->orientation = "accelerometer busy";
        return ESP_ERR_TIMEOUT;
    }

    /* The status register says whether a fresh sample is ready.  At 100 Hz it always is unless
     * the chip was just configured, so a missing flag is not an error - the read below still
     * returns the last sample - but it is worth one retry to avoid reporting the power-on
     * values. */
    uint8_t status = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (read_regs(B220_GS_STATUS, &status, 1) == ESP_OK && (status & B220_GS_STATUS_ZYXDA)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /*
     * TAKE A READING THAT COULD BE A STATIONARY ACCELEROMETER.
     *
     * This loop exists because of a measured failure: the very first {BASE+ACCEL} after boot
     * reported x=0 y=-71 z=-3 mg.  A stationary accelerometer's magnitude is 1 g - about
     * 1000 mg whatever its orientation - so 71 mg is not a reading of anything.  It came from
     * reading the output registers in the window right after the chip was configured, before
     * it had produced a sample of its own; the status-flag retry above was too short to cover
     * it.
     *
     * The chip cannot report "this is not a measurement", so the magnitude does it instead:
     * anything outside 500..1500 mg is rejected and re-read.  That also catches a genuinely
     * moving device, which is worth saying out loud rather than reporting as an orientation -
     * hence the explicit "moving" answer below instead of a made-up tilt.
     */
    uint8_t buf[B220_GS_OUT_BYTES] = {};
    esp_err_t err = ESP_ERR_INVALID_STATE;
    int rejected = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        err = read_regs(B220_GS_OUT_AUTO_INC, buf, sizeof(buf));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "output read failed (%s)", esp_err_to_name(err));
            state->orientation = "read failed";
            lock_give();
            return err;
        }

        const int16_t rx = (int16_t)((buf[1] << 8) | buf[0]);
        const int16_t ry = (int16_t)((buf[3] << 8) | buf[2]);
        const int16_t rz = (int16_t)((buf[5] << 8) | buf[4]);

        /*
         * The datasheet's twelve significant bits sit in the high half of the pair, so shift the
         * low four out before doing anything else.  That makes the numbers here directly
         * comparable with the datasheet's own example: 1024 counts = 1 g at FS=10.
         */
        const int16_t cx = (int16_t)(rx >> 4);
        const int16_t cy = (int16_t)(ry >> 4);
        const int16_t cz = (int16_t)(rz >> 4);

        int64_t ax = cx < 0 ? -(int64_t)cx : (int64_t)cx;
        int64_t ay = cy < 0 ? -(int64_t)cy : (int64_t)cy;
        int64_t az = cz < 0 ? -(int64_t)cz : (int64_t)cz;
        int64_t dominant = ax;
        if (ay > dominant) {
            dominant = ay;
        }
        if (az > dominant) {
            dominant = az;
        }

        /*
         * A reading is usable when its largest axis looks like gravity: that axis IS the 1 g
         * vector, and it gives the scale at the same time.  Everything else - a part that has
         * not produced a sample yet, a unit being waved around - lands outside the window.
         */
        if (dominant < B220_GS_COUNTS_PER_G_MIN || dominant > B220_GS_COUNTS_PER_G_MAX) {
            rejected++;
            ESP_LOGW(TAG,
                     "read %d rejected: raw %d/%d/%d (12-bit %d/%d/%d), largest axis %d counts "
                     "(a resting part's largest axis must be near %d counts for 1g)",
                     attempt + 1, rx, ry, rz, cx, cy, cz, (int)dominant,
                     B220_GS_TYPICAL_COUNTS_PER_G);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        state->raw_x = rx;
        state->raw_y = ry;
        state->raw_z = rz;
        /* Scale from this sample: the largest axis is 1 g by definition, so the orientations
         * below stay right whatever full-scale the part is really in. */
        state->mg_x = (int)((int64_t)cx * 1000 / dominant);
        state->mg_y = (int)((int64_t)cy * 1000 / dominant);
        state->mg_z = (int)((int64_t)cz * 1000 / dominant);
        state->counts_per_g = (uint16_t)dominant;
        state->ready = true;
        state->orientation = orientation_of(state->mg_x, state->mg_y, state->mg_z);
        state->who_am_i = s_who_am_i;
        if (rejected > 0) {
            ESP_LOGI(TAG, "accepted after %d rejected read(s): largest axis %d counts = 1g",
                     rejected, (int)dominant);
        }
        lock_give();
        return ESP_OK;
    }

    /* Nothing plausible came back.  Say so, and hand back the last raw numbers so the caller
     * can see what was actually there rather than a zeroed struct. */
    state->who_am_i = s_who_am_i;
    state->raw_x = (int16_t)((buf[1] << 8) | buf[0]);
    state->raw_y = (int16_t)((buf[3] << 8) | buf[2]);
    state->raw_z = (int16_t)((buf[5] << 8) | buf[4]);
    state->mg_x = state->raw_x >> 6;
    state->mg_y = state->raw_y >> 6;
    state->mg_z = state->raw_z >> 6;
    state->orientation = "moving (or no valid sample)";
    lock_give();
    return ESP_ERR_INVALID_STATE;
}

void b220_gsensor_describe(const b220_gsensor_state_t* state, char* out, size_t out_size) {
    if (state == nullptr || out == nullptr || out_size == 0) {
        return;
    }
    if (!state->ready) {
        snprintf(out, out_size, "%s",
                 state->orientation != nullptr ? state->orientation : "no accelerometer");
        return;
    }
    /*
     * The RAW counts are part of the answer deliberately.  Several rounds of this feature were
     * spent reasoning about what the numbers "should" be, and the raw values settled it every
     * time; whoever reads this line should not have to repeat that.  counts_per_g is the raw
     * value this reading treated as 1 g, so the arithmetic can be checked.
     */
    snprintf(out, out_size, "x=%d y=%d z=%d mg (1g = %u counts; raw %d/%d/%d), %s", state->mg_x,
             state->mg_y, state->mg_z, state->counts_per_g, state->raw_x, state->raw_y,
             state->raw_z, state->orientation);
}
