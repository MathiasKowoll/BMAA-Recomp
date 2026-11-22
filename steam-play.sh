#!/bin/bash
# What Steam's PLAY button ends up running: steam-install.sh puts a launcher in the place of the game's program, and
# the launcher starts this script (with Steam's arguments, which are not used). The game is
# Steam's child, so Steam shows it as running and counts the time. Steam's overlay library is built for the
# original's processor and is not loaded into this one.
cd "$(dirname "$0")"
unset DYLD_INSERT_LIBRARIES DYLD_LIBRARY_PATH DYLD_FRAMEWORK_PATH
export BMAA_FPS=${BMAA_FPS-}
exec ./play.sh
