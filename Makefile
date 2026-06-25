# Cross-platform Makefile for the HEIC -> PDF converters.
#
# Dependencies:
#   macOS:    brew install libheif jpeg
#   Windows:  install MSYS2 (https://www.msys2.org), then in a UCRT64 shell:
#               pacman -S mingw-w64-ucrt-x86_64-gcc \
#                         mingw-w64-ucrt-x86_64-libheif \
#                         mingw-w64-ucrt-x86_64-libjpeg-turbo
#             (use mingw-w64-x86_64-* packages in a MINGW64 shell instead)
#   Linux:    sudo apt install build-essential libheif-dev libjpeg-dev
#
# Targets:
#   make                    build both converters (dynamic linking)
#   make heic_converter     single-threaded C version
#   make heic_converter_mt  multithreaded C++ version
#   make static             portable heic_converter_mt with statically linked
#                           libheif/libde265/libjpeg (see scripts/build_static.sh)
#   make clean              remove built binaries

CC  := gcc
CXX := g++

CFLAGS   := -O2 -Wall
CXXFLAGS := -std=c++17 -O2 -Wall
LDLIBS   := -lheif -ljpeg

THREADFLAGS := -pthread

# Detect Windows both from cmd.exe (OS=Windows_NT) and from
# Git Bash / MSYS2 shells (uname reports MINGW.../MSYS.../CYGWIN...).
ifeq ($(OS),Windows_NT)
    WINDOWS := 1
else
    UNAME_S := $(shell uname -s)
    ifneq (,$(findstring MINGW,$(UNAME_S))$(findstring MSYS,$(UNAME_S))$(findstring CYGWIN,$(UNAME_S)))
        WINDOWS := 1
    endif
endif

ifeq ($(WINDOWS),1)
    # MSYS2 / MinGW-w64: libheif and libjpeg-turbo from pacman live on the
    # default include/library paths, so no extra -I/-L flags are needed.
    EXE := .exe
else
    EXE :=
    ifeq ($(UNAME_S),Darwin)
        # Homebrew prefix differs between Apple Silicon (/opt/homebrew)
        # and Intel (/usr/local); ask brew when available.
        BREW_PREFIX ?= $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
        INCLUDES := -I$(BREW_PREFIX)/include -I$(BREW_PREFIX)/opt/jpeg/include
        LIBPATHS := -L$(BREW_PREFIX)/lib -L$(BREW_PREFIX)/opt/jpeg/lib
    endif
endif

TARGETS := heic_converter$(EXE) heic_converter_mt$(EXE)

all: $(TARGETS)

heic_converter$(EXE): heic_converter.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBPATHS) $(LDLIBS)

heic_converter_mt$(EXE): heic_converter_mt.cpp
	$(CXX) $(CXXFLAGS) $(THREADFLAGS) $(INCLUDES) -o $@ $< $(LIBPATHS) $(LDLIBS)

static:
	./scripts/build_static.sh

clean:
	rm -f heic_converter heic_converter.exe heic_converter_mt heic_converter_mt.exe
	rm -rf build

.PHONY: all static clean
