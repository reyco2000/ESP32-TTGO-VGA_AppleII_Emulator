/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : Apple2Machine.cpp
 *  Module : Top-level machine orchestrator. Creates and wires CPU
 *           + Memory + Apple2Device and the slot cards for the
 *           selected MachineProfile, loads the system and Disk II
 *           ROMs from /roms on the SD card (falling back to the
 *           built-in images where the model has them), and drives
 *           the per-frame cycle budget.
 * ============================================================
*/

#include "Predef.h"
#include "rombios.h"
#include "Apple2Machine.h"
#include "RomLoader.h"
#include "../Tools/Log.h"
#include "../VGA/VGA.h"

Apple2Machine::Apple2Machine(const MachineProfile& p)
	: profile(p), bootNote("")
{
	DEBUG_PRINTLN("Construct Apple2Machine");
	cpu.cmos = (profile.cpu == CPU_65C02);
	device.slots[6] = profile.diskIISlot6 ? &device.disk6 : NULL;
}

Apple2Machine::~Apple2Machine()
{

}

void Apple2Machine::InitMachine()
{
	mem.Create(profile.systemRomSize);
	LoadRoms();
	device.InsetFloppy();
	// unset the Power-UP byte
	mem.WriteByte(0x3F4, 0);
	cpu.Reset(mem);
	mem.WriteByte(0x4D, 0xAA);   // Just crashes if this memory location equals zero
	mem.WriteByte(0xD0, 0xAA);   // won't work if this memory location equals zero

	device.Create(&cpu);
	mem.device = &device;

	Booting();
}

// Once per power-up: the images stay in memory across resets. A file in
// /roms wins over the built-in copy, so a user can supply their own dump.
void Apple2Machine::LoadRoms()
{
	DEBUG_PRINTLN("Load ROMs");
	if (RomLoader::Load(profile.systemRom, mem.rom, profile.systemRomSize) != ROM_OK)
	{
		// setup() only boots a model without a built-in image when its ROM
		// is on the card, so in practice this is the ][+
		if (profile.embeddedRom)
		{
			Serial.printf("[rom] using the built-in %s ROM\n", profile.name);
			memcpy(mem.rom, appleIIrom, ROMSIZE);
		}
		else
			Serial.printf("[rom] no system ROM for %s\n", profile.name);
	}

	if (RomLoader::Load(DISKII_ROM, device.disk6.rom, SL6SIZE) != ROM_OK)
		memcpy(device.disk6.rom, diskII, SL6SIZE);
}

bool Apple2Machine::Booting()
{
	DEBUG_PRINTLN("====> BOOTING ...");
	// Maps the slot ROMs as the cards now present them. With no disk in
	// either drive the Disk II hides its PROM, so the machine boots to BASIC.
	mem.Remap();
	cpu.Reset(mem);
	return true;
}

void Apple2Machine::Reset()
{
	mem.Reset();
	cpu.Reboot(mem);
	device.Reset();

	// unset the Power-UP byte
	mem.WriteByte(0x3F4, 0);
	// dirty hack, fix soon... if I understand why
	mem.WriteByte(0x4D, 0xAA);   // Joust crashes if this memory location equals zero
	mem.WriteByte(0xD0, 0xAA);   // Planetoids won't work if this memory location equals zero

	Booting();
}

bool Apple2Machine::Mount(const char* path, int drive)
{
	if (!device.Mount(path, drive))
		return false;
	// machine may have booted disk-less with the Disk II PROM hidden;
	// PR#6 needs it present
	mem.Remap();
	return true;
}

void Apple2Machine::Unmount(int drive)
{
	device.Unmount(drive);
	// no disk left: the card hides its PROM, so a Reset boots to BASIC
	// instead of hanging on an empty drive scan
	mem.Remap();
}

void Apple2Machine::Run(long long cycle)
{
/*
	if (device.resetMachine)
	{
		Reset();
		return;
	}
*/
	device.UpdateInput();
	cpu.Run(mem, cycle);
	while (1)
	{
		if( device.UpdateFloppyDisk() == false )
			break;
		cpu.Run(mem, 1000);
	}
}

void Apple2Machine::Render(VGA *vga, int frame)
{
	// Device draws into the VGA framebuffer itself - no backbuffer, no blit,
	// so an unchanged screen costs nothing beyond the dirty-cell checks.
	device.Render(mem, frame, vga);
}


// Load a DUMP file and resume
void Apple2Machine::LoadMachine(std::string path)
{
}

// Save the entire current state
void Apple2Machine::DumpMachine(std::string path)
{
}

