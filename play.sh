#!/bin/bash
# Play: the game as Steam would start it (Steam must be running), with no time limit, PLAY pressed in the launcher,
# and a frame counter on screen (BMAA_FPS= turns it off;
# the Metal performance HUD is asked for too, but the game draws through OpenGL and it has no layer to show on). The log goes to dr-run.log. Extra arguments for the game: DR_ARGS="...".
cd "$(dirname "$0")"
export DR_DIALOGO=${DR_DIALOGO:-1} SteamAppId=35140 SteamGameId=35140 BMAA_AUTOPLAY=1 MTL_HUD_ENABLED=${MTL_HUD_ENABLED:-1} BMAA_FPS=${BMAA_FPS-1}
# The movies play (bink.c). DR_ARGS="-nomovie -nostartupmovies" starts without them, for a quicker test.
export DR_ARGS="${DR_ARGS-}" LAUNCHER=1
exec ./dr-try.sh 1000000
