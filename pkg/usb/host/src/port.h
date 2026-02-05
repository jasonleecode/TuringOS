/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <iostream>

namespace USB {

// ! Currently works for OHCI only !
class Port {

private:
	int* status_register;

public:
	Port(int* status_register): status_register(status_register) {};
	bool is_device_connected() {return *status_register & 1 << 0;}
	void clear_enabled() {*status_register = 1 << 0;}
	bool is_enabled() {return *status_register & 1 << 1;}
	void enable() {*status_register = 1 << 1;}
	bool is_suspended() {return *status_register & 1 << 2;}
	void suspend() {*status_register = 1 << 2;}
	bool is_overcurrented() {return *status_register & 1 << 3;}
	void clear_suspend() {*status_register = 1 << 3;}
	bool is_reset() {return *status_register & 1 << 4;}
	void reset() {*status_register = 1 << 4;}
	bool is_powered() {return *status_register & 1 << 8;}
	void power() {*status_register = 1 << 8;}
	bool is_low_speed() {return *status_register & 1 << 9;}
	void clear_power() {*status_register = 1 << 9;}
	bool is_connected_status_changed() {return *status_register & 1 << 16;}
	void clear_connected_status_changed() {*status_register = 1 << 16;}
	bool is_enabled_status_changed() {return *status_register & 1 << 17;}
	void clear_enabled_status_changed() {*status_register = 1 << 17;}
	bool is_suspended_status_changed() {return *status_register & 1 << 18;}
	void clear_suspended_status_changed() {*status_register = 1 << 18;}
	bool is_overcurrented_status_changed() {return *status_register & 1 << 19;}
	void clear_overcurrented_status_changed() {*status_register = 1 << 19;}
	bool is_reset_completed() {return *status_register & 1 << 20;}
	void clear_reset_completed() {*status_register = 1 << 20;}
	void get_status() {
		if (is_device_connected()) printf("- device connected\n");
		if (is_enabled()) printf("- port is enabled\n");
		if (is_suspended()) printf("- port is suspended\n");
		if (is_overcurrented()) printf("- overcurrent condition detected\n");
		if (is_reset()) printf("- port reset signal is active\n");
		if (is_powered()) printf("- port power is on\n");
		if (is_enabled()) printf(is_low_speed() ? "- low speed device attached\n" : "- high speed device attached\n");
		if (is_connected_status_changed()) printf("- change in CurrentConnectStatus\n");
		if (is_enabled_status_changed()) printf("- change in PortEnableStatus\n");
		if (is_suspended_status_changed()) printf("- resume completed\n");
		if (is_overcurrented_status_changed()) printf("- PortOverCurrentIndicator has changed\n");
		if (is_reset_completed()) printf("- port reset is complete\n");
		// std::cout << "---" << std::endl;

	};
};

}
