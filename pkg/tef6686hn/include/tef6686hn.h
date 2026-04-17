/*
 * TEF6686HN software-defined car radio tuner driver (I2C).
 *
 * The TEF6686HN is an NXP SDR car radio IC that covers:
 *   FM   64 – 108 MHz  (step 50 kHz, 100 kHz or 200 kHz)
 *   MW  522 – 1710 kHz (step 9 kHz or 10 kHz)
 *   LW  144 –  288 kHz (step 1 kHz)
 *
 * I2C address: 0x1C (7-bit, fixed).
 *
 * Command protocol (NXP radio framework):
 *   Set command (write only):
 *     [module][function][0x01][p0_hi][p0_lo] ... [pN_hi][pN_lo]
 *   Get command (write then read):
 *     Write: [module][function][0x01]
 *     Read:  N × 2 bytes, big-endian int16 values
 *
 * Typical usage:
 *   Tef6686hn radio(i2c_cap);
 *   radio.init();
 *   radio.tune(Tef6686hn::Band::FM, 98000); // 98.0 MHz
 *   Tef6686hn::Quality q;
 *   radio.get_quality(&q);
 *   printf("RSSI %.1f dBuV  SNR %.1f dB\n",
 *          q.rssi / 10.0, q.snr / 10.0);
 */
#pragma once

#include <l4/sys/types.h>
#include <l4/sys/capability>
#include <l4/i2c-driver/i2c_device_if.h>

class Tef6686hn
{
public:
  /** 7-bit I2C address (hard-wired on the TEF6686HN). */
  static constexpr l4_uint8_t I2c_addr = 0x1C;

  /** Receiving band. */
  enum class Band
  {
    FM, /**< 64 – 108 MHz                       */
    MW, /**< 522 – 1710 kHz (medium wave)        */
    LW, /**< 144 –  288 kHz (long wave)          */
  };

  /** FM seek direction. */
  enum class Seek_dir { Up, Down };

  /**
   * Signal quality indicators, returned by get_quality().
   *
   * All fields are signed 16-bit; see individual descriptions for units.
   */
  struct Quality
  {
    l4_int16_t rssi;        /**< Received signal level, ×10 dBuV. 600 = 60.0 dBuV. */
    l4_int16_t snr;         /**< Signal-to-noise ratio,  ×10 dB.  300 = 30.0 dB.   */
    l4_int16_t multipath;   /**< Multipath indicator 0–100 (FM only; 0 on AM/LW).  */
    l4_int16_t freq_offset; /**< Tuner frequency error, ×10 Hz.  (AFC correction.)  */
  };

  /**
   * One decoded RDS group (FM only).
   *
   * Valid only when valid == true.  block[0..3] = blocks A, B, C, D.
   * err_bits: bit N set means block N contained uncorrectable errors.
   */
  struct Rds_group
  {
    l4_uint16_t block[4]; /**< Raw RDS blocks A–D. */
    l4_uint8_t  err_bits; /**< Uncorrectable-error flags, one bit per block. */
    bool        valid;    /**< True when a complete group is available. */
  };

  /**
   * Construct a TEF6686HN driver.
   *
   * \param i2c  I2C device capability from the i2c-driver factory,
   *             already bound to address 0x1C.
   */
  explicit Tef6686hn(L4::Cap<I2c_device_ops> i2c);

  /**
   * Reset the chip and run the startup initialisation sequence.
   *
   * Must be called once before any other method.  Takes ~150 ms.
   *
   * \retval L4_EOK  Device initialised successfully.
   * \retval <0      I2C communication error.
   */
  long init();

  /**
   * Tune to the given frequency.
   *
   * \param band     Target band (FM, MW or LW).
   * \param freq_khz Frequency in kHz.
   *                 FM:  64000 – 108000  (e.g. 98000 for 98.0 MHz)
   *                 MW:    522 –   1710  (e.g.  999 for 999 kHz)
   *                 LW:    144 –    288  (e.g.  198 for 198 kHz)
   * \retval L4_EOK     Command sent; allow ~30 ms for AFC to settle.
   * \retval -L4_EINVAL Frequency out of range for the selected band.
   * \retval <0         I2C error.
   */
  long tune(Band band, l4_uint32_t freq_khz);

  /**
   * Read signal quality of the currently tuned frequency.
   *
   * \param[out] q  Filled with RSSI, SNR, multipath and frequency offset.
   * \retval L4_EOK  Success.
   * \retval <0      I2C error.
   */
  long get_quality(Quality *q);

  /**
   * Read the frequency the tuner has settled on (after AFC).
   *
   * \param[out] freq_khz  Current centre frequency in kHz.
   * \retval L4_EOK  Success.
   * \retval <0      I2C error or chip not in FM mode.
   */
  long get_frequency(l4_uint32_t *freq_khz);

  /**
   * Initiate an FM seek and wait (poll) until a station is locked.
   *
   * After this call returns L4_EOK the tuner is parked on the found
   * station.  get_frequency() and get_quality() will return its data.
   *
   * \param dir      Search direction.
   * \param rssi_min Minimum RSSI (×10 dBuV) for a valid stop.  A typical
   *                 value is 200 (20.0 dBuV).
   * \retval L4_EOK     Station found.
   * \retval -L4_ENOENT No station found (seek wrapped around).
   * \retval <0         I2C error.
   */
  long seek(Seek_dir dir, l4_int16_t rssi_min = 200);

  /**
   * Read the latest RDS group from the internal FIFO (FM only).
   *
   * \param[out] group  Set valid=true and filled with block data when
   *                    a complete group is available; valid=false otherwise.
   * \retval L4_EOK  Command succeeded (check group.valid for data).
   * \retval <0      I2C error.
   */
  long get_rds_group(Rds_group *group);

  /**
   * Set the analogue audio output level.
   *
   * \param level_db10  Level in ×10 dB, 0 = muted, 255 = maximum.
   *                    Typical values: 150 (15 dB), 200 (20 dB).
   * \retval L4_EOK  Success.
   * \retval <0      I2C error.
   */
  long set_output_level(l4_uint16_t level_db10);

  /**
   * Mute or unmute the audio output.
   *
   * \param on  true = mute, false = unmute.
   * \retval L4_EOK  Success.
   * \retval <0      I2C error.
   */
  long mute(bool on);

private:
  /* Send a Set command (write-only, no response expected). */
  long cmd_set(l4_uint8_t module, l4_uint8_t func,
               l4_uint16_t const *params, unsigned n_params);

  /* Send a Get command and read back n_words × 2 bytes of result. */
  long cmd_get(l4_uint8_t module, l4_uint8_t func,
               l4_int16_t *result, unsigned n_words);

  L4::Cap<I2c_device_ops> _i2c;
  Band                    _band    = Band::FM;
  l4_uint32_t             _freq_khz = 0;
};
