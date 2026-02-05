/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <cstdint>
#include <cstdlib>
#include <l4/re/error_helper>
#include <l4/sys/cxx/ipc_array>
#include <l4/sys/cxx/ipc_iface>

enum Direction {
	USB_DIR_IN = 0b10,
	USB_DIR_OUT = 0b01,
};

// we use these structs for parsing raw data sent from the device, so disable padding
#pragma pack(push,1)
struct device_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t bcdUSB;
	uint8_t bDeviceClass;
	uint8_t bDeviceSubClass;
	uint8_t bDeviceProtocol;
	uint8_t bMaxPacketSize0;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t iManufacturer;
	uint8_t iProduct;
	uint8_t iSerialNumber;
	uint8_t bNumConfigurations;
};
struct configuration_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t wTotalLength;
	uint8_t bNumInterfaces;
	uint8_t bConfigurationValue;
	uint8_t iConfiguration;
	uint8_t bmAttributes;
	uint8_t bMaxPower;
};
struct interface_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bInterfaceNumber;
	uint8_t bAlternateSetting;
	uint8_t bNumEndpoints;
	uint8_t bInterfaceClass;
	uint8_t bInterfaceSubClass;
	uint8_t bInterfaceProtocol;
	uint8_t iInterface;
};
struct endpoint_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bEndpointAddress;
	uint8_t bmAttributes;
	uint16_t wMaxPacketSize;
	uint8_t bInterval;
};
struct hid_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t bcdHID;
	uint8_t bCountryCode;
	uint8_t bNumDescriptors;
	uint8_t bDescriptorSubType;
	uint16_t wDescriptorLength;
};
#pragma pack(pop)

namespace USB {

class Device: public L4::Kobject_t<Device, L4::Kobject> {
public:

	char* desc_data = nullptr;
	device_descriptor dev_desc;
	configuration_descriptor* conf_desc;

	void populate_device_infos() {

		L4Re::chksys(this->get_dev_desc(&dev_desc));
		
		long desc_length = L4Re::chksys(this->get_desc_length());
		
		desc_data = new char[desc_length];
		
		L4::Ipc::Array<char> ipc_array(desc_length, desc_data);
		
		L4Re::chksys(this->get_desc_data(ipc_array));
		
		conf_desc = reinterpret_cast<configuration_descriptor*>(desc_data);
	}

	~Device() { delete[] desc_data; }

	long control_transfer(l4_uint8_t endpoint, l4_uint8_t bmRequestType, l4_uint8_t bRequest, l4_uint16_t wValue, l4_uint16_t wIndex, l4_uint16_t wLength, char* data, Direction dir) {
		if (dir == USB_DIR_IN) {
			L4::Ipc::Array<char> ipc_array(wLength, data);
			return control_transfer_in(endpoint, bmRequestType, bRequest, wValue, wIndex, wLength, ipc_array);
		} else {
			L4::Ipc::Array<const char> ipc_array(wLength, data);
			return control_transfer_out(endpoint, bmRequestType, bRequest, wValue, wIndex, wLength, ipc_array);
		}
	}

	long interrupt_transfer(l4_uint8_t endpoint, l4_uint8_t length, char* data, Direction dir) {
		if (dir == USB_DIR_IN) {
			L4::Ipc::Array<char> ipc_array(length, data);
			return interrupt_transfer_in(endpoint, length, ipc_array);
		} else {
			L4::Ipc::Array<const char> ipc_array(length, data);
			return interrupt_transfer_out(endpoint, length, ipc_array);
		}
	}

	L4_INLINE_RPC(long, get_desc_length, ());
	L4_INLINE_RPC(long, get_desc_data, (L4::Ipc::Array<char>&));
	L4_INLINE_RPC(long, get_dev_desc, (device_descriptor*));
	L4_INLINE_RPC(long, control_transfer_in, (l4_uint8_t, l4_uint8_t, l4_uint8_t, l4_uint16_t, l4_uint16_t, l4_uint16_t, L4::Ipc::Array<char>&));
	L4_INLINE_RPC(long, control_transfer_out, (l4_uint8_t, l4_uint8_t, l4_uint8_t, l4_uint16_t, l4_uint16_t, l4_uint16_t, L4::Ipc::Array<const char>));
	L4_INLINE_RPC(long, interrupt_transfer_in, (l4_uint8_t, l4_uint8_t, L4::Ipc::Array<char>&));
	L4_INLINE_RPC(long, interrupt_transfer_out, (l4_uint8_t, l4_uint8_t, L4::Ipc::Array<const char>));

	typedef L4::Typeid::Rpcs<
		get_desc_length_t,
		get_desc_data_t,
		get_dev_desc_t,
		control_transfer_in_t,
		control_transfer_out_t,
		interrupt_transfer_in_t,
		interrupt_transfer_out_t
	> Rpcs;

};

};
