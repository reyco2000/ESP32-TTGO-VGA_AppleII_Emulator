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