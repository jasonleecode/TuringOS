/*
 * Copyright (C) 2024-2025 Kernkonzept GmbH.
 * Author(s): Philipp Eppelt philipp.eppelt@kernkonzept.com
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#include <l4/vbus/vbus>
#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/sys/icu>

#include <l4/i2c-driver/server/i2c_server.h>

#include <terminate_handler-l4>
#include <getopt.h>
#include <cstdio>
#include <thread>

#include "debug.h"
#include "controller.h"
#include "bcm2835.h"
#include "imx8.h"
#include "am335x.h"

// List of all supported controllers
#ifdef CONFIG_I2C_DRIVER_RPI4
static Ctrl_bcm2835 __bcm2835;
#endif

#ifdef CONFIG_I2C_DRIVER_IMX8
static Imx8::Ctrl_imx8 __imx8;
#endif

#ifdef CONFIG_I2C_DRIVER_AM335X
static Am335x_i2c::Ctrl_am335x __am335x;
#endif

// Pointer to active controller
static Ctrl_base *__ctrl;

static char const *const options = "vq";
static struct option const loptions[] =
{
    { "verbose",                 no_argument,       NULL, 'v' },
    { "quiet",                   no_argument,       NULL, 'q' },
    { 0, 0, 0, 0}
};

static void parse_cmdline(int argc, char **argv)
{
  unsigned verbosity = Dbg::Warn;

  int opt;
  while ((opt = getopt_long(argc, argv, options, loptions, NULL)) != -1)
    {
      switch (opt)
      {
      case 'q':
        verbosity = Dbg::Quiet;
        break;
      case 'v':
        verbosity |= (verbosity << 1) | 1;
        break;

      default: break;
      }
    }

  Dbg::set_level(verbosity);
}

int main(int argc, char **argv)
{
  parse_cmdline(argc, argv);
  Dbg warn(Dbg::Warn);
  Dbg info(Dbg::Info);

  warn.printf("Hello, i2c-driver\n");

  L4::Cap<L4vbus::Vbus> vbus = L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus");
  if (!vbus)
    {
      warn.printf("No vbus capability provided. No hardware to drive\n");
      return -1;
    }

  L4vbus::Icu icudev;
  L4Re::chksys(vbus->root().device_by_hid(&icudev, "L40009"),
               "Look for ICU device.");
  L4::Cap<L4::Icu> icu = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4::Icu>(),
                                      "Allocate ICU capability.");
  L4Re::chksys(icudev.vicu(icu), "Request ICU capability.");

  if (!(__ctrl = Ctrl_base::find_ctrl(vbus, icu)))
    {
      warn.printf("No i2c driver found. No hardware to drive.\n");
      return -2;
    }


  std::thread server(I2c_server::start_server, __ctrl);
  info.printf("Wait for server termination.\n");
  server.join();

  warn.printf("Bye\n");
  return 0;
}
