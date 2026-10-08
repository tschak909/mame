-- license:BSD-3-Clause
-- copyright-holders:Thomas Cherryhomes
---------------------------------------------------------------------------
--
--   fngo.lua
--
--   The FujiNet Go OSD: headless. MAME renders, mixes and reads inputs
--   for a host application (FujiNet Go Atari 7800) instead of a window
--   of its own, so there is no SDL, no bgfx and no input or sound
--   module here. The main project becomes a shared library,
--   libmame_fngo, whose only exports are the C API in
--   src/fngo/fngo_mame.h; everything else, MAME's bundled zlib and expat
--   included, stays inside it.
--
--     make TARGET=fngo SUBTARGET=a7800 OSD=fngo NOWERROR=1
--          ARCHOPTS=-fPIC (not on Windows) BUILDDIR=<relative path>
--
--   produces <BUILDDIR>/fngo/libmame_fngo.so (.dylib), or mame_fngo.dll
--   and its import library libmame_fngo.dll.a.
--
---------------------------------------------------------------------------

dofile("modules.lua")

-- The host owns audio and MIDI is meaningless here.
_OPTIONS["NO_USE_MIDI"] = "1"
_OPTIONS["NO_USE_PORTAUDIO"] = "1"
_OPTIONS["NO_USE_PULSEAUDIO"] = "1"
_OPTIONS["NO_USE_PIPEWIRE"] = "1"

BASE_TARGETOS       = "unix"
FNGO_TARGETOS       = "unix"
if _OPTIONS["targetos"]=="windows" then
	BASE_TARGETOS       = "win32"
	FNGO_TARGETOS       = "win32"
elseif _OPTIONS["targetos"]=="macosx" then
	FNGO_TARGETOS       = "macosx"
end

local fngo_dir = MAME_DIR .. _OPTIONS["build-dir"] .. "/fngo"

function maintargetosdoptions(_target, _subtarget)
	kind "SharedLib"
	targetname "mame_fngo"
	targetdir(fngo_dir)

	-- main.lua asks for symbols in every executable; a library to ship
	-- carries them only for SYMBOLS=1
	if _OPTIONS["SYMBOLS"]==nil or _OPTIONS["SYMBOLS"]=="0" then
		removeflags {
			"Symbols",
		}
	end

	configuration { "Release" }
		targetsuffix ""
	configuration { "Debug" }
		targetsuffix ""

	configuration { "linux-* or freebsd or netbsd or openbsd" }
		targetprefix "lib"
		targetextension ".so"
		linkoptions {
			"-Wl,--version-script=" .. MAME_DIR .. "src/osd/fngo/fngo_mame.map",
			"-Wl,-soname,libmame_fngo.so",
			"-Wl,-Bsymbolic",
			"-Wl,--no-undefined",
		}
		links {
			"pthread",
			"util",
		}

	configuration { "osx*" }
		targetprefix "lib"
		targetextension ".dylib"
		linkoptions {
			"-Wl,-exported_symbols_list," .. MAME_DIR .. "src/osd/fngo/fngo_mame.exp",
			"-Wl,-install_name,@rpath/libmame_fngo.dylib",
		}
		links {
			"Carbon.framework",
			"CoreFoundation.framework",
		}

	configuration { "mingw*" }
		targetprefix ""
		targetextension ".dll"
		linkoptions {
			"-Wl,--out-implib," .. fngo_dir .. "/libmame_fngo.dll.a",
		}
		links {
			"ole32",
			"uuid",
			"bcrypt",
		}

	configuration { }
end


project ("qtdbg_" .. _OPTIONS["osd"])
	uuid (os.uuid("qtdbg_" .. _OPTIONS["osd"]))
	kind (LIBTYPE)

	-- main.lua links a debugger UI library for every OSD; the host draws
	-- the debugger (src/fngo/fngo_mame.h), so this one is empty.
	dofile("fngo_cfg.lua")
	includedirs {
		MAME_DIR .. "src/osd",
	}
	files {
		MAME_DIR .. "src/osd/fngo/fngo_nodebugger.cpp",
	}


project ("osd_" .. _OPTIONS["osd"])
	uuid (os.uuid("osd_" .. _OPTIONS["osd"]))
	kind (LIBTYPE)

	dofile("fngo_cfg.lua")

	includedirs {
		MAME_DIR .. "src/emu",
		MAME_DIR .. "src/devices",
		MAME_DIR .. "src/osd",
		MAME_DIR .. "src/lib",
		MAME_DIR .. "src/lib/util",
		MAME_DIR .. "src/frontend/mame",
		MAME_DIR .. "src/osd/fngo",
		MAME_DIR .. "3rdparty",
	}

	-- the interface sources every OSD compiles (modules.lua's
	-- osdmodulesbuild() lists them with the modules fngo does without)
	files {
		MAME_DIR .. "src/osd/osdepend.h",
		MAME_DIR .. "src/osd/interface/audio.cpp",
		MAME_DIR .. "src/osd/interface/audio.h",
		MAME_DIR .. "src/osd/interface/inputcode.h",
		MAME_DIR .. "src/osd/interface/inputdev.h",
		MAME_DIR .. "src/osd/interface/inputfwd.h",
		MAME_DIR .. "src/osd/interface/inputman.h",
		MAME_DIR .. "src/osd/interface/inputseq.cpp",
		MAME_DIR .. "src/osd/interface/inputseq.h",
		MAME_DIR .. "src/osd/interface/midiport.h",
		MAME_DIR .. "src/osd/interface/nethandler.cpp",
		MAME_DIR .. "src/osd/interface/nethandler.h",
		MAME_DIR .. "src/osd/interface/output.h",
		MAME_DIR .. "src/osd/interface/uievents.h",
		MAME_DIR .. "src/osd/fngo/fngo_osd.cpp",
		MAME_DIR .. "src/osd/fngo/fngo_osd.h",
	}


project ("ocore_" .. _OPTIONS["osd"])
	uuid (os.uuid("ocore_" .. _OPTIONS["osd"]))
	kind (LIBTYPE)

	removeflags {
		"SingleOutputDir",
	}

	dofile("fngo_cfg.lua")

	includedirs {
		MAME_DIR .. "src/emu",
		MAME_DIR .. "src/osd",
		MAME_DIR .. "src/lib",
		MAME_DIR .. "src/lib/util",
		ext_includedir("asio"),
	}

	files {
		MAME_DIR .. "src/osd/asio.cpp",
		MAME_DIR .. "src/osd/asio.h",
		MAME_DIR .. "src/osd/osdcore.cpp",
		MAME_DIR .. "src/osd/osdcore.h",
		MAME_DIR .. "src/osd/osdfile.h",
		MAME_DIR .. "src/osd/strconv.cpp",
		MAME_DIR .. "src/osd/strconv.h",
		MAME_DIR .. "src/osd/osdsync.cpp",
		MAME_DIR .. "src/osd/osdsync.h",
		MAME_DIR .. "src/osd/modules/osdmodule.cpp",
		MAME_DIR .. "src/osd/modules/osdmodule.h",
		MAME_DIR .. "src/osd/modules/lib/osdlib_" .. FNGO_TARGETOS .. ".cpp",
		MAME_DIR .. "src/osd/modules/lib/osdlib.h",
	}

	if BASE_TARGETOS=="unix" then
		files {
			MAME_DIR .. "src/osd/modules/file/posixdir.cpp",
			MAME_DIR .. "src/osd/modules/file/posixfile.cpp",
			MAME_DIR .. "src/osd/modules/file/posixfile.h",
			MAME_DIR .. "src/osd/modules/file/posixptty.cpp",
			MAME_DIR .. "src/osd/modules/file/posixsocket.cpp",
		}
	else
		includedirs {
			MAME_DIR .. "src/osd/windows",
		}
		files {
			MAME_DIR .. "src/osd/modules/file/windir.cpp",
			MAME_DIR .. "src/osd/modules/file/winfile.cpp",
			MAME_DIR .. "src/osd/modules/file/winfile.h",
			MAME_DIR .. "src/osd/modules/file/winptty.cpp",
			MAME_DIR .. "src/osd/modules/file/winsocket.cpp",
			MAME_DIR .. "src/osd/windows/winutil.cpp",
			MAME_DIR .. "src/osd/windows/winutil.h",
			MAME_DIR .. "src/osd/windows/winutf8.cpp",
			MAME_DIR .. "src/osd/windows/winutf8.h",
		}
	end
