// license:BSD-3-Clause
// copyright-holders:Nigel Barnes
/**********************************************************************

    Acorn ANC01 6502 2nd Processor

    http://chrisacorns.computinghistory.org.uk/8bit_Upgrades/Acorn_ANC01_65022ndproc.html

**********************************************************************/


#include "emu.h"
#include "tube_6502.h"
#include "softlist_dev.h"


//**************************************************************************
//  DEVICE DEFINITIONS
//**************************************************************************

DEFINE_DEVICE_TYPE(BBC_TUBE_6502, bbc_tube_6502_device, "bbc_tube_6502", "Acorn 6502 2nd Processor")


//-------------------------------------------------
//  ADDRESS_MAP( tube_6502_mem )
//-------------------------------------------------

void bbc_tube_6502_device::tube_6502_mem(address_map &map)
{
	map(0x0000, 0xffff).rw(FUNC(bbc_tube_6502_device::read), FUNC(bbc_tube_6502_device::write));
}

//-------------------------------------------------
//  ROM( tube_6502 )
//-------------------------------------------------

ROM_START( tube_6502 )
	ROM_REGION(0x1000, "rom", 0)
	ROM_LOAD("6502tube.rom", 0x0000, 0x1000, CRC(98b5fe42) SHA1(338269d03cf6bfa28e09d1651c273ea53394323b))
ROM_END

//-------------------------------------------------
//  device_add_mconfig - add device configuration
//-------------------------------------------------

void bbc_tube_6502_device::device_add_mconfig(machine_config &config)
{
	M65C02(config, m_m6502, 12_MHz_XTAL / 4);
	m_m6502->set_addrmap(AS_PROGRAM, &bbc_tube_6502_device::tube_6502_mem);

	TUBE(config, m_ula);
	m_ula->pnmi_handler().set_inputline(m_m6502, M65C02_NMI_LINE);
	m_ula->pirq_handler().set_inputline(m_m6502, M65C02_IRQ_LINE);

	/* internal ram */
	RAM(config, m_ram).set_default_size("64K").set_default_value(0);

	/* software lists */
	SOFTWARE_LIST(config, "flop_ls_6502").set_original("bbc_flop_6502");
	SOFTWARE_LIST(config, "flop_ls_65c102").set_original("bbc_flop_65c102");
}

//-------------------------------------------------
//  rom_region - device-specific ROM region
//-------------------------------------------------

const tiny_rom_entry *bbc_tube_6502_device::device_rom_region() const
{
	return ROM_NAME( tube_6502 );
}

//**************************************************************************
//  LIVE DEVICE
//**************************************************************************

//-------------------------------------------------
//  bbc_tube_6502_device - constructor
//-------------------------------------------------

bbc_tube_6502_device::bbc_tube_6502_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, BBC_TUBE_6502, tag, owner, clock),
		device_bbc_tube_interface(mconfig, *this),
		m_m6502(*this, "m6502"),
		m_ula(*this, "ula"),
		m_ram(*this, "ram"),
		m_rom(*this, "rom"),
		m_rom_enabled(true)
{
}

//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void bbc_tube_6502_device::device_start()
{
	m_slot = dynamic_cast<bbc_tube_slot_device *>(owner());
}

//-------------------------------------------------
//  device_reset - device-specific reset
//-------------------------------------------------

void bbc_tube_6502_device::device_reset()
{
	m_ula->reset();

	m_rom_enabled = true;
}


//**************************************************************************
//  IMPLEMENTATION
//**************************************************************************

READ8_MEMBER(bbc_tube_6502_device::host_r)
{
	return m_ula->host_r(space, offset);
}

WRITE8_MEMBER(bbc_tube_6502_device::host_w)
{
	m_ula->host_w(space, offset, data);
}


READ8_MEMBER(bbc_tube_6502_device::read)
{
	uint8_t data;

	if (offset >= 0xfef0 && offset <= 0xfeff)
	{
		if (!machine().side_effects_disabled()) m_rom_enabled = false;
		data = m_ula->parasite_r(space, offset);
	}
	else if (m_rom_enabled && (offset >= 0xf000))
	{
		data = m_rom->base()[offset & 0xfff];
	}
	else
	{
		data = m_ram->pointer()[offset];
	}
	return data;
}

WRITE8_MEMBER(bbc_tube_6502_device::write)
{
	if (offset >= 0xfef0 && offset <= 0xfeff)
	{
		m_ula->parasite_w(space, offset, data);
	}
	else
	{
		m_ram->pointer()[offset] = data;
	}
}
