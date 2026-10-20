# PiTrex on MiSTer - PC harness build (run in WSL Debian: make)
#
#   make            patch the library, build hello + ptxplay for the PC
#   make run        hello -> stream.ptxs -> build/frames/hello_*.png
#   make arm        same objects cross-compiled for MiSTer (needs arm-linux-gnueabihf-gcc)

CC      ?= gcc
ARMCC   ?= arm-linux-gnueabihf-gcc
LIB     := build/lib
SRC     := src
CFLAGS  := -O2 -g -DPITREX_MISTER -DPIZERO -DRPI0 -DAVOID_TICKS -DDISABLE_FIQ \
           -DSETTINGS_DIR="\"./settings\"" -I$(LIB) -I$(LIB)/vectrex -I$(SRC) -w
PTX     := $(SRC)/ptx_core.c $(SRC)/ptx_backend_file.c $(SRC)/ptx_backend_mister.c $(SRC)/pitrexio-mister.c
LIBSRC  := $(LIB)/vectrex/vectrexInterface.c $(LIB)/vectrex/osWrapper.c \
           $(LIB)/vectrex/baremetalUtil.c $(LIB)/vectrex/ini.c $(LIB)/pitrex/bcm2835.c
HELLO   := upstream/pitrex/hello_world/hello.c

.PHONY: all patch run arm clean

all: build/hello build/ptxplay

$(LIB)/vectrex/vectrexInterface.c: tools/patch_lib.py
	python3 -I tools/patch_lib.py

build/hello: $(LIB)/vectrex/vectrexInterface.c $(PTX) $(HELLO)
	$(CC) $(CFLAGS) -o $@ $(HELLO) $(PTX) $(LIBSRC) -lm

build/ptxplay: tools/ptxplay.c
	$(CC) -O2 -g -Wall -o $@ $<

run: all
	mkdir -p build/frames
	cd build && PTX_STREAM=stream.ptxs PTX_MAX_FRAMES=12 ./hello
	build/ptxplay build/stream.ptxs build/frames/hello 2 4

arm: $(LIB)/vectrex/vectrexInterface.c
	mkdir -p build/arm
	$(ARMCC) $(CFLAGS) -O2 -static -o build/arm/hello $(HELLO) $(PTX) $(LIBSRC) -lm

clean:
	rm -rf build
