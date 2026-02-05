/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#include <cstdio>
#include <cstdlib>

#include <l4/re/error_helper>
#include <l4/re/env>
#include <l4/re/util/unique_cap>
#include <l4/util/util.h>

#include "hid.h"


HID::HID(L4::Cap<USB::Device> dev): _dev(dev) {

	_dev->populate_device_infos();

	// SET_IDLE
	L4Re::chksys(_dev->control_transfer(0x00, 0x21, 0x0a, 0x0000, 0x0000, 0, nullptr, USB_DIR_OUT));

	// search for HID and Endpoint Descriptor
	int idx;
	for (idx = 0; _dev->desc_data[idx + 1] != 0x21; idx += _dev->desc_data[idx]) {
		if (idx == _dev->conf_desc->wTotalLength) L4Re::throw_error(-L4_ENODEV, "USB Device doesn't have hid descriptors");
	}

	_hid_desc = reinterpret_cast<hid_descriptor*>(_dev->desc_data + idx);
	_endp_desc = reinterpret_cast<endpoint_descriptor*>(_dev->desc_data + idx + _dev->desc_data[idx]);
	_hid_endpoint = _endp_desc->bEndpointAddress;
	_data_prev = new char[_endp_desc->wMaxPacketSize];

	// GET_DESCRIPTOR - HID Report
	char report_data[_hid_desc->wDescriptorLength];
	L4Re::chksys(_dev->control_transfer(0x00, 0x81, 0x06, 0x2200, 0x0000, _hid_desc->wDescriptorLength, report_data, USB_DIR_IN));
	if (report_data[1] != USAGE_PAGE_GENERIC_DESKTOP_CONTROLS) L4Re::throw_error(-L4_ENODEV, "HID Device other than generic desktop controls not implemented\n");
	_device_usage = report_data[3];

	printf("USB HID Report Descriptor: ");
	for (int i = 0; i < _hid_desc->wDescriptorLength; i++) printf("%02x", static_cast<unsigned char>(report_data[i]));
	printf("\n");

	// for now we only parse the first collection, as this is the default one (and most of the time also the only one)
	L4Re::chksys(parse_report_collection(report_data + 4, _hid_desc->wDescriptorLength - 4, _reports));
	for (hid_report report : _reports) {
		_report_size += report.size * report.count + report.padding;
		printf("\tpage: %x, usage: %x, size: %i, count: %i, usage min-max: %i-%i, logical min-max: %i-%i, padding: %i\n", report.page, report.usage, report.size, report.count, report.usage_minimum, report.usage_maximum, report.logical_minimum, report.logical_maximum, report.padding);
	}
	_report_size >>= 3;	// bit to byte
	printf("Detected HID-%s, report size: %i bytes, report items: %li\n", usage_page_generic_desktop_controls_strings[_device_usage], _report_size, _reports.size());

	L4Re::chksys(init_event_buffer());

}

int HID::parse_report_collection(char* report_desc, int report_length, std::vector<hid_report>& reports) {

	hid_report report = hid_report();
	std::vector<l4_uint16_t> usages;

	for (int idx = 2; idx < report_length; idx += (report_desc[idx] & 0b11) + 1) {

		if ((report_desc[idx] & 0b11) == 0b11) {
			printf("Parsing for HID report item with size 0b11 in header not supported");
			return -L4_EINVAL;
		}

		switch (static_cast<l4_uint8_t>(report_desc[idx]) & ~0b11) {
			
			// Usage Page
			case 0x04: report.page = report_desc[idx + 1]; break;
			
			// Usage
			case 0x08: usages.push_back(parse(report_desc + idx + 1, (report_desc[idx] & 0b11) << 3 /*byte to bit*/)); break;
			
			// Logical Minimum
			case 0x14: report.logical_minimum = parse(report_desc + idx + 1, (report_desc[idx] & 0b11) << 3 /*byte to bit*/); break;
			
			// Usage Minimum
			case 0x18: report.usage_minimum = parse(report_desc + idx + 1, (report_desc[idx] & 0b11) << 3 /*byte to bit*/); break;
						
			// Logical Maximum
			case 0x24: report.logical_maximum = parse(report_desc + idx + 1, (report_desc[idx] & 0b11) << 3 /*byte to bit*/); break;

			// Usage Maximum
			case 0x28: report.usage_maximum = parse(report_desc + idx + 1, (report_desc[idx] & 0b11) << 3 /*byte to bit*/); break;
			
			// Report Size
			case 0x74: report.size = report_desc[idx + 1]; break;
			
			// Report Count
			case 0x94: report.count = report_desc[idx + 1]; break;
			
			// Input
			case 0x80:
				
				// don't ask me why we should have a report without size header, but e.g. virtual box usb tablet does this
				if (!report.size) report.size = 8;

				report.input = report_desc[idx + 1];

				// const output is used as padding to the next 8/16/32-bit-boundary
				if (report.input & REPORT_INPUT_CONST) {
					reports.back().padding += report.count * report.size;
				
				// we rather like to parse multiple usages once here, than on every poll 
				} else if (usages.size()) {
					report.count = 1;
					for (l4_uint16_t usage : usages) {
						report.usage = usage;
						reports.push_back(report);
					}
					usages.clear();
				
				} else {
					reports.push_back(report);
				}
				
				// keep global, reset local items
				report = {
                                    .page = report.page,
                                    .usage = 0,
                                    .usage_minimum = 0,
                                    .usage_maximum = 0,
                                    .logical_minimum = report.logical_minimum,
                                    .logical_maximum = report.logical_maximum,
                                    .size = report.size,
                                    .count = report.count,
                                    .input = 0,
                                    .padding = 0,
                                };
				break;
			
			// Report ID
			case 0x84:
				// for now we just treat this as padding of size 8
				hid_report id;
				id.padding = (report_desc[idx] & 0b11) << 3; // byte to bit
				reports.push_back(id);
				break;
			
			// Output (unimplemented)
			case 0x90: report = {
                                       .page = report.page,
                                       .usage = 0,
                                       .usage_minimum = 0,
                                       .usage_maximum = 0,
                                       .logical_minimum = report.logical_minimum,
                                       .logical_maximum = report.logical_maximum,
                                       .size = report.size,
                                       .count = report.count,
                                       .input = 0,
                                       .padding = 0,
                                   };
                                break;
			
			// Collection
			case 0xa0: idx += L4Re::chksys(parse_report_collection(report_desc + idx, report_length - idx, reports)); break;
			
			// Collection end
			case 0xc0: return idx;
			
			default: printf("unimplemented header: %x\n", static_cast<unsigned>((report_desc[idx]) & ~0b11));
		}
	}

	return -L4_EINVAL;
}

void HID::poll() {

	char data_curr[_endp_desc->wMaxPacketSize];
	if (int size = L4Re::chksys(_dev->interrupt_transfer(_hid_endpoint, _endp_desc->wMaxPacketSize, data_curr, USB_DIR_IN)); size < _report_size) {
		printf("HID report size was %i, should have been %i!\n", size, _report_size);
		return;
	}
	// for (int i = 0; i < _report_size; i++) printf("%02x", static_cast<unsigned char>(data_curr[i])); printf("\n");

	L4Re::Event_buffer::Event ev;
	ev.time = l4_kip_clock(l4re_kip());
	ev.payload.stream_id = 0;
	char* report_curr = data_curr;
	char* report_prev = _data_prev;

	for (hid_report report : _reports) {
		
		if (report.page == USAGE_PAGE_BUTTON) {
			ev.payload.type = L4RE_EV_KEY;
			unsigned short base_btn = L4RE_BTN_MISC;
			switch (_device_usage) {
				case USAGE_MOUSE: base_btn = L4RE_BTN_MOUSE; break;
				case USAGE_JOYSTICK: base_btn = L4RE_BTN_JOYSTICK; break;
				case USAGE_GAMEPAD: base_btn = L4RE_BTN_GAMEPAD; break;
			}
			int mask_new = parse(report_curr, report.count);
			int mask_old = parse(report_prev, report.count);
			for (int i = 0; i < report.count; i++) {
				if ((mask_new & 1 << i) != (mask_old & 1 << i)) {
					ev.payload.code = base_btn + i;
					ev.payload.value = (mask_new >> i) & 1;
					_evbuf.put(ev);
				}
			}
		}

		else if (report.page == USAGE_PAGE_GENERIC_DESKTOP_CONTROLS) {
			ev.payload.type = report.input & REPORT_INPUT_REL ? L4RE_EV_REL : L4RE_EV_ABS;
			ev.payload.code = report.usage & 0xf;
			if (ev.payload.type == L4RE_EV_REL && (ev.payload.value = parse_signed(report_curr, report.size))) _evbuf.put(ev);
			else if ((ev.payload.value = parse(report_curr, report.size)) != parse(report_prev, report.size)) _evbuf.put(ev);
		}

		else if (report.page == USAGE_PAGE_KEYBOARD) {
			ev.payload.type = L4RE_EV_KEY;
			// Ctrl, Alt, Shift, Super, ...
			if (report.input & REPORT_INPUT_VAR) {
				int mask_new = parse(report_curr, report.count);
				int mask_old = parse(report_prev, report.count);
				for (int i = 0; i < report.count; i++) {
					ev.payload.code = key2event[report.usage_minimum + i];
					// if key was pressed before and now
					if ((mask_new & 1 << i) && (mask_old & 1 << i)) {
						ev.payload.value = 2;
						_evbuf.put(ev);
					}
					// else only emit event when key has changed
					else if ((mask_new & 1 << i) != (mask_old & 1 << i)) {
						ev.payload.value = (mask_new >> i) & 1;
						_evbuf.put(ev);
					}
				}
			} else {
				for (int i = 0; i < report.count; i++) {
					if (report_curr[i] > 0 && report_curr[i] < 4) {
						printf("Keyboard detected %s (%i)!\n", report_curr[i] == 1 ? "ERROR_ROLL_OVER" : (report_curr[i] == 2 ? "POST_FAIL" : "ERROR_UNDEFINED"), report_curr[i]);
						break;
					}
					if (report_curr[i] && (ev.payload.code = key2event[(int)report_curr[i]]) != L4RE_KEY_UNKNOWN) {
						// if key was already pressed before, value = 2, else 1
						if (memchr(report_prev, report_curr[i], report.count)) {
							ev.payload.value = 2;
						} else {
							ev.payload.value = 1;
							// TODO: do we need to emit LED-Events? 
							switch (ev.payload.code) {
								case L4RE_KEY_CAPSLOCK:
									_led_state ^= 1;
									L4Re::chksys(_dev->control_transfer(0x00, 0x21, 0x09, 0x0200, 0x0000, 1, &_led_state, USB_DIR_OUT));
									break;
								case L4RE_KEY_NUMLOCK:
									_led_state ^= 2;
									L4Re::chksys(_dev->control_transfer(0x00, 0x21, 0x09, 0x0200, 0x0000, 1, &_led_state, USB_DIR_OUT));
									break;
								case L4RE_KEY_SCROLLLOCK:
									_led_state ^= 4;
									L4Re::chksys(_dev->control_transfer(0x00, 0x21, 0x09, 0x0200, 0x0000, 1, &_led_state, USB_DIR_OUT));
									break;
							}
						}
						_evbuf.put(ev);
					}

					// check for key up
					if (report_prev[i] && !memchr(report_curr, report_prev[i], report.count) && (ev.payload.code = key2event[(int)report_prev[i]]) != L4RE_KEY_UNKNOWN) {
						ev.payload.value = 0;
						_evbuf.put(ev);
					}
				}
			}
		}
		report_curr += (report.count * report.size + report.padding) >> 3;
		report_prev += (report.count * report.size + report.padding) >> 3;
		
	}

	ev.payload.type = L4RE_EV_SYN;
	ev.payload.code = L4RE_SYN_REPORT;
	ev.payload.value = 0;
	ev.payload.stream_id = 0;
	_evbuf.put(ev);

	_irq.trigger();

	memcpy(_data_prev, data_curr, _report_size);

}


int HID::get_stream_info_for_id(l4_umword_t /* id */, L4Re::Event_stream_info* info) {

	memset(info, 0, sizeof(*info));
	strcpy(info->name, "hid-drv");

	// snprintf(info->phys, 32, "Device %03i", _dev->_device_id);

	info->id.bustype = 0x03; // BUS_USB
	info->id.vendor = _dev->dev_desc.idVendor;
	info->id.product = _dev->dev_desc.idProduct;
	info->id.version = _dev->dev_desc.bcdDevice;

	for (hid_report report : _reports) {
		if (report.page == USAGE_PAGE_BUTTON) {
			info->set_evbit(L4RE_EV_KEY, true);
			if (_device_usage == USAGE_MOUSE) {
				for (int i = 0; i < report.count; i++)
					info->set_keybit(L4RE_BTN_MOUSE + i, true);
			}
		}
		else if (report.page == USAGE_PAGE_GENERIC_DESKTOP_CONTROLS) {
			if (report.input & 0x4) {
				info->set_evbit(L4RE_EV_REL, true);
				info->set_relbit(report.usage & 0xf, true);
			} else {
				info->set_evbit(L4RE_EV_ABS, true);
				info->set_absbit(report.usage & 0xf, true); // FIXME: there could be more than 16 absolute inputs
			}
		}
		else if (report.page == USAGE_PAGE_KEYBOARD) {
			info->set_evbit(L4RE_EV_KEY, true);
			if (report.input & REPORT_INPUT_VAR) {
				for (int i = 0; i < report.count; i++)
					info->set_keybit(key2event[report.usage_minimum + i], true);
			} else {
				int limit = sizeof(key2event) / sizeof(*key2event) < report.usage_maximum ? sizeof(key2event) / sizeof(*key2event) : report.usage_maximum;	// quick min
				for (int i = report.usage_minimum; i < limit; i++) {
					info->set_keybit(key2event[i], true);
				}
			}
			
			}
	}

	info->set_evbit(L4RE_EV_SYN, true);

	return 0;
}

int HID::get_axis_info(l4_umword_t /* id */, unsigned naxes, unsigned const * axes, L4Re::Event_absinfo* info) {

	unsigned int axis = 0;
	for (hid_report report : _reports) {
		if (report.page == USAGE_PAGE_GENERIC_DESKTOP_CONTROLS && !(report.input & 0x4)) {
			for (unsigned int idx = 0; idx < naxes; idx++) {
				if (axes[idx] == axis) {
					info[idx].min = report.logical_minimum;
					info[idx].max = report.logical_maximum;
				}
			}
			axis++;
		}
	}
	// report descriptor contains less relative axes than requested
	if (axis < naxes) return -L4_EINVAL;
	
	return L4_EOK;

}


int HID::init_event_buffer() {

	L4Re::Util::Unique_cap<L4Re::Dataspace> ds = L4Re::Util::make_unique_cap<L4Re::Dataspace>();
	if (!ds.is_valid())
		return -L4_ENOMEM;

	int r;
	if ((r = L4Re::Env::env()->mem_alloc()->alloc(L4_PAGESIZE, ds.get())) < 0)
		return r;

	if ((r = _evbuf.attach(ds.get(), L4Re::Env::env()->rm())) < 0)
		return r;

	memset(_evbuf.buf(), 0, ds.get()->size());

	_ds = ds.release();

	return 0;
}


int HID::free_event_buffer() {
	
	int r = _evbuf.detach(L4Re::Env::env()->rm());

	if (r < 0)
		return r;

	if (r != L4Re::Rm::Detached_ds)
		return -L4_EINVAL;

	L4Re::Util::cap_alloc.free(_ds, L4Re::This_task);

	return 0;
}
