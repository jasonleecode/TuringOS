/*
 * (c) 2024 Artur Gutmann <artur.gutmann@mailbox.tu-dresden.de>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once
#include <l4/re/event_enums.h>



enum hid_usage_page {
	USAGE_PAGE_GENERIC_DESKTOP_CONTROLS = 	0x01,
	USAGE_PAGE_SIMULATION_CONTROLS =		0x02,
	USAGE_PAGE_VR_CONTROLS =				0x03,
	USAGE_PAGE_SPORT_CONTROLS =				0x04,
	USAGE_PAGE_GAME_CONTROLS =				0x05,
	USAGE_PAGE_GENERIC_DEVICE_CONTROLS =	0x06,
	USAGE_PAGE_KEYBOARD = 					0x07,
	USAGE_PAGE_LED = 						0x08,
	USAGE_PAGE_BUTTON = 					0x09,
};

enum hid_usage_generic_desktop_controls {
	USAGE_POINTER =		 					0x01,
	USAGE_MOUSE = 							0x02,
	USAGE_JOYSTICK = 						0x04,
	USAGE_GAMEPAD = 						0x05,
	USAGE_KEYBOARD = 						0x06,
	USAGE_KEYPAD = 							0x07,
	USAGE_X = 								0x30,
	USAGE_Y = 								0x31,
	USAGE_Z = 								0x32,
	USAGE_RX = 								0x33,
	USAGE_RY = 								0x34,
	USAGE_RZ = 								0x35,
	USAGE_SLIDER =							0x36,
	USAGE_WHEEL = 							0x38,
	USAGE_SYSTEM_CONTROL = 					0x80,
};

#define KEYBOARD_ERROR_ROLL_OVER				0x01
#define KEYBOARD_POST_FAIL						0x02
#define KEYBOARD_ERROR_UNDEFINED				0x03

const L4Re_events_key key2event[] = {
	L4RE_KEY_RESERVED,
	L4RE_KEY_UNKNOWN,	// KEYBOARD_ERROR_ROLL_OVER
	L4RE_KEY_UNKNOWN,	// KEYBOARD_POST_FAIL
	L4RE_KEY_UNKNOWN,	// KEYBOARD_ERROR_UNDEFINED
	L4RE_KEY_A,
	L4RE_KEY_B,
	L4RE_KEY_C,
	L4RE_KEY_D,
	L4RE_KEY_E,
	L4RE_KEY_F,
	L4RE_KEY_G,
	L4RE_KEY_H,
	L4RE_KEY_I,
	L4RE_KEY_J,
	L4RE_KEY_K,
	L4RE_KEY_L,
	L4RE_KEY_M,
	L4RE_KEY_N,
	L4RE_KEY_O,
	L4RE_KEY_P,
	L4RE_KEY_Q,
	L4RE_KEY_R,
	L4RE_KEY_S,
	L4RE_KEY_T,
	L4RE_KEY_U,
	L4RE_KEY_V,
	L4RE_KEY_W,
	L4RE_KEY_X,
	L4RE_KEY_Y,
	L4RE_KEY_Z,
	L4RE_KEY_1,
	L4RE_KEY_2,
	L4RE_KEY_3,
	L4RE_KEY_4,
	L4RE_KEY_5,
	L4RE_KEY_6,
	L4RE_KEY_7,
	L4RE_KEY_8,
	L4RE_KEY_9,
	L4RE_KEY_0,
	L4RE_KEY_ENTER,
	L4RE_KEY_ESC,
	L4RE_KEY_DELETE,
	L4RE_KEY_TAB,
	L4RE_KEY_SPACE,
	L4RE_KEY_MINUS,
	L4RE_KEY_EQUAL,
	L4RE_KEY_LEFTBRACE,
	L4RE_KEY_RIGHTBRACE,
	L4RE_KEY_BACKSLASH,
	L4RE_KEY_UNKNOWN,	// Non-US #, Typical language mappings: US: \|Belg: µ `£French Canadian: <}>Danish: ’* Dutch: <>French: *µ German: # ’Italian: ù §LatinAmerica: } `] Norwegian: , * Spain: }Ç Swedish: , * Swiss: $ £UK: # ~
	L4RE_KEY_SEMICOLON,
	L4RE_KEY_APOSTROPHE,
	L4RE_KEY_GRAVE,
	L4RE_KEY_COMMA,
	L4RE_KEY_DOT,
	L4RE_KEY_SLASH,
	L4RE_KEY_CAPSLOCK,
	L4RE_KEY_F1,
	L4RE_KEY_F2,
	L4RE_KEY_F3,
	L4RE_KEY_F4,
	L4RE_KEY_F5,
	L4RE_KEY_F6,
	L4RE_KEY_F7,
	L4RE_KEY_F8,
	L4RE_KEY_F9,
	L4RE_KEY_F10,
	L4RE_KEY_F11,
	L4RE_KEY_F12,
	L4RE_KEY_PRINT,
	L4RE_KEY_SCROLLLOCK,
	L4RE_KEY_PAUSE,
	L4RE_KEY_INSERT,
	L4RE_KEY_HOME,
	L4RE_KEY_PAGEUP,
	L4RE_KEY_DELETEFILE,
	L4RE_KEY_END,
	L4RE_KEY_PAGEDOWN,
	L4RE_KEY_RIGHT,
	L4RE_KEY_LEFT,
	L4RE_KEY_DOWN,
	L4RE_KEY_UP,
	L4RE_KEY_NUMLOCK,
	L4RE_KEY_KPSLASH,
	L4RE_KEY_KPASTERISK,
	L4RE_KEY_KPMINUS,
	L4RE_KEY_KPPLUS,
	L4RE_KEY_KPENTER,
	L4RE_KEY_KP1,
	L4RE_KEY_KP2,
	L4RE_KEY_KP3,
	L4RE_KEY_KP4,
	L4RE_KEY_KP5,
	L4RE_KEY_KP6,
	L4RE_KEY_KP7,
	L4RE_KEY_KP8,
	L4RE_KEY_KP9,
	L4RE_KEY_KP0,
	L4RE_KEY_KPDOT,
	L4RE_KEY_102ND,	// Non-US Backslash, Typical language mappings: Belg: <\>French Canadian: <°>Danish: <\>Dutch: ]|[ French: <>German: <|>Italian: <>LatinAmerica: <>Norwegian: <>Spain: <>Swedish: <|>Swiss: <>UK: \|Brazil: \|
	L4RE_KEY_COMPOSE,
	L4RE_KEY_POWER,
	L4RE_KEY_UNKNOWN,	// Keypad =
	L4RE_KEY_F13,
	L4RE_KEY_F14,
	L4RE_KEY_F15,
	L4RE_KEY_F16,
	L4RE_KEY_F17,
	L4RE_KEY_F18,
	L4RE_KEY_F19,
	L4RE_KEY_F20,
	L4RE_KEY_F21,
	L4RE_KEY_F22,
	L4RE_KEY_F23,
	L4RE_KEY_F24,
	L4RE_KEY_UNKNOWN,	// Execute
	L4RE_KEY_HELP,
	L4RE_KEY_MENU,
	L4RE_KEY_SELECT,
	L4RE_KEY_STOP,
	L4RE_KEY_AGAIN,
	L4RE_KEY_UNDO,
	L4RE_KEY_CUT,
	L4RE_KEY_COPY,
	L4RE_KEY_PASTE,
	L4RE_KEY_FIND,
	L4RE_KEY_MUTE,
	L4RE_KEY_VOLUMEUP,
	L4RE_KEY_VOLUMEDOWN,
	L4RE_KEY_UNKNOWN,	// Locking Caps Lock, Implemented as a locking key; sent as a toggle button. Available for legacy support; however, most systems should use the non-locking version of this key.
	L4RE_KEY_UNKNOWN,	// Locking Num Lock, Implemented as a locking key; sent as a toggle button. Available for legacy support; however, most systems should use the non-locking version of this key.
	L4RE_KEY_UNKNOWN,	// Locking Scroll Lock, Implemented as a locking key; sent as a toggle button. Available for legacy support; however, most systems should use the non-locking version of this key.
	L4RE_KEY_KPCOMMA,
	L4RE_KEY_UNKNOWN,	// Used on AS/400 keyboards.
	L4RE_KEY_UNKNOWN,	// International1
	L4RE_KEY_UNKNOWN,	// International2
	L4RE_KEY_UNKNOWN,	// International3
	L4RE_KEY_UNKNOWN,	// International4
	L4RE_KEY_UNKNOWN,	// International5
	L4RE_KEY_UNKNOWN,	// International6
	L4RE_KEY_UNKNOWN,	// International7
	L4RE_KEY_UNKNOWN,	// International8
	L4RE_KEY_UNKNOWN,	// International9
	L4RE_KEY_UNKNOWN,	// LANG1
	L4RE_KEY_UNKNOWN,	// LANG2
	L4RE_KEY_UNKNOWN,	// LANG3
	L4RE_KEY_UNKNOWN,	// LANG4
	L4RE_KEY_UNKNOWN,	// LANG5
	L4RE_KEY_UNKNOWN,	// LANG6
	L4RE_KEY_UNKNOWN,	// LANG7
	L4RE_KEY_UNKNOWN,	// LANG8
	L4RE_KEY_UNKNOWN,	// LANG9
	L4RE_KEY_ALTERASE,
	L4RE_KEY_SYSRQ,
	L4RE_KEY_CANCEL,
	L4RE_KEY_CLEAR,
	L4RE_KEY_UNKNOWN,	// Keyboard Prior (not sure)
	L4RE_KEY_UNKNOWN,	// Keyboard Return (not sure)
	L4RE_KEY_UNKNOWN,	// Keyboard Separator
	L4RE_KEY_UNKNOWN,	// Keyboard Out
	L4RE_KEY_UNKNOWN,	// Keyboard Oper
	L4RE_KEY_UNKNOWN,	// Keyboard Clear/Again
	L4RE_KEY_UNKNOWN,	// Keyboard CrSel/Props
	L4RE_KEY_UNKNOWN,	// Keyboard ExSel
	L4RE_KEY_RESERVED,
	L4RE_KEY_RESERVED,
	L4RE_KEY_RESERVED,
	L4RE_KEY_RESERVED,
	L4RE_KEY_RESERVED,
	L4RE_KEY_RESERVED,
	L4RE_KEY_RESERVED,
	L4RE_KEY_RESERVED,
	L4RE_KEY_RESERVED,
	L4RE_KEY_RESERVED,
	L4RE_KEY_RESERVED,
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_UNKNOWN,	// Keypad Special
	L4RE_KEY_RESERVED,
	L4RE_KEY_RESERVED,
	L4RE_KEY_LEFTCTRL,
	L4RE_KEY_LEFTSHIFT,
	L4RE_KEY_LEFTALT,
	L4RE_KEY_LEFTMETA,
	L4RE_KEY_RIGHTCTRL,
	L4RE_KEY_RIGHTSHIFT,
	L4RE_KEY_RIGHTALT,
	L4RE_KEY_RIGHTMETA,
	// E8-FFFF Reserved
};

constexpr const char* usage_page_generic_desktop_controls_strings[] = { 
	"Undefined",
	"Pointer",
	"Mouse",
	"",
	"Joystick",
	"Gamepad",
	"Keyboard",
	"Keypad",
	"Multi-axis Controller",
	"Tablet PC System Controls",
	"Water Cooling Device",
	"Computer Chassis Device",
	"Wireless Radio Controls",
	"Portable Device Control",
	"System Multi-Axis Controller",
	"Spatial Controller",
	"Assistive Control",
	"Device Dock",
	"Dockable Device",
	"Call State Management Control",
	// [0x14 ... 0x39] = nullptr,
	// "Counted Buffer",
	// [0x3B ... 0x7F] = nullptr,
	// "System Control",
	// [0x81 ... 0x95] = nullptr,
	// "Thumbstick",
	// [0x97 ... 0xBF] = nullptr,
	// "Sensor Zone",
	// [0xC1 ... 0xC4] = nullptr,
	// "Chassis Enclosure"
};
