/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : DiskIICard.cpp
 *  Module : Disk II controller card. Two drives backed by nibblized
 *           .nib images read from SD: head stepping from the four
 *           phase switches, drive select, motor, and the shift/load
 *           data latch at registers $C-$F. Shows its boot PROM only
 *           while a disk is inserted.
 * ============================================================
*/

#include "DiskIICard.h"

DiskIICard::DiskIICard()
{
	memset(rom, 0, sizeof(rom));
	EjectAll();
	Reset();
}

void DiskIICard::Reset()
{
	updatedrive = 0;
	currentDrive = 0;
	// I/O register
	dLatch = 0;

	memset(phases, 0, sizeof(phases));
	memset(phasesB, 0, sizeof(phasesB));
	memset(phasesBB, 0, sizeof(phasesBB));
	memset(pIdx, 0, sizeof(pIdx));
	memset(pIdxB, 0, sizeof(pIdxB));
	memset(halfTrackPos, 0, sizeof(halfTrackPos));
}

void DiskIICard::EjectAll()
{
	disk[0].Reset();
	disk[1].Reset();
}

// No disk, no PROM: the autostart ROM's slot scan then finds nothing to boot
// and drops into BASIC instead of hanging on an empty drive.
BYTE* DiskIICard::SlotRom()
{
	return (HasFloppy(0) || HasFloppy(1)) ? rom : NULL;
}

BYTE DiskIICard::Io(int reg, BYTE value, bool write)
{
	switch (reg)
	{
		case 0x0: case 0x1: case 0x2: case 0x3:
		case 0x4: case 0x5: case 0x6: case 0x7:
			stepMotor(reg);
			break; // MOVE DRIVE HEAD

		// MOTOR OFF
		case 0x8:
			disk[currentDrive].motorOn = false;
			break;

		// MOTOR ON
		case 0x9:
			disk[currentDrive].motorOn = true;
			break;

		// DRIVE 0
		case 0xA:
			setDrv(0);
			break;
		// DRIVE 1
		case 0xB:
			setDrv(1);
			break;

		// Shift Data Latch
		case 0xC:
		{
			int idx = disk[currentDrive].track * 0x1A00 + disk[currentDrive].nibble;
			if (idx < 0 || idx >= DISKSIZE)
			{
				// head parked beyond the 35 tracks a .nib actually holds:
				// leave the latch alone rather than running off the buffer
			}
			else if (disk[currentDrive].writeMode)
				disk[currentDrive].data[idx] = dLatch;                                  // writing
			else
				dLatch = disk[currentDrive].data[idx];                                  // reading

			// turn floppy of 1 nibble
			disk[currentDrive].nibble = (disk[currentDrive].nibble + 1) % 0x1A00;
		}
		return(dLatch);

		// Load Data Latch
		case 0xD:
			dLatch = value;
			break;

		// latch for READ
		case 0xE:
			disk[currentDrive].writeMode = false;
			return(disk[currentDrive].readOnly ? 0x80 : 0);                                 // check protection

		// latch for WRITE
		case 0xF:
			disk[currentDrive].writeMode = true;
			break;
	}
	return 0;
}

// Apple Disk II
bool DiskIICard::InsertFloppy(const char* filename, int drv)
{
	int readlen = filesystem.ReadFile(filename, disk[drv].data, DISKSIZE);
	if ( readlen != DISKSIZE)
	{
		Serial.printf("Read Floppy Fail : %s\n",filename);
		return false;
	}


	Serial.printf("Read Floppy OK : %s\n",filename);
	sprintf(disk[drv].filename, "%s", filename);

	// For now, proceed in write-disabled mode
	disk[drv].readOnly = false;	// read only
	return true;
}

bool DiskIICard::Mount(const char* path, int drive)
{
	if (drive < 0 || drive > 1)
		return false;
	if (!InsertFloppy(path, drive))
	{
		// short/failed read clobbered the buffer - never leave it half-mounted
		disk[drive].Reset();
		return false;
	}
	return true;
}

void DiskIICard::Unmount(int drive)
{
	if (drive < 0 || drive > 1)
		return;
	disk[drive].Reset();
}

void DiskIICard::stepMotor(WORD address)
{
	address &= 7;
	int phase = address >> 1;

	phasesBB[currentDrive][pIdxB[currentDrive]] = phasesB[currentDrive][pIdxB[currentDrive]];
	phasesB[currentDrive][pIdx[currentDrive]] = phases[currentDrive][pIdx[currentDrive]];
	pIdxB[currentDrive] = pIdx[currentDrive];
	pIdx[currentDrive] = phase;

	if (!(address & 1))
	{                                                         // head not moving (PHASE x OFF)
		phases[currentDrive][phase] = false;
		return;
	}

	if ((phasesBB[currentDrive][(phase + 1) & 3]) && (--halfTrackPos[currentDrive] < 0))      // head is moving in
		halfTrackPos[currentDrive] = 0;

	if ((phasesBB[currentDrive][(phase - 1) & 3]) && (++halfTrackPos[currentDrive] > 140))    // head is moving out
		halfTrackPos[currentDrive] = 140;

	phases[currentDrive][phase] = true;                                                 // update track#
	disk[currentDrive].track = (halfTrackPos[currentDrive] + 1) / 2;
}

void DiskIICard::setDrv(int drv)
{
	disk[drv].motorOn = disk[!drv].motorOn || disk[drv].motorOn;                  // if any of the motors were ON
	disk[!drv].motorOn = false;                                                   // motor of the other drive is set to OFF
	currentDrive = drv;                                                                 // set the current drive
}

// Floppy disk update
bool DiskIICard::UpdateFloppyDisk()
{
	// Done once the floppy motor is off or updatedrive reaches 0
	if (disk[currentDrive].motorOn && ++updatedrive)
		return true;
	else
		return false;
}
