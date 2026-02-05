/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <l4/re/env>
#include <l4/re/error_helper>
#include <l4/re/util/shared_cap>
#include <l4/re/util/object_registry>
#include <l4/vbus/vbus>
#include <l4/vbus/vbus_pci>
#include <l4/cxx/bitfield>
#include <l4/sys/l4int.h>
// #include <l4/drivers/hw_mmio_register_block>

#include <list>
#include <vector>
#include <stdio.h>
#include <cassert>
#include <string>
#include "types.h"
#include "port.h"

namespace USB {

class Server;

/**
 * Encapsulates one single USB Host Controller.
 *
 * Includes a server loop for handling device interrupts.
 */
class Host_Controller : public L4::Irqep_t<Host_Controller> {

protected:

	L4vbus::Pci_dev _dev;
	l4vbus_device_t _device_info;
	L4::Cap<L4::Icu> _icu;
	L4Re::Util::Object_registry* _registry;
	l4_size_t _dma_size = L4_PAGESIZE * 16; // 16 chosen arbitrarily
	L4Re::Util::Unique_cap<L4Re::Dataspace> _dma_ds;
	L4Re::Util::Shared_cap<L4Re::Dma_space> _dma;
	L4Re::Dma_space::Dma_addr _dma_paddr;
	L4Re::Rm::Unique_region<void *> _dma_region;
	L4Re::Rm::Unique_region<l4_addr_t> _regs_region;
	L4::Cap<L4::Irq> irq;
	// Iomem _iomem;
	// L4drivers::Register_block<32> _regs;
	unsigned char _irq_trigger_type;

public:

	endpoint_struct* init_endpoint;
	std::vector<Server*> devices;
	int current_device_id = 1;
	Port* port[16];
	int num_ports;

	Host_Controller(L4vbus::Pci_dev const& dev, l4vbus_device_t device_info, L4::Cap<L4::Icu> icu, L4Re::Util::Object_registry* registry); // L4Re::Util::Shared_cap<L4Re::Dma_space> const &dma
	void register_interrupt_handler();

	// called from the hc-driver to report device-changes to the stack
	int callback_device_attached(int portnum);
	// int callback_device_detached(int portnum); UNIMPLEMENTED


	// The following functions must be overridden in all derived Host Controller Drivers

	// as we are using L4Re::Util::Registry_server for irqs, they only get delivered after starting the server-loop
	// therefore irqs must be handled manually during setup via 'irq->receive(); handle_irq()'
	// at the end of interrupt handling, it must be unmasked via "if (!_irq_trigger_type) obj_cap()->unmask();""
	virtual void handle_irq() = 0;

	virtual int init() = 0;

	virtual endpoint_struct* create_control_endpoint(int device_id, int speed, int bEndpointAddress, int wMaxPacketSize) = 0;
	virtual endpoint_struct* create_interrupt_endpoint(int device_id, int speed, int bEndpointAddress, int wMaxPacketSize, int bInterval) = 0;

	virtual int control_transfer(endpoint_struct* endpoint, int bmRequestType, int bRequest, int wValue, int wIndex, int wLength, char* data, int direction) = 0;
	virtual int interrupt_transfer(endpoint_struct* endpoint, int length, char* data, int direction) = 0;

};

}
