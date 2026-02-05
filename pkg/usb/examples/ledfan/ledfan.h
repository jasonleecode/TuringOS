/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#ifndef __led_fan_include__
#define __led_fan_include__

// #include "oostubs_usb_device.h"
#include <l4/usb/usb.h>


// Abstraction of an usb-device
class LedFan {
private:

	L4::Cap<USB::Device> _dev;
	// endpoint_struct* hid_endpoint;
	// endpoint_struct* interrupt_endpoint;
	int hid_endpoint;
	int interrupt_endpoint;
	struct ledfan_color {
		enum { red = 0b0010, blue = 0b0100, green = 0b1000, magenta = 0b0110, yellow = 0b1010, cyan = 0b1100, white = 0b1110};
	};

public:
	// LedFan(const LedFan &copy) = delete; // prevent copying
	// LedFan(Oostubs_Port* port, int deviceID, Oostubs_OHCI* ohci): Oostubs_Device(port, deviceID, ohci) {};
	LedFan(L4::Cap<USB::Device> dev) : _dev(dev) {_dev->populate_device_infos();};
	int setupHid();
	int program(char* data, int length);
	void ascii_print(const char *input, int length);

};

#endif
