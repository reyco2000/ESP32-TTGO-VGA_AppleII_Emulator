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