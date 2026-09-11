/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : Arduino.h (host shim)
 *  Module : Stand-in for the Arduino core so the emulation core's
 *           headers compile under g++ on the host. Provides only
 *           what those headers touch: Serial logging and delay().
 *           Never part of the firmware build.
 * ============================================================
*/

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>

struct HostSerial
{
	void print(const char* s) { fputs(s, stdout); }
	void println(const char* s = "") { puts(s); }
	void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
	{
		va_list ap;
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
	}
};

static HostSerial Serial;

inline void delay(unsigned long) {}

// ESP32 placement attribute (code in internal RAM); meaningless on the host
#define IRAM_ATTR
