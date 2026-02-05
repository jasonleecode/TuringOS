/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 * (c) 2025 Adam Lackorzynski <adam@l4re.org>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#include <cstring>

#include <l4/util/util.h>
#include <l4/vbus/vbus_types.h>

#include "ohci.h"
#include "types.h"

namespace USB {

int OHCI::init() {

    // 5.1.1.1 Load and Locate
    // calculate 32-bit regs-address of memory mapped registers for the controller
    regs = reinterpret_cast<int*>(_regs_region.get());
    hcca = this->_dma_region.get();
    area_ed = reinterpret_cast<endpoint_struct *>((char *)hcca + 0x100);             // first 252 bytes are reserved for HC
    area_td = reinterpret_cast<transfer_struct *>((char *)hcca + L4_PAGESIZE);       // keep one page for endpoint descriptors (arbitrary, allows 240 endpoints)
    area_data = (char *)hcca + L4_PAGESIZE * 4;                                     // keep three pages for descriptors (arbitrary, allows 768 concurrent transfers)
    // remaining dma-space is used for data

    // dump_operational_registers(0, 28);

    // 5.1.1.2 Verify Host Controller and Allocate Resources
    std::cout << "Revision of OHCI: " << std::hex << (short)(regs[0] & 0xff) << std::endl;

    // 5.1.1.3.5 OS Driver, neither SMM nor BIOS
    // make sure neither InterruptRouting is set, nor the HostController is in another state than USBReset
    if (regs[1] & 1 << 8) {
        std::cout << "SMM Driver active, requesting ownership change!" << std::endl;
        regs[2] = 1 << 3;
        while (regs[1] & 1 << 8) {l4_usleep(1);}

    } else if (regs[1] & 0b11 << 6) {
        std::cout << "BIOS Driver active, continuing setup!" << std::endl;
        if ((regs[1] & (0b11 << 6)) == (0x11 << 6)) {
            regs[1] = 0b01 << 6;
            l4_sleep(3); // frame length is 1 ms, suspend time is 3 frames
        }
    }

    // 5.1.1.4 Setup Host Controller

    // issue a software reset for the HostController
    regs[2] = 0x1;

    // wait at least 10us for reset to complete
    l4_usleep(10);

    // make sure we are in Suspended state
    if ((regs[1] & 0b11 << 6) != (0b11 << 6)) {return -1;}

    // make sure reset is finished (HostControllerReset bit ist cleared)
    if (regs[2] & 1) {return -1;}

    // // dismiss all pending interrupt reasons
    regs[3] = 0x0000007f;

    // Set HcInterruptEnable to have all interrupt enabled except SOF detect.
    regs[4] = 0x8000007b; // All except SOF
    // regs[4] = 0x8000007f; // Also SOF

    // sleep until all ports are reset (50ms = default time value given by usb-spec)
    // buzzer.sleep(50);
    l4_sleep(50);

    // enable remote wakeup
    // regs[20] = 0x00008000;
    // generates resume-detected interrupts

    // write default time interval of HcFmInterval-Register
    regs[13] = 0xa7782edf;

    // write default time interval to HcPeriodicStart, start processing periodic transfers afer 10% of the packet
    regs[16] = 0x00002a2f;
    // HcFmInterval: The nominal value is set to be 11,999.
    // HcPeriodicStart: A typical value will be 3E67h

    // disable power switching -> all ports are always on
    regs[18] |= 0x00000200;


    // address of hcca must be within 32 bit to be reachable to the OHCI
    if ((_dma_paddr + _dma_size) >> 32) return -1;


    // figure out alignment of HcHCCA-Register
    regs[6] = 0xffffffff;
    l4_usleep(1);
    int alignment = (regs[6] ^ 0xffffffff) + 1;
    std::cout << "Alignment of HcHCCA-Register: " << std::dec << alignment << "bit" << std::endl;
    if ((long)_dma_paddr & alignment) {return -1;}

    // Set the HcHCCA to the physical address of the HCCA block.
    regs[6] = (long)_dma_paddr;

    this->hcca_done_head = (int*)(long)(this->hcca) + 33;
    // *hcca_done_head = 0;

    std::cout << "Host Controller Communications Area starts at " << std::hex << (long)this->hcca << std::endl;
    std::cout << "HccaDoneHead lays at " << (long)(this->hcca_done_head) << std::dec << std::endl;

    // Set functional state of HostController to USBOperational
    // Keep control list disabled for now
    std::cout << "Enabling Host Controller" << std::endl;
    regs[1] = (regs[1] & 0xffffff00) | 0b10 << 6;
    l4_cpu_time_t time = l4_kip_clock_ns(l4re_kip());
    printf("reset at: %lli\n", time);
    // reset all downstream ports, write default value in ControlBulkServiceRatio, set Controller in UsbReset-Mode
    // regs[1] = 0x00000000;

    // -------------------------------------------------------------------------------------------

    // Setup complete. the Host Controller begins sending SOF tokens within one ms

    num_ports = regs[18] & 0xff;

    std::cout << "Number of downstream ports: " << num_ports << std::endl;
    for (int i = 0; i < num_ports; i++) {
        port[i] = new Port(regs + 21 + i);
        std::cout << "Port " << i << ":" << std::endl;
        port[i]->get_status();
    }
    // std::cout << "this->next_desc_pos: " << this->next_desc_pos << " , phys: " << (void*)virt2phys((char*)this->next_desc_pos) << std::endl;

    this->control_list_enable();

    return 0;
}


endpoint_struct* OHCI::create_control_endpoint(int device_id, int speed, int bEndpointAddress, int wMaxPacketSize) {

    endpoint_struct* endpoint = new (this->area_ed++) endpoint_struct;

    endpoint->control = endpoint->next = 0;
    endpoint->tail = 0;
    endpoint->head = 0;

    endpoint->control |= device_id;                         // Function address
    endpoint->control |= (bEndpointAddress & 0xf) << 7;     // Endpoint address
    endpoint->control |= 0b00 << 11;                        // get Direction from TD (otherwise direction is encoded in bEndpointAddress)
    endpoint->control |= speed << 13;                       // Speed
    endpoint->control |= wMaxPacketSize << 16;              // Maximum package size

    // ------------------------------------------

    // After setting control bits, link from prev to this endpoint
    endpoint_struct* it = (endpoint_struct *)phys2virt(regs[8]);

    if (it == nullptr){
        regs[8] = (long) virt2phys(endpoint);
    } else {
        while (it->next != 0) it = (endpoint_struct *)phys2virt(it->next);
        it->next = virt2phys(endpoint);
    }
    // printf("endpoint: %p, %p\n", endpoint, virt2phys(endpoint));

    return endpoint;
}




endpoint_struct* OHCI::create_interrupt_endpoint(int device_id, int speed, int bEndpointAddress, int wMaxPacketSize, int bInterval) {

    endpoint_struct* interrupt_endpoint = new (this->area_ed++) endpoint_struct;

    // Make sure that values are actually 0
    interrupt_endpoint->control = interrupt_endpoint->next = 0;
    interrupt_endpoint->tail = 0;
    interrupt_endpoint->head = 0;

    interrupt_endpoint->control |= device_id;                                              // Function address
    interrupt_endpoint->control |= (bEndpointAddress & 0xf) << 7;                         // interrupt_Endpoint address
    interrupt_endpoint->control |= ((bEndpointAddress & 0x00000080) ? 0b10 : 0b01) << 11; // Direction
    interrupt_endpoint->control |= speed << 13;                       // High-Speed
    interrupt_endpoint->control |= wMaxPacketSize << 16;                                  // Maximum package size

    // After setting control bits and linking to placeholder transfer, link from prev to this interrupt_endpoint
    int* hcca_interupt_list = (int*)(long)this->hcca;

    // Write address of new interupt interrupt_endpoint into hcca register
    int amount = (bInterval + 32 - 1) / bInterval; // Divide 32 by bInterval, but rounding up instead of down
    int step = 32 / amount;
    for (int i = 0; i < 32; i+=step) hcca_interupt_list[i] = (long)virt2phys(interrupt_endpoint);

    std::cout << "created new interrupt endpoint with " << (amount) << " pointers" << std::endl;

    return interrupt_endpoint;
}



int OHCI::control_transfer(endpoint_struct* endpoint, int bmRequestType, int bRequest, int wValue, int wIndex, int wLength, char* data, int direction) {

    control_list_disable(endpoint);
    char* setup_data = this->area_data;
    this->area_data += 8; // always 8 byte setup data
    char* transfer_data = this->area_data;
    this->area_data += wLength;

    // ------------------------------------ Setup Transfer ------------------------------------
    transfer_struct* setup_descriptor = new (this->area_td++) transfer_struct;

    setup_descriptor->control = 0x02000000;
    setup_descriptor->first = (long)virt2phys(setup_data);
    setup_descriptor->last = (long)virt2phys(setup_data + 7);
    setup_descriptor->next = 0;

    setup_data[0] = bmRequestType;
    setup_data[1] = bRequest;
    setup_data[2] = wValue;
    setup_data[3] = wValue >> 8;
    setup_data[4] = wIndex;
    setup_data[5] = wIndex >> 8;
    setup_data[6] = wLength;
    setup_data[7] = wLength >> 8;

    // ------------------------------------ Data Transfer ------------------------------------

    transfer_struct* data_descriptor = new (this->area_td++) transfer_struct;

    // if Direction == OUT, put data to be send in data buffer
    if (direction == USB_DIR_OUT)
        memcpy(transfer_data, data, wLength);

    data_descriptor->control = 0x03000000 | 1 << 18 | direction << 19;
    data_descriptor->first = (long)virt2phys(transfer_data);
    data_descriptor->last = (long)virt2phys(transfer_data + (wLength ? wLength - 1 : 0));
    data_descriptor->next = 0;

    // ------------------------------------ Ack Transfer ------------------------------------
    transfer_struct* ack_descriptor = new (this->area_td++) transfer_struct;
    ack_descriptor->control = 0x03000000 | (direction ^ 0b11) << 19;
    ack_descriptor->first = 0;
    ack_descriptor->last = 0;
    ack_descriptor->next = 0;

    // ------------------------------------- Linking -----------------------------------------
    endpoint->head = (long)virt2phys(setup_descriptor); // Automatically clear halt bit
    setup_descriptor->next = (long)virt2phys(wLength ? data_descriptor : ack_descriptor);
    data_descriptor->next = (long)virt2phys(ack_descriptor);


    // ------------------------------------- Running ------------------------------------- 

    this->control_list_enable_and_signal();

    // TODO async: need some way of callback to access this transfer from irq-hccadonehead
    do { irq->receive(/*(l4_timeout_t){20}*/); handle_irq();}
    while ((*hcca_done_head & ~1) != (long)virt2phys(ack_descriptor));
    // l4_usleep(5);   // needed when using pci-passthrough, otherwise irq is handled before data is there

    // TODO async: data should be copied async after irq-hccadonehead
    int bytes_transfered = data_descriptor->first ? data_descriptor->first - (long)virt2phys(transfer_data) : wLength;
    if (direction == USB_DIR_IN) {
        memcpy(data, transfer_data, bytes_transfered);
    }

    // TODO async: replace allocation and deallocation with proper memory-allocator
    this->area_data -= 8 + wLength;
    this->area_td -= 3;

    return bytes_transfered;
}



int OHCI::interrupt_transfer(endpoint_struct* endpoint, int length, char* data, int direction) {

    this->interrupt_list_disable(endpoint);

    char* transfer_data = this->area_data;
    this->area_data += length;

    transfer_struct* transfer_desc = new (this->area_td++) transfer_struct;
    transfer_desc->control = 1 << 18;
    transfer_desc->first = (long)virt2phys(transfer_data);
    transfer_desc->last = (long)virt2phys(transfer_data + (length ? length - 1 : 0));

    // Link from new transfer
    endpoint->head = (int)(long)virt2phys(transfer_desc); // Automatically unhalt endpoint

    this->interrupt_list_enable();

    do { irq->receive(/*(l4_timeout_t){20}*/); handle_irq();}
    while ((*hcca_done_head & ~1) != (long)virt2phys(transfer_desc));
    // l4_usleep(150);   // needed when using pci-passthrough, otherwise irq is handled before data is there

    int bytes_transfered = transfer_desc->first ? transfer_desc->first - (long)virt2phys(transfer_data) : length;
    if (direction == USB_DIR_IN) {
        memcpy(data, transfer_data, bytes_transfered);
    }
    // printf("wLength: %i, transf: %i\n", length, bytes_transfered);

    this->area_data -= length;
    this->area_td -= 1;

    return bytes_transfered;
}

void OHCI::handle_irq() {

    int int_status = regs[3] & ~(STARTOFFRAME | 0);
    unsigned int long done_head = *this->hcca_done_head;
    regs[3] = 0x0000007f;

    if (!_irq_trigger_type) obj_cap()->unmask();

    if (int_status & SCHEDULINGOVERRUN){
        printf("[INT] SchedulingOverrun, count: %i\n", (regs[2] & 0x00030000) >> 16);
    }
    if (int_status & WRITEBACKDONEHEAD){
        // printf("[INT] WritebackDoneHead, now pointing at: %x\n", done_head);
        if (int err = *reinterpret_cast<l4_uint32_t *>(phys2virt(done_head)) >> 28) {
            printf("[ERR] Transfer at %p (phys: %lx): %s\n",
                   phys2virt(done_head), done_head, td_completion_names[err]);
            for (int i = -4; i < 8; i++)
              printf("%lx - %08x\n", done_head + i * 4, ((int *)phys2virt(done_head))[i]);
        }
    }
    if (int_status & STARTOFFRAME){
        printf("[INT] StartofFrame\n");
    }
    if (int_status & RESUMEDETECTED){
        printf("[INT] ResumeDetected\n");
    }
    if (int_status & UNRECOVERABLEERROR){
        printf("[INT] Unrecoverable Error detected by USB Host Controller\n");
    }
    if (int_status & FRAMENUMBEROVERFLOW){
        printf("[INT] FrameNumberOverflow\n");
    }
    if (int_status & ROOTHUBSTATUSCHANGE){
        printf("[INT] RootHubStatusChange\n");
    }

}


void OHCI::dump_operational_registers(int start, int end) {

    // 21 operational registers + HcRhPortStatus-Register per port on root hub
    for (int i = start; i < end; i++) {
        // std::cout << regs[i] << " :" << regs_names[i] << std::endl;
        printf("0x%08x\t%s\n", regs[i], ohci_regs_names[i]);
    }
}


void OHCI::dump_hcca(int start, int end) {
	for (int i = start; i<end; i++){
		// std::cout << &((int*)hcca)[i] << " - " << ((int*)hcca)[i] << std::endl;
		printf("%llx - 0x%08x\n",
                       virt2phys(&((int *)this->hcca)[i]), ((int *)this->hcca)[i]);
	}
}

};
