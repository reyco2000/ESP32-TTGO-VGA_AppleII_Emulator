/*
 * ============================================================
 *        APPLE II Emulator for ESP32-TTGO-VGA
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
 *   Based on codesafe/ESP32-VGA_AppleII_Emulator , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : FileSystem.h
 *  Module : Small SD-card helper used to open and read .nib disk
 *           images from the emulated floppy drives.
 * ============================================================
*/

#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "Log.h"

class FileSystem
{
public :
     FileSystem()
     {
        DEBUG_PRINTLN("Construct FileSystem");
     }

    ~FileSystem()
    {
        SD.end();
    }

    int ReadFile(const char *path, unsigned char *buffer, int len)
    {
        File file = SD.open(path);
        if(!file)
        {
            Serial.printf("- failed to open file for reading: %s\n", path);
            return -1;
        }

        int readlen = file.read(buffer, len);
        Serial.printf("- read from file : %d\n", readlen);
        file.close();

        return readlen;
    }
};