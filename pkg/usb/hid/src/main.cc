/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#include <iostream>
#include <pthread-l4.h>

#include <l4/re/env>
#include <l4/re/util/object_registry>
#include <l4/re/util/br_manager>
#include <l4/usb/usb.h>

#include "hid.h"

L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> registry;

static void* polling_loop(void* data) {

	HID* hid = static_cast<HID*>(data);
	while (true) {
		hid->poll();
	}
}

int main() {

	std::cout << "hello from hid-driver :)" << std::endl;

	L4::Cap<USB::Device> dev = L4Re::Env::env()->get_cap<USB::Device>("dev");
	if (!dev.is_valid()) {
		printf("could not get device capability");
		return -L4_ENODEV;
	}

	HID hid(dev);

	L4Re::chkcap(registry.registry()->register_obj(&hid, "ev"), "could not register event interface");

	pthread_t thread;
	pthread_attr_t a;
	pthread_attr_init(&a);

	if (pthread_create(&thread, &a, polling_loop, &hid)) {
		printf("error creating thread!\n");
		return -1;
	}

	registry.loop();

	return 0;
}
