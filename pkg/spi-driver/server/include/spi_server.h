/*
 * SPI server entry point.
 */
#pragma once

#include "spi_controller_if.h"

namespace Spi_server {
void start_server(Controller_if *ctrl);
}
