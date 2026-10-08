// license:BSD-3-Clause
// copyright-holders:Thomas Cherryhomes
//============================================================
//
//  fngo_osd.cpp - the FujiNet Go OSD
//
//  MAME draws nothing and plays nothing itself here. Each emulated
//  frame, the screen's finished bitmap goes to the host as XRGB, the
//  mixed audio as stereo samples, and the host gets a turn (osd_service)
//  to apply input and work it has queued. The host paces emulation by
//  blocking in osd_frame(); MAME's own throttle is off.
//
//============================================================

#include "emu.h"
#include "fngo_osd.h"

#include "debugger.h"
#include "debug/debugcpu.h"
#include "emuopts.h"
#include "render.h"
#include "screen.h"

#include "ui/menuitem.h"

#include <algorithm>


fngo_osd::fngo_osd(fngo_osd_host &host)
	: m_host(host)
{
	osd_output::push(this);
}

fngo_osd::~fngo_osd()
{
	osd_output::pop(this);
}

void fngo_osd::init(running_machine &machine)
{
	m_machine = &machine;

	// A screen only updates while a visible render target shows it
	// (render_manager::is_live), so there must be one, though nothing is
	// ever rendered through it: the host gets the screen's own bitmap.
	m_target = machine.render().target_alloc();
	m_screen = screen_device_enumerator(machine.root_device()).first();

	machine.output().add_global_notifier(
			[] (void *param, osd::output_item const &item, s32, s64)
			{
				std::string const name(item.name());
				static_cast<fngo_osd *>(param)->m_host.osd_notify_output(name.c_str(), item.value());
			}, this);

	machine.add_notifier(MACHINE_NOTIFY_EXIT, machine_notify_delegate(&fngo_osd::machine_exit, this));
}

void fngo_osd::machine_exit()
{
	if (m_target)
		m_machine->render().target_free(m_target);
	m_target = nullptr;
	m_screen = nullptr;
	m_machine = nullptr;
}

bool fngo_osd::debugger_stopped() const
{
	return m_machine && (m_machine->debug_flags & DEBUG_FLAG_ENABLED) && m_machine->debugger().cpu().is_stopped();
}

void fngo_osd::update(bool skip_redraw)
{
	// The latest finished frame, every emulated frame whether it changed or
	// not: the host paces on these calls.
	if (m_screen && m_screen->palette().palette())
	{
		screen_bitmap &bitmap = m_screen->curbitmap();
		const rectangle vis = m_screen->visible_area();
		const int width = vis.width();
		const int height = vis.height();

		if (bitmap.valid() && width > 0 && height > 0 && vis.bottom() < bitmap.height() && vis.right() < bitmap.width())
		{
			m_frame.resize(size_t(width) * size_t(height));
			uint32_t *dst = m_frame.data();
			if (bitmap.format() == BITMAP_FORMAT_IND16)
			{
				// pens() are indices for an indexed bitmap; the colours, with
				// MAME's brightness, contrast and gamma, are the adjusted entries
				const rgb_t *colours = m_screen->palette().palette()->entry_list_adjusted();
				const bitmap_ind16 &src = bitmap.as_ind16();
				for (int y = vis.top(); y <= vis.bottom(); y++)
				{
					const uint16_t *row = &src.pix(y, vis.left());
					for (int x = 0; x < width; x++)
						*dst++ = uint32_t(colours[row[x]]) & 0x00ffffff;
				}
			}
			else
			{
				const bitmap_rgb32 &src = bitmap.as_rgb32();
				for (int y = vis.top(); y <= vis.bottom(); y++)
				{
					const uint32_t *row = &src.pix(y, vis.left());
					for (int x = 0; x < width; x++)
						*dst++ = row[x] & 0x00ffffff;
				}
			}
			m_host.osd_frame(m_frame.data(), width, height, m_screen->frame_number(), !debugger_stopped());
		}
	}

	m_host.osd_service();
}

void fngo_osd::wait_for_debugger(device_t &device, bool firststop)
{
	// The machine is stopped in the debugger: the host's turn for commands,
	// input, a reset or exit (which the debugger's loop sees as a scheduled
	// event, and resumes for), then a short wait for more.
	m_host.osd_service();
	m_host.osd_wait(20);
}

osd::audio_info fngo_osd::sound_get_information()
{
	osd::audio_info info;
	info.m_generation = 1;
	info.m_default_sink = 1;
	info.m_default_source = 0;

	osd::audio_info::node_info node;
	node.m_name = "fngo";
	node.m_display_name = "FujiNet Go";
	node.m_id = 1;
	node.m_rate = osd::audio_rate_range{ 48000, 48000, 48000 };
	node.m_port_names = { "Left", "Right" };
	node.m_port_positions = { osd::channel_position::FL(), osd::channel_position::FR() };
	node.m_sinks = 2;
	node.m_sources = 0;
	info.m_nodes.emplace_back(std::move(node));
	return info;
}

uint32_t fngo_osd::sound_stream_sink_open(uint32_t node, std::string name, uint32_t rate)
{
	return 1;
}

void fngo_osd::sound_stream_sink_update(uint32_t id, const int16_t *buffer, int samples_this_frame)
{
	if (samples_this_frame > 0)
		m_host.osd_audio(buffer, samples_this_frame);
}

std::vector<ui::menu_item> fngo_osd::get_slider_list()
{
	return std::vector<ui::menu_item>();
}

std::unique_ptr<osd::midi_input_port> fngo_osd::create_midi_input(std::string_view name)
{
	return nullptr;
}

std::unique_ptr<osd::midi_output_port> fngo_osd::create_midi_output(std::string_view name)
{
	return nullptr;
}

std::vector<osd::midi_port_info> fngo_osd::list_midi_ports()
{
	return std::vector<osd::midi_port_info>();
}

std::unique_ptr<osd::network_device> fngo_osd::open_network_device(int id, osd::network_handler &handler)
{
	return nullptr;
}

std::vector<osd::network_device_info> fngo_osd::list_network_devices()
{
	return std::vector<osd::network_device_info>();
}

void fngo_osd::output_callback(osd_output_channel channel, util::format_argument_pack<char> const &args)
{
	std::string const text(util::string_format(args));
	m_host.osd_log(int(channel), text.c_str());
}
