/*
 * Copyright (C) 2024-2025 Kernkonzept GmbH.
 * Author(s): Philipp Eppelt philipp.eppelt@kernkonzept.com
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include "i2c_controller_if.h"

namespace I2c_server {
void start_server(Controller_if *ctrl);
}
