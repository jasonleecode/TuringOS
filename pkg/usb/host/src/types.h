/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

// Multiple transfers descriptors are owned by a single endpoint descriptor
struct transfer_struct {
	int control;
	int first;
	int next;		// Transfer descriptor
	int last;
};
struct endpoint_struct {
	int control;
	int tail;		// Transfer descriptor
	int head;		// Transfer descriptor
	int next;		// Endpoint descriptor
};

constexpr const char* interface_classes[] = {
	"Unspecified",
	"Audio",
	"Communications and CDC Control",
	"Human interface device (HID)",
	nullptr,
	"Physical interface device (PID)",
	"Media (PTP/MTP)",
	"Printer",
	"Unknown",
	"Mass Storage",
	"Hub",
	"CDC-Data",
	"Smart Card",
	nullptr,
	"Content security",
	"Video",
	"Personal healthcare",
	"Audio/Video (AV)",
	"Billboard",
	// [0x12 ... 0xdc] = nullptr,
	// "Diagnostic device",
	// [0xde ... 0xe0] = nullptr,
	// "Wireless Controller",
	// [0xe1 ... 0xef] = nullptr,
	// "Miscellaneous",
	// [0xf0 ... 0xfe] = nullptr,
	// "Application-specific",
	// "Vendor-specific"
};
