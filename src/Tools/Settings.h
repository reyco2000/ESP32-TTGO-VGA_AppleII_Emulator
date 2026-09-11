/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : Settings.h
 *  Module : Persistent user settings in the ESP32 NVS partition,
 *           via the Arduino Preferences library. Holds the selected
 *           machine model and the two mounted disk paths, so that a
 *           machine switch - which restarts the ESP32 - comes back
 *           up as the chosen model with the same disks mounted.
 * ============================================================
*/

#pragma once

#include <Arduino.h>
#include <Preferences.h>

class Settings
{
public:
	// Machine model id (MachineProfile.h). Returns def when nothing has been
	// saved yet; the caller also treats an id it does not know as def.
	static uint8_t LoadMachine(uint8_t def)
	{
		Preferences p;
		if (!p.begin(NS, true))            // read-only open fails on first boot
			return def;
		uint8_t id = p.getUChar("machine", def);
		p.end();
		return id;
	}

	static void SaveMachine(uint8_t id)
	{
		Preferences p;
		p.begin(NS, false);
		p.putUChar("machine", id);
		p.end();
	}

	// Mounted disk image path for drive 0 or 1, "" when the drive is empty.
	static String LoadDisk(int drive)
	{
		Preferences p;
		if (!p.begin(NS, true))
			return String();
		String path = p.getString(DiskKey(drive), "");
		p.end();
		return path;
	}

	static void SaveDisk(int drive, const char* path)
	{
		Preferences p;
		p.begin(NS, false);
		p.putString(DiskKey(drive), path ? path : "");
		p.end();
	}

private:
	static constexpr const char* NS = "apple2";
	static const char* DiskKey(int drive) { return drive == 0 ? "d1" : "d2"; }
};
