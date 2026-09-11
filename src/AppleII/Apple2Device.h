/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : Apple2Device.h
 *  Module : Apple II peripherals (interface). Slot cards,
 *           soft-switch and video-mode flags, the video renderer
 *           and the FPS overlay state.
 * ============================================================
*/

#ifndef APPLE2_DEVICE_H
#define APPLE2_DEVICE_H

#include <stdio.h>
#include <string>
#include "AppleVideo.h"
#include "DiskIICard.h"
#include "../Tools/Log.h"

class CPU;	// 6502 cpu
class Memory;
class VGA;


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

	// Video soft switches; AppleVideo renders from them
	bool textMode;
	bool mixedMode;
	bool hires_Mode;
	BYTE videoPage;
	// IIe video switches
	bool col80;                 // 80COL: 80-column text
	bool altCharset;            // ALTCHARSET: MouseText and inverse lowercase
	bool dhires;                // AN3 off ($C05E): double hires, with 80COL and HIRES

	AppleVideo video;

	// The IIe MMU and I/O at $C000-$C01F. Set by Apple2Machine from the profile.
	bool iie;
	// Ctrl+F12: Apple2Machine pulls the RESET line before the next run
	bool resetRequested;


private:
	CPU* cpu;

	// Keyboard input value
	BYTE keyboard;
	// the key behind the latched code, for the IIe's any-key-down at $C010
	int lastVK;

	// Characters typed but not yet latched: the program has not taken the
	// previous one (the strobe is still set).
	static const int KEY_QUEUE_LEN = 16;
	BYTE keyQueue[KEY_QUEUE_LEN];
	int  keyQueueVK[KEY_QUEUE_LEN];
	int  keyHead;
	int  keyCount;

	// Keyboard
	void UpdateKeyBoard();
	// GamePad
	void UpdateGamepad();

	BYTE IIeSwitch(Memory* mem, WORD address, bool WRT);
	bool AnyKeyDown();
	bool ButtonDown(int button);

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
	void InvalidateRenderCache() { video.InvalidateRenderCache(); }
	void InvalidateFpsOverlayRegion() { video.InvalidateFpsOverlayRegion(); }

	BYTE SoftSwitch(Memory* mem, WORD address, BYTE value, bool WRT);
	void PlaySound();
	void Render(Memory& mem, int frame, VGA* vga) { video.Render(mem, *this, frame, vga); }

	void UpdateInput();
};



#endif
