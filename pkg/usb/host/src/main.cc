/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#include <iostream>
#include <vector>
#include <map>
#include <l4/util/util.h>
#include <l4/cxx/ipc_server>
#include <l4/re/util/object_registry>
#include <l4/re/util/br_manager>
#include <l4/re/error_helper>
#include <l4/vbus/vbus>
#include <l4/vbus/vbus_pci>
#include <l4/vbus/vbus_interfaces.h>
#include <l4/re/util/shared_cap>

#include <l4/usb/usb.h>
#include "hc.h"
#include "server.h"
#include "ohci.h"

L4Re::Util::Registry_server<L4Re::Util::Br_manager_hooks> registry;


class Device_manager: public L4::Epiface_t<Device_manager, L4::Factory> {
private:
	std::vector<USB::Host_Controller*> controllers;

public:

	void discover_controllers() {

		L4::Cap<L4vbus::Vbus> vbus = L4Re::chkcap(L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus"), "could not get vbus cap");
		L4vbus::Device root = vbus->root();

		L4vbus::Icu icudev;
		L4Re::chksys(root.device_by_hid(&icudev, "L40009"), "Look for ICU device.");

		L4::Cap<L4::Icu> icu = L4Re::chkcap(L4Re::Util::cap_alloc.alloc<L4::Icu>(), "Allocate ICU capability.");
		L4Re::chksys(icudev.vicu(icu), "Request ICU capability.");

		L4vbus::Pci_dev vbus_device;
		l4vbus_device_t vbus_device_info;

		printf("Scanning vbus for usb host controllers...\n");
		while (root.next_device(&vbus_device, L4VBUS_MAX_DEPTH, &vbus_device_info) == L4_EOK) {

			// check if we got an pci device
			if (l4vbus_subinterface_supported(vbus_device_info.type, L4VBUS_INTERFACE_PCIDEV)) {

				L4vbus::Pci_dev& pci_dev = static_cast<L4vbus::Pci_dev&>(vbus_device);

				// vendor and device id
				l4_uint32_t val;
				if (pci_dev.cfg_read(0, &val, 32) == L4_EOK) {

					printf("PCI device [%04x:%04x] - ", val & 0xffff, val >> 16);
					L4Re::chksys(pci_dev.cfg_read(8, &val, 32));

					// class    = 0c (serial bus controller)
					// subclass = 03 (USB controller)
					// prgif    = 00/10/20/30 (UHCI/OHCI/EHCI/XHCI)
					switch (val >> 8) {

					case 0x0c0300:
						printf("UHCI\n");
						break;

					case 0x0c0310:
						printf("OHCI\n");
						// FIXME: when testing on real hardware with multiple OHCIs, both requested the same irq (?) so for now we skip all but the first
						if (this->controllers.size() > 0) break;

						this->controllers.push_back(new USB::OHCI(pci_dev, vbus_device_info, icu, registry.registry()));
						break;

					case 0x0c0320:
						printf("EHCI\n");
						break;

					case 0x0c0330:
						printf("XHCI\n");
						break;

					default:
						printf("Not an USB-Hostcontroller\n");
					}
				}
			}
		}

		printf("Scan finished.\n");

	}

	void initialize_bus() {

		for (USB::Host_Controller* controller : this->controllers) {

			if (controller->init()) {
				printf("Error initializing controller!\n");
				continue;
			}

			// used to assign device-id to newly attached devices
			controller->init_endpoint = controller->create_control_endpoint(0, 0, 0x00, 8);

			for (int portnum = 0; portnum < controller->num_ports; portnum++){

				if (controller->port[portnum]->is_device_connected()) {

					if (int err = controller->callback_device_attached(portnum))
						printf("Error %i initializing device on port %i!\n", err, portnum);

				}
			}
		}
	}

	void lsusb() {

		int busnum = 0;
		for (USB::Host_Controller* controller : this->controllers) {

			busnum++;
			for (USB::Server* dev : controller->devices) {

				printf("Bus %03i ", busnum);
				dev->lsusb();
			}
		}
	}

	// serve ipc-request for attaching to a usb-device
	long op_create(L4::Factory::Rights, L4::Ipc::Cap<void>& res, l4_mword_t type, L4::Ipc::Varg_list<> args) {

		L4::Ipc::Varg tag = args.pop_front();
		if (!tag.is_of<char const*>()) return -L4_EINVAL;

		printf("Request for USB device '%s' (Type: %ld)\n", tag.data(), type);

		for (USB::Host_Controller* controller : this->controllers) {

			for (USB::Server* dev : controller->devices) {
				if (dev->dev_desc.idVendor == strtol(tag.data(), NULL, 16) && dev->dev_desc.idProduct == strtol(tag.data() + 5, NULL, 16)) {
				// dev->interface_descriptor.bInterfaceClass == USB_HID...

					printf("Device found, handing out capability!\n");
					registry.registry()->register_obj(dev);
					res = L4::Ipc::make_cap_rw(dev->obj_cap());
					return L4_EOK;
				}
			}
		}
		printf("Device not present on any USB bus\n");
		res = L4::Ipc::make_cap_rw(L4::Cap<USB::Device>(L4_INVALID_CAP_BIT));
		return -L4_ENODEV;
	}

};

int main() {

	printf("Hello from usb driver :)\n");

	Device_manager device_manager;

	device_manager.discover_controllers();
	device_manager.initialize_bus();
	device_manager.lsusb();

	L4Re::chkcap(registry.registry()->register_obj(&device_manager, "svr"), "Could not register manager");

	printf("Starting server loop\n");
	registry.loop();

}
