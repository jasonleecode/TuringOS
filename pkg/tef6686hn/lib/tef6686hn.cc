/*
 * TEF6686HN car radio tuner driver implementation.
 *
 * NXP radio framework command protocol:
 *
 *   Set command (write-only):
 *     [module][function][0x01][p0_hi][p0_lo]...[pN_hi][pN_lo]
 *
 *   Get command (write then repeated-start read):
 *     Write: [module][function][0x01]
 *     Read:  2 × N bytes  (big-endian int16 values)
 *
 * Module / function reference (TEF6686 Application Manual):
 *
 *   MODULE_APPL  (0x00) — application control
 *     Set 0xFF  System_Reset       (no params)
 *     Set 0x00  System_StartupInit (no params)
 *
 *   MODULE_FM    (0x24) — FM receiver
 *     Set 0x01  Tune_To            p[0]=method, p[1]=freq_10khz
 *     Get 0x80  Get_Quality        → rssi, snr, multipath, freq_offset (4 × int16)
 *     Get 0x81  Get_Status         → flags, freq_10khz             (2 × int16)
 *     Get 0x82  Get_RDS            → blockA, blockB, blockC, blockD, status (5 × int16)
 *
 *   MODULE_AM    (0x28) — AM/LW receiver
 *     Set 0x01  Tune_To            p[0]=method, p[1]=freq_khz
 *     Get 0x80  Get_Quality        → rssi, snr, 0, freq_offset     (4 × int16)
 *     Get 0x81  Get_Status         → flags, freq_khz               (2 × int16)
 *
 *   MODULE_DSP   (0x0E) — digital signal processing / audio
 *     Set 0x01  Set_OutputMute     p[0]=mute  (0=off, 1=on)
 *     Set 0x0D  Set_OutputLevel    p[0]=level_db10
 *
 * Tune method codes:
 *   0 = Preset  (go directly to freq, no seek)
 *   1 = Search up
 *   2 = Search down
 *   3 = Scan (brief stop per station)
 *
 * Frequencies:
 *   FM uses 10 kHz units  → 9800 for 98.0 MHz
 *   AM/LW uses kHz units  →  999 for 999 kHz
 *
 * Status flags (Get_Status word 0):
 *   bit 0: frequency locked
 *   bit 1: AFC locked
 *   bit 4: seek completed / station found
 */

#include <l4/tef6686hn/tef6686hn.h>
#include <l4/util/util.h>
#include <l4/sys/err.h>

/* ---- Module IDs ---- */

static constexpr l4_uint8_t MOD_APPL = 0x00;
static constexpr l4_uint8_t MOD_FM   = 0x24;
static constexpr l4_uint8_t MOD_AM   = 0x28;
static constexpr l4_uint8_t MOD_DSP  = 0x0E;

/* ---- APPL function codes ---- */

static constexpr l4_uint8_t APPL_RESET        = 0xFF;
static constexpr l4_uint8_t APPL_STARTUP_INIT = 0x00;

/* ---- FM / AM function codes ---- */

static constexpr l4_uint8_t TUNER_SET_TUNE_TO    = 0x01;
static constexpr l4_uint8_t TUNER_GET_QUALITY    = 0x80;
static constexpr l4_uint8_t TUNER_GET_STATUS     = 0x81;
static constexpr l4_uint8_t TUNER_GET_RDS        = 0x82;

/* ---- DSP function codes ---- */

static constexpr l4_uint8_t DSP_SET_OUTPUT_MUTE  = 0x01;
static constexpr l4_uint8_t DSP_SET_OUTPUT_LEVEL = 0x0D;

/* ---- Tune method codes ---- */

static constexpr l4_uint16_t METHOD_PRESET       = 0x0000;
static constexpr l4_uint16_t METHOD_SEARCH_UP    = 0x0001;
static constexpr l4_uint16_t METHOD_SEARCH_DOWN  = 0x0002;

/* ---- Status flag bits ---- */

static constexpr l4_int16_t  STATUS_LOCKED       = 0x0001;
static constexpr l4_int16_t  STATUS_AFC_LOCKED   = 0x0002;

/* ---- Seek poll parameters ---- */

static constexpr unsigned SEEK_POLL_MAX     = 200;   /* iterations  */
static constexpr unsigned SEEK_POLL_US      = 20000; /* 20 ms each  */

/* ---- Frequency range limits ---- */

static constexpr l4_uint32_t FM_MIN_KHZ  =  64000;
static constexpr l4_uint32_t FM_MAX_KHZ  = 108000;
static constexpr l4_uint32_t MW_MIN_KHZ  =    522;
static constexpr l4_uint32_t MW_MAX_KHZ  =   1710;
static constexpr l4_uint32_t LW_MIN_KHZ  =    144;
static constexpr l4_uint32_t LW_MAX_KHZ  =    288;

/* ====================================================================
 * Private helpers
 * ==================================================================== */

/*
 * cmd_set — Send a Set command (write-only, no read-back).
 *
 * Wire format: [module][function][0x01][p0_hi][p0_lo]...[pN_hi][pN_lo]
 * Maximum 4 parameters (8 payload bytes) → 11 bytes total, well within
 * the I2C transaction size limit.
 */
long Tef6686hn::cmd_set(l4_uint8_t module, l4_uint8_t func,
                        l4_uint16_t const *params, unsigned n_params)
{
  l4_uint8_t buf[3 + 8]; /* header(3) + up to 4 × uint16 */
  buf[0] = module;
  buf[1] = func;
  buf[2] = 0x01; /* index = 1 */
  for (unsigned i = 0; i < n_params; ++i)
    {
      buf[3 + 2 * i]     = static_cast<l4_uint8_t>(params[i] >> 8);
      buf[3 + 2 * i + 1] = static_cast<l4_uint8_t>(params[i] & 0xFF);
    }
  L4::Ipc::Array<l4_uint8_t const> wr(3 + 2 * n_params, buf);
  return _i2c->write(wr);
}

/*
 * cmd_get — Send a Get command header, then read n_words × 2 bytes.
 *
 * Write: [module][function][0x01]
 * Read:  2 × n_words bytes, each pair decoded as big-endian int16.
 */
long Tef6686hn::cmd_get(l4_uint8_t module, l4_uint8_t func,
                        l4_int16_t *result, unsigned n_words)
{
  l4_uint8_t cmd[3] = { module, func, 0x01 };
  l4_uint8_t raw[10] = {}; /* max 5 words = 10 bytes */

  unsigned n_bytes = n_words * 2;
  if (n_bytes > sizeof(raw))
    return -L4_EINVAL;

  L4::Ipc::Array<l4_uint8_t const> wr(3, cmd);
  L4::Ipc::Array<l4_uint8_t>       rb(n_bytes, raw);
  long r = _i2c->write_read(wr, static_cast<unsigned char>(n_bytes), rb);
  if (r < 0)
    return r;

  for (unsigned i = 0; i < n_words; ++i)
    result[i] = static_cast<l4_int16_t>(
                  (static_cast<l4_uint16_t>(raw[2 * i]) << 8)
                  | raw[2 * i + 1]);
  return L4_EOK;
}

/* ====================================================================
 * Public API
 * ==================================================================== */

Tef6686hn::Tef6686hn(L4::Cap<I2c_device_ops> i2c)
: _i2c(i2c)
{}

long Tef6686hn::init()
{
  /* 1. Hardware reset */
  long r = cmd_set(MOD_APPL, APPL_RESET, nullptr, 0);
  if (r < 0)
    return r;
  l4_usleep(100000); /* 100 ms for oscillator and DSP start-up */

  /* 2. Startup initialisation (loads internal firmware) */
  r = cmd_set(MOD_APPL, APPL_STARTUP_INIT, nullptr, 0);
  if (r < 0)
    return r;
  l4_usleep(50000); /* 50 ms for init sequence */

  /* 3. Unmute and set a reasonable output level (20 dB) */
  r = mute(false);
  if (r < 0)
    return r;

  return set_output_level(200);
}

long Tef6686hn::tune(Band band, l4_uint32_t freq_khz)
{
  /* Validate frequency range */
  switch (band)
    {
    case Band::FM:
      if (freq_khz < FM_MIN_KHZ || freq_khz > FM_MAX_KHZ)
        return -L4_EINVAL;
      break;
    case Band::MW:
      if (freq_khz < MW_MIN_KHZ || freq_khz > MW_MAX_KHZ)
        return -L4_EINVAL;
      break;
    case Band::LW:
      if (freq_khz < LW_MIN_KHZ || freq_khz > LW_MAX_KHZ)
        return -L4_EINVAL;
      break;
    }

  _band     = band;
  _freq_khz = freq_khz;

  if (band == Band::FM)
    {
      /* FM uses 10 kHz units: 98000 kHz → 9800 */
      l4_uint16_t params[2] = { METHOD_PRESET,
                                 static_cast<l4_uint16_t>(freq_khz / 10) };
      return cmd_set(MOD_FM, TUNER_SET_TUNE_TO, params, 2);
    }
  else
    {
      /* AM/LW uses kHz units directly */
      l4_uint16_t params[2] = { METHOD_PRESET,
                                 static_cast<l4_uint16_t>(freq_khz) };
      return cmd_set(MOD_AM, TUNER_SET_TUNE_TO, params, 2);
    }
}

long Tef6686hn::get_quality(Quality *q)
{
  if (!q)
    return -L4_EINVAL;

  l4_int16_t raw[4] = {};
  l4_uint8_t module = (_band == Band::FM) ? MOD_FM : MOD_AM;
  long r = cmd_get(module, TUNER_GET_QUALITY, raw, 4);
  if (r < 0)
    return r;

  /* Response layout: [rssi][snr][multipath][freq_offset] */
  q->rssi        = raw[0];
  q->snr         = raw[1];
  q->multipath   = raw[2];
  q->freq_offset = raw[3];
  return L4_EOK;
}

long Tef6686hn::get_frequency(l4_uint32_t *freq_khz)
{
  if (!freq_khz)
    return -L4_EINVAL;

  l4_int16_t raw[2] = {};
  l4_uint8_t module = (_band == Band::FM) ? MOD_FM : MOD_AM;
  long r = cmd_get(module, TUNER_GET_STATUS, raw, 2);
  if (r < 0)
    return r;

  /* raw[1]: frequency in 10 kHz units (FM) or kHz (AM/LW) */
  if (_band == Band::FM)
    *freq_khz = static_cast<l4_uint32_t>(raw[1]) * 10;
  else
    *freq_khz = static_cast<l4_uint32_t>(static_cast<l4_uint16_t>(raw[1]));

  return L4_EOK;
}

long Tef6686hn::seek(Seek_dir dir, l4_int16_t rssi_min)
{
  if (_band != Band::FM)
    return -L4_EINVAL; /* seek only supported on FM */

  l4_uint16_t method = (dir == Seek_dir::Up) ? METHOD_SEARCH_UP
                                              : METHOD_SEARCH_DOWN;
  l4_uint16_t params[2] = { method,
                             static_cast<l4_uint16_t>(_freq_khz / 10) };
  long r = cmd_set(MOD_FM, TUNER_SET_TUNE_TO, params, 2);
  if (r < 0)
    return r;

  /* Poll for lock: check status flags and RSSI threshold */
  for (unsigned i = 0; i < SEEK_POLL_MAX; ++i)
    {
      l4_usleep(SEEK_POLL_US);

      l4_int16_t status[2] = {};
      r = cmd_get(MOD_FM, TUNER_GET_STATUS, status, 2);
      if (r < 0)
        return r;

      /* status[0] bit 4: seek completed */
      if (status[0] & 0x0010)
        {
          /* Check signal quality at the stopped frequency */
          Quality q = {};
          r = get_quality(&q);
          if (r < 0)
            return r;

          if (q.rssi >= rssi_min)
            {
              _freq_khz = static_cast<l4_uint32_t>(
                            static_cast<l4_uint16_t>(status[1])) * 10;
              return L4_EOK; /* station found */
            }

          /* Too weak — resume seeking */
          r = cmd_set(MOD_FM, TUNER_SET_TUNE_TO, params, 2);
          if (r < 0)
            return r;
        }
    }

  return -L4_ENOENT; /* nothing found within timeout */
}

long Tef6686hn::get_rds_group(Rds_group *group)
{
  if (!group)
    return -L4_EINVAL;

  if (_band != Band::FM)
    {
      group->valid = false;
      return L4_EOK;
    }

  /* Response: blockA, blockB, blockC, blockD, status_word (5 × int16) */
  l4_int16_t raw[5] = {};
  long r = cmd_get(MOD_FM, TUNER_GET_RDS, raw, 5);
  if (r < 0)
    return r;

  /*
   * Status word (raw[4]):
   *   bits 7:4 = block D error flags
   *   bits 3:0 = valid-group flag (bit 0 set = complete group available)
   */
  group->valid    = (raw[4] & 0x0001) != 0;
  group->err_bits = static_cast<l4_uint8_t>((raw[4] >> 4) & 0x0F);
  for (unsigned i = 0; i < 4; ++i)
    group->block[i] = static_cast<l4_uint16_t>(raw[i]);

  return L4_EOK;
}

long Tef6686hn::set_output_level(l4_uint16_t level_db10)
{
  return cmd_set(MOD_DSP, DSP_SET_OUTPUT_LEVEL, &level_db10, 1);
}

long Tef6686hn::mute(bool on)
{
  l4_uint16_t param = on ? 1 : 0;
  return cmd_set(MOD_DSP, DSP_SET_OUTPUT_MUTE, &param, 1);
}
