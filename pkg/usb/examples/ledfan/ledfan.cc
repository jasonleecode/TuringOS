/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#include "ledfan.h"
#include "ascii.h"
#include <l4/util/util.h>
#include <iostream>
// #include "ohci.h"
// #include "library/random.h"


int LedFan::setupHid() {

	// TODO Find out direction of endpoint
	std::cout << "Configuring HID endpoint" << std::endl;
	// hid_endpoint = this->ohci->create_control_endpoint(port->is_low_speed(), deviceID, 0x00, 0x8);
	hid_endpoint = 0x00;

	// SET_IDLE
	// this->ohci->control_transfer(this, hid_endpoint, 0x21, 0x0a, 0x0000, 0x0000, 0, nullptr, direction::OUT);

	// GET_DESCRIPTOR HID REPORT
	// this->ohci->control_transfer(this, hid_endpoint, 0x81, 0x06, 0x0022, 0x0000, 31, nullptr, direction::IN);

	// setup-interrupt-transfer
	// interrupt_endpoint = this->ohci->create_interrupt_endpoint(port->is_low_speed(), deviceID, 0x81, 0x8, 10);
	// USB::endpoint_descriptor* hid_endp_desc = reinterpret_cast<USB::endpoint_descriptor*>(_desc_data + conf_desc->bLength + _desc_data[conf_desc->bLength]);
	interrupt_endpoint = 0x81;
	// buzzer.sleep(50);
	l4_sleep(50);

	return 0;
}


int LedFan::program(char* data, int length) {

	std::cout << "                                 " << std::endl;
	std::cout << ">>> LED-FAN Flasher <<<" << std::endl;
	std::cout << "                                            " << std::endl;

	std::cout << "Flashing 3 times - " << length * 8 << " bytes each time" << std::endl << std::endl;

	for (int i = 0; i < 3; i++){

		std::cout << "Programming " << i + 1 << ". iteration: " << std::endl;

		// this->ohci->interrupt_transfer(this, interrupt_endpoint, 9);
		// buzzer.sleep(150);
		l4_sleep(150);


		long* data_ptr = (long *) data;
		for (int i = 0; i < length; i++){

			_dev->control_transfer(hid_endpoint, 0x21, 9, 0x200, 0, 8, (char *)&data_ptr[i], USB_DIR_OUT);

			//char interrupt_data[8];
			//int transfered = _dev->interrupt_transfer(interrupt_endpoint, 8, interrupt_data, USB_DIR_IN);
			// for (int i = 0; i < transfered; i++) {
				// printf("%02x", interrupt_data[i]);
			// }
			// printf("\n");
			// this->ohci->dump_operational_registers(7, 17);
			std::cout << "." << std::flush;

			// buzzer.sleep(50);
			// l4_sleep(50);
		}

		// this->ohci->interrupt_transfer(this, interrupt_endpoint, 9);

		// buzzer.sleep(50);
		l4_sleep(50);
		// this->ohci->regs[1] &= 0xfffffffd; // Disable interval list
	    // interrupt_endpoint->head = 1; // Halt endpoint

	    std::cout << std::endl << "Iteration " << i+1 << " done!" << std::endl << std::endl;

	    // buzzer.sleep(300);
	    l4_sleep(300);
	}

	std::cout << ">>> LED-FAN flash complete! <<<" << std::endl;
	std::cout << "                                            " << std::endl;

	return 0;
}


void LedFan::ascii_print(const char *input, int length) {
	length = length > 16 ? 16 : length;

	// Random random = Random();

	int num_collums = length * 8;
	short pixels[length * 8];
	char colors[length * 8];

	for (int i = 0; i < length; i++) {
		char color;
		do {
			// color = random.number() & 0x0000000e;
			color = 0x2;
		} while (color == 0x0);

		for (int j = 0; j < 8; j++) {
			pixels[i * 8 + j] = ascii[(int)input[i]][j];
			colors[i * 8 + j] = color;
		}
	}


	int pindex = 0;
	char programm[length * 8 * 2 + 13 + 2 + 10];
	programm[pindex++] = num_collums >= 119 ? 0x02 : 0x01; 	// max(floor(log_2(data_length) - 6), 1)
	programm[pindex++] = num_collums * 2 + 18;
	programm[pindex++] = (num_collums * 2 + 18) << 8;
	programm[pindex++] = 0x00;
	programm[pindex++] = 0x00;
	programm[pindex++] = 0x00;
	programm[pindex++] = 0x80 + 1; 	// number of programms
	programm[pindex++] = num_collums + 2;
	programm[pindex++] = '\0';
	programm[pindex++] = '\0'; // message_style
	programm[pindex++] = '\0'; // open_transition << 4 | close_transition
	programm[pindex++] = '\0';
	programm[pindex++] = '\0';

	for (int i = num_collums - 1; i >= 0; i--) {
		programm[pindex++] = colors[i] << 4 | pixels[i] >> 8;
		programm[pindex++] = pixels[i];
	}

	programm[pindex++] = 0x00;
	programm[pindex++] = 0x00;

	for (int i = 0; i < 10; i++) {
		programm[pindex++] = 0x00;
	}

	int dindex = 0;
	char data[(num_collums * 2 + 18) * 8 / 5];

	data[dindex++] = 0x40;
	data[dindex++] = 0x40;
	for (int i = 0; i < 5; i++) {
		data[dindex++] = programm[i];

	}
	data[dindex++] = data[0] + data[1] + data[2] + data[3] + data[4] + data[5] + data[6];

	for (int i = 5; i < pindex; i += 5) {
		data[dindex++] = 0x40;
		data[dindex++] = 0x23;
		data[dindex++] = 0xa4 - programm[i];
		data[dindex++] = 0xa4 - programm[i + 1];
		data[dindex++] = 0xa4 - programm[i + 2];
		data[dindex++] = 0xa4 - programm[i + 3];
		data[dindex++] = 0xa4 - programm[i + 4];
		data[dindex] = data[dindex - 1] + data[dindex - 2] + data[dindex - 3] + data[dindex - 4] + data[dindex - 5] + data[dindex - 6] + data[dindex - 7];
                ++dindex;
	}

	program(data, (num_collums * 2 + 18) / 5);


}
