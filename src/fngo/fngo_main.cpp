// license:BSD-3-Clause
// copyright-holders:Thomas Cherryhomes
/***************************************************************************

    fngo_main.cpp

    What the emulator core needs from a "frontend": the driver list (the
    Atari 7800, NTSC and PAL) and emulator_info. FujiNet Go has no MAME
    UI; the host application is the frontend (fngo_mame.cpp).

***************************************************************************/

#include "emu.h"

#include "drivenum.h"
#include "main.h"

#include <map>
#include <string>
#include <utility>
#include <vector>


extern const char bare_build_version[];
extern const char build_version[];

GAME_EXTERN(a7800);
GAME_EXTERN(a7800p);

const game_driver * const driver_list::s_drivers_sorted[3] =
{
	&GAME_NAME(___empty),
	&GAME_NAME(a7800),
	&GAME_NAME(a7800p),
};

std::size_t const driver_list::s_driver_count = 3;


const char *emulator_info::get_appname() { return "FujiNet Go Atari 7800"; }
const char *emulator_info::get_appname_lower() { return "fngo"; }
const char *emulator_info::get_configname() { return "fngo"; }
const char *emulator_info::get_copyright() { return "Copyright Nicola Salmoria and the MAME team"; }

const char *emulator_info::get_copyright_info()
{
	return "MAME (https://www.mamedev.org/) is licensed under the GNU General Public License,\n"
			"version 2 or later; the Atari 7800 driver and the FujiNet cartridge are BSD-3-Clause.";
}

const char *emulator_info::get_bare_build_version() { return bare_build_version; }
const char *emulator_info::get_build_version() { return build_version; }

// The host drives everything through fngo_mame.h; there is no command line.
int emulator_info::start_frontend(emu_options &options, osd_interface &osd, std::vector<std::string> &args) { return 0; }
int emulator_info::start_frontend(emu_options &options, osd_interface &osd, int argc, char *argv[]) { return 0; }

void emulator_info::display_ui_chooser(running_machine &machine) { }
bool emulator_info::draw_user_interface(running_machine &machine) { return false; }
void emulator_info::periodic_check() { }
bool emulator_info::frame_hook() { return false; }
void emulator_info::sound_hook(const std::map<std::string, std::vector<std::pair<const float *, int>>> &sound) { }
void emulator_info::layout_script_cb(layout_file &file, const char *script) { }
bool emulator_info::standalone() { return true; }
