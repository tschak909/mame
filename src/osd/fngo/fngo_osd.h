// license:BSD-3-Clause
// copyright-holders:Thomas Cherryhomes
//============================================================
//
//  fngo_osd.h - the FujiNet Go OSD: MAME, headless, for a host
//  application that owns the window, the audio and the input
//
//============================================================
#ifndef MAME_OSD_FNGO_FNGO_OSD_H
#define MAME_OSD_FNGO_FNGO_OSD_H

#pragma once

#include "osdepend.h"
#include "osdcore.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>


// What the OSD needs from the program driving MAME (src/fngo/fngo_mame.cpp).
class fngo_osd_host
{
public:
	virtual ~fngo_osd_host() = default;

	// a finished frame; `paced` is false while the debugger holds the machine
	virtual void osd_frame(const uint32_t *xrgb, int width, int height, uint64_t frame_no, bool paced) = 0;
	// interleaved stereo
	virtual void osd_audio(const int16_t *stereo, int frames) = 0;
	// once per frame, and repeatedly while the debugger holds the machine
	virtual void osd_service() = 0;
	// wait up to `ms` for the host to have something for osd_service()
	virtual void osd_wait(int ms) = 0;
	virtual void osd_log(int channel, const char *text) = 0;
	virtual void osd_notify_output(const char *name, int32_t value) = 0;
};


class fngo_osd : public osd_interface, public osd_output
{
public:
	fngo_osd(fngo_osd_host &host);
	virtual ~fngo_osd();

	running_machine *machine() const { return m_machine; }

	// osd_interface
	virtual void init(running_machine &machine) override;
	virtual void update(bool skip_redraw) override;
	virtual void input_update(bool relative_reset) override { }
	virtual void check_osd_inputs() override { }
	virtual void set_verbose(bool print_verbose) override { m_verbose = print_verbose; }

	virtual void init_debugger() override { }
	virtual void wait_for_debugger(device_t &device, bool firststop) override;

	virtual bool no_sound() override { return false; }
	virtual bool sound_external_per_channel_volume() override { return false; }
	virtual bool sound_split_streams_per_source() override { return false; }
	virtual uint32_t sound_get_generation() override { return 1; }
	virtual osd::audio_info sound_get_information() override;
	virtual uint32_t sound_stream_sink_open(uint32_t node, std::string name, uint32_t rate) override;
	virtual uint32_t sound_stream_source_open(uint32_t node, std::string name, uint32_t rate) override { return 0; }
	virtual void sound_stream_close(uint32_t id) override { }
	virtual void sound_stream_sink_update(uint32_t id, const int16_t *buffer, int samples_this_frame) override;
	virtual void sound_stream_source_update(uint32_t id, int16_t *buffer, int samples_this_frame) override { }
	virtual void sound_stream_set_volumes(uint32_t id, const std::vector<float> &db) override { }
	virtual void sound_begin_update() override { }
	virtual void sound_end_update() override { }

	virtual void customize_input_type_list(std::vector<input_type_entry> &typelist) override { }

	virtual void add_audio_to_recording(const int16_t *buffer, int samples_this_frame) override { }
	virtual std::vector<ui::menu_item> get_slider_list() override;

	virtual osd_font::ptr font_alloc() override { return nullptr; }
	virtual bool get_font_families(std::string const &font_path, std::vector<std::pair<std::string, std::string> > &result) override { return false; }

	virtual bool execute_command(const char *command) override { return false; }

	virtual std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view name) override;
	virtual std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view name) override;
	virtual std::vector<osd::midi_port_info> list_midi_ports() override;

	virtual std::unique_ptr<osd::network_device> open_network_device(int id, osd::network_handler &handler) override;
	virtual std::vector<osd::network_device_info> list_network_devices() override;

	// osd_output
	virtual void output_callback(osd_output_channel channel, util::format_argument_pack<char> const &args) override;

private:
	void machine_exit();
	bool debugger_stopped() const;

	fngo_osd_host &m_host;
	running_machine *m_machine = nullptr;
	render_target *m_target = nullptr;
	screen_device *m_screen = nullptr;
	std::vector<uint32_t> m_frame;
	bool m_verbose = false;
};

#endif // MAME_OSD_FNGO_FNGO_OSD_H
