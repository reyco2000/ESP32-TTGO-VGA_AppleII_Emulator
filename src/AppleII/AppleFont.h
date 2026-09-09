/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : AppleFont.h
 *  Module : Apple II character generator (interface). Glyph
 *           storage and the RenderFont entry point.
 * ============================================================
*/

#ifndef APPLE2_FONT_H
#define APPLE2_FONT_H

#include "Predef.h"

class VGA;

class AppleFont
{
private:
	unsigned char font[FONT_NUM][FONT_X*FONT_Y];
	unsigned char invfont[FONT_NUM][FONT_X*FONT_Y];
	//unsigned char* read_bmp(const char* fname, int* _w, int* _h);
	unsigned char* read_bmp_memory(char* buffer, int* _w, int* _h);
public:
	AppleFont();
	~AppleFont();

	void Create();
	// Draws straight into the VGA framebuffer. There is no intermediate
	// backbuffer: the framebuffer itself persists between frames, which is
	// what lets the callers' dirty-cell caches skip unchanged glyphs.
	void RenderFont(VGA *vga, int fontnum, int x, int y, bool inv);

};



#endif