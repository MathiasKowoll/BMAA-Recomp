# Batman: Arkham Asylum GOTY (Feral's Mac port, i386) on Recomp-Framework's mac32 target.
#   ln -s "<...>/Batman Arkham Asylum GOTY.app/Contents/MacOS/Batman Arkham Asylum GOTY" orig.bin
#   make gen && make -j app
# The framework is the user's fork, branch mac32-function-starts (the executable is stripped: its functions come
# from LC_FUNCTION_STARTS).
FW        = ../Recomp-Framework
PYTHON    = .venv/bin/python
GAME_BIN  = orig.bin
TEXT_LO   = c2d0
TEXT_HI   = 290be29
HLE       = hle.txt
APP_NAME  = BMAA
GAME_SRC  = game32.c
include $(FW)/mac32/mac32.mk

.venv:
	python3 -m venv .venv && .venv/bin/pip install -q capstone lief

# ---- The same translated code as the CPU of DecompRecomp's runtime (aot.c) ----
# make dr      build/dr-bmaa: DecompRecomp's loader, bridge and driver (all of its sources but the interpreter),
#              with the translated functions answering its CPU interface.
# make interp  build/dr-interp: the same runtime on DecompRecomp's own interpreter, as the reference.
#
# DecompRecomp is used from its folder and never changed. One thing of it is built differently here, on copies
# made at build time: the guest heap is 1 GB, not 512 MB (four more size classes of the same formula). With its
# interpreter the game never got far enough to need more; with translated code it fills 512 MB while it loads.
DR      = ../DecompRecomp/runtime
DR_C    = guest macho32 vfs objc cxxshim cxxstring carbon glcontext cbtramp cfbridge testgl bridge dr_run gfx libcpura cxxstream
DR_M    = appkit glcore objcinvoke
DR_OBJ  = $(patsubst %,$(BUILD)/dr/%.o,$(DR_C) $(DR_M))
DR_INC  = $(BUILD)/dr-src/include
DR_HDR  = $(BUILD)/dr-src/stamp
DR_FLAGS = -O2 -g -arch arm64 -std=c11 -Wall -Wextra -Wno-unused-parameter -I$(DR_INC)
$(DR_HDR): $(wildcard $(DR)/include/*.h) $(DR)/src/guest.c Makefile
	@mkdir -p $(DR_INC)
	cp $(DR)/include/*.h $(DR_INC)/
	sed -i '' 's/^#define DR_HEAP_SIZE 0x20000000u.*/#define DR_HEAP_SIZE 0x80000000u \/* 2 GB (BMAA-Recomp) *\//' $(DR_INC)/dr_guest.h
	grep -q 'DR_HEAP_SIZE 0x80000000u' $(DR_INC)/dr_guest.h
	sed 's/^#define HEAP_NCLASES 141u/#define HEAP_NCLASES 149u/' $(DR)/src/guest.c > $(BUILD)/dr-src/guest.c
	grep -q 'HEAP_NCLASES 149u' $(BUILD)/dr-src/guest.c
	touch $@
$(BUILD)/dr/guest.o: $(DR_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(DR_FLAGS) -c $(BUILD)/dr-src/guest.c -o $@
$(BUILD)/dr/%.o: $(DR)/src/%.c $(DR_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(DR_FLAGS) -c $< -o $@
$(BUILD)/dr/%.o: $(DR)/src/%.m $(DR_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(DR_FLAGS) -fobjc-arc -c $< -o $@
# The driver calls the bridge through trace.c in both programs, so that their calls can be compared.
$(BUILD)/dr/dr_run.o: $(DR)/src/dr_run.c $(DR_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(DR_FLAGS) -Ddr_bridge_dispatch=traced_dispatch -c $< -o $@
$(BUILD)/dr/trace.o: trace.c $(DR_HDR)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -arch arm64 -Wall -I$(DR_INC) -c trace.c -o $@
$(BUILD)/dr/x86.o: $(DR)/src/x86.c $(DR_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(DR_FLAGS) -c $< -o $@
$(BUILD)/dr-interp: $(DR_OBJ) $(BUILD)/dr/x86.o $(BUILD)/dr/trace.o
	$(CC) -arch arm64 $^ -lobjc -framework Foundation -framework AppKit -framework Metal -framework QuartzCore \
	    -framework ImageIO -framework CoreGraphics -framework CoreText -framework OpenGL -framework Carbon -o $@
interp: $(BUILD)/dr-interp
$(BUILD)/dr/aot.o: aot.c $(M32_HDR) $(DR_HDR)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -arch arm64 -Wall -Wno-unused-parameter -I$(DR_INC) -I$(M32)/runtime -c aot.c -o $@
$(BUILD)/dr/game.o: game.c $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -arch arm64 -Wall -Wno-unused-parameter -I$(M32)/runtime -c game.c -o $@
$(BUILD)/dr/threads.o: threads.c $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -arch arm64 -Wall -Wno-unused-parameter -fblocks -fobjc-arc -x objective-c -I$(M32)/runtime -c threads.c -o $@
$(BUILD)/dr/steam.o: steam.c $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -arch arm64 -Wall -Wno-unused-parameter -I$(M32)/runtime -c steam.c -o $@
$(BUILD)/dr/dbg.o: dbg.m
	$(CC) -O2 -g -arch arm64 -Wall -fobjc-arc -c dbg.m -o $@
$(BUILD)/dr/ct.o: ct.c $(M32_HDR) $(DR_HDR)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -arch arm64 -Wall -Wno-unused-parameter -Wno-deprecated-declarations -I$(DR_INC) -I$(M32)/runtime -c ct.c -o $@
$(BUILD)/dr/eh.o: $(M32)/runtime/eh.c $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -arch arm64 -Wall -Wno-unused-parameter -I$(M32)/runtime -c $(M32)/runtime/eh.c -o $@
$(BUILD)/dr/input.o: input.c $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -arch arm64 -Wall -Wno-unused-parameter -Wno-deprecated-declarations -I$(M32)/runtime -c input.c -o $@
$(BUILD)/dr/display.o: display.c $(M32_HDR) $(DR_HDR)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -arch arm64 -Wall -Wno-unused-parameter -Wno-deprecated-declarations -I$(DR_INC) -I$(M32)/runtime -c display.c -o $@
$(BUILD)/dr/glx.o: glx.c $(M32_HDR) $(DR_HDR)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -arch arm64 -Wall -Wno-unused-parameter -Wno-deprecated-declarations -Wno-\#warnings -I$(DR_INC) -I$(M32)/runtime -c glx.c -o $@
$(BUILD)/dr/gl_gen.c: $(M32)/tools/glgen.py
	@mkdir -p $(dir $@)
	$(PYTHON) $(M32)/tools/glgen.py $@
$(BUILD)/dr/gl_gen.o: $(BUILD)/dr/gl_gen.c $(M32_HDR)
	$(CC) -O2 -g -arch arm64 -Wno-deprecated-declarations -Wno-\#warnings -I$(M32)/runtime -c $(BUILD)/dr/gl_gen.c -o $@
$(BUILD)/dr/fast.o: fast.c $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -arch arm64 -Wall -Wno-unused-parameter -I$(M32)/runtime -c fast.c -o $@
$(BUILD)/dr/dates.o: dates.c $(M32_HDR)
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -arch arm64 -Wall -Wno-unused-parameter -I$(M32)/runtime -c dates.c -o $@
BMAA_OBJ = $(BUILD)/dr/aot.o $(BUILD)/dr/game.o $(BUILD)/dr/trace.o $(BUILD)/dr/threads.o $(BUILD)/dr/steam.o $(BUILD)/dr/dates.o $(BUILD)/dr/dbg.o $(BUILD)/dr/fast.o $(BUILD)/dr/ct.o $(BUILD)/dr/eh.o $(BUILD)/dr/glx.o $(BUILD)/dr/input.o $(BUILD)/dr/display.o $(BUILD)/dr/gl_gen.o
$(BUILD)/dr-bmaa: $(BUILD)/libgame32.a $(DR_OBJ) $(BMAA_OBJ)
	$(CC) -arch arm64 $(DR_OBJ) $(BMAA_OBJ) $(BUILD)/libgame32.a -lobjc -framework Foundation \
	    -framework AppKit -framework Metal -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework OpenGL -framework Carbon -o $@
dr: $(BUILD)/dr-bmaa
.PHONY: dr interp
