#!/bin/bash
# Steam's PLAY button starts this port instead of the original (which no longer runs on current macOS):
# in Steam, the game's Properties > General > Launch Options:
#     "/full/path/to/BMAA-Recomp/steam-play.sh" %command%
# Steam then runs this script with the original's command line as arguments, which are not used. The game is
# Steam's child, so Steam shows it as running and counts the time. Steam's overlay library is built for the
# original's processor and is not loaded into this one.
cd "$(dirname "$0")"
unset DYLD_INSERT_LIBRARIES DYLD_LIBRARY_PATH DYLD_FRAMEWORK_PATH
export BMAA_FPS=${BMAA_FPS-}
exec ./play.sh
