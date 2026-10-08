// license:BSD-3-Clause
// copyright-holders:Thomas Cherryhomes
#ifndef MAME_BUS_A7800_FUJINET_HOST_H
#define MAME_BUS_A7800_FUJINET_HOST_H

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// How a program embedding MAME (FujiNet Go Atari 7800) wants the FujiNet
// cartridge, and what the cartridge reports back. Process-wide, like the
// cartridge itself (one slot, one cart): fujimail's port is C function
// pointers with no context argument.
//
// Until a host calls a78_fujinet_set_host(), the cartridge configures itself
// from the environment as the stock tools expect (fujinet-firmware's run.sh,
// abrun.py, soak.py):
//
//   FUJINET_TCP=host:port   where fujinet-pc's BoIP listener is (127.0.0.1:9995)
//   FUJINET_IMAGE=game.bin  that image in the SRAM from power-on (DIRECT)
//   FUJINET_MAPPER=kind     the mapper for FUJINET_IMAGE
//   FUJINET_HSC=hsc.bin     the HSC ROM, installed and on
//   FUJINET_DEBUG           log every transaction
//   FUJINET_SRAMDUMP=path   write the SRAM to path.sram after each load

enum a78_fujinet_boot_mode : int
{
	A78_FUJINET_BOOT_NONE = 0,     // CONFIG (or the -cart client) at power-on
	A78_FUJINET_BOOT_STAGED,       // the image staged and armed: the boot block,
	                               // the loader and the hand-over run as for a
	                               // network boot
	A78_FUJINET_BOOT_DIRECT        // the image in the SRAM from power-on, as the
	                               // loader would leave it (the A/B tests)
};

struct a78_fujinet_host_config
{
	std::string host = "127.0.0.1";
	int port = 9995;
	bool debug = false;

	// what the next power-on boots
	a78_fujinet_boot_mode boot_mode = A78_FUJINET_BOOT_NONE;
	std::vector<uint8_t> boot_image;
	std::string boot_mapper;       // a78map kind name, or empty: the plan's own

	// a claimed client in place of the baked CONFIG, or empty
	std::vector<uint8_t> client;

	// the High Score Cart
	std::vector<uint8_t> hsc_rom;  // 4K, or empty
	bool hsc_on = false;

	std::string sramdump;          // FUJINET_SRAMDUMP
};

// A snapshot for a host's UI and debugger, published every 50 ms.
struct a78_fujinet_status
{
	uint32_t instance = 0;         // which cartridge published it
	bool present = false;
	bool link_up = false;
	bool worker = false;           // the mailbox service is running
	uint8_t mode = 0;              // FN_MODE_*
	uint8_t handover = 0;          // FN_HO_*
	uint8_t ackseq = 0;
	uint8_t err = 0;
	uint8_t boot_state = 0, boot_pct = 0, boot_err = 0;
	uint8_t load_state = 0, load_pct = 0;
	uint8_t mapper = 0;            // A78MAP_* of the live image
	uint8_t staged = 0, staged_kind = 0;
	uint32_t staged_crc = 0;
	uint32_t live_crc = 0;         // CRC-32 of the image last loaded
	uint8_t hsc = 0;               // FN_HSC_*
	uint8_t tv = 0;                // FN_TV_*
	uint8_t inptctrl = 0;
	bool inpt_locked = false;
	bool booted_image = false;     // a game (not CONFIG or a client) is running
	uint32_t queue_depth = 0;
	char live_kind[16] = {};
	char link_error[128] = {};
};

// Takes effect at the cartridge's next power-on (MAME's soft reset); the
// link's host and port at the next machine.
void a78_fujinet_set_host(const a78_fujinet_host_config &config);
a78_fujinet_host_config a78_fujinet_get_host();

// false while no cartridge is running
bool a78_fujinet_get_status(a78_fujinet_status &status);

// the cartridge most recently built; its status is current once
// status.instance reaches this
uint32_t a78_fujinet_latest_instance();

// The baked CONFIG, without the .a78 header.
const uint8_t *a78_fujinet_config_rom(uint32_t &size);

#endif // MAME_BUS_A7800_FUJINET_HOST_H
