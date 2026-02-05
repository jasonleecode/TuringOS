/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 * (c) 2025 Adam Lackorzynski <adam@l4re.org>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <string.h>

#include <l4/sys/cxx/ipc_epiface>
#include <l4/usb/usb.h>

#include "types.h"
#include "port.h"

namespace USB {

class Server : public L4::Epiface_t<Server, USB::Device> {

private:
	char* _desc_data = new char[9];
	Host_Controller* _controller;
	endpoint_struct* _endpoint[16] = {};

	configuration_descriptor* _config_desc = reinterpret_cast<configuration_descriptor*>(_desc_data);
	interface_descriptor* _iface_desc;
	endpoint_descriptor* _endp_desc;

public:
	Port* port;
	int device_id;
	device_descriptor dev_desc;

	Server(Host_Controller* controller, Port* port, int device_id)
        : _controller(controller), port(port), device_id(device_id) { };

	~Server() { delete[] _desc_data; };

	int init();
	void lsusb();

	long op_get_desc_length(L4::Typeid::Rights<Device>&) {
		return _config_desc->wTotalLength;
	}

	long op_get_desc_data(L4::Typeid::Rights<Device>&, L4::Ipc::Array_ref<char>& desc_cata) {
		memcpy(desc_cata.data, _desc_data, _config_desc->wTotalLength);
		return 0;
	};

	long op_get_dev_desc(L4::Typeid::Rights<Device>&, device_descriptor& dev_desc) {
		dev_desc = this->dev_desc;
		return 0;
	};

	long op_get_dev_id(L4::Typeid::Rights<Device>&) {
		return device_id;
	};

	long op_control_transfer_in(
		L4::Typeid::Rights<Device>& rights,
		l4_uint8_t endpoint,
		l4_uint8_t bmRequestType,
		l4_uint8_t bRequest,
		l4_uint16_t wValue,
		l4_uint16_t wIndex,
		l4_uint16_t wLength,
		L4::Ipc::Array_ref<char>& data
	);
	long op_control_transfer_out(
		L4::Typeid::Rights<Device>& rights,
		l4_uint8_t endpoint,
		l4_uint8_t bmRequestType,
		l4_uint8_t bRequest,
		l4_uint16_t wValue,
		l4_uint16_t wIndex,
		l4_uint16_t wLength,
		L4::Ipc::Array_ref<const char> data
	);
	long op_interrupt_transfer_in(
		L4::Typeid::Rights<Device>& rights,
		l4_uint8_t endpoint,
		l4_uint8_t length,
		L4::Ipc::Array_ref<char>& data
	);
	long op_interrupt_transfer_out(
		L4::Typeid::Rights<Device>& rights,
		l4_uint8_t endpoint,
		l4_uint8_t length,
		L4::Ipc::Array_ref<const char> data
	);
	// long op_bulk_transfer(
		// L4::Typeid::Rights<Device>& rights,
	// );


};

};
