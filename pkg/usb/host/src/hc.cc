/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#include "hc.h"
#include "server.h"

namespace USB {

Host_Controller::Host_Controller(
	L4vbus::Pci_dev const& dev,
	l4vbus_device_t device_info,
	L4::Cap<L4::Icu> icu,
	L4Re::Util::Object_registry* registry
): _dev(dev), _device_info(device_info), _icu(icu), _registry(registry) {

	// PCI bus master is disabled by fiasco, mandatory to generate DMA requests
	l4_uint32_t cmd;
	this->_dev.cfg_read(0x04, &cmd, 16);
	if (!(cmd & 4)) {
		this->_dev.cfg_write(0x04, cmd | 4, 16);
	}


	for (auto i = 0u; i < this->_device_info.num_resources; ++i) {

		l4vbus_resource_t res;
		L4Re::chksys(this->_dev.get_resource(i, &res), "Getting resource.");
		//   printf("  index: %i, type: %i, start: %x, end: %x, prov: %x\n", i, res, res.type, res.start, res.end, res.provider);

		if (res.type == L4VBUS_RESOURCE_DMA_DOMAIN) {

			this->_dma = L4Re::chkcap(L4Re::Util::make_shared_cap<L4Re::Dma_space>(), "Allocate capability for DMA space.");

			L4Re::chksys(L4Re::Env::env()->user_factory()->create(this->_dma.get()), "Create DMA space.");

			L4Re::chksys(this->_dev.bus_cap()->assign_dma_domain(res.start, L4VBUS_DMAD_BIND | L4VBUS_DMAD_L4RE_DMA_SPACE, this->_dma.get()), "Binding DMA Space to device`s DMA domain.");

			_dma_ds = L4Re::chkcap(L4Re::Util::make_unique_cap<L4Re::Dataspace>(), "Allocate dataspace capability for DMA memory.");

			L4Re::chksys(L4Re::Env::env()->mem_alloc()->alloc(_dma_size, _dma_ds.get(), L4Re::Mem_alloc::Continuous | L4Re::Mem_alloc::Pinned), "Allocate memory for DMA.");

			L4Re::chksys(L4Re::Env::env()->rm()->attach(&_dma_region, _dma_size, L4Re::Rm::F::Search_addr | L4Re::Rm::F::Cache_uncached | L4Re::Rm::F::RW, L4::Ipc::make_cap_rw(_dma_ds.get()), 0, L4_PAGESHIFT), "Attach DMA memory to region map.");

			L4Re::chksys(this->_dma->map(L4::Ipc::make_cap_rw(_dma_ds.get()), 0, &_dma_size, L4Re::Dma_space::Attributes::None, L4Re::Dma_space::Direction::Bidirectional, &_dma_paddr), "Lock memory region for DMA.");

			printf("\t[HCD]: DMA - domain %lu, size: %li bytes, physical addr: %p, virtual addr: %p\n", res.start, _dma_size, (void*)_dma_paddr, _dma_region.get());

		} else if (res.type == L4VBUS_RESOURCE_IRQ) {

			printf("\t[HCD]: Interrupt number: %li\n", res.start);

			// l4irq_request(res.start, handle_irq, 0, 0xff, 0);
			this->register_interrupt_handler();

		} else if (res.type == L4VBUS_RESOURCE_MEM) {

			L4Re::chksys(L4Re::Env::env()->rm()->attach(&_regs_region, res.end - res.start, L4Re::Rm::F::Search_addr | L4Re::Rm::F::Cache_uncached | L4Re::Rm::F::RW, L4::Ipc::make_cap_rw(_dev.bus_cap()), res.start, L4_PAGESHIFT));

			printf("\t[HCD]: MMIO-Registers: %p - %p, mapped at addr %p\n", (void*)res.start, (void*)res.end, (void*)_regs_region.get());
			// this->_regs = new L4drivers::Mmio_register_block<32>(regs_region.get());
		}
	}

}


void Host_Controller::register_interrupt_handler() {

	unsigned char polarity;
	int irqnum = L4Re::chksys(_dev.irq_enable(&_irq_trigger_type, &polarity), "Enabling interrupt.");

	// printf("Device: interrupt: %d trigger: %d, polarity: %d\n", irqnum, _irq_trigger_type, polarity);

	// TODO: is it better to switch to an src/l4/pkg/io/io/server/src/irq_server.cc? (desired advantage: interrupts get delivered during setup)
	this->irq = L4Re::chkcap(_registry->register_irq_obj(this), "Registering IRQ server object.");
	// L4Re::chksys(irqcap->bind_thread(L4Re::Env::env()->main_thread(), 0), "could not bind main thread to interrupt");

	if (L4Re::chksys(l4_error(_icu->bind(irqnum, irq)), "Binding interrupt to ICU.")) {
		L4Re::chksys(l4_ipc_error(_icu->unmask(irqnum), l4_utcb()), "Unmasking interrupt");
	} else {
		L4Re::chksys(l4_ipc_error(irq->unmask(), l4_utcb()), "Unmasking interrupt");
	}

	// printf("Attached to interrupt %d\n", irqnum);
}

int Host_Controller::callback_device_attached(int portnum) {

	USB::Server* dev = new USB::Server(this, this->port[portnum], this->current_device_id++);

	if (int err = dev->init()) {
		return err;
	}

	this->devices.push_back(dev);
	return L4_EOK;
}


};
