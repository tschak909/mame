// license:BSD-3-Clause
// copyright-holders:Thomas Cherryhomes
/***********************************************************************************************************

 Atari 7800 FujiNet cartridge emulation

 See fujinet.h for the design; pico/atari-7800/README.md in fujinet-firmware
 for the bring-up this device serves.

 ***********************************************************************************************************/

#include "emu.h"
#include "fujinet.h"
#include "speaker.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

// The cartridge firmware's own sources, compiled in as C++ (they sit next to
// this file), so no extern "C".
#include "fuji_mailbox.h"
#include "fujimail.h"
#include "fujiconfigrom.h"
#include "a78loaderrom.h"
#include "a78bootblk.h"

DEFINE_DEVICE_TYPE(A78_FUJINET, a78_fujinet_device, "a78_fujinet", "Atari 7800 FujiNet Cartridge")

#define STORE_RAM_MAX  (160u * 1024)
#define STORE_MAX      0x80000u

namespace {

// fujimail's port is C function pointers with no context argument, so the one
// running cartridge is reached through this. One slot, one cart: the
// constraint is the hardware's too.
std::mutex s_active_lock;
a78_fujinet_device *s_active = nullptr;

std::mutex s_host_lock;
a78_fujinet_host_config s_host;
bool s_host_set = false;

std::mutex s_status_lock;
a78_fujinet_status s_status;
std::atomic<uint32_t> s_instances { 0 };

// Which thread is the mailbox worker: port_poke() publishes from there, and
// writes the arena directly from the emulation thread.
thread_local bool s_on_worker = false;

bool s_debug = false;

void c_poke(unsigned offset, uint8_t value) { s_active->port_poke(offset, value); }
void c_paint(const uint8_t *src, uint8_t fill) { s_active->port_paint(src, fill); }
void c_window(uint16_t word) { s_active->port_window(word); }
bool c_link_up() { return s_active->port_link_up(); }
void c_wait_link_ms(uint32_t ms) { s_active->port_wait_link(ms); }

fb_status_t c_transact(uint8_t device, uint8_t command, const fb_param_t *params, unsigned nparams,
		const uint8_t *payload, uint16_t payload_len, uint32_t timeout_ms, fb_reply_t *reply)
{
	return s_active->port_transact(device, command, params, nparams, payload, payload_len, timeout_ms, reply);
}

void c_send_bare(uint8_t device, uint8_t command, const uint8_t *payload, uint16_t payload_len)
{
	s_active->port_send_bare(device, command, payload, payload_len);
}

uint8_t c_stream_open(int stream, uint32_t size) { return s_active->port_stream_open(stream, size); }
void c_stream_write(int stream, const uint8_t *chunk, unsigned len) { s_active->port_stream_write(stream, chunk, len); }
uint8_t c_stream_close(int stream, uint32_t got, bool aborted) { return s_active->port_stream_close(stream, got, aborted); }
void c_arm_swap() { s_active->port_arm_swap(); }

uint32_t c_now_ms()
{
	return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

void c_on_txn(const fujimail_txn_t *t)
{
	char txt[40];
	unsigned k, m = 0;

	for (k = 0; k < t->rxlen && m < sizeof txt - 1; k++)
	{
		uint8_t c = t->rx[k];
		if (c == 0)
			break;
		txt[m++] = (c >= 0x20 && c < 0x7f) ? char(c) : '.';
	}
	txt[m] = '\0';
	osd_printf_info("fujinet: dev=%02X cmd=%02X nparam=%u txlen=%u seq=%u -> err=%d reply=%02X rxlen=%u%s%s%s\n",
			t->device, t->command, t->nparam, t->txlen, t->seq,
			t->status, t->reply_cmd, t->rxlen,
			m ? " \"" : "", txt, m ? "\"" : "");
}

void c_on_dbc(fujimail_dbc_ev_t ev, int stream, uint32_t expect, unsigned got, bool aborted)
{
	if (ev == FUJIMAIL_DBC_OPEN)
		osd_printf_info("fujinet: DBC open stream=%d size=%u\n", stream, expect);
	else
		osd_printf_info("fujinet: DBC close stream=%d got=%u%s\n", stream, got, aborted ? " ABORTED" : "");
}

bool c_inbound(const fb_reply_t &frame)
{
	return fujimail_inbound(&frame);
}

const fujimail_port_t port_debug = {
	c_poke, c_link_up, c_transact, c_send_bare,
	c_stream_open, c_stream_write, c_stream_close, c_arm_swap,
	c_wait_link_ms, nullptr, c_on_txn, c_on_dbc,
};

const fujimail_port_t port_quiet = {
	c_poke, c_link_up, c_transact, c_send_bare,
	c_stream_open, c_stream_write, c_stream_close, c_arm_swap,
	c_wait_link_ms, nullptr, nullptr, nullptr,
};

void load_file(std::vector<uint8_t> &out, const char *path)
{
	out.clear();
	if (!path)
		return;
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		osd_printf_error("fujinet: cannot open %s\n", path);
		return;
	}
	uint8_t buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		out.insert(out.end(), buf, buf + n);
	fclose(f);
}

// What the stock tools ask for, until a host says otherwise.
a78_fujinet_host_config host_from_env()
{
	a78_fujinet_host_config c;

	if (const char *tcp = getenv("FUJINET_TCP"))
	{
		std::string hp(tcp);
		size_t colon = hp.rfind(':');
		if (colon != std::string::npos)
		{
			c.port = atoi(hp.c_str() + colon + 1);
			hp.resize(colon);
		}
		if (!hp.empty())
			c.host = hp;
	}
	c.debug = getenv("FUJINET_DEBUG") != nullptr;
	if (const char *image = getenv("FUJINET_IMAGE"))
	{
		load_file(c.boot_image, image);
		if (!c.boot_image.empty())
			c.boot_mode = A78_FUJINET_BOOT_DIRECT;
	}
	if (const char *mapper = getenv("FUJINET_MAPPER"))
		c.boot_mapper = mapper;
	if (const char *hsc = getenv("FUJINET_HSC"))
	{
		load_file(c.hsc_rom, hsc);
		if (c.hsc_rom.size() == A78MAP_HSC_ROM_SIZE)
			c.hsc_on = true;
		else
			c.hsc_rom.clear();
	}
	if (const char *dump = getenv("FUJINET_SRAMDUMP"))
		c.sramdump = dump;
	return c;
}

} // anonymous namespace


/*-------------------------------------------------
    host API
-------------------------------------------------*/

void a78_fujinet_set_host(const a78_fujinet_host_config &config)
{
	std::lock_guard<std::mutex> lock(s_host_lock);
	s_host = config;
	s_host_set = true;
}

a78_fujinet_host_config a78_fujinet_get_host()
{
	std::lock_guard<std::mutex> lock(s_host_lock);
	if (!s_host_set)
	{
		s_host = host_from_env();
		s_host_set = true;
	}
	return s_host;
}

bool a78_fujinet_get_status(a78_fujinet_status &status)
{
	std::lock_guard<std::mutex> lock(s_status_lock);
	status = s_status;
	return s_status.present;
}

uint32_t a78_fujinet_latest_instance()
{
	return s_instances.load();
}

const uint8_t *a78_fujinet_config_rom(uint32_t &size)
{
	size = FUJI_CONFIGROM_SIZE;
	return _configrom;
}


/*-------------------------------------------------
    device
-------------------------------------------------*/

a78_fujinet_device::a78_fujinet_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, A78_FUJINET, tag, owner, clock)
	, device_a78_cart_interface(mconfig, *this)
	, m_pokey(*this, "pokey")
	, m_mode_out(*this, "fujinet_mode")
	, m_handover_out(*this, "fujinet_handover")
{
}

a78_fujinet_device::~a78_fujinet_device()
{
	stop_worker();
	std::lock_guard<std::mutex> lock(s_active_lock);
	if (s_active == this)
		s_active = nullptr;
}

void a78_fujinet_device::device_add_mconfig(machine_config &config)
{
	SPEAKER(config, "fujinet_pokey").front_center();
	POKEY(config, m_pokey, DERIVED_CLOCK(1, 1)).add_route(ALL_OUTPUTS, "fujinet_pokey", 1.00);
}

void a78_fujinet_device::device_start()
{
	m_instance = ++s_instances;
	m_cpu = dynamic_cast<cpu_device *>(machine().root_device().subdevice("maincpu"));

	m_sram.assign(A78MAP_SRAM_SIZE, 0x00);
	// Never reallocated: a load reads one tier while the worker fills the other.
	m_store[0].reserve(STORE_MAX);
	m_store[1].reserve(STORE_MAX);
	std::memset(m_arena, 0, sizeof m_arena);
	std::memset(m_hsc_ram, 0xff, sizeof m_hsc_ram);

	m_load_port.poke = c_poke;
	m_load_port.paint = c_paint;
	m_load_port.window = c_window;
	m_hsc_port.transact = c_transact;
	m_hsc_port.link_up = c_link_up;
	m_hsc_port.now_ms = c_now_ms;

	m_service_timer = timer_alloc(FUNC(a78_fujinet_device::service), this);

	// No save_item: the protocol state lives in the shared C service's
	// globals, and replaying a transaction is never safe.
}

void a78_fujinet_device::device_stop()
{
	stop_worker();
	std::lock_guard<std::mutex> lock(s_active_lock);
	if (s_active == this)
	{
		s_active = nullptr;
		std::lock_guard<std::mutex> status_lock(s_status_lock);
		s_status = a78_fujinet_status();
	}
}

bool a78_fujinet_device::bios_present() const
{
	// The BIOS's reset vector: the end of the 4K (NTSC) or 16K (PAL) image,
	// both at the top of the region. All $FF when "none" loaded nothing.
	memory_region *r = machine().root_device().memregion("maincpu");
	if (!r || r->bytes() < 0x4000)
		return false;
	return !(r->base()[0x3ffc] == 0xff && r->base()[0x3ffd] == 0xff);
}

// The image in the SRAM as the loader would leave it, and the console's own
// BIOS starts it: the side of an A/B test that is the FujiNet cart.
void a78_fujinet_device::direct_boot(const std::vector<uint8_t> &img, const char *mapper)
{
	static uint8_t scratch[0x400];
	a78map_plan_t plan;
	fuji_load_t l{};
	fuji_slice_t s;

	if (a78map_plan(img.data(), uint32_t(img.size()), mapper, &plan) != A78MAP_OK)
	{
		osd_printf_error("fujinet: the boot image cannot be mapped; serving the boot block\n");
		return;
	}
	std::lock_guard<std::mutex> lock(m_load_lock);
	std::fill(m_sram.begin(), m_sram.end(), 0xa5);
	l.base = img.data();
	l.plan = plan;
	l.hsc = m_hsc_on && !m_hsc_rom.empty();
	l.hsc_rom = m_hsc_rom.empty() ? nullptr : m_hsc_rom.data();
	l.hsc_ram = m_hsc_ram;
	for (unsigned n = 0, k = fuji_load_slices(&plan, l.hsc); n < k; n++)
	{
		fuji_load_slice(&l, n, &s, scratch);
		uint8_t *dst = &m_sram[uint32_t(s.page) * A78MAP_PAGE_SIZE + s.k * 0x400u];
		if (s.src)
			std::memcpy(dst, s.src, 0x400);
		else
			std::memset(dst, s.fill, 0x400);
	}
	a78map_init(&m_loader.next, &plan, l.hsc);
	m_loader.plan = plan;
	m_loader.next_mode = plan.claim ? FN_MODE_APP : FN_MODE_GAME;
	m_loader.next_pokey = plan.pokey;
	m_loader.next_hsc = l.hsc;
	flip();
	osd_printf_info("fujinet: direct kind=%s crc=%08X size=%u claim=%d hsc=%d\n",
			a78map_kind_name(plan.kind), plan.crc, plan.size, plan.claim, l.hsc);
}

// The host's image, staged and armed: the loader's first FN_HOT_SWAP at
// power-on loads it instead of CONFIG, and the hand-over starts it.
void a78_fujinet_device::stage_boot_image()
{
	a78map_plan_t plan;
	const char *mapper = m_host.boot_mapper.empty() ? nullptr : m_host.boot_mapper.c_str();

	if (a78map_plan(m_host.boot_image.data(), uint32_t(m_host.boot_image.size()), mapper, &plan) != A78MAP_OK)
	{
		osd_printf_error("fujinet: the boot image cannot be mapped; booting CONFIG\n");
		return;
	}
	std::lock_guard<std::mutex> lock(m_load_lock);
	m_boot_img = m_host.boot_image;
	fuji_load_stage(&m_loader, m_boot_img.data(), &plan);
	fuji_load_arm(&m_loader);
	publish_staged(plan);
	osd_printf_info("fujinet: staged kind=%s crc=%08X size=%u claim=%d biosok=%u\n",
			a78map_kind_name(plan.kind), plan.crc, plan.size, plan.claim, plan.biosok);
}

void a78_fujinet_device::device_reset()
{
	address_space &space = m_cpu->space(AS_PROGRAM);

	// MAME's 7800 gives a stock cart $4000-$FFFF only; the cart's other
	// ranges are installed here, after the driver's own machine_start.
	space.install_readwrite_handler(0x0400, 0x047f,
			read8sm_delegate(*this, NAME([this] (offs_t offset) { return bus_read(uint16_t(0x0400 + offset)); })),
			write8sm_delegate(*this, NAME([this] (offs_t offset, u8 data) { bus_write(uint16_t(0x0400 + offset), data); })));
	space.install_readwrite_handler(0x0600, 0x17ff,
			read8sm_delegate(*this, NAME([this] (offs_t offset) { return bus_read(uint16_t(0x0600 + offset)); })),
			write8sm_delegate(*this, NAME([this] (offs_t offset, u8 data) { bus_write(uint16_t(0x0600 + offset), data); })));
	space.install_readwrite_handler(0x3000, 0x3fff,
			read8sm_delegate(*this, NAME([this] (offs_t offset) { return bus_read(uint16_t(0x3000 + offset)); })),
			write8sm_delegate(*this, NAME([this] (offs_t offset, u8 data) { bus_write(uint16_t(0x3000 + offset), data); })));

	if (!m_init_done)
	{
		m_init_done = true;
		m_host = a78_fujinet_get_host();
		s_debug = m_host.debug;

		// Take the mailbox service over from any cartridge still running.
		{
			std::lock_guard<std::mutex> lock(s_active_lock);
			if (s_active && s_active != this)
				s_active->stop_worker();
			s_active = this;
		}
		fujimail_init(m_host.debug ? &port_debug : &port_quiet);
		fujimail_paint();

		// Every TIA write reaches INPTCTRL until it locks; the console's own
		// decode keeps them from the cart, so they come off a tap.
		m_tap_tia = space.install_write_tap(0x0000, 0x03ff, "fujinet_tia",
				[this] (offs_t offset, u8 &data, u8)
				{
					if (machine().side_effects_disabled() || !a78_in_inptctrl(uint16_t(offset)))
						return;
					if (a78_inptctrl_write(&m_bus, data))
					{
						{
							std::lock_guard<std::mutex> lock(m_load_lock);
							flip();
							fuji_load_event(&m_loader, A78_W_GO_BIOS);
						}
						m_pokey->reset();
					}
				}, &m_tap_tia);
		// Only the PAL BIOS runs from $C000-$EFFF.
		m_tap_bios = space.install_read_tap(0xc000, 0xefff, "fujinet_bios",
				[this] (offs_t, u8 &, u8)
				{
					if (!machine().side_effects_disabled() && m_bus.bootblk)
						m_loader.tv = FN_TV_PAL;
				}, &m_tap_bios);
		if (!strcmp(machine().system().name, "a7800p"))
			m_loader.tv = FN_TV_PAL;

		if (m_host.hsc_rom.size() == A78MAP_HSC_ROM_SIZE)
		{
			m_hsc_rom = m_host.hsc_rom;
			m_hsc_on = m_host.hsc_on;
		}
		m_hsc_have_rom = !m_hsc_rom.empty();
		m_loader.hsc_rom = m_hsc_rom.empty() ? nullptr : m_hsc_rom.data();
		m_loader.hsc_ram = m_hsc_ram;
		m_loader.hsc_on = m_hsc_on;

		// CONFIG: baked, or a claimed client from the host or -cart; an
		// unclaimed -cart is a game, run direct.
		m_config.assign(_configrom, _configrom + FUJI_CONFIGROM_SIZE);
		std::vector<uint8_t> client = m_host.client;
		auto *slot = dynamic_cast<device_image_interface *>(owner());
		if (client.empty() && slot && slot->exists())
			load_file(client, slot->filename());
		if (!client.empty())
		{
			a78map_plan_t plan;

			if (a78map_plan(client.data(), uint32_t(client.size()), nullptr, &plan) == A78MAP_OK)
			{
				if (plan.claim)
				{
					m_config.assign(client.begin() + plan.offset, client.end());
					osd_printf_info("fujinet: %u-byte client in place of CONFIG\n", unsigned(m_config.size()));
				}
				else if (m_host.boot_mode != A78_FUJINET_BOOT_DIRECT)
				{
					m_direct_img = client;
				}
			}
		}
		{
			a78map_plan_t cp;

			if (a78map_plan(m_config.data(), uint32_t(m_config.size()), nullptr, &cp) != A78MAP_OK)
				std::memset(&cp, 0, sizeof cp);
			m_loader.port = &m_load_port;
			m_loader.bus = &m_bus;
			fuji_load_init(&m_loader, m_config.data(), &cp);
		}
		hsc_init(&m_hsc, &m_hsc_port, m_hsc_ram, &m_hsc_dirty);
	}
	else
	{
		// A reset is a power cycle (the 7800 has no reset line): the boot
		// block again, with whatever the host now wants booted. The mailbox,
		// the store and the HSC's RAM survive, as they do on a USB-powered
		// cart. The link's host and port hold for the machine's lifetime.
		a78_fujinet_host_config host = a78_fujinet_get_host();
		m_host.boot_mode = host.boot_mode;
		m_host.boot_image = std::move(host.boot_image);
		m_host.boot_mapper = host.boot_mapper;
		{
			std::lock_guard<std::mutex> lock(m_load_lock);
			fuji_load_abort(&m_loader);
		}
	}

	to_boot();
	m_live_crc = 0;
	m_booted_image = false;

	bool fujinet = true;
	if (m_host.boot_mode == A78_FUJINET_BOOT_DIRECT && !m_host.boot_image.empty())
	{
		direct_boot(m_host.boot_image, m_host.boot_mapper.empty() ? nullptr : m_host.boot_mapper.c_str());
		fujinet = m_loader.plan.claim;
	}
	else if (!m_direct_img.empty())
	{
		direct_boot(m_direct_img, nullptr);
		fujinet = m_loader.plan.claim;
	}
	else if (m_host.boot_mode == A78_FUJINET_BOOT_STAGED && !m_host.boot_image.empty())
	{
		stage_boot_image();
	}

	// A game alone needs no FujiNet: no socket, so A/B runs can go in
	// parallel against fujinet-pc's single BoIP client.
	if (fujinet && !m_worker.joinable())
	{
		start_worker();
		m_service_timer->adjust(attotime::from_msec(50), 0, attotime::from_msec(50));
	}
	publish_status();
}

/*-------------------------------------------------
    the modes, as core1 switches them
-------------------------------------------------*/

void a78_fujinet_device::to_boot()
{
	a78_bus_reset(&m_bus);
	m_live = nullptr;
	std::memset(m_load_slots, 0, sizeof m_load_slots);
	m_mode_out = FN_MODE_BOOT;
	m_mode_shared = FN_MODE_BOOT;
}

void a78_fujinet_device::to_load()
{
	a78_bus_load(&m_bus);
	m_live = nullptr;
	std::memset(m_load_slots, 0, sizeof m_load_slots);
	poke(FN_R_LOAD_STATE, FN_LOAD_IDLE);
	m_mode_out = FN_MODE_LOAD;
	m_mode_shared = FN_MODE_LOAD;
}

// Called with m_load_lock held.
void a78_fujinet_device::flip()
{
	m_live = &m_loader.next;
	a78_bus_run(&m_bus, m_loader.next_mode, m_loader.next_pokey, m_loader.next_hsc);
	m_mode_out = m_loader.next_mode;                // for the soak harness
	m_mode_shared = m_loader.next_mode;
	m_live_crc = m_live->plan.crc;
	m_booted_image = m_live->plan.crc != m_loader.config_plan.crc;
	if (s_debug)
		osd_printf_info("fujinet: flip to mode %u (%s)\n", m_bus.mode, a78map_kind_name(m_live->plan.kind));
}

uint16_t a78_fujinet_device::slot_word(uint16_t a) const
{
	switch (m_bus.mode)
	{
	case FN_MODE_LOAD:
		return m_load_slots[a >> 13];
	case FN_MODE_GAME:
	case FN_MODE_APP:
		return m_live ? m_live->slot[a >> 13] : 0;
	default:
		return 0;
	}
}

/*-------------------------------------------------
    the bus
-------------------------------------------------*/

uint8_t a78_fujinet_device::bus_read(uint16_t a)
{
	uint16_t w = slot_word(a);

	if (a78_glue_oe(true, w, a, true))
	{
		uint32_t lo = a & 0x1fff;
		if (!a78_glue_a8(w, a))
			lo &= ~0x100u;
		return m_sram[(uint32_t(w & A78S_PAGE_MASK) << 13) | lo];
	}
	unsigned off;
	switch (a78_read_kind(&m_bus, a, &off))
	{
	case A78_R_BOOTBLK: return _bootblk[off];
	case A78_R_LOADER:  return _loaderrom[off];
	case A78_R_ARENA:
		if (m_publish_ready.load(std::memory_order_acquire))
			drain_published();
		return m_arena[off];
	case A78_R_POKEY:   return m_pokey->read(off);
	default:
		// as MAME's own carts read: $FF from the slot, open bus below it
		return a < 0x4000 ? m_cpu->space(AS_PROGRAM).unmap() : 0xff;
	}
}

void a78_fujinet_device::bus_write(uint16_t a, uint8_t data)
{
	uint16_t w;
	int kind;

	if (machine().side_effects_disabled())
		return;
	w = slot_word(a);
	if (a78_glue_we(true, w, a, false, true))
	{
		uint32_t lo = a & 0x1fff;
		if (!a78_glue_a8(w, a))
			lo &= ~0x100u;
		m_sram[(uint32_t(w & A78S_PAGE_MASK) << 13) | lo] = data;
	}
	kind = a78_write_kind(&m_bus, a);
	switch (kind)
	{
	case A78_W_MAPPER:
		if (m_live)
			a78map_write(m_live, a, data);
		break;
	case A78_W_HSC:
		m_hsc_ram[a & (A78MAP_HSC_RAM_SIZE - 1)] = data;
		__atomic_fetch_or(&m_hsc_dirty, 1u << ((a & (A78MAP_HSC_RAM_SIZE - 1)) >> 6), __ATOMIC_SEQ_CST);
		break;
	case A78_W_POKEY:
		m_pokey->write(a & 0x0f, data);
		break;
	case A78_W_MAILBOX:
		mailbox_event(uint16_t(a - FN_ARENA_BASE), data);
		break;
	case A78_W_SWAP:
		// BOOTLOCK, written just before, arms the swap on the worker; on the
		// cart that takes core0 microseconds.
		sync_worker(2000);
		[[fallthrough]];
	case A78_W_CONFIG:
		{
			std::lock_guard<std::mutex> lock(m_load_lock);
			to_load();
			fuji_load_event(&m_loader, kind);
		}
		break;
	case A78_W_GO:
		{
			std::lock_guard<std::mutex> lock(m_load_lock);
			flip();
			fuji_load_event(&m_loader, A78_W_GO);
		}
		m_pokey->reset();
		break;
	case A78_W_GO_BIOS:
		m_bus.go_bios = true;
		break;
	default:
		break;
	}
}

uint8_t a78_fujinet_device::read_40xx(offs_t offset) { return bus_read(uint16_t(0x4000 + offset)); }
void a78_fujinet_device::write_40xx(offs_t offset, uint8_t data) { bus_write(uint16_t(0x4000 + offset), data); }

/*-------------------------------------------------
    the arena
-------------------------------------------------*/

void a78_fujinet_device::dump_sram()
{
	std::string path = m_host.sramdump + ".sram";
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return;
	fwrite(m_sram.data(), 1, m_sram.size(), f);
	fclose(f);
}

void a78_fujinet_device::port_poke(unsigned offset, uint8_t value)
{
	if (offset >= FN_R_PAINT_END)
		return;

	if (s_on_worker)
	{
		{
			std::lock_guard<std::mutex> lock(m_publish_lock);
			m_published[offset] = value;
			m_publish_lo = std::min<size_t>(m_publish_lo, offset);
			m_publish_hi = std::max<size_t>(m_publish_hi, offset + 1);
		}
		// Every byte is offered, not just ACKSEQ, so a mount's progress can
		// be watched while the transaction is out. The interlock survives:
		// fujimail publishes ACKSEQ last, and a drain copies the whole pending
		// range under one lock.
		m_publish_ready.store(true, std::memory_order_release);
	}
	else
	{
		poke(offset, value);
	}
}

void a78_fujinet_device::drain_published()
{
	std::lock_guard<std::mutex> lock(m_publish_lock);
	for (size_t i = m_publish_lo; i < m_publish_hi; i++)
		m_arena[i] = m_published[i];
	m_publish_lo = FN_R_PAINT_END;
	m_publish_hi = 0;
	m_publish_ready.store(false, std::memory_order_relaxed);
}

// A status byte from the emulation thread (core1, or fuji_load on it).
void a78_fujinet_device::poke(unsigned offset, uint8_t value)
{
	if (offset >= FN_R_PAINT_END)
		return;

	// No BIOS to hand over to: the loader starts the game itself. Called from
	// fuji_load_ack() with m_load_lock held, so its own record follows.
	if (offset == FN_R_HANDOVER && value == FN_HO_BIOS && !bios_present())
	{
		value = FN_HO_DIRECT;
		m_loader.handover = FN_HO_DIRECT;
	}

	if (m_publish_ready.load(std::memory_order_acquire))
		drain_published();
	{
		// Into the publish shadow too: a drain copies the whole pending range,
		// not just the bytes the worker wrote, so a byte written only here
		// would otherwise come back as the shadow's stale value.
		std::lock_guard<std::mutex> lock(m_publish_lock);
		m_published[offset] = value;
		m_arena[offset] = value;
	}

	if (offset == FN_R_HANDOVER)
		m_handover_out = value;                // for the soak harness
	if (offset == FN_R_LOAD_STATE && value == FN_LOAD_DONE)
	{
		const a78map_plan_t &p = m_loader.plan;
		osd_printf_info("fujinet: loaded kind=%s crc=%08X size=%u claim=%d hsc=%d handover=%s\n",
				a78map_kind_name(p.kind), p.crc, p.size, p.claim, m_loader.hsc,
				m_loader.handover == FN_HO_BIOS ? "BIOS" : "direct");
		if (!m_host.sramdump.empty())
			dump_sram();
	}
}

// The 1K reply window, painted by fuji_load on the emulation thread.
void a78_fujinet_device::port_paint(const uint8_t *src, uint8_t fill)
{
	if (m_publish_ready.load(std::memory_order_acquire))
		drain_published();
	std::lock_guard<std::mutex> lock(m_publish_lock);
	if (src)
	{
		std::memcpy(m_arena + FN_R_DATA, src, FN_R_SLICE_LEN);
		std::memcpy(m_published.data() + FN_R_DATA, src, FN_R_SLICE_LEN);
	}
	else
	{
		std::memset(m_arena + FN_R_DATA, fill, FN_R_SLICE_LEN);
		std::memset(m_published.data() + FN_R_DATA, fill, FN_R_SLICE_LEN);
	}
}

bool a78_fujinet_device::own_register(unsigned reg, uint8_t data)
{
	switch (reg)
	{
	case FN_REG_SLICE_ACK:
		{
			std::lock_guard<std::mutex> lock(m_load_lock);
			fuji_load_ack(&m_loader);
		}
		return true;
	case FN_REG_TV:
		m_loader.tv = data ? FN_TV_PAL : FN_TV_NTSC;
		return true;
	case FN_REG_HSC:
		{
			std::lock_guard<std::mutex> lock(m_load_lock);
			if (data == FN_HSCOP_INSTALL)
			{
				const a78map_plan_t &p = m_loader.staged_plan;

				if (!m_loader.have_staged || p.kind != A78MAP_HSC || p.size != A78MAP_HSC_ROM_SIZE)
					return true;
				m_hsc_rom.assign(m_loader.staged_base + p.offset,
						m_loader.staged_base + p.offset + A78MAP_HSC_ROM_SIZE);
				fuji_load_unstage(&m_loader);
				poke(FN_R_STAGED, 0);
				m_hsc_on = true;
				osd_printf_info("fujinet: HSC ROM installed\n");
			}
			else if (data == FN_HSCOP_FORGET)
			{
				m_hsc_rom.clear();
				m_hsc_on = false;
			}
			else
			{
				m_hsc_on = data == FN_HSCOP_ON && !m_hsc_rom.empty();
			}
			m_hsc_have_rom = !m_hsc_rom.empty();
			m_loader.hsc_rom = m_hsc_rom.empty() ? nullptr : m_hsc_rom.data();
			m_loader.hsc_on = m_hsc_on;
		}
		return true;
	default:
		return false;
	}
}

// One hotspot write, decoded as fujinet.c does on the cart. The swap, the
// loader's slice ack and the cart's own registers belong to the bus layer
// (inline); everything else is fujimail's, on the worker.
void a78_fujinet_device::mailbox_event(uint16_t offset, uint8_t data)
{
	unsigned page = offset & FN_H_PAGE_MASK;
	unsigned low = offset & 0xff;

	if (page == FN_H_REGSEL)
	{
		if (own_register(low, data))
			return;
		if (low >= 0x80)
			return;
		note(uint16_t(FN_H_REGSEL + low));
		note(uint16_t(FN_H_REGDATA + data));
	}
	else if (page == FN_H_DATA)
	{
		note(uint16_t(FN_H_DATA + data));
	}
}

void a78_fujinet_device::publish_status()
{
	a78_fujinet_status st;

	if (m_publish_ready.load(std::memory_order_acquire))
		drain_published();
	st.instance = m_instance;
	st.present = true;
	st.link_up = m_link.is_open();
	st.worker = m_worker.joinable();
	st.mode = m_bus.mode;
	st.handover = m_arena[FN_R_HANDOVER];
	st.ackseq = m_arena[FN_R_ACKSEQ];
	st.err = m_arena[FN_R_ERR];
	st.boot_state = m_arena[FN_R_BOOT_STATE];
	st.boot_pct = m_arena[FN_R_BOOT_PCT];
	st.boot_err = m_arena[FN_R_BOOT_ERR];
	st.load_state = m_arena[FN_R_LOAD_STATE];
	st.load_pct = m_arena[FN_R_LOAD_PCT];
	st.mapper = m_arena[FN_R_MAPPER];
	st.staged = m_arena[FN_R_STAGED];
	st.staged_kind = m_arena[FN_R_STAGED_KIND];
	st.staged_crc = uint32_t(m_arena[FN_R_STAGED_CRC]) | (uint32_t(m_arena[FN_R_STAGED_CRC + 1]) << 8)
			| (uint32_t(m_arena[FN_R_STAGED_CRC + 2]) << 16) | (uint32_t(m_arena[FN_R_STAGED_CRC + 3]) << 24);
	st.live_crc = m_live_crc;
	st.hsc = m_arena[FN_R_HSC];
	st.tv = m_loader.tv;
	st.inptctrl = m_bus.inptctrl;
	st.inpt_locked = m_bus.inpt_locked;
	st.booted_image = m_booted_image;
	st.queue_depth = uint32_t((m_queue_head.load() + QUEUE_SIZE - m_queue_tail.load()) % QUEUE_SIZE);
	if (m_live)
		snprintf(st.live_kind, sizeof st.live_kind, "%s", a78map_kind_name(m_live->plan.kind));
	snprintf(st.link_error, sizeof st.link_error, "%s", m_link.last_error().c_str());

	std::lock_guard<std::mutex> lock(s_status_lock);
	s_status = st;
}

// The emulation side of core0's loop, every 50 ms of emulated time.
TIMER_CALLBACK_MEMBER(a78_fujinet_device::service)
{
	uint8_t v = 0;

	if (m_publish_ready.load(std::memory_order_acquire))
		drain_published();
	if (m_hsc_have_rom)
		v |= FN_HSC_ROM;
	if (m_hsc_on)
		v |= FN_HSC_ON;
	if (m_hsc_sd_ok)
		v |= FN_HSC_SD;
	if (__atomic_load_n(&m_hsc_dirty, __ATOMIC_RELAXED) || m_hsc_pending)
		v |= FN_HSC_DIRTY;
	poke(FN_R_HSC, v);
	poke(FN_R_TV, m_loader.tv);
	poke(FN_R_INPTCTRL, m_bus.inptctrl);
	poke(FN_R_INPT_LOCK, m_bus.inpt_locked ? 1 : 0);
	poke(FN_R_MODE, m_bus.mode);
	poke(FN_R_LINK, m_link.is_open() ? 1 : 0);
	publish_status();
}

/*-------------------------------------------------
    the worker: fujimail, as core0 runs it
-------------------------------------------------*/

void a78_fujinet_device::start_worker()
{
	if (m_worker.joinable())
		return;
	m_stop.store(false, std::memory_order_relaxed);
	m_link.set_inbound_handler(c_inbound);
	m_worker = std::thread([this] () { worker_loop(); });
}

void a78_fujinet_device::stop_worker()
{
	if (!m_worker.joinable())
		return;

	// Closed FIRST: a worker parked in select() on a sixty-second mount wakes
	// at once instead of holding up the machine's teardown.
	m_stop.store(true, std::memory_order_release);
	m_link.close();
	m_wake.notify_all();
	m_worker.join();
	m_link.close();
}

bool a78_fujinet_device::try_open_link()
{
	if (m_link.is_open())
		return true;
	return m_link.open(m_host.host, m_host.port);
}

void a78_fujinet_device::worker_loop()
{
	s_on_worker = true;
	try_open_link();
	auto last_attempt = std::chrono::steady_clock::now();
	auto last_hsc = last_attempt;

	for (;;)
	{
		{
			std::unique_lock<std::mutex> lock(m_wake_lock);
			m_wake.wait_for(lock, std::chrono::milliseconds(50), [this] () {
				return m_stop.load(std::memory_order_acquire) ||
						m_queue_head.load(std::memory_order_acquire) != m_queue_tail.load(std::memory_order_relaxed);
			});
		}
		if (m_stop.load(std::memory_order_acquire))
			return;

		// A FujiNet that came up after the cartridge: keep trying, gently,
		// so CONFIG finds the link without waiting out a transaction.
		auto now = std::chrono::steady_clock::now();
		if (!m_link.is_open() && now - last_attempt >= std::chrono::seconds(1))
		{
			try_open_link();
			last_attempt = std::chrono::steady_clock::now();
		}

		{
			// fujimail's file-scope state is touched by whoever holds this.
			std::lock_guard<std::timed_mutex> mail(m_mail_lock);
			for (;;)
			{
				size_t tail = m_queue_tail.load(std::memory_order_relaxed);
				if (tail == m_queue_head.load(std::memory_order_acquire) || m_stop.load(std::memory_order_acquire))
					break;
				uint16_t offset = m_queue[tail];
				m_queue_tail.store((tail + 1) % QUEUE_SIZE, std::memory_order_release);
				fujimail_read_hotspot(offset);
			}
		}

		if (std::chrono::steady_clock::now() - last_hsc >= std::chrono::milliseconds(50))
		{
			worker_hsc();
			last_hsc = std::chrono::steady_clock::now();
		}
	}
}

// The HSC's saves, on the link's own thread so they never interleave with a
// transaction.
void a78_fujinet_device::worker_hsc()
{
	if (!m_hsc_have_rom)
		return;
	uint8_t mode = m_mode_shared.load();
	if (!m_hsc.restored)
	{
		if (mode == FN_MODE_BOOT || mode == FN_MODE_APP)
		{
			hsc_restore(&m_hsc);
			if (s_debug && m_hsc.restored)
				osd_printf_info("fujinet: HSC RAM restored (sd=%d): %02X %02X %02X %02X\n",
						m_hsc.sd_ok, m_hsc_ram[0], m_hsc_ram[1], m_hsc_ram[2], m_hsc_ram[3]);
		}
	}
	else
	{
		hsc_service(&m_hsc, mode == FN_MODE_GAME);
	}
	m_hsc_sd_ok = m_hsc.sd_ok;
	m_hsc_pending = m_hsc.pending != 0;
}

// One decoded hotspot access, to the worker: the cart's core1 -> core0
// ring. Nothing is dropped unless the ring is full, as on the hardware.
void a78_fujinet_device::note(uint16_t offset)
{
	size_t head = m_queue_head.load(std::memory_order_relaxed);
	size_t next = (head + 1) % QUEUE_SIZE;
	if (next == m_queue_tail.load(std::memory_order_acquire))
		return;
	m_queue[head] = offset;
	m_queue_head.store(next, std::memory_order_release);
	m_wake.notify_one();
}

// Wait until the worker has nothing queued and is not inside fujimail: the
// bus layer must see the effect of events the console has already written.
bool a78_fujinet_device::sync_worker(uint32_t timeout_ms)
{
	if (!m_worker.joinable())
		return true;

	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	for (;;)
	{
		if (m_queue_head.load(std::memory_order_acquire) == m_queue_tail.load(std::memory_order_acquire))
		{
			if (m_mail_lock.try_lock_until(deadline))
			{
				bool empty = m_queue_head.load(std::memory_order_acquire) == m_queue_tail.load(std::memory_order_acquire);
				m_mail_lock.unlock();
				if (empty)
					return true;
			}
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			osd_printf_verbose("fujinet: the mailbox worker did not go idle\n");
			return false;
		}
		std::this_thread::yield();
	}
}

void a78_fujinet_device::port_wait_link(uint32_t ms)
{
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (!m_link.is_open() && !m_stop.load(std::memory_order_acquire))
	{
		if (try_open_link() || std::chrono::steady_clock::now() >= deadline)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
}

fb_status_t a78_fujinet_device::port_transact(uint8_t device, uint8_t command, const fb_param_t *params, unsigned nparams,
		const uint8_t *payload, uint16_t payload_len, uint32_t timeout_ms, fb_reply_t *reply)
{
	fb_status_t status = m_link.transact(device, command, params, nparams, payload, payload_len, timeout_ms, reply);
	if (status == FB_ENOLINK)
		m_link.close();          // fujinet-pc went away: the next transaction redials
	return status;
}

void a78_fujinet_device::port_send_bare(uint8_t device, uint8_t command, const uint8_t *payload, uint16_t payload_len)
{
	m_link.send_bare(device, command, payload, payload_len);
}

/*-------------------------------------------------
    push streams: fuji_store's two tiers (worker)
-------------------------------------------------*/

uint8_t a78_fujinet_device::port_stream_open(int stream, uint32_t size)
{
	if (stream != FN_STREAM_ROM)
	{
		m_cfg.clear();
		return 0;
	}
	uint8_t err = a78map_gate(size);
	if (err)
		return err;

	std::lock_guard<std::mutex> lock(m_load_lock);
	fuji_load_unstage(&m_loader);
	port_poke(FN_R_STAGED, 0);
	m_open_tier = -1;
	if (size <= STORE_RAM_MAX && !fuji_load_busy(&m_loader, m_store[0].data()))
		m_open_tier = 0;
	else if (size <= STORE_MAX && !fuji_load_busy(&m_loader, m_store[1].data()))
		m_open_tier = 1;
	else
		return size <= STORE_MAX ? FN_BOOT_ERR_STOREBUSY : FN_BOOT_ERR_TOOBIG;
	m_store[m_open_tier].clear();
	return 0;
}

void a78_fujinet_device::port_stream_write(int stream, const uint8_t *chunk, unsigned len)
{
	if (stream != FN_STREAM_ROM)
	{
		m_cfg.insert(m_cfg.end(), chunk, chunk + len);
		return;
	}
	if (m_open_tier >= 0 && m_store[m_open_tier].size() + len <= STORE_MAX)
		m_store[m_open_tier].insert(m_store[m_open_tier].end(), chunk, chunk + len);
}

// Called with m_load_lock held.
void a78_fujinet_device::publish_staged(const a78map_plan_t &p)
{
	uint8_t ok = m_loader.tv == FN_TV_PAL ? A78_BIOSOK_PAL : A78_BIOSOK_NTSC;
	uint8_t v = FN_STAGED_READY;

	if (p.claim)
		v |= FN_STAGED_CLAIM;
	if (p.biosok & ok)
		v |= FN_STAGED_BIOSOK;
	if (p.kind == A78MAP_HSC)
		v |= FN_STAGED_HSCROM;
	port_poke(FN_R_STAGED_KIND, p.kind);
	for (unsigned i = 0; i < 4; i++)
		port_poke(FN_R_STAGED_CRC + i, uint8_t(p.crc >> (8 * i)));
	port_poke(FN_R_STAGED, v);
}

uint8_t a78_fujinet_device::port_stream_close(int stream, uint32_t got, bool aborted)
{
	a78map_plan_t plan;
	int tier = m_open_tier;

	if (stream != FN_STREAM_ROM)
	{
		m_cfg_mapper.clear();
		std::string text(m_cfg.begin(), m_cfg.end());
		size_t at = text.find("mapper=");
		if (!aborted && at != std::string::npos)
		{
			size_t end = text.find_first_of(" \r\n", at + 7);
			m_cfg_mapper = text.substr(at + 7, end == std::string::npos ? std::string::npos : end - at - 7);
		}
		return 0;
	}
	m_open_tier = -1;
	if (aborted || tier < 0 || m_store[tier].empty())
	{
		m_cfg_mapper.clear();
		return 0;
	}
	int err = a78map_plan(m_store[tier].data(), got, m_cfg_mapper.empty() ? nullptr : m_cfg_mapper.c_str(), &plan);
	m_cfg_mapper.clear();
	if (err == A78MAP_ETOOBIG)
		return FN_BOOT_ERR_TOOBIG;
	if (err != A78MAP_OK)
	{
		osd_printf_info("fujinet: pushed image (%u bytes) is not mappable (%d)\n", got, err);
		return FN_BOOT_ERR_NOMAP;
	}
	osd_printf_info("fujinet: staged kind=%s crc=%08X size=%u claim=%d biosok=%u\n",
			a78map_kind_name(plan.kind), plan.crc, plan.size, plan.claim, plan.biosok);
	std::lock_guard<std::mutex> lock(m_load_lock);
	fuji_load_stage(&m_loader, m_store[tier].data(), &plan);
	publish_staged(plan);
	return 0;
}

void a78_fujinet_device::port_arm_swap()
{
	std::lock_guard<std::mutex> lock(m_load_lock);
	fuji_load_arm(&m_loader);
}
