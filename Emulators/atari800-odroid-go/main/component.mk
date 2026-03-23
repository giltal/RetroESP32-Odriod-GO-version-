#
# Main component makefile for atari800-go
#
# This Makefile can be left empty. By default, it will take the sources in the
# src/ directory, compile them and link them into lib(subdirectory_name).a
# in the build directory.
#

CFLAGS += -O2 -Wno-unused-variable -Wno-unused-but-set-variable
CXXFLAGS += -O2
COMPONENT_ADD_INCLUDEDIRS += ../components/atari800 ../components/odroid
COMPONENT_OBJEXCLUDE := emu_atari800_ref.o
