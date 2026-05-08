/*
 * syslogd — centralized log daemon for TuringOS
 *
 * Receives structured log entries from all processes via IPC (Klog_svr
 * protocol 0x5900), formats them, and appends to /ext4/syslog.txt.
 *
 * Flush/reopen strategy: close and reopen the file every ~3 KB of
 * accumulated writes to avoid DS overflow in Ext4_file_vfs (the DS is
 * sized to round_up(file_size, 4096) at open time; writes past that
 * limit are silently dropped).
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/br_manager>
#include <l4/re/util/object_registry>
#include <l4/sys/cxx/ipc_epiface>

#include <l4/klog/klog_ipc.h>

static const char     LOG_PATH[]     = "/ext4/syslog.txt";
static const unsigned REOPEN_BYTES   = 3000;

/* ------------------------------------------------------------------ */
/* Format helpers (mirrors klog.cc)                                    */
/* ------------------------------------------------------------------ */
static const char *level_str(int lvl)
{
    static const char *t[] = {
        "EMERG", "ALERT", "CRIT ", "ERR  ", "WARN ", "NOTE ", "INFO ", "DEBUG"
    };
    return (lvl >= 0 && lvl <= 7) ? t[lvl] : "?    ";
}

static const char *fac_str(int fac)
{
    static const char *t[] = {
        "kern ", "shell", "net  ", "fs   ", "blk  ", "rtc  ", "user ", "disp "
    };
    return (fac >= 0 && fac <= 7) ? t[fac] : "user ";
}

/* ------------------------------------------------------------------ */
/* Syslogd IPC server                                                  */
/* ------------------------------------------------------------------ */
class Syslogd : public L4::Epiface_t<Syslogd, Klog_svr>
{
public:
  Syslogd() : _f(nullptr), _written(0) {}

  l4_ret_t op_log(Klog_svr::Rights,
                  l4_uint64_t ts_us,
                  l4_uint8_t  level,
                  l4_uint8_t  facility,
                  L4::Ipc::Array_ref<char const> msg)
  {
    /* Reopen before we exceed the DS allocation */
    if (_written >= REOPEN_BYTES)
      reopen();

    if (!_f)
      open_file();

    if (!_f) return -L4_EIO;

    unsigned long long sec = ts_us / 1000000ULL;
    unsigned long long us  = ts_us % 1000000ULL;

    char line[320];
    int n = snprintf(line, sizeof(line), "[%5llu.%06llu] %s %s: %.*s\n",
                     sec, us,
                     level_str(level),
                     fac_str(facility),
                     (int)msg.length, msg.data);
    if (n > 0 && (unsigned)n < sizeof(line))
    {
      fwrite(line, 1, n, _f);
      _written += (unsigned)n;
    }

    /* Flush immediately on WARN (4) or higher severity */
    if (level <= 4)
      reopen();

    return L4_EOK;
  }

private:
  FILE     *_f;
  unsigned  _written;

  void open_file()
  {
    _f = fopen(LOG_PATH, "a");
    if (_f)
    {
      /* Ext4_file_vfs returns O_RDWR without O_APPEND; seek manually */
      fseek(_f, 0, SEEK_END);
    }
  }

  void reopen()
  {
    if (_f)
    {
      fclose(_f);
      _f = nullptr;
    }
    _written = 0;
    open_file();
  }
};

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> server;
static Syslogd syslogd_obj;

int main()
{
  printf("[syslogd] starting, log: %s\n", LOG_PATH);

  auto svr_cap = server.registry()->register_obj(&syslogd_obj, "svr");
  if (!svr_cap.is_valid())
  {
    printf("[syslogd] ERROR: 'svr' capability not found — exiting\n");
    return 1;
  }

  printf("[syslogd] ready\n");
  server.loop();

  return 0;
}
