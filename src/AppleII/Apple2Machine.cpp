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
 *           + Memory + Apple2Device, copies the embedded Apple II
 *           and Disk II ROM images into memory at boot, and drives
 *           the per-frame cycle budget.
 * ============================================================
*/

#include "Predef.h"
#include "rombios.h"
#include "Apple2Machine.h"
#include "../Tools/Log.h"
#include "../VGA/VGA.h"

Apple2Machine::Apple2Machine()
{
	DEBUG_PRINTLN("Construct Apple2Machine");
}

Apple2Machine::~Apple2Machine()
{

}

void Apple2Machine::InitMachine()
{
	mem.Create();
	device.InsetFloppy();
	// unset the Power-UP byte
	mem.WriteByte(0x3F4, 0);
	cpu.Reset(mem);
	mem.WriteByte(0x4D, 0xAA);   // Just crashes if this memory location equals zero
	mem.WriteByte(0xD0, 0xAA);   // won't work if this memory location equals zero

	device.Create(&cpu);
	mem.device = &device;

	Booting();
	//UploadRom();	
}

// 롬을 내장
bool Apple2Machine::Booting()
{
	DEBUG_PRINTLN("====> BOOTING ...");
	DEBUG_PRINTLN("Load Apple II Rom");
	memcpy(mem.rom, appleIIrom, ROMSIZE);
	if (device.HasFloppy(0) || device.HasFloppy(1))
	{
		DEBUG_PRINTLN("Load Disk II");
		memcpy(mem.sl6, diskII, SL6SIZE);
	}
	else
	{
		DEBUG_PRINTLN("No floppy loaded, clearing Slot 6 ROM for direct BASIC boot");
		memset(mem.sl6, 0, SL6SIZE);
	}
	cpu.Reset(mem);
	return true;
}

// 롬을 파일에서 로딩
bool Apple2Machine::UploadRom()
{
	bool ret = false;

	// load the Apple II+ ROM
	BYTE* rom =(BYTE*)ps_malloc(ROMSIZE);
	FILE* fp = fopen("/apple2.rom", "rb"); 
	if (fp)
	{
#if 0
		fread(rom, ROMSIZE, 1, fp);
		mem.UpLoadProgram(ROMSTART, rom, ROMSIZE);
#else
		fread(mem.rom, ROMSIZE, 1, fp);
#endif
		fclose(fp);
		ret = true;
	}
	free(rom);

	// load Apple II+ / Disk II
	BYTE* disk2 = (BYTE*)ps_malloc(SL6SIZE);
	FILE* disk2fp = fopen("rom/diskII.rom", "rb");
	if (disk2fp)
	{
#if 0
		fread(disk2, SL6SIZE, 1, disk2fp);
		mem.UpLoadProgram(SL6START, disk2, SL6SIZE);
#else
		fread(mem.sl6, SL6SIZE, 1, disk2fp);
#endif
		fclose(disk2fp);
		ret = true;
	}
	free(disk2);

	cpu.Reset(mem);
	return ret;
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
	// machine may have booted disk-less with a cleared slot 6 ROM;
	// PR#6 needs the Disk II PROM present
	memcpy(mem.sl6, diskII, SL6SIZE);
	return true;
}

void Apple2Machine::Unmount(int drive)
{
	device.Unmount(drive);
	// no disk left: clear the PROM so a Reset boots to BASIC
	// instead of hanging on an empty drive scan
	if (!device.HasFloppy(0) && !device.HasFloppy(1))
		memset(mem.sl6, 0, SL6SIZE);
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


// DUMP파일을 로드하여 재개
void Apple2Machine::LoadMachine(std::string path)
{
}

// 현재의 모든 상태를 저장
void Apple2Machine::DumpMachine(std::string path)
{
}

