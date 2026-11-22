# IVS Studio - TI-Nspire CX (Ndless 4.x SDK). Needs: nspire-gcc, nspire-ld, genzehn, make-prg on PATH.
# Also copy into this folder:  stb_image.h,  libvpx.a  and the  vpx/  headers folder (from github.com/pbfy0/nvid)
EXE = ivstudio
GCC = nspire-gcc
LD = nspire-ld
GENZEHN = genzehn
GCCFLAGS = -Os -Wall -W -marm -I.
LDFLAGS = -lSDL libvpx.a
ZEHNFLAGS = --name "IVS Studio" --compress

all: $(EXE).prg.tns

main.o: main.c
	$(GCC) $(GCCFLAGS) -c main.c -o main.o

$(EXE).elf: main.o
	$(LD) main.o -o $@ $(LDFLAGS)

$(EXE).tns: $(EXE).elf
	$(GENZEHN) --input $^ --output $@ $(ZEHNFLAGS)

$(EXE).prg.tns: $(EXE).tns
	make-prg $^ $@

clean:
	rm -f *.o *.elf *.tns
