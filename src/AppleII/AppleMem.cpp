/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : AppleMem.cpp
 *  Module : Apple II memory subsystem. Allocates the RAM / ROM /
 *           slot-6 / Language Card blocks in internal SRAM (never
 *           PSRAM), implements the segmented address map and
 *           Language Card banking, and routes $C000-$C0FF I/O page
 *           accesses to Apple2Device::SoftSwitch.
 * ============================================================
*/


/*
	MOS 6502 CPU Emulator
*/

#include "AppleMem.h"
#include "../Tools/Log.h"
#include <esp_heap_caps.h>

Memory::Memory()
{
	DEBUG_PRINTLN("Construct Memory");
	device = NULL;
}

Memory::~Memory()
{
	Destroy();
}

// The 6502 core hits these blocks on nearly every emulated cycle, so they must
// live in internal SRAM. PSRAM is on a 40MHz SPI bus and the 32K data cache
// cannot hold a 76K working set, so a PSRAM-backed address space stalls the
// interpreter on almost every access.
//
// Plain malloc() is not enough: the Arduino ESP32 core builds with
// CONFIG_SPIRAM_USE_MALLOC, so allocations above the always-internal threshold
// may be served from PSRAM. Ask for MALLOC_CAP_INTERNAL explicitly.
static BYTE* AllocFast(const char* name, size_t size)
{
	BYTE* p = (BYTE*)heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	if (p)
	{
		Serial.printf("[mem] %-4s %6u bytes @ %p (internal)\n", name, (unsigned)size, p);
		return p;
	}

	// Not enough internal RAM left - fall back so we degrade in speed, not function.
	p = (BYTE*)ps_malloc(size);
	Serial.printf("[mem] %-4s %6u bytes @ %p (PSRAM FALLBACK - slow)\n", name, (unsigned)size, p);
	return p;
}

void Memory::Create()
{
	// Allocated hottest-first so that if internal RAM runs short, the least
	// frequently accessed blocks are the ones pushed out to PSRAM.
	ram = AllocFast("ram", RAMSIZE);  // 48K of ram in $000-$BFFF
	rom = AllocFast("rom", ROMSIZE);  // 12K of rom in $D000-$FFFF
	lgc = AllocFast("lgc", LGCSIZE);
	bk2 = AllocFast("bk2", BK2SIZE);
	sl6 = AllocFast("sl6", SL6SIZE);

	Serial.printf("[mem] internal free after alloc: %u bytes\n",
	              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

	Reset();
}

void Memory::Destroy()
{
	free(ram);
	free(rom);
	free(lgc);
	free(bk2);
	free(sl6);
}

void Memory::Reset()
{
	LCWritable = true;
	LCReadable = false;
	LCBank2Enable = true;
	LCPreWriteFlipflop = false;

	memset(ram, 0, RAMSIZE);
	memset(rom, 0, ROMSIZE);
	memset(lgc, 0, LGCSIZE);
	memset(bk2, 0, BK2SIZE);
	memset(sl6, 0, SL6SIZE);
}


BYTE Memory::ReadByte(int address)
{
#if 0
	BYTE v = memory[address];
	if (address == 0xCFFF || ((address & 0xFF00) == 0xC000))
		v = device->SoftSwitch(address, 0, false);
	return v;
#else
	if (address < RAMSIZE)
		return ram[address];                                                        // RAM

	if (address >= ROMSTART) {
		if (!LCReadable)
			return rom[address - ROMSTART];                                           // ROM

		if (LCBank2Enable && (address < 0xE000))
			return bk2[address - BK2START];                                           // BK2

		return lgc[address - LGCSTART];                                             // LC
	}

	if ((address & 0xFF00) == SL6START)
		return sl6[address - SL6START];  // disk][

	if ((address & 0xF000) == 0xC000)
		return (device->SoftSwitch(this, address, 0, false));

	return 0;
#endif
}

void Memory::WriteByte(int address, BYTE value)
{
#if 0

	memory[address] = value;
	if (address == 0xCFFF || ((address & 0xFF00) == 0xC000))
		device->SoftSwitch(address, value, true);
#else
	if (address < RAMSIZE) {
		ram[address] = value;                                                       // RAM
		return;
	}

	if (LCWritable && (address >= ROMSTART)) {
		if (LCBank2Enable && (address < 0xE000)) {
			bk2[address - BK2START] = value;                                          // BK2
			return;
		}
		lgc[address - LGCSTART] = value;                                            // LC
		return;
	}

	if ((address & 0xF000) == 0xC000)
	{
		device->SoftSwitch(this, address, value, true);
		return;
	}
#endif
}

WORD Memory::ReadWord(int addr)
{
	BYTE m0 = ReadByte(addr);
	BYTE m1 = ReadByte(addr + 1);
	WORD w = (m1 << 8) | m0;
	return w;
}

void Memory::WriteWord(WORD value, int addr)
{
	WriteByte(addr, value >> 8);
	WriteByte(addr + 1, value & 0xFF);
}


void Memory::UpLoadToRom(BYTE* code)
{
	memcpy(rom, code, ROMSIZE);
}

void Memory::ResetRam()
{
	memset(ram, 0, RAMSIZE);
}

void Memory::Dump(FILE* fp)
{

}

void Memory::LoadDump(FILE* fp)
{

}