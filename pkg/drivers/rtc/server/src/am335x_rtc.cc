/*
 * AM335x RTC controller driver.
 *
 * Register layout based on AM335x Technical Reference Manual (SPRUH73),
 * Chapter 20: RTC.
 *
 * The AM335x has one RTC module at base 0x44E3E000, IRQ 75/76.
 * It is a simple seconds/minutes/hours/... BCD register-based RTC
 * with 32 kHz crystal input.
 */

#include "rtc.h"
#include "bcd.h"

#include <l4/re/env>
#include <l4/re/rm>
#include <l4/re/error_helper>
#include <l4/util/util.h>
#include <l4/vbus/vbus>
#include <l4/drivers/hw_mmio_register_block>

#include <cstdio>
#include <time.h>

// AM335x RTC register offsets
enum Rtc_regs : unsigned
{
  SECONDS_REG    = 0x00,
  MINUTES_REG    = 0x04,
  HOURS_REG      = 0x08,
  DAYS_REG       = 0x0C,
  MONTHS_REG     = 0x10,
  YEARS_REG      = 0x14,
  WEEKS_REG      = 0x18,

  ALARM_SECONDS  = 0x20,
  ALARM_MINUTES  = 0x24,
  ALARM_HOURS    = 0x28,
  ALARM_DAYS     = 0x2C,
  ALARM_MONTHS   = 0x30,
  ALARM_YEARS    = 0x34,

  CTRL_REG       = 0x40,
  STATUS_REG     = 0x44,
  INTERRUPTS_REG = 0x48,
  COMP_LSB_REG   = 0x4C,
  COMP_MSB_REG   = 0x50,
  OSC_REG        = 0x54,

  KICK0R         = 0x6C,
  KICK1R         = 0x70,

  REVISION_REG   = 0x74,
  SYSCONFIG_REG  = 0x78,

  ALARM2_SECONDS = 0x80,
  ALARM2_MINUTES = 0x84,
  ALARM2_HOURS   = 0x88,
  ALARM2_DAYS    = 0x8C,
  ALARM2_MONTHS  = 0x90,
  ALARM2_YEARS   = 0x94,

  PMIC_REG       = 0x98,
};

// CTRL_REG bits
enum Ctrl_bits : l4_uint32_t
{
  Ctrl_stop_rtc   = 1U << 0,  // RTC stop/run (1=run)
  Ctrl_round_30s  = 1U << 1,
  Ctrl_auto_comp  = 1U << 2,
  Ctrl_mode_12h   = 1U << 3,  // 0=24h, 1=12h
  Ctrl_test_mode  = 1U << 4,
  Ctrl_set_32_cnt = 1U << 5,  // set 32kHz counter
  Ctrl_rtc_dis    = 1U << 6,  // RTC disable
};

// STATUS_REG bits
enum Status_bits : l4_uint32_t
{
  Status_busy    = 1U << 0,  // RTC busy (updating regs)
  Status_run     = 1U << 1,  // RTC is running
  Status_1s_evt  = 1U << 2,
  Status_1m_evt  = 1U << 3,
  Status_1h_evt  = 1U << 4,
  Status_1d_evt  = 1U << 5,
  Status_alarm   = 1U << 6,
  Status_alarm2  = 1U << 7,
};

// Kick register magic values to unlock write access
static constexpr l4_uint32_t Kick0_key = 0x83E70B13;
static constexpr l4_uint32_t Kick1_key = 0x95A4F1E0;


struct Am335x_rtc : Rtc
{
  bool probe() override
  {
    auto vbus = L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus");
    if (!vbus.is_valid())
      return false;

    L4vbus::Device dev;
    l4vbus_device_t devinfo;
    int found = 0;

    while (vbus->root().next_device(&dev, L4VBUS_MAX_DEPTH, &devinfo) == 0)
      {
        if ((found = dev.is_compatible("ti,am3352-rtc")) == 1)
          break;
        if ((found = dev.is_compatible("ti,am335x-rtc")) == 1)
          break;
      }

    if (found <= 0)
      return false;

    for (unsigned i = 0; i < devinfo.num_resources; ++i)
      {
        l4vbus_resource_t res;
        L4Re::chksys(dev.get_resource(i, &res), "Get RTC resources");

        if (res.type != L4VBUS_RESOURCE_MEM)
          continue;

        auto iods =
          L4::Ipc::make_cap_rw(L4::cap_reinterpret_cast<L4Re::Dataspace>(vbus));

        l4_size_t sz = res.end - res.start + 1;
        l4_addr_t addr = 0;
        L4Re::Env const *env = L4Re::Env::env();
        L4Re::chksys(env->rm()->attach(&addr, sz,
                                       L4Re::Rm::F::Search_addr
                                       | L4Re::Rm::F::Cache_uncached
                                       | L4Re::Rm::F::RW,
                                       iods, res.start, L4_PAGESHIFT),
                     "Attach RTC MMIO memory");
        _regs = new L4drivers::Mmio_register_block<32>(addr);
        break;
      }

    if (!_regs)
      return false;

    // Ensure the RTC is running
    unlock();
    l4_uint32_t ctrl = read32(CTRL_REG);
    if (!(ctrl & Ctrl_stop_rtc))
      {
        ctrl |= Ctrl_stop_rtc;
        ctrl &= ~Ctrl_mode_12h; // 24h mode
        write32(CTRL_REG, ctrl);
      }
    lock();

    // Read and print current time
    l4_uint64_t offset;
    if (get_time(&offset) == 0)
      {
        l4_uint64_t ns = offset + l4_kip_clock_ns(l4re_kip());
        time_t secs = ns / 1'000'000'000ull;
        char buf[26];
        ctime_r(&secs, buf);
        printf("Found AM335x RTC. Current time: %s", buf);
      }

    return true;
  }

  int get_time(l4_uint64_t *nsec_offset_1970) override
  {
    if (!_regs)
      return -L4_ENODEV;

    // Wait until the RTC is not busy updating
    wait_not_busy();

    l4_uint32_t sec  = bcd2bin(read32(SECONDS_REG) & 0x7F);
    l4_uint32_t min  = bcd2bin(read32(MINUTES_REG) & 0x7F);
    l4_uint32_t hour = bcd2bin(read32(HOURS_REG) & 0x3F);
    l4_uint32_t day  = bcd2bin(read32(DAYS_REG) & 0x3F);
    l4_uint32_t mon  = bcd2bin(read32(MONTHS_REG) & 0x1F);
    l4_uint32_t year = bcd2bin(read32(YEARS_REG) & 0xFF);

    struct tm tm;
    tm.tm_sec  = sec;
    tm.tm_min  = min;
    tm.tm_hour = hour;
    tm.tm_mday = day;
    tm.tm_mon  = mon - 1;       // tm_mon is 0-based
    tm.tm_year = year + 100;    // AM335x RTC year is 0-99 (2000-2099)
    tm.tm_wday = 0;
    tm.tm_yday = 0;
    tm.tm_isdst = 0;
    tm.tm_gmtoff = 0;
    tm.tm_zone = nullptr;

    l4_uint64_t ns = timegm(&tm) * 1'000'000'000ull;
    *nsec_offset_1970 = ns - l4_kip_clock_ns(l4re_kip());
    return 0;
  }

  int set_time(l4_uint64_t offset_nsec) override
  {
    if (!_regs)
      return -L4_ENODEV;

    l4_uint64_t ns = offset_nsec + l4_kip_clock_ns(l4re_kip());
    time_t secs = ns / 1'000'000'000ull;
    struct tm tm;
    gmtime_r(&secs, &tm);

    unlock();

    // Stop the RTC before writing
    l4_uint32_t ctrl = read32(CTRL_REG);
    ctrl &= ~Ctrl_stop_rtc;
    write32(CTRL_REG, ctrl);

    // Wait for the RTC to stop
    for (int i = 0; i < 100; ++i)
      {
        if (!(read32(STATUS_REG) & Status_run))
          break;
        l4_sleep(1);
      }

    write32(SECONDS_REG, bin2bcd(tm.tm_sec));
    write32(MINUTES_REG, bin2bcd(tm.tm_min));
    write32(HOURS_REG,   bin2bcd(tm.tm_hour));
    write32(DAYS_REG,    bin2bcd(tm.tm_mday));
    write32(MONTHS_REG,  bin2bcd(tm.tm_mon + 1));
    write32(YEARS_REG,   bin2bcd(tm.tm_year - 100));
    write32(WEEKS_REG,   bin2bcd(tm.tm_wday));

    // Restart the RTC
    ctrl |= Ctrl_stop_rtc;
    write32(CTRL_REG, ctrl);

    lock();
    return 0;
  }

private:
  l4_uint32_t read32(unsigned reg)
  { return (*_regs).read<l4_uint32_t>(reg); }

  void write32(unsigned reg, l4_uint32_t val)
  { (*_regs).write<l4_uint32_t>(val, reg); }

  void unlock()
  {
    write32(KICK0R, Kick0_key);
    write32(KICK1R, Kick1_key);
  }

  void lock()
  {
    // Writing any wrong value locks the registers
    write32(KICK0R, 0);
  }

  void wait_not_busy()
  {
    for (int i = 0; i < 100; ++i)
      {
        if (!(read32(STATUS_REG) & Status_busy))
          return;
        l4_sleep(1);
      }
  }

  L4drivers::Register_block<32> _regs;
};

static Am335x_rtc _am335x_rtc;
