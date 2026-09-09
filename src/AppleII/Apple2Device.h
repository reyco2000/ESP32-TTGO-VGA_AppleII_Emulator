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
 *           FloppyDrive state, soft-switch and video-mode flags,
 *           render caches and the FPS overlay API.
 * ============================================================
*/

#ifndef APPLE2_DEVICE_H
#define APPLE2_DEVICE_H

#include <stdio.h>
#include <string>
#include "AppleFont.h"
#include "../Tools/Log.h"
#include "../Tools/FileSystem.h"

class CPU;	// 6502 cpu
class Memory;
class VGA;

struct _RECT
{
	int x, y, width, height;
};

// two disk ][ drive units
struct FloppyDrive
{
	char filename[400];
	bool readOnly;
	// nibblelized disk image
	BYTE *data;
	bool motorOn;
	bool writeMode;
	BYTE track;
	WORD nibble;

	FloppyDrive()
	{
		DEBUG_PRINTLN("Construct FloppyDrive");
		data = (BYTE*)ps_malloc(DISKSIZE);
	}

	void Reset()
	{
		memset(data, 0, DISKSIZE);
		memset(filename,0, 400);
		readOnly = false;
		motorOn = false;
		writeMode = false;
		track = 0;
	 	nibble = 0;
	}
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

	// Current floppy disks (1,2)
	int	currentDrive;

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

	////////////////////////////////////////////////

	FloppyDrive disk[2];
	BYTE updatedrive;

	bool phases[2][4];
	// phases states Before
	bool phasesB[2][4];
	// phases states Before Before
	bool phasesBB[2][4];
	// phase index (for both drives)
	int pIdx[2];
	// phase index Before
	int pIdxB[2];
	int halfTrackPos[2];
	BYTE dLatch;

	////////////////////////////////////////////////

	FileSystem filesystem;

	////////////////////////////////////////////////

	// DISK2
	bool InsertFloppy(const char* filename, int drv);
	void stepMotor(WORD address);
	void setDrv(int drv);

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
	bool HasFloppy(int drive) { return disk[drive].filename[0] != '\0'; }
	void Dump(FILE* fp);
	void LoadDump(FILE* fp);

	// Supervisor menu support
	bool supervisorRequested;

	// F2 FPS overlay: toggled from the keyboard, value fed in by the main loop
	bool fpsOverlay;
	int  fpsValue;
	bool Mount(const char* path, int drive);
	void Unmount(int drive);
	void InvalidateRenderCache();
	// marks only the cells under the FPS overlay dirty, so the emulator
	// repaints them instead of trusting a cache the overlay has scribbled on
	void InvalidateFpsOverlayRegion();

	BYTE SoftSwitch(Memory* mem, WORD address, BYTE value, bool WRT);
	void PlaySound();
	void Render( Memory& mem, int frame, VGA* vga);

	void UpdateInput();

	bool UpdateFloppyDisk();
	void InsetFloppy();

	bool GetDiskMotorState();
	std::string GetDiskName(int i);


};



#endif
