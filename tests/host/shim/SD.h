/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : SD.h (host shim)
 *  Module : Stand-in for the ESP32 SD library. The host tests never
 *           touch a card; this only has to satisfy the core's headers
 *           (Tools/FileSystem.h) at compile time. Every open fails.
 * ============================================================
*/

#pragma once

#include "Arduino.h"

class File
{
public:
	explicit operator bool() const { return false; }
	int read(unsigned char*, int) { return -1; }
	size_t size() { return 0; }
	void close() {}
	bool isDirectory() { return false; }
	File openNextFile() { return File(); }
	const char* name() { return ""; }
};

struct HostSD
{
	File open(const char*) { return File(); }
	void end() {}
};

static HostSD SD;
