// license:BSD-3-Clause
// copyright-holders:Thomas Cherryhomes
#ifndef MAME_BUS_A7800_FUJINET_H
#define MAME_BUS_A7800_FUJINET_H

#pragma once

#include "a78_slot.h"
#include "sound/pokey.h"
#include "a78_cart.h"
#include "a78map.h"
#include "fuji_load.h"
#include "fujilink.h"
#include "fujinet_host.h"
#include "hsc.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// ======================> a78_fujinet_device
//
// Model of the FujiNet RP2354B cartridge: 512K of SRAM behind the slot table,
// the signed boot block at $F000 until the first image is in, the loader at
// $0600 and the arena at $0800, a POKEY at $4000 or $0450 (MAME's own, so a
// game's RANDOM reads match its native cart), and the High Score Cart. The
// mapper engine (a78map.c), the bus decode and glue (a78_cart.h), the load
// sequence (fuji_load.c), the HSC's saves (hsc.c), the protocol (fujimail.c)
// and the wire codec (fujibus.c) are the cartridge firmware's own sources;
// frames go over TCP to fujinet-pc (fujilink.cpp).
//
// THE MAILBOX RUNS ON ITS OWN THREAD, as it runs on the RP2354B's core0 while
// core1 serves the bus. A transaction can block for five seconds, or sixty on
// a mount, and the console keeps running meanwhile. Hotspot writes reach the
// worker through a ring (core1 -> core0 on the cart); what it publishes is
// copied into the arena on the emulation thread, and the SEQ/ACKSEQ
// interlock (ACKSEQ published last) keeps that safe. The load sequence and
// the bus modes stay on the emulation thread, as on core1.
//
//   -cartslot fujinet -cart client.a78   a claimed client in place of CONFIG
//   a78_fujinet_set_host()               an embedding host's configuration;
//                                        otherwise FUJINET_* (fujinet_host.h)
//
// A reset is a power cycle (the 7800 has no reset line on the cart edge):
// the boot block again. The mailbox, the store and the HSC's RAM survive, as
// they do on a USB-powered cart.

class a78_fujinet_device : public device_t, public device_a78_cart_interface
{
public:
	a78_fujinet_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
	virtual ~a78_fujinet_device();

	virtual uint8_t read_40xx(offs_t offset) override;
	virtual void write_40xx(offs_t offset, uint8_t data) override;

	// The fujimail, fuji_load and hsc ports. Public only because those ports
	// are C function pointers with no context argument.
	void port_poke(unsigned offset, uint8_t value);
	void port_paint(const uint8_t *src, uint8_t fill);
	void port_window(uint16_t word) { m_load_slots[FN_LOADWIN_BASE >> 13] = word; }
	bool port_link_up() { return m_link.is_open(); }
	void port_wait_link(uint32_t ms);
	fb_status_t port_transact(uint8_t device, uint8_t command, const fb_param_t *params, unsigned nparams,
			const uint8_t *payload, uint16_t payload_len, uint32_t timeout_ms, fb_reply_t *reply);
	void port_send_bare(uint8_t device, uint8_t command, const uint8_t *payload, uint16_t payload_len);
	uint8_t port_stream_open(int stream, uint32_t size);
	void port_stream_write(int stream, const uint8_t *chunk, unsigned len);
	uint8_t port_stream_close(int stream, uint32_t got, bool aborted);
	void port_arm_swap();

protected:
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override;
	virtual void device_stop() override ATTR_COLD;
	virtual void device_add_mconfig(machine_config &config) override ATTR_COLD;

private:
	static constexpr size_t QUEUE_SIZE = 4096;

	// the bus, as core1 serves it
	uint8_t bus_read(uint16_t a);
	void bus_write(uint16_t a, uint8_t data);
	uint16_t slot_word(uint16_t a) const;
	void to_boot();
	void to_load();
	void flip();
	void mailbox_event(uint16_t offset, uint8_t data);
	bool own_register(unsigned reg, uint8_t data);
	void poke(unsigned offset, uint8_t value);
	void drain_published();
	void publish_staged(const a78map_plan_t &p);
	void stage_boot_image();
	void direct_boot(const std::vector<uint8_t> &img, const char *mapper);
	void dump_sram();
	bool bios_present() const;
	void publish_status();
	TIMER_CALLBACK_MEMBER(service);

	// the worker: fujimail, as core0 runs it
	void start_worker();
	void stop_worker();
	void worker_loop();
	void worker_hsc();
	bool try_open_link();
	bool sync_worker(uint32_t timeout_ms);
	void note(uint16_t offset);

	required_device<pokey_device> m_pokey;

	a78_fujinet_host_config m_host;
	uint32_t m_instance = 0;

	uint8_t m_arena[FN_ARENA_SIZE];
	std::vector<uint8_t> m_sram;
	std::vector<uint8_t> m_config;            // CONFIG, or the client
	std::vector<uint8_t> m_store[2];          // the cart's RAM and flash tiers
	std::vector<uint8_t> m_cfg;
	std::vector<uint8_t> m_boot_img;          // the host's staged boot image
	std::vector<uint8_t> m_hsc_rom;
	int m_open_tier = -1;
	std::string m_cfg_mapper;

	a78_bus_t m_bus{};
	a78map_t *m_live = nullptr;
	uint16_t m_load_slots[A78MAP_SLOTS]{};
	fuji_load_t m_loader{};
	fuji_load_port_t m_load_port{};
	std::mutex m_load_lock;                   // m_loader and the HSC ROM: both threads
	hsc_t m_hsc{};
	hsc_port_t m_hsc_port{};
	uint8_t m_hsc_ram[A78MAP_HSC_RAM_SIZE];
	uint32_t m_hsc_dirty = 0;                 // atomically, from both threads
	std::atomic<bool> m_hsc_have_rom { false };
	std::atomic<bool> m_hsc_on { false };
	std::atomic<bool> m_hsc_sd_ok { false };
	std::atomic<bool> m_hsc_pending { false };
	bool m_direct = false;
	std::vector<uint8_t> m_direct_img;
	std::string m_direct_mapper;
	uint32_t m_live_crc = 0;
	bool m_booted_image = false;

	memory_passthrough_handler m_tap_tia, m_tap_bios;
	emu_timer *m_service_timer = nullptr;
	output_finder<> m_mode_out;
	output_finder<> m_handover_out;
	bool m_init_done = false;
	cpu_device *m_cpu = nullptr;

	// the worker
	a78_fujilink m_link;
	std::thread m_worker;
	std::atomic<bool> m_stop { false };
	std::atomic<uint8_t> m_mode_shared { FN_MODE_BOOT };
	std::mutex m_wake_lock;
	std::condition_variable m_wake;
	std::timed_mutex m_mail_lock;             // held while fujimail's globals are in use

	std::array<uint16_t, QUEUE_SIZE> m_queue = {};
	std::atomic<size_t> m_queue_head { 0 };
	std::atomic<size_t> m_queue_tail { 0 };

	std::atomic<bool> m_publish_ready { false };
	std::mutex m_publish_lock;
	std::array<uint8_t, FN_R_PAINT_END> m_published = {};
	size_t m_publish_lo = FN_R_PAINT_END;
	size_t m_publish_hi = 0;
};

DECLARE_DEVICE_TYPE(A78_FUJINET, a78_fujinet_device)

#endif // MAME_BUS_A7800_FUJINET_H
