// license:BSD-3-Clause
// copyright-holders:Thomas Cherryhomes
/***************************************************************************

    fngo_mame.cpp

    The C API of libmame_fngo (fngo_mame.h): the machine manager that
    runs MAME's Atari 7800 on the host's thread, the host side of the
    fngo OSD, and thin wrappers over MAME's input ports, debugger, debug
    views and the FujiNet cartridge's host interface.

***************************************************************************/

#define FNGO_MAME_BUILD 1

#include "emu.h"
#include "fngo_mame.h"
#include "fngo_osd.h"

#include "bus/a7800/a78map.h"
#include "bus/a7800/fujinet_host.h"

#include "debug/debugcon.h"
#include "debug/debugcpu.h"
#include "debug/debugvw.h"
#include "debug/dvdisasm.h"
#include "debug/dvmemory.h"
#include "debugger.h"
#include "drivenum.h"
#include "emuopts.h"
#include "main.h"
#include "screen.h"
#include "ui/uimain.h"
#include "video.h"

#include "corestr.h"
#include "unzip.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>


extern const char build_version[];

namespace {

// The one machine manager: MAME's core expects one per process.
class fngo_machine_manager : public machine_manager
{
public:
	fngo_machine_manager(emu_options &options, osd_interface &osd)
		: machine_manager(options, osd)
	{
		// the core asks for it every run; inactive (no -http)
		start_http_server();
	}

	virtual ui_manager *create_ui(running_machine &machine) override
	{
		m_ui = std::make_unique<ui_manager>(machine);
		return m_ui.get();
	}

	// After the machine is built and before it runs.
	virtual void ui_initialize(running_machine &machine) override
	{
		// MAME's own UI applies -throttle; with none, the host does:
		// emulation is paced by the host's frame callback, never by MAME.
		machine.video().set_throttled(false);

		// The debugger is always on (it cannot be attached to a running
		// machine) and starts out holding the machine: let go now, unless
		// the host wants to start stopped.
		if ((machine.debug_flags & DEBUG_FLAG_ENABLED) && !(m_start_stopped && *m_start_stopped))
			machine.debugger().cpu().set_execution_running();
	}

	std::atomic<bool> *m_start_stopped = nullptr;

private:
	std::unique_ptr<ui_manager> m_ui;
};

std::atomic<bool> s_exists { false };

void copy_cstr(char *dst, int size, const std::string &src)
{
	if (!dst || size <= 0)
		return;
	std::strncpy(dst, src.c_str(), size_t(size) - 1);
	dst[size - 1] = '\0';
}

} // anonymous namespace


struct fngo_mame : public fngo_osd_host
{
	fngo_mame_callbacks cb {};
	std::string data_dir;

	std::mutex config_lock;
	std::string system = "a7800";
	std::string bios = "none";
	std::string rompath;
	std::atomic<bool> start_stopped { false };

	std::atomic<bool> stop_requested { false };
	std::atomic<int> reset_requested { -1 };     // FNGO_RESET_*, or -1
	std::mutex wake_lock;
	std::condition_variable wake_cv;
	bool woken = false;

	std::unique_ptr<emu_options> options;
	std::unique_ptr<fngo_osd> osd;
	std::unique_ptr<fngo_machine_manager> manager;
	running_machine *machine = nullptr;         // the emulation thread's
	uint64_t frame_no = 0;
	bool in_service = false;
	bool was_stopped = false;

	// ---- fngo_osd_host ----

	virtual void osd_frame(const uint32_t *xrgb, int width, int height, uint64_t frame, bool paced) override
	{
		frame_no = frame;
		if (cb.frame)
			cb.frame(cb.user, xrgb, width, height, frame, paced ? 1 : 0);
	}

	virtual void osd_audio(const int16_t *stereo, int frames) override
	{
		if (cb.audio)
			cb.audio(cb.user, stereo, frames);
	}

	virtual void osd_service() override
	{
		// The debugger redraws through the OSD (refresh_display() runs a
		// frame update), so a debugger call made from the host's service
		// callback comes back here: once is enough.
		if (in_service)
			return;
		in_service = true;
		if (machine)
		{
			if (stop_requested.load())
			{
				if (!machine->exit_pending())
					machine->schedule_exit();
			}
			else
			{
				int const reset = reset_requested.exchange(-1);
				if (reset == FNGO_RESET_SOFT)
					machine->schedule_soft_reset();
				else if (reset == FNGO_RESET_HARD)
					machine->schedule_hard_reset();
			}
		}
		// Tell the host when the debugger has stopped the machine. The
		// debugger redraws (and so comes here) before it waits, and a host
		// may resume from right here, so this is the one place that always
		// sees the stop.
		bool const stopped = machine && (machine->debug_flags & DEBUG_FLAG_ENABLED) && machine->debugger().cpu().is_stopped();
		if (stopped && !was_stopped && cb.debug_stopped)
			cb.debug_stopped(cb.user);
		was_stopped = stopped;

		if (cb.service)
			cb.service(cb.user);
		in_service = false;
	}

	virtual void osd_wait(int ms) override
	{
		std::unique_lock<std::mutex> lock(wake_lock);
		wake_cv.wait_for(lock, std::chrono::milliseconds(ms), [this] () { return woken; });
		woken = false;
	}

	virtual void osd_log(int channel, const char *text) override
	{
		if (cb.log)
			cb.log(cb.user, channel, text);
	}

	virtual void osd_notify_output(const char *name, int32_t value) override
	{
		if (cb.output)
			cb.output(cb.user, name, value);
	}

	// ---- helpers ----

	void wake()
	{
		{
			std::lock_guard<std::mutex> lock(wake_lock);
			woken = true;
		}
		wake_cv.notify_all();
	}

	void set_path(const char *option, const char *sub)
	{
		std::string const path = data_dir + PATH_SEPARATOR + sub;
		options->set_value(option, path, OPTION_PRIORITY_CMDLINE);
	}

	ioport_field *field(const char *tag, uint32_t mask)
	{
		if (!machine || !tag)
			return nullptr;
		ioport_port *port = machine->root_device().ioport(tag);
		if (!port)
			return nullptr;
		for (ioport_field &f : port->fields())
			if (f.mask() & mask)
				return &f;
		return nullptr;
	}
};


struct fngo_view
{
	fngo_mame *owner;
	debug_view *view;
};


/*-------------------------------------------------
    lifecycle
-------------------------------------------------*/

int fngo_mame_api_version(void)
{
	return FNGO_MAME_API_VERSION;
}

const char *fngo_mame_build_version(void)
{
	return build_version;
}

fngo_mame *fngo_mame_create(const fngo_mame_callbacks *callbacks, const char *data_dir)
{
	bool expected = false;
	if (!s_exists.compare_exchange_strong(expected, true))
		return nullptr;

	auto *m = new fngo_mame();
	if (callbacks)
		m->cb = *callbacks;
	m->data_dir = (data_dir && *data_dir) ? data_dir : ".";

	m->osd = std::make_unique<fngo_osd>(*m);
	m->options = std::make_unique<emu_options>();
	m->manager = std::make_unique<fngo_machine_manager>(*m->options, *m->osd);
	m->manager->m_start_stopped = &m->start_stopped;

	emu_options &o = *m->options;
	o.set_value(OPTION_READCONFIG, false, OPTION_PRIORITY_CMDLINE);
	o.set_value(OPTION_THROTTLE, false, OPTION_PRIORITY_CMDLINE);
	o.set_value(OPTION_FRAMESKIP, 0, OPTION_PRIORITY_CMDLINE);
	o.set_value(OPTION_SAMPLERATE, FNGO_MAME_SAMPLE_RATE, OPTION_PRIORITY_CMDLINE);
	o.set_value(OPTION_DEBUG, true, OPTION_PRIORITY_CMDLINE);
	o.set_value(OPTION_NVRAM_SAVE, false, OPTION_PRIORITY_CMDLINE);
	o.set_value(OPTION_SKIP_GAMEINFO, true, OPTION_PRIORITY_CMDLINE);
	o.set_value(OPTION_CHEAT, false, OPTION_PRIORITY_CMDLINE);

	// MAME's own files stay under the host's directory, never the cwd.
	m->set_path(OPTION_PLUGINDATAPATH, "home");
	m->set_path(OPTION_MEDIAPATH, "roms");
	m->set_path(OPTION_HASHPATH, "hash");
	m->set_path(OPTION_SAMPLEPATH, "samples");
	m->set_path(OPTION_ARTPATH, "artwork");
	m->set_path(OPTION_CTRLRPATH, "ctrlr");
	m->set_path(OPTION_INIPATH, "ini");
	m->set_path(OPTION_FONTPATH, "fonts");
	m->set_path(OPTION_CHEATPATH, "cheat");
	m->set_path(OPTION_CROSSHAIRPATH, "crosshair");
	m->set_path(OPTION_PLUGINSPATH, "plugins");
	m->set_path(OPTION_LANGUAGEPATH, "language");
	m->set_path(OPTION_SWPATH, "software");
	m->set_path(OPTION_CFG_DIRECTORY, "cfg");
	m->set_path(OPTION_NVRAM_DIRECTORY, "nvram");
	m->set_path(OPTION_INPUT_DIRECTORY, "inp");
	m->set_path(OPTION_STATE_DIRECTORY, "sta");
	m->set_path(OPTION_SNAPSHOT_DIRECTORY, "snap");
	m->set_path(OPTION_DIFF_DIRECTORY, "diff");
	m->set_path(OPTION_COMMENT_DIRECTORY, "comments");
	m->set_path(OPTION_SHARE_DIRECTORY, "share");

	// What the cartridge reports, as MAME's own console would: until the
	// host configures it, the FUJINET_* environment applies.
	(void)a78_fujinet_get_host();
	return m;
}

void fngo_mame_destroy(fngo_mame *m)
{
	if (!m)
		return;
	m->manager.reset();
	m->options.reset();
	m->osd.reset();
	delete m;
	s_exists = false;
}

void fngo_mame_configure(fngo_mame *m, const char *system, const char *bios, const char *rompath)
{
	std::lock_guard<std::mutex> lock(m->config_lock);
	if (system && *system)
		m->system = system;
	if (bios && *bios)
		m->bios = bios;
	if (rompath)
		m->rompath = rompath;
}

int fngo_mame_run(fngo_mame *m)
{
	int error = EMU_ERR_NONE;
	emu_options &o = *m->options;

	while (!m->stop_requested.load())
	{
		std::string system, bios, rompath;
		{
			std::lock_guard<std::mutex> lock(m->config_lock);
			system = m->system;
			bios = m->bios;
			rompath = m->rompath;
		}

		int const index = driver_list::find(system.c_str());
		if (index < 0)
			return EMU_ERR_NO_SUCH_SYSTEM;
		game_driver const &driver = driver_list::driver(index);

		// The system first: it decides which slot and BIOS options exist.
		o.set_system_name(system);
		o.set_value("cartslot", "fujinet", OPTION_PRIORITY_CMDLINE);
		o.set_value(OPTION_BIOS, bios, OPTION_PRIORITY_CMDLINE);
		if (!rompath.empty())
			o.set_value(OPTION_MEDIAPATH, rompath, OPTION_PRIORITY_CMDLINE);

		m->reset_requested = -1;
		{
			machine_config config(driver, o);
			running_machine machine(config, *m->manager);
			m->manager->set_machine(&machine);
			m->machine = &machine;
			error = machine.run(false);
			m->machine = nullptr;
			m->manager->set_machine(nullptr);

			if (error != EMU_ERR_NONE || !machine.hard_reset_pending())
				break;
		}
	}
	return error;
}

void fngo_mame_stop(fngo_mame *m)
{
	m->stop_requested = true;
	m->wake();
}

void fngo_mame_reset(fngo_mame *m, int kind)
{
	m->reset_requested = (kind == FNGO_RESET_HARD) ? FNGO_RESET_HARD : FNGO_RESET_SOFT;
	m->wake();
}

void fngo_mame_wake(fngo_mame *m)
{
	m->wake();
}

void fngo_mame_start_stopped(fngo_mame *m, int on)
{
	m->start_stopped = on != 0;
}


/*-------------------------------------------------
    the FujiNet cartridge
-------------------------------------------------*/

void fngo_mame_fujinet_link(fngo_mame *m, const char *host, int port, int debug)
{
	a78_fujinet_host_config c = a78_fujinet_get_host();
	c.host = (host && *host) ? host : "127.0.0.1";
	c.port = port;
	c.debug = debug != 0;
	a78_fujinet_set_host(c);
}

void fngo_mame_fujinet_boot(fngo_mame *m, int mode, const uint8_t *image, uint32_t size, const char *mapper)
{
	a78_fujinet_host_config c = a78_fujinet_get_host();
	if (mode == FNGO_BOOT_NONE || !image || !size)
	{
		c.boot_mode = A78_FUJINET_BOOT_NONE;
		c.boot_image.clear();
	}
	else
	{
		c.boot_mode = (mode == FNGO_BOOT_DIRECT) ? A78_FUJINET_BOOT_DIRECT : A78_FUJINET_BOOT_STAGED;
		c.boot_image.assign(image, image + size);
	}
	c.boot_mapper = mapper ? mapper : "";
	a78_fujinet_set_host(c);
}

void fngo_mame_fujinet_client(fngo_mame *m, const uint8_t *image, uint32_t size)
{
	a78_fujinet_host_config c = a78_fujinet_get_host();
	if (image && size)
		c.client.assign(image, image + size);
	else
		c.client.clear();
	a78_fujinet_set_host(c);
}

void fngo_mame_fujinet_hsc(fngo_mame *m, const uint8_t *rom, uint32_t size, int on)
{
	a78_fujinet_host_config c = a78_fujinet_get_host();
	if (rom && size == A78MAP_HSC_ROM_SIZE)
		c.hsc_rom.assign(rom, rom + size);
	else
		c.hsc_rom.clear();
	c.hsc_on = on && !c.hsc_rom.empty();
	a78_fujinet_set_host(c);
}

int fngo_mame_fujinet_status(fngo_mame_cart_status *out)
{
	a78_fujinet_status s;
	bool const present = a78_fujinet_get_status(s);
	if (!out)
		return present ? 1 : 0;

	*out = fngo_mame_cart_status();
	out->instance = s.instance;
	out->present = s.present ? 1 : 0;
	out->link_up = s.link_up ? 1 : 0;
	out->worker = s.worker ? 1 : 0;
	out->mode = s.mode;
	out->handover = s.handover;
	out->ackseq = s.ackseq;
	out->err = s.err;
	out->boot_state = s.boot_state;
	out->boot_pct = s.boot_pct;
	out->boot_err = s.boot_err;
	out->load_state = s.load_state;
	out->load_pct = s.load_pct;
	out->mapper = s.mapper;
	out->staged = s.staged;
	out->staged_kind = s.staged_kind;
	out->staged_crc = s.staged_crc;
	out->live_crc = s.live_crc;
	out->hsc = s.hsc;
	out->tv = s.tv;
	out->inptctrl = s.inptctrl;
	out->inpt_locked = s.inpt_locked ? 1 : 0;
	out->booted_image = s.booted_image ? 1 : 0;
	out->queue_depth = s.queue_depth;
	std::memcpy(out->live_kind, s.live_kind, sizeof out->live_kind);
	std::memcpy(out->link_error, s.link_error, sizeof out->link_error);
	return present ? 1 : 0;
}

uint32_t fngo_mame_fujinet_latest(void)
{
	return a78_fujinet_latest_instance();
}


/*-------------------------------------------------
    images
-------------------------------------------------*/

int fngo_mame_plan(const uint8_t *image, uint32_t size, const char *mapper,
		fngo_mame_plan_t *out, char *why, int why_size)
{
	a78map_plan_t p;
	int const err = a78map_plan(image, size, mapper, &p);

	if (err != A78MAP_OK)
	{
		switch (err)
		{
		case A78MAP_ETOOBIG:
			copy_cstr(why, why_size, "The image is larger than the FujiNet cartridge's 448K.");
			break;
		case A78MAP_EUNSUPPORTED:
			copy_cstr(why, why_size, "The image needs a cartridge board the FujiNet cartridge does not emulate (XM, XBoard, Versaboard or Megacart).");
			break;
		default:
			copy_cstr(why, why_size, "The image is empty.");
			break;
		}
		return err;
	}
	if (out)
	{
		*out = fngo_mame_plan_t();
		copy_cstr(out->kind, sizeof out->kind, a78map_kind_name(p.kind));
		out->crc = p.crc;
		out->size = p.size;
		out->offset = p.offset;
		out->claim = p.claim ? 1 : 0;
		out->pokey = p.pokey;
		out->biosok = p.biosok;
		out->in_db = p.db ? 1 : 0;
		out->ram_size = p.ram_size;
	}
	copy_cstr(why, why_size, "");
	return 0;
}

const uint8_t *fngo_mame_config_rom(uint32_t *size)
{
	uint32_t n = 0;
	const uint8_t *rom = a78_fujinet_config_rom(n);
	if (size)
		*size = n;
	return rom;
}

namespace {

bool name_matches(const std::string &name, const char *exts)
{
	if (!exts || !*exts)
		return true;
	std::string const lower = strmakelower(name);
	std::string list(exts);
	size_t start = 0;
	while (start <= list.size())
	{
		size_t end = list.find(';', start);
		if (end == std::string::npos)
			end = list.size();
		std::string const ext = strmakelower(list.substr(start, end - start));
		if (!ext.empty() && lower.size() >= ext.size() && !lower.compare(lower.size() - ext.size(), ext.size(), ext))
			return true;
		start = end + 1;
	}
	return false;
}

} // anonymous namespace

int fngo_mame_archive_read(const char *path, const char *exts, uint8_t **data, uint32_t *size,
		char *name, int name_size)
{
	if (!path || !data || !size)
		return -1;
	*data = nullptr;
	*size = 0;

	util::archive_file::ptr archive;
	if (!util::archive_file::open_zip(path, archive) || !util::archive_file::open_7z(path, archive))
	{
		for (int i = archive->first_file(); i >= 0; i = archive->next_file())
		{
			if (archive->current_is_directory() || !name_matches(archive->current_name(), exts))
				continue;
			uint64_t const length = archive->current_uncompressed_length();
			if (length == 0 || length > 64 * 1024 * 1024)
				continue;
			auto *buffer = static_cast<uint8_t *>(std::malloc(size_t(length)));
			if (!buffer)
				return -1;
			if (archive->decompress(buffer, size_t(length)))
			{
				std::free(buffer);
				return -1;
			}
			*data = buffer;
			*size = uint32_t(length);
			copy_cstr(name, name_size, archive->current_name());
			return 0;
		}
		return -1;
	}

	// not an archive: the file itself
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	std::vector<uint8_t> bytes;
	uint8_t chunk[65536];
	size_t n;
	while ((n = fread(chunk, 1, sizeof chunk, f)) > 0 && bytes.size() <= 64 * 1024 * 1024)
		bytes.insert(bytes.end(), chunk, chunk + n);
	fclose(f);
	if (bytes.empty())
		return -1;
	auto *buffer = static_cast<uint8_t *>(std::malloc(bytes.size()));
	if (!buffer)
		return -1;
	std::memcpy(buffer, bytes.data(), bytes.size());
	*data = buffer;
	*size = uint32_t(bytes.size());
	std::string base(path);
	size_t const slash = base.find_last_of("/\\");
	copy_cstr(name, name_size, slash == std::string::npos ? base : base.substr(slash + 1));
	return 0;
}

void fngo_mame_free(void *p)
{
	std::free(p);
}


/*-------------------------------------------------
    input
-------------------------------------------------*/

int fngo_mame_ioport_set(fngo_mame *m, const char *tag, uint32_t mask, int32_t value)
{
	ioport_field *f = m->field(tag, mask);
	if (!f)
		return -1;
	f->set_value(ioport_value(value));
	return 0;
}

int fngo_mame_ioport_clear(fngo_mame *m, const char *tag, uint32_t mask)
{
	ioport_field *f = m->field(tag, mask);
	if (!f)
		return -1;
	f->clear_value();
	return 0;
}

int fngo_mame_ioport_setting(fngo_mame *m, const char *tag, uint32_t mask, uint32_t value)
{
	ioport_field *f = m->field(tag, mask);
	if (!f)
		return -1;
	ioport_field::user_settings settings;
	f->get_user_settings(settings);
	settings.value = value;
	f->set_user_settings(settings);
	return 0;
}

int fngo_mame_ioport_read(fngo_mame *m, const char *tag, uint32_t *value)
{
	if (!m->machine || !tag)
		return -1;
	ioport_port *port = m->machine->root_device().ioport(tag);
	if (!port)
		return -1;
	if (value)
		*value = port->read();
	return 0;
}


/*-------------------------------------------------
    the debugger
-------------------------------------------------*/

namespace {

bool have_debugger(fngo_mame *m)
{
	return m && m->machine && (m->machine->debug_flags & DEBUG_FLAG_ENABLED);
}

device_t *visible_cpu(fngo_mame *m)
{
	device_t *cpu = m->machine->debugger().console().get_visible_cpu();
	if (!cpu)
		cpu = m->machine->root_device().subdevice("maincpu");
	return cpu;
}

} // anonymous namespace

int fngo_mame_debug_command(fngo_mame *m, const char *command)
{
	if (!have_debugger(m) || !command)
		return -1;
	CMDERR const err = m->machine->debugger().console().execute_command(command, true);
	if (err.error_class() != CMDERR::NONE)
		return int(err.error_offset()) + 1;
	m->machine->debug_view().update_all();
	return 0;
}

int fngo_mame_debug_validate(fngo_mame *m, const char *command)
{
	if (!have_debugger(m) || !command)
		return -1;
	CMDERR const err = m->machine->debugger().console().validate_command(command);
	return err.error_class() == CMDERR::NONE ? 0 : int(err.error_offset()) + 1;
}

void fngo_mame_debug_break(fngo_mame *m)
{
	if (have_debugger(m))
		m->machine->debugger().debug_break();
}

void fngo_mame_debug_go(fngo_mame *m)
{
	if (!have_debugger(m))
		return;
	if (device_t *cpu = visible_cpu(m))
		cpu->debug()->go();
	m->wake();
}

void fngo_mame_debug_step(fngo_mame *m, int kind)
{
	if (!have_debugger(m))
		return;
	device_t *cpu = visible_cpu(m);
	if (!cpu)
		return;
	switch (kind)
	{
	case FNGO_STEP_OVER: cpu->debug()->single_step_over(); break;
	case FNGO_STEP_OUT:  cpu->debug()->single_step_out(); break;
	default:             cpu->debug()->single_step(); break;
	}
	m->wake();
}

int fngo_mame_debug_stopped(fngo_mame *m)
{
	return have_debugger(m) && m->machine->debugger().cpu().is_stopped() ? 1 : 0;
}


/*-------------------------------------------------
    debug views
-------------------------------------------------*/

fngo_view *fngo_mame_view_alloc(fngo_mame *m, int type)
{
	if (!have_debugger(m))
		return nullptr;
	debug_view_type dvt;
	switch (type)
	{
	case FNGO_VIEW_CONSOLE:     dvt = DVT_CONSOLE; break;
	case FNGO_VIEW_STATE:       dvt = DVT_STATE; break;
	case FNGO_VIEW_DISASM:      dvt = DVT_DISASSEMBLY; break;
	case FNGO_VIEW_MEMORY:      dvt = DVT_MEMORY; break;
	case FNGO_VIEW_LOG:         dvt = DVT_LOG; break;
	case FNGO_VIEW_BREAKPOINTS: dvt = DVT_BREAK_POINTS; break;
	case FNGO_VIEW_WATCHPOINTS: dvt = DVT_WATCH_POINTS; break;
	default: return nullptr;
	}
	debug_view *view = m->machine->debug_view().alloc_view(dvt, nullptr, nullptr);
	if (!view)
		return nullptr;
	// as MAME's own debuggers start them: the disassembly follows the PC
	if (dvt == DVT_DISASSEMBLY)
		downcast<debug_view_disasm *>(view)->set_expression("curpc");
	return new fngo_view{ m, view };
}

void fngo_mame_view_free(fngo_mame *m, fngo_view *v)
{
	if (!v)
		return;
	if (have_debugger(m))
		m->machine->debug_view().free_view(*v->view);
	delete v;
}

void fngo_mame_view_set(fngo_view *v, int cols, int rows, int left, int top)
{
	if (!v)
		return;
	debug_view_xy size = v->view->visible_size();
	debug_view_xy pos = v->view->visible_position();
	if (cols > 0) size.x = cols;
	if (rows > 0) size.y = rows;
	if (left >= 0) pos.x = left;
	if (top >= 0) pos.y = top;
	v->view->set_visible_size(size);
	v->view->set_visible_position(pos);
}

int fngo_mame_view_get(fngo_view *v, fngo_cell *cells, int max, fngo_view_geom *geom)
{
	if (!v)
		return 0;
	debug_view &view = *v->view;
	debug_view_xy const total = view.total_size();
	debug_view_xy const vis = view.visible_size();
	debug_view_xy const pos = view.visible_position();
	debug_view_xy const cursor = view.cursor_position();
	if (geom)
	{
		geom->total_cols = total.x;
		geom->total_rows = total.y;
		geom->cols = vis.x;
		geom->rows = vis.y;
		geom->left = pos.x;
		geom->top = pos.y;
		geom->cursor_col = cursor.x;
		geom->cursor_row = cursor.y;
		geom->cursor_visible = view.cursor_supported() && view.cursor_visible() ? 1 : 0;
	}
	int const n = std::min(max, vis.x * vis.y);
	const debug_view_char *data = view.viewdata();
	for (int i = 0; i < n; i++)
	{
		cells[i].ch = data[i].byte;
		cells[i].attr = data[i].attrib;
	}
	return n;
}

int fngo_mame_view_sources(fngo_view *v, char *names, int size)
{
	if (!v)
		return 0;
	int count = 0;
	int used = 0;
	for (auto const &source : v->view->source_list())
	{
		std::string const &name = source->name();
		if (names && used + int(name.size()) + 1 <= size)
		{
			std::memcpy(names + used, name.c_str(), name.size() + 1);
			used += int(name.size()) + 1;
		}
		count++;
	}
	return count;
}

void fngo_mame_view_source(fngo_view *v, int index)
{
	if (!v)
		return;
	const debug_view_source *source = v->view->source(unsigned(index));
	if (source)
		v->view->set_source(*source);
}

void fngo_mame_view_expression(fngo_view *v, const char *expression)
{
	if (!v || !expression)
		return;
	if (v->view->type() == DVT_DISASSEMBLY)
		downcast<debug_view_disasm *>(v->view)->set_expression(expression);
	else if (v->view->type() == DVT_MEMORY)
		downcast<debug_view_memory *>(v->view)->set_expression(expression);
}

void fngo_mame_view_click(fngo_view *v, int button, int col, int row)
{
	if (v)
		v->view->process_click(button, debug_view_xy(col, row));
}

void fngo_mame_view_char(fngo_view *v, int ch)
{
	if (v)
		v->view->process_char(ch);
}

int fngo_mame_view_selected_address(fngo_view *v, uint32_t *address)
{
	if (!v || v->view->type() != DVT_DISASSEMBLY)
		return -1;
	offs_t const a = downcast<debug_view_disasm *>(v->view)->selected_address();
	if (address)
		*address = a;
	return 0;
}


/*-------------------------------------------------
    the machine's state
-------------------------------------------------*/

int fngo_mame_read(fngo_mame *m, uint32_t address, uint8_t *dst, int n)
{
	if (!m->machine || !dst || n <= 0)
		return 0;
	device_t *cpu = m->machine->root_device().subdevice("maincpu");
	device_memory_interface *memory;
	if (!cpu || !cpu->interface(memory) || !memory->has_space(AS_PROGRAM))
		return 0;
	address_space &space = memory->space(AS_PROGRAM);
	auto dis = m->machine->disable_side_effects();
	for (int i = 0; i < n; i++)
		dst[i] = space.read_byte((address + i) & space.addrmask());
	return n;
}

int fngo_mame_save_item(fngo_mame *m, const char *device, const char *name, void *dst, int size)
{
	if (!m->machine || !device || !name)
		return -1;
	std::string const tag = std::string("/") + device + "/";
	std::string const tail = std::string("/") + name;
	save_manager &save = m->machine->save();
	for (int i = 0; i < save.registration_count(); i++)
	{
		void *base;
		u32 valsize, valcount, blockcount, stride;
		const char *item = save.indexed_item(i, base, valsize, valcount, blockcount, stride);
		if (!item)
			continue;
		std::string const full(item);
		if (full.find(tag) == std::string::npos || full.size() < tail.size()
				|| full.compare(full.size() - tail.size(), tail.size(), tail))
			continue;
		int const bytes = int(valsize * valcount * blockcount);
		if (dst && size > 0)
		{
			// blocks may be strided (struct members); copy each value
			auto *out = static_cast<uint8_t *>(dst);
			int written = 0;
			for (u32 b = 0; b < blockcount && written < size; b++)
			{
				const uint8_t *src = static_cast<const uint8_t *>(base) + size_t(b) * stride * valsize;
				int const chunk = std::min(int(valsize * valcount), size - written);
				std::memcpy(out + written, src, size_t(chunk));
				written += chunk;
			}
		}
		return bytes;
	}
	return -1;
}

int fngo_mame_palette(fngo_mame *m, uint32_t *xrgb, int max)
{
	if (!m->machine)
		return 0;
	screen_device *screen = screen_device_enumerator(m->machine->root_device()).first();
	if (!screen || !screen->palette().palette())
		return 0;
	int const entries = int(screen->palette().entries());
	const rgb_t *colours = screen->palette().palette()->entry_list_adjusted();
	int const n = std::min(entries, max);
	for (int i = 0; i < n && xrgb; i++)
		xrgb[i] = uint32_t(colours[i]) & 0x00ffffff;
	return entries;
}

uint64_t fngo_mame_frame_number(fngo_mame *m)
{
	return m->frame_no;
}

double fngo_mame_time(fngo_mame *m)
{
	return m->machine ? m->machine->time().as_double() : 0.0;
}
