#!/bin/bash
export IDF_PATH=/c/Users/97254/esp/v3.3.1/esp-idf
export PATH="/c/Users/97254/esp/toolchains/xtensa-esp32-elf/bin:$PATH"
cd /c/ESPIDFprojects/RetroESP32-master/Emulators/atari800-odroid-go
make -j4 2>&1 | tail -40
