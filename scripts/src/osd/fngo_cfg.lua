-- license:BSD-3-Clause
-- copyright-holders:Thomas Cherryhomes

defines {
	"OSD_FNGO",
}

if _OPTIONS["targetos"]=="windows" then
	defines {
		"OSD_WINDOWS",
		"UNICODE",
		"_UNICODE",
		"WIN32_LEAN_AND_MEAN",
		"NOMINMAX",
		"WINVER=0x0A00",
		"_WIN32_WINNT=0x0A00",
	}
end
