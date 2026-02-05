/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#include <iostream>
#include <l4/re/env>
#include <l4/usb/usb.h>
#include "ledfan.h"

int main()
{
	std::cout << "Hello from ledfan-example :)" << std::endl;

	L4::Cap<USB::Device> dev = L4Re::Env::env()->get_cap<USB::Device>("dev");
	if (!dev.is_valid()) {
		printf("Could not get device capability");
		return -L4_ENODEV;
	}

	LedFan ledfan(dev);
	if (int ret = ledfan.setupHid()) {
		printf("Error initializing HID device");
		return ret;
	}

	ledfan.ascii_print("test", 4);

	std::cout << "bye" << std::endl;
	return 0;
}
