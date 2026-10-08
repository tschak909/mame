-- license:BSD-3-Clause
-- copyright-holders:Thomas Cherryhomes
---------------------------------------------------------------------------
--
--   fngo/a7800.lua
--
--   FujiNet Go Atari 7800: MAME's a7800 driver (NTSC and PAL) and the
--   FujiNet cartridge, standalone, with no frontend, UI, Lua or plugins.
--   Built with OSD=fngo into a shared library (libmame_fngo) that the
--   desktop application drives through src/fngo/fngo_mame.h:
--
--     make TARGET=fngo SUBTARGET=a7800 OSD=fngo
--
--   The device sets are exactly what scripts/build/makedep.py emits for
--   src/mame/atari/a7800.cpp, so the emulation matches SUBTARGET=a7800
--   SOURCES=src/mame/atari/a7800.cpp.
--
---------------------------------------------------------------------------

STANDALONE = true

BUSES["A7800"] = true
CPUS["M6502"] = true
MACHINES["MOS6530"] = true
SOUNDS["POKEY"] = true
SOUNDS["TIA"] = true
SOUNDS["YM2151"] = true

function standalone()
	includedirs {
		MAME_DIR .. "src/mame/shared",
		MAME_DIR .. "src/fngo",
		MAME_DIR .. "src/osd/fngo",
		ext_includedir("asio"),
	}
	files {
		MAME_DIR .. "src/mame/atari/a7800.cpp",
		MAME_DIR .. "src/mame/atari/maria.cpp",
		MAME_DIR .. "src/mame/atari/maria.h",
		MAME_DIR .. "src/fngo/fngo_main.cpp",
		MAME_DIR .. "src/fngo/fngo_mame.cpp",
		MAME_DIR .. "src/fngo/fngo_mame.h",
		GEN_DIR .. "version.cpp",
	}
end
