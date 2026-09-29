/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : hires_test.cpp
 *  Module : Host-side hires renderer test: the lookup-table
 *           HiresRender::DrawCell against the per-dot loop
 *           AppleVideo::RenderHires used before it, for every pair
 *           of bytes and both left neighbours.
 *           Driven by tests/host/run-hires-tests.sh.
 * ============================================================
*/

#include <cstdio>
#include <cstring>
#include <string>

#include "HiresRender.h"

static int failures = 0;
static int checks = 0;

static void expect(bool ok, const std::string& what)
{
	checks++;
	if (!ok)
	{
		printf("  FAIL %s\n", what.c_str());
		failures++;
	}
}

// AppleVideo::RenderHires's inner loop as it was before the table, for one
// 14-dot cell. Returns the cell's last dot.
static uint8_t OldCell(uint8_t b0, uint8_t b1, uint8_t pbit, uint8_t* out)
{
	static const uint8_t hiresColor[16] =
	{
		0, 12, 3, 15, 0, 9, 6, 15,
		0, 3, 12, 15, 0, 6, 9, 15
	};
	uint16_t word = ((uint16_t)b1 << 8) | b0;
	uint8_t bits[16];
	for (int bit = 0; bit < 16; bit++)
		bits[bit] = (word >> bit) & 1;

	int x = 0;
	uint8_t colorSet = bits[7] * 4;
	uint8_t even = 0;
	int bit = 0;
	while (bit < 15)
	{
		if (bit == 7)
		{
			colorSet = bits[15] * 4;
			bit++;
		}
		out[x++] = hiresColor[even + colorSet + (bits[bit] << 1) + pbit] * 0x11;
		pbit = bits[bit++];
		even = even ? 0 : 8;
	}
	return pbit;
}

static void TestEveryCell()
{
	HiresRender::Init();
	int bad = 0;
	for (int pbit = 0; pbit < 2; pbit++)
		for (int w = 0; w < 0x10000; w++)
		{
			uint8_t want[14], got[16];
			memset(got, 0xAB, sizeof(got));
			uint8_t wantLast = OldCell(w & 0xFF, w >> 8, pbit, want);
			uint8_t gotLast = HiresRender::DrawCell(w & 0xFF, w >> 8, pbit, got);
			if (memcmp(want, got, 14) != 0 || wantLast != gotLast || got[14] != 0xAB)
			{
				if (bad < 5)
					printf("  mismatch: bytes %02X %02X, left dot %d\n", w & 0xFF, w >> 8, pbit);
				bad++;
			}
		}
	expect(bad == 0, "every cell matches the old renderer, and nothing past its 14 bytes is written");
}

int main()
{
	TestEveryCell();
	if (failures)
	{
		printf("FAIL %d of %d checks\n", failures, checks);
		return 1;
	}
	printf("PASS %d checks\n", checks);
	return 0;
}
