/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : Supervisor.h
 *  Module : F1 supervisor menu (interface). The menu's screens,
 *           and the SD directory listing the disk manager shows.
 * ============================================================
*/

#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include "../AppleII/Predef.h"
#include "../AppleII/MachineProfile.h"

class Apple2Machine;

#define SUP_MAX_ENTRIES 256
#define SUP_NAME_LEN    64
#define SUP_PATH_LEN    256

// one SD directory entry shown in the disk manager
struct SupEntry
{
	char name[SUP_NAME_LEN];
	bool isDir;
};

// Tiled menu over the paused Apple picture: disks, setup, reset, about
class Supervisor
{
public:
	Supervisor(Apple2Machine* machine);

	void Open() { requested = true; }    // the menu starts at the next frame
	bool Requested() { return requested; }
	void Run();                          // returns when the menu is closed

private:
	Apple2Machine* machine;
	bool requested;
	bool bootNoteShown;                  // the boot fallback note is said once

	int  MainMenu(int selected);
	void DrawMain(int selected);
	void AboutScreen();
	void DiskMenu();
	void SetupMenu();
};

#endif
