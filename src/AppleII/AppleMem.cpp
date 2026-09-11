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
 *           Language Card blocks and the page tables in internal
 *           SRAM (never PSRAM), rebuilds the page tables on bank
 *           switches, maps the slot cards' ROMs at $Cn00, and routes
 *           the rest of $C000-$CFFF to Apple2Device::SoftSwitch.
 * ============================================================
*/

#include "AppleMem.h"
#include "../Tools/Log.h"
#include <esp_heap_caps.h>

// Writes to ROM land here and are never read back. A sink rather than a
// NULL page keeps the write fast path free of any ROM check.
static BYTE romSink[256];

Memory::Memory()
{
	DEBUG_PRINTLN("Construct Memory");
	device = NULL;
	ram = rom = lgc = bk2 = NULL;
	readPage = writePage = NULL;
	romSize = 0;
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

void Memory::Create(int size)
{
	romSize = size;

	// Allocated hottest-first so that if internal RAM runs short, the least
	// frequently accessed blocks are the ones pushed out to PSRAM. The page
	// tables are read on every single access.
	readPage  = (BYTE**)AllocFast("rdpg", 256 * sizeof(BYTE*));
	writePage = (BYTE**)AllocFast("wrpg", 256 * sizeof(BYTE*));
	ram = AllocFast("ram", RAMSIZE);  // 48K of ram in $000-$BFFF
	rom = AllocFast("rom", romSize);  // 12K or 16K of system rom
	lgc = AllocFast("lgc", LGCSIZE);
	bk2 = AllocFast("bk2", BK2SIZE);

	Serial.printf("[mem] internal free after alloc: %u bytes\n",
	              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

	memset(rom, 0, romSize);          // filled by Apple2Machine::LoadRoms
	Reset();
}

void Memory::Destroy()
{
	free(readPage);
	free(writePage);
	free(ram);
	free(rom);
	free(lgc);
	free(bk2);
	readPage = writePage = NULL;
	ram = rom = lgc = bk2 = NULL;
}

void Memory::Reset()
{
	LCWritable = true;
	LCReadable = false;
	LCBank2Enable = true;
	LCPreWriteFlipflop = false;

	// The system ROM is loaded once at boot and survives resets.
	memset(ram, 0, RAMSIZE);
	memset(lgc, 0, LGCSIZE);
	memset(bk2, 0, BK2SIZE);

	Remap();
}

void Memory::Remap()
{
	for (int p = 0x00; p < 0xC0; p++)
		readPage[p] = writePage[p] = ram + (p << 8);

	// $C000-$CFFF is soft switches and peripheral ROM, all on the slow path
	// except the slot cards' $Cn00 ROMs, which read as plain memory. Before
	// the device is wired up (during Create) there are no cards yet.
	for (int p = 0xC0; p < 0xD0; p++)
		readPage[p] = writePage[p] = NULL;
	if (device)
		for (int slot = 1; slot < 8; slot++)
			if (device->slots[slot])
				readPage[0xC0 + slot] = device->slots[slot]->SlotRom();

	RemapLanguageCard();
}

// $D000-$FFFF reads ROM, or Language Card RAM when it is read-enabled; writes
// reach the Language Card only when it is write-enabled. Bank 2 replaces bank
// 1 in $D000-$DFFF.
void Memory::RemapLanguageCard()
{
	BYTE* romD000 = rom + romSize - 0x3000;
	for (int p = 0xD0; p < 0x100; p++)
	{
		int off = (p - 0xD0) << 8;
		BYTE* lc = (LCBank2Enable && p < 0xE0) ? bk2 + off : lgc + off;
		readPage[p]  = LCReadable ? lc : romD000 + off;
		writePage[p] = LCWritable ? lc : romSink;
	}
}

// The CPU's only way into memory. A NULL page means $C000-$CFFF: soft
// switches and peripheral ROM, handled by the device. In IRAM because it
// runs for every emulated memory access, and IRAM cannot miss the flash
// cache that the rest of the interpreter competes for.
BYTE IRAM_ATTR Memory::ReadByte(int address)
{
	BYTE* page = readPage[(address >> 8) & 0xFF];
	if (page)
		return page[address & 0xFF];
	if ((address & 0xF000) == 0xC000)
		return device->SoftSwitch(this, address, 0, false);
	return 0;
}

void IRAM_ATTR Memory::WriteByte(int address, BYTE value)
{
	BYTE* page = writePage[(address >> 8) & 0xFF];
	if (page)
		page[address & 0xFF] = value;
	else if ((address & 0xF000) == 0xC000)
		device->SoftSwitch(this, address, value, true);
}

WORD Memory::ReadWord(int addr)
{
	BYTE m0 = ReadByte(addr);
	BYTE m1 = ReadByte(addr + 1);
	WORD w = (m1 << 8) | m0;
	return w;
}

// little-endian, the mirror of ReadWord
void Memory::WriteWord(WORD value, int addr)
{
	WriteByte(addr, value & 0xFF);
	WriteByte(addr + 1, value >> 8);
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
