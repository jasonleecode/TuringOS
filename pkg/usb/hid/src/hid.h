/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <vector>

#include <l4/sys/l4int.h>
#include <l4/re/event>
#include <l4/re/util/event_buffer>
#include <l4/re/util/event_svr>
#include <l4/usb/usb.h>

// hid usage table definitions
#include "hut.h"

#define REPORT_INPUT_CONST		1
#define REPORT_INPUT_VAR		2
#define REPORT_INPUT_REL		4

struct hid_report {
	l4_uint8_t page;
	l4_uint16_t usage;
	l4_uint16_t usage_minimum;
	l4_uint16_t usage_maximum;
	l4_uint16_t logical_minimum;
	l4_uint16_t logical_maximum;
	l4_uint8_t size;
	l4_uint8_t count;
	l4_uint8_t input;
	l4_uint8_t padding;
};

// singed parse <size> bits from <data>-byte-stream
inline int parse_signed(char* data, int size) {
	if (size <= 8) return data[0]; 
	else if (size <= 16) return static_cast<unsigned char>(data[0]) | data[1] << 8; 
	return 0;
}

// unsinged parse <size> bits from <data>-byte-stream
inline int parse(char* data, int size) {
	if (size <= 8) return static_cast<unsigned char>(data[0]); 
	else if (size <= 16) return static_cast<unsigned char>(data[0]) | static_cast<unsigned char>(data[1]) << 8; 
	return 0;
}

class HID: public L4Re::Util::Event_svr<HID>, public L4::Epiface_t<HID, L4Re::Event> {

private:
	int _hid_endpoint;
	int _device_usage;
	int _report_size = 0;
	char _led_state = 0;
	char* _data_prev;
	endpoint_descriptor* _endp_desc;
	hid_descriptor* _hid_desc;
	L4::Cap<USB::Device> _dev;
	L4Re::Util::Event_buffer _evbuf;
	std::vector<hid_report> _reports;

public:
	HID(L4::Cap<USB::Device> dev);
	~HID() {
		if (_data_prev) {delete[] _data_prev;}
		free_event_buffer();
		L4Re::Util::cap_alloc.free(_dev, L4Re::This_task);
	}

	int parse_report_collection(char* report_data, int report_length, std::vector<hid_report>& reports);
	void poll();
	int init_event_buffer();
	int free_event_buffer();
	int get_stream_info_for_id(l4_umword_t id, L4Re::Event_stream_info* info);
	int get_axis_info(l4_umword_t, unsigned naxes, unsigned const * axes, L4Re::Event_absinfo* info);
	void reset_event_buffer() { _evbuf.reset(); }

};
