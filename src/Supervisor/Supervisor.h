/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : Supervisor.h
 *  Module : F1 supervisor menu (interface). Menu state, SD
 *           directory listing buffers and the dirty flag that
 *           gates repainting.
 * ============================================================
*/

#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include "../AppleII/Predef.h"
#include "../AppleII/AppleFont.h"
#include "../AppleII/MachineProfile.h"

class Apple2Machine;
class VGA;

#define SUP_MAX_ENTRIES 128
#define SUP_NAME_LEN    64
#define SUP_PATH_LEN    256
#define SUP_LIST_TOP    4    // first text row of the picker lists
#define SUP_LIST_ROWS   16   // visible picker rows
#define SUP_FILES_TOP   8    // first text row of the SD browser, below the buttons
#define SUP_FILES_ROWS  12   // visible SD browser rows

// Buttons above the SD browser: two rows, laid out in Supervisor.cpp
enum SupButton
{
	BTN_RESET, BTN_MACHINE, BTN_KEYBOARD, BTN_SERIAL, BTN_ABOUT,   // top row
	BTN_UNMOUNT1, BTN_UNMOUNT2,                        // bottom row
	BTN_COUNT,
	BTN_NONE = -1                                      // focus is in the SD list
};

// one SD directory entry shown in the browser
struct SupEntry
{
	char name[SUP_NAME_LEN];
	bool isDir;
};

// SD card browser / disk mount menu, shown while emulation is paused
class Supervisor
{
public:
	Supervisor(Apple2Machine* machine);
	~Supervisor();

	bool IsActive() { return active; }
	void Open();
	void Close();
	void Update();               // sole PS2 keyboard reader while active
	void Render(VGA* vga);

private:
	enum Mode { BROWSE, PICK_DRIVE, ABOUT, PICK_MACHINE, PICK_KEYBOARD, PICK_SERIAL, RESTARTING };

	Apple2Machine* machine;
	AppleFont font;
	VGA* vga;                    // valid only for the duration of Render()

	bool active;
	bool dirty;                  // menu needs repainting (set on every keypress)
	bool paletteSet;             // supPalette loaded since Open()
	Mode mode;

	char curPath[SUP_PATH_LEN];
	SupEntry entries[SUP_MAX_ENTRIES];
	int entryCount;
	bool sdError;

	int focusBtn;                // focused SupButton, BTN_NONE when the list has focus
	int lastTop;                 // button to return to when moving up to the top row
	int lastBottom;              // same for the unmount row
	int cursor;                  // index into the virtual list
	int scroll;                  // first visible virtual index
	char status[SCREENTEXT_X + 1];
	char pickPath[SUP_PATH_LEN]; // full path of file awaiting drive choice

	int machineCursor;           // PICK_MACHINE selection, then the model restarting into
	const char* machineMissing[MACHINE_COUNT];   // first missing ROM per model, NULL = bootable
	bool bootNoteShown;          // the boot fallback note goes in the first status only

	int keyboardCursor;          // PICK_KEYBOARD selection

	int serialCursor;            // PICK_SERIAL selection
	bool serialRomMissing;       // no /roms/ssc.rom: the slots cannot be picked

	// virtual list layout: ".." when not at root, then entries[]
	bool AtRoot() { return curPath[1] == '\0'; }
	int VirtualCount();
	void VirtualLabel(int index, char* out, int outlen);

	void ScanDir();
	void EnterDir(const char* name);
	void UpDir();
	void Select();               // Enter pressed on an SD entry
	void Press(int btn);         // Enter pressed on a button
	void MountTo(int drive);
	void MoveCursor(int delta);
	void MoveFocus(int dx, int dy);   // arrow keys in BROWSE mode
	void FocusButton(int btn);
	void ButtonHint(int btn);
	void OpenMachinePicker();
	void ChooseMachine(int id);
	void OpenKeyboardPicker();
	void ChooseKeyboard(int id);
	void OpenSerialPicker();
	void ChooseSerial(int row);

	void SetStatus(const char* msg);

	// Drawing. fg/bg are indices into the menu's palette in Supervisor.cpp;
	// the selection bar is just a swapped pair.
	void DrawText(int col, int row, const char* text, int fg, int bg);
	void DrawRow(int row, const char* text, int fg, int bg);
	void DrawBar(int row, const char* text, int fg, int bg);
	void DrawTextXY(int x, int y, const char* text, int fg, int bg);
	void DrawRule(int row);          // six-band Apple stripe rule
	void DrawChrome(int listTop = SUP_LIST_TOP, int listRows = SUP_LIST_ROWS);
	void DrawDiskSlot(int col, int drive);
	void DrawButton(int btn);
	void RenderBrowse();
	void RenderAbout();
	void RenderMachines();
	void RenderKeyboards();
	void RenderSerial();
	void RenderRestarting();
	int  RowColor(int index, bool selected, int* bg);
	bool ListFocused() { return focusBtn == BTN_NONE; }
};

#endif
