/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 * (c) 2025 Adam Lackorzynski <adam@l4re.org>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#include <string.h>

#include <l4/util/util.h>

#include "hc.h"
#include "server.h"
#include "port.h"


namespace USB {

int Server::init() {

	if (!(port->is_powered() && port->is_device_connected())) {
		return -L4_ENODEV;
	}

	port->clear_connected_status_changed();
	port->reset();

	while (!port->is_reset_completed()) { l4_usleep(1); }

	if (!(port->is_reset_completed() && port->is_enabled() && port->is_device_connected())) {
		return -L4_EFAULT;
	}

	port->clear_reset_completed();
	_controller->handle_irq();

	// SET_ADDRESS:
	// tell device listening at init-endpoint to use given device_id
	_controller->control_transfer(_controller->init_endpoint, 0x00, 0x05, device_id, 0x0000, 0, nullptr, USB_DIR_OUT);
	l4_sleep(20); // wait for device to appear at new address, chosen arbitrarily (should be somewhere in specs, not able to find it)

	// create control-endpoint for configuration
	_endpoint[0] = _controller->create_control_endpoint(this->device_id, this->port->is_low_speed(), 0x00, 0x8);

	// GET_DESCRIPTOR - DEVICE
	// read device descriptor (usually 18 Byte)
	int transfered = _controller->control_transfer(_endpoint[0], 0x80, 0x06, 0x0100, 0x0000, 18, reinterpret_cast<char*>(&dev_desc), USB_DIR_IN);
	if (transfered == 0) {
		printf("could not read device descriptor\n");
		return -L4_EFAULT;
	} else if (dev_desc.bLength != 18) {
		printf("device descriptor length != 18, unimplemented\n");
		return -L4_EINVAL;
	}

	// GET_DESCRIPTOR - CONFIGURATION
	// read configuration descriptor for configuration 0 (first 9 bytes)
	_controller->control_transfer(_endpoint[0], 0x80, 0x06, 0x0200, 0x0000, 9, _desc_data, USB_DIR_IN);

	// now we know the actual length of all descriptors, so read it all out
	_desc_data = static_cast<char*>(realloc(_desc_data, _config_desc->wTotalLength));
	// GET_DESCRIPTOR - CONFIGURATION
	_controller->control_transfer(_endpoint[0], 0x80, 0x06, 0x0200, 0x0000, _config_desc->wTotalLength, _desc_data, USB_DIR_IN);

	// iterate the descriptors until we find the first interface descriptor
	int index = 0;
	while (_desc_data[index + 1] != 0x04) {
		if (index == _config_desc->wTotalLength) { return -L4_EINVAL; }
		index += _desc_data[index];
	}
	_iface_desc = reinterpret_cast<interface_descriptor*>(_desc_data + index);

	while (_desc_data[index + 1] != 0x05) {
		if (index == _config_desc->wTotalLength) { return -L4_EINVAL; }
		index += _desc_data[index];
	}
	_endp_desc = reinterpret_cast<endpoint_descriptor*>(_desc_data + index);

	// SET_CONFIGURATION
	// finish with enabling the desired configuration, as long as we don't support multiple configurations we just use configuration one
	_controller->control_transfer(_endpoint[0], 0x00, 0x09, 0x0001, 0x0000, 0, nullptr, USB_DIR_OUT);

	return L4_EOK;
}

void Server::lsusb() {

	printf("Device %03i: ID %04x:%04x", device_id, dev_desc.idVendor, dev_desc.idProduct);

	char string_data[255];

	// read available language ids
	// GET_DESCRIPTOR - STRING
	_controller->control_transfer(_endpoint[0], 0x80, 0x06, 0x0300, 0x0000, 255, string_data, USB_DIR_IN);

	// proceed if we have at least one language id present
	if (string_data[0] > 2) {

		int langid = string_data[3] << 8 | string_data[2];

		if (dev_desc.iManufacturer) {
			printf(", Manufacturer ");
			// GET_DESCRIPTOR - STRING
			int length = _controller->control_transfer(_endpoint[0], 0x80, 0x06, 0x0300 | dev_desc.iManufacturer, langid, 255, string_data, USB_DIR_IN);
			for (int i = 2; i < length; i += 2) { printf("%c", string_data[i]); }
		}
		if (dev_desc.iProduct) {
			printf(", Product ");
			// GET_DESCRIPTOR - STRING
			int length = _controller->control_transfer(_endpoint[0], 0x80, 0x06, 0x0300 | dev_desc.iProduct, langid, 255, string_data, USB_DIR_IN);
			for (int i = 2; i < length; i += 2) { printf("%c", string_data[i]); }
		}
		if (dev_desc.iSerialNumber) {
			printf(", SerialNumber ");
			// GET_DESCRIPTOR - STRING
			int length = _controller->control_transfer(_endpoint[0], 0x80, 0x06, 0x0300 | dev_desc.iSerialNumber, langid, 255, string_data, USB_DIR_IN);
			for (int i = 2; i < length; i += 2) { printf("%c", string_data[i]); }
		}
	}

	printf(", Class %s, %s-speed\n", interface_classes[dev_desc.bDeviceClass ? dev_desc.bDeviceClass : _iface_desc->bInterfaceClass], port->is_low_speed() ? "low" : "full");

}

long Server::op_control_transfer_in(L4::Typeid::Rights<Device>&, l4_uint8_t endpoint, l4_uint8_t bmRequestType, l4_uint8_t bRequest, l4_uint16_t wValue, l4_uint16_t wIndex, l4_uint16_t wLength, L4::Ipc::Array_ref<char>& data) {
	if (!_endpoint[endpoint & 0xf]) { _endpoint[endpoint & 0xf] = _controller->create_control_endpoint(this->device_id, this->port->is_low_speed(), endpoint, _endp_desc->wMaxPacketSize); }	// TODO: support more than one additional endpoint
	return _controller->control_transfer(_endpoint[endpoint & 0xf], bmRequestType, bRequest, wValue, wIndex, wLength, data.data, USB_DIR_IN);
}

long Server::op_control_transfer_out(L4::Typeid::Rights<Device>&, l4_uint8_t endpoint, l4_uint8_t bmRequestType, l4_uint8_t bRequest, l4_uint16_t wValue, l4_uint16_t wIndex, l4_uint16_t wLength, L4::Ipc::Array_ref<const char> data) {
	if (!_endpoint[endpoint & 0xf]) { _endpoint[endpoint & 0xf] = _controller->create_control_endpoint(this->device_id, this->port->is_low_speed(), endpoint, _endp_desc->wMaxPacketSize); }	// TODO: support more than one additional endpoint
	return _controller->control_transfer(_endpoint[endpoint & 0xf], bmRequestType, bRequest, wValue, wIndex, wLength, const_cast<char*>(data.data), USB_DIR_OUT);
}

long Server::op_interrupt_transfer_in(L4::Typeid::Rights<Device>&, l4_uint8_t endpoint, l4_uint8_t length, L4::Ipc::Array_ref<char>& data) {
	if (!_endpoint[endpoint & 0xf]) { _endpoint[endpoint & 0xf] = _controller->create_interrupt_endpoint(this->device_id, this->port->is_low_speed(), endpoint, _endp_desc->wMaxPacketSize, _endp_desc->bInterval); }	// TODO: support more than one additional endpoint
	int transfered = _controller->interrupt_transfer(_endpoint[endpoint & 0xf], length, const_cast<char*>(data.data), USB_DIR_IN);
	return transfered;
}

long Server::op_interrupt_transfer_out(L4::Typeid::Rights<Device>&, l4_uint8_t endpoint, l4_uint8_t length, L4::Ipc::Array_ref<const char> data) {
	if (!_endpoint[endpoint & 0xf]) { _endpoint[endpoint & 0xf] = _controller->create_interrupt_endpoint(this->device_id, this->port->is_low_speed(), endpoint, _endp_desc->wMaxPacketSize, _endp_desc->bInterval); }	// TODO: support more than one additional endpoint
	return _controller->interrupt_transfer(_endpoint[endpoint & 0xf], length, const_cast<char*>(data.data), USB_DIR_OUT);
}


}
