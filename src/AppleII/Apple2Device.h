/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : Apple2Device.h
 *  Module : Apple II peripherals and video (interface).
 *           Slot cards, soft-switch and video-mode flags,
 *           render caches and the FPS overlay API.
 * ============================================================
*/

#ifndef APPLE2_DEVICE_H
#define APPLE2_DEVICE_H

#include <stdio.h>
#include <string>
#include "AppleFont.h"
#include "DiskIICard.h"
#include "../Tools/Log.h"

class CPU;	// 6502 cpu
class Memory;
class VGA;

struct _RECT
{
	int x, y, width, height;
};


// Apple II devices - everything except the CPU and memory
class Apple2Device
{
public:
	bool loaddumpmachine;
	bool dumpMachine;
	bool loadromfile;

	bool resetMachine;
	bool colorMonitor;
	BYTE zoomscale;

	//////////////////////////////////////////////////////////////////////////

	// Peripheral slots 1-7; [0] stays NULL (slot 0 is the Language Card,
	// handled by SoftSwitch). Apple2Machine installs what its profile has.
	Card* slots[8];
	DiskIICard disk6;

	//////////////////////////////////////////////////////////////////////////

	bool textMode;
	bool mixedMode;
	bool hires_Mode;
	BYTE videoPage;
	WORD videoAddress;

	_RECT pixelGR;

	int LoResCache[24][40];
	// text cells already drawn: glyph | 0x100 when drawn inverse, -1 = dirty.
	// TEXT had no cache, so every frame redrew all 960 cells (53,760 pixels).
	int TextCache[24][40];
	int HiResCache[192][40];
	BYTE previousBit[192][40];
	BYTE flashCycle;


private:
	CPU* cpu;
	// Set for the duration of Render(); the drawing helpers below write
	// straight into the VGA framebuffer, so there is no backbuffer.
	VGA* vga;
	//Texture2D renderTexture;
	//Image renderImage;

	AppleFont font;

	// Keyboard input value
	BYTE keyboard;

	// Keyboard
	void UpdateKeyBoard();
	// GamePad
	void UpdateGamepad();

	void ClearScreen();
	void RenderFpsOverlay();
	void DrawPoint(int x, int y, int r, int g, int b);
	void DrawRect(_RECT rect, int r, int g, int b);
	int GetScreenMode();

public:
	Apple2Device();
	~Apple2Device();

	void Create(CPU* cpu);
	void Reset();
	void Dump(FILE* fp);
	void LoadDump(FILE* fp);

	// The Disk II in slot 6, for Apple2Machine and the supervisor
	bool HasFloppy(int drive) { return disk6.HasFloppy(drive); }
	bool Mount(const char* path, int drive) { return disk6.Mount(path, drive); }
	void Unmount(int drive) { disk6.Unmount(drive); }
	bool UpdateFloppyDisk() { return disk6.UpdateFloppyDisk(); }
	void InsetFloppy() { disk6.EjectAll(); }
	bool GetDiskMotorState() { return disk6.MotorOn(); }
	std::string GetDiskName(int i) { return disk6.GetDiskName(i); }

	// Supervisor menu support
	bool supervisorRequested;

	// F2 FPS overlay: toggled from the keyboard, value fed in by the main loop
	bool fpsOverlay;
	int  fpsValue;
	void InvalidateRenderCache();
	// marks only the cells under the FPS overlay dirty, so the emulator
	// repaints them instead of trusting a cache the overlay has scribbled on
	void InvalidateFpsOverlayRegion();

	BYTE SoftSwitch(Memory* mem, WORD address, BYTE value, bool WRT);
	void PlaySound();
	void Render( Memory& mem, int frame, VGA* vga);

	void UpdateInput();
};



#endif
