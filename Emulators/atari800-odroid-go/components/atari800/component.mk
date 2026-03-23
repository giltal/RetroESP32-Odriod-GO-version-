#
# Component Makefile for atari800 emulator core
#
# All .cpp source files in this directory are compiled.
# The ESP-IDF build system automatically finds them.
#

CXXFLAGS += -O2 -Wno-unused-variable -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-format -Wno-sign-compare
CFLAGS += -O2 -Wno-unused-variable -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-format -Wno-sign-compare

COMPONENT_ADD_INCLUDEDIRS += .
COMPONENT_SRCDIRS := .

# Exclude files not needed for ESP-IDF build:
# rdevice.cpp - R: device serial/network emulation (requires termios.h / sockets not available)
COMPONENT_OBJEXCLUDE := rdevice.o
