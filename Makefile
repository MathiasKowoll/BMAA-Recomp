# Batman: Arkham Asylum GOTY (Feral's Mac port, i386) on Recomp-Framework's mac32 target.
#   ln -s "<...>/Batman Arkham Asylum GOTY.app/Contents/MacOS/Batman Arkham Asylum GOTY" orig.bin
#   make gen && make -j app
# The framework is the user's fork, branch mac32-function-starts (the executable is stripped: its functions come
# from LC_FUNCTION_STARTS).
FW        = ../Recomp-Framework
PYTHON    = .venv/bin/python
GAME_BIN  = orig.bin
TEXT_LO   = c2d0
TEXT_HI   = 2901549
HLE       = hle.txt
APP_NAME  = BMAA
GAME_SRC  = game32.c
include $(FW)/mac32/mac32.mk

.venv:
	python3 -m venv .venv && .venv/bin/pip install -q capstone lief
