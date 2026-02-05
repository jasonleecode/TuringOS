/*
 * Copyright (C) 2024-2025 Kernkonzept GmbH.
 * Author(s): Philipp Eppelt philipp.eppelt@kernkonzept.com
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <l4/vbus/vbus>
#include <l4/sys/icu>
#include <l4/re/error_helper>
#include <l4/i2c-driver/server/i2c_controller_if.h>

#include <cstdio>
#include <vector>

class Ctrl_base : public I2c_server::Controller_if
{
public:
  Ctrl_base()
  {
    _ctrls.push_back(this);
  }

  virtual bool probe(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu) = 0;
  virtual char const *name() = 0;
  static Ctrl_base *find_ctrl(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu);

private:
  static std::vector<Ctrl_base *> _ctrls;
};

std::vector<Ctrl_base *> Ctrl_base::_ctrls;

Ctrl_base *
Ctrl_base::find_ctrl(L4::Cap<L4vbus::Vbus> vbus, L4::Cap<L4::Icu> icu)
{
  for (auto *ctrl : _ctrls)
    if (ctrl->probe(vbus, icu))
      {
        printf("Found controller %s\n", ctrl->name());
        return ctrl;
      }

  printf("No driver for controler found\n");

  return nullptr;
}


