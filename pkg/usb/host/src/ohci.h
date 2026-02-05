/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 * (c) 2025 Adam Lackorzynski <adam@l4re.org>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include "hc.h"

#include <iostream>

namespace USB {

enum Direction {
	USB_DIR_IN = 0b10,
	USB_DIR_OUT = 0b01,
};

class OHCI : public Host_Controller {

private:

	int* regs;
	void *hcca;
	int* hcca_done_head;
	endpoint_struct* area_ed;
	transfer_struct* area_td;
	char* area_data;

	inline void control_list_disable(endpoint_struct* endpoint){
		endpoint->head = 1; // Point to nullptr and halt manually
		regs[1] &= 0xFFFFFFEF;
	}
	inline void interrupt_list_disable(endpoint_struct* endpoint){
		endpoint->head = 1; // Point to nullptr and halt manually
		regs[1] &= 0xFFFFFFCF;
	}
	unsigned long long virt2phys(void *virt) {
          if (virt < hcca) return ~0ull;
          return (unsigned long)virt - (unsigned long)hcca + _dma_paddr;
	}
	void *phys2virt(unsigned long long phys) {
		if (phys < _dma_paddr) {return nullptr;}
		return (void *)(phys - (unsigned long long)_dma_paddr + (unsigned long)hcca);
	}

	inline void interrupt_list_enable()			{ regs[1] |= 1 << 2;	 }
	inline void control_list_enable()                       { regs[1] |= 1 << 4;     }
	inline void control_list_signal()                       { regs[2] |= 1 << 1;     }
	inline void control_list_enable_and_signal(){ regs[1] |= 1 << 4; regs[2] |= 1 << 1; }

	void dump_operational_registers(int start, int end);
	void dump_hcca(int start, int end);

public:

	virtual int init() override;

	using Host_Controller::Host_Controller;

	virtual endpoint_struct* create_control_endpoint(int device_id, int speed, int bEndpointAddress, int wMaxPacketSize) override;
	virtual endpoint_struct* create_interrupt_endpoint(int device_id, int speed, int bEndpointAddress, int wMaxPacketSize, int bInterval) override;

	virtual int control_transfer(endpoint_struct* endpoint, int bmRequestType, int bRequest, int wValue, int wIndex, int wLength, char* data, int direction) override;
	virtual int interrupt_transfer(endpoint_struct* endpoint, int length, char* data, int direction) override;

	// libirq
	// static void handle_irq(void* data) {
	//      printf("IMPLEMENT ME: libirq: handle_irq to ohci\n");
	// }

	// registry->register_irq_obj
	virtual void handle_irq() override;


};

enum ohci_transfer_descriptor_fields {
	BUFFERROUNDING = 0b1 << 18,
	DIRECTION_PID = 0b11 << 19,
	DELAYINTERRUPT = 0b111 << 21,
	DATATOGGLE = 0b11 << 24,
	ECCORCOUNT = 0b11 << 26,
	CONDITIONCODE = 0b1111 << 28,
};

enum ohci_interrupt_status {
	SCHEDULINGOVERRUN = 0x1,
	WRITEBACKDONEHEAD = 0x2,
	STARTOFFRAME = 0x4,
	RESUMEDETECTED = 0x8,
	UNRECOVERABLEERROR = 0x10,
	FRAMENUMBEROVERFLOW = 0x20,
	ROOTHUBSTATUSCHANGE = 0x40,
	OWNERSHIPCHANGE = 0x40000000,
};

enum td_completion_codes {
	NOERROR,
	CRC,
	BITSTUFFING,
	DATATOGGLEMISMATCH,
	STALL,
	DEVICENOTRESPONDING,
	PIDCHECKFAILURE,
	UNEXPECTEDPID,
	DATAOVERRUN,
	DATAUNDERRUN,
	RESERVED1,
	RESERVED2,
	BUFFEROVERRUN,
	BUFFERUNDERRUN,
	NOT_ACCESSED1,
	NOT_ACCESSED2
};
constexpr const char* td_completion_names[] {
	"NoError",
	"CRC",
	"BitStuffing",
	"DataToggleMismatch",
	"Stall",
	"DeviceNotResponding",
	"PidCheckFailure",
	"UnexpectedPid",
	"DataOverrun",
	"DataUnderrun",
	"",
	"",
	"BufferOverrun",
	"bufferUnderrun",
	"Not Accessed",
	"Not Accessed"
};


constexpr const char* ohci_regs_names[] = {
	"HcRevision",
	"HcControl",
	"HcCommandStatus",
	"HcInterruptStatus",
	"HcInterruptEnable",
	"HcInterruptDisable",
	"HcHCCA",
	"HcPeriodCurrentED",
	"HcControlHeadED",
	"HcControlCurrentED",
	"HcBulkHeadED",
	"HcBulkCurrentED",
	"HcDoneHead",
	"HcFmInterval",
	"HcFmRemaining",
	"HcFmNumber",
	"HcPeriodicStart",
	"HcLSThreshold",
	"HcRhDescriptorA",
	"HcRhDescriptorB",
	"HcRhStatus",
	"HcRhPortStatus0",
	"HcRhPortStatus1",
	"HcRhPortStatus2",
	"HcRhPortStatus3",
	"HcRhPortStatus4",
	"HcRhPortStatus5",
	"HcRhPortStatus6",
};

}
