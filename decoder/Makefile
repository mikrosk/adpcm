# APM decoder: apmdec (Linux, writes <input>_decoded.wav)
# and apmplay.ttp (Atari, plays through the DMA sound, decoded by the CPU
# or by the DSP).

JANSEN			= ../adpcm

CC				= gcc
CFLAGS			= -O2 -Wall

ATARI_CC		= m68k-atari-mintelf-gcc
ATARI_CFLAGS	= -O2 -Wall -m68020-60 -fomit-frame-pointer

# Jansen's adpcm.c uses K&R function definitions
JANSEN_CFLAGS	= -std=gnu89

AS56			= asm56000
CLDLOD			= cldlod
LOD2P56			= lod2p56
XXD				= xxd

all: apmdec apmplay.ttp

apmdec: main-linux.o adpcm-linux.o
	$(CC) -o $@ $^

main-linux.o: main.c $(JANSEN)/adpcm.h
	$(CC) $(CFLAGS) -I$(JANSEN) -c -o $@ $<

adpcm-linux.o: $(JANSEN)/adpcm.c $(JANSEN)/adpcm.h
	$(CC) -O2 $(JANSEN_CFLAGS) -c -o $@ $<

apmplay.ttp: main-atari.o adpcm-atari.o
	$(ATARI_CC) $(ATARI_CFLAGS) -s -o $@ $^

main-atari.o: main.c $(JANSEN)/adpcm.h dspdec.p56.h
	$(ATARI_CC) $(ATARI_CFLAGS) -I$(JANSEN) -c -o $@ $<

adpcm-atari.o: $(JANSEN)/adpcm.c $(JANSEN)/adpcm.h
	$(ATARI_CC) -O2 -m68020-60 -fomit-frame-pointer $(JANSEN_CFLAGS) -c -o $@ $<

dspdec.cld: dspdec.asm
	$(AS56) -a -b$@ $<

dspdec.lod: dspdec.cld
	$(CLDLOD) $< > $@

dspdec.p56: dspdec.lod
	$(LOD2P56) $< $@

dspdec.p56.h: dspdec.p56
	$(XXD) -i $< > $@

clean:
	rm -f *.o apmdec apmplay.ttp dspdec.cld dspdec.lod dspdec.p56 dspdec.p56.h

.PHONY: all clean
