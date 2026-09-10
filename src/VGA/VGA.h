/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : VGA.h
 *  Module : Hardware seam between the emulator and FabGL. Thin
 *           wrapper over the global fabgl::VGAController:
 *           dot()/row()/clear() pack RGB222 values straight into
 *           VGA scanline buffers, preserving the two sync bits in
 *           each byte and applying the required x^2 byte swizzle.
 * ============================================================
*/

#ifndef VGA_H
#define VGA_H

#include "fabgl.h"

extern fabgl::VGAController DisplayController;

class VGA
{
public:
	fabgl::Canvas *canvas;

public:
	VGA() : canvas(nullptr) {}
	~VGA() {}

	void setCanvas(fabgl::Canvas *c) {
		canvas = c;
	}

	bool init() { return true; }
	bool start() { return true; }
	bool show() { return true; }

	void dot(int x, int y, uint8_t r, uint8_t g, uint8_t b)
	{
		uint8_t *scanline = DisplayController.getScanline(y);
		if (scanline && x >= 0 && x < 320) {
			int targetX = x ^ 2;
			int rgbVal = ((b >> 6) << 4) | ((g >> 6) << 2) | (r >> 6);
			scanline[targetX] = (scanline[targetX] & 0xC0) | rgbVal;
		}
	}

	void dot(int x, int y, int rgb)
	{
		uint8_t *scanline = DisplayController.getScanline(y);
		if (scanline && x >= 0 && x < 320) {
			int targetX = x ^ 2;
			scanline[targetX] = (scanline[targetX] & 0xC0) | rgb;
		}
	}

	// Scanline pointer for a whole row. Callers blitting a full frame should
	// fetch this once per row instead of paying getScanline() per pixel.
	uint8_t *row(int y)
	{
		return DisplayController.getScanline(y);
	}

	int rgb(uint8_t r, uint8_t g, uint8_t b)
	{
		return ((b >> 6) << 4) | ((g >> 6) << 2) | (r >> 6);
	}

	// Partial-row fill. clear() can memset because it covers whole rows;
	// a sub-row span has to honour the x^2 byte order pixel by pixel.
	void fillRect(int x, int y, int w, int h, int rgb)
	{
		if (x < 0) { w += x; x = 0; }
		if (y < 0) { h += y; y = 0; }
		for (int yy = y; yy < y + h && yy < 200; ++yy) {
			uint8_t *scanline = DisplayController.getScanline(yy);
			if (!scanline)
				continue;
			for (int xx = x; xx < x + w && xx < 320; ++xx) {
				int t = xx ^ 2;
				scanline[t] = (scanline[t] & 0xC0) | rgb;
			}
		}
	}

	void clear(int rgb = 0)
	{
		for (int y = 0; y < 200; ++y) {
			uint8_t *scanline = DisplayController.getScanline(y);
			if (scanline) {
				uint8_t sync = scanline[0] & 0xC0;
				uint8_t fillVal = sync | rgb;
				memset(scanline, fillVal, 320);
			}
		}
	}
};

#endif // VGA_H