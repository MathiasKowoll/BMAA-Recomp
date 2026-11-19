// Batman: Arkham Asylum GOTY (Feral, com.feralinteractive.bmaa 1.1) on Recomp-Framework's mac32 target.
//
// State: the first milestone only. The host loads the executable, binds its imports and runs its static
// initialisers; the engine is not started yet (game32_start says so), so what a run shows is which imports the
// initialisers call that the runtime does not have.
#include "game32.h"
#include "tables.h"
#include <stdio.h>

const Rt32Game rt32_game = {
    .name = "Batman: Arkham Asylum",
    .app_bundle = "Batman Arkham Asylum GOTY.app",
    .exe_name = "Batman Arkham Asylum GOTY",
    .steam_folder = "Batman Arkham Asylum GOTY",
    .prefs_domain = "com.feralinteractive.bmaa",
    .support_dir = "bmaa-recomp",
    .log_name = "bmaa-recomp",
    .width = 1280, .height = 720,
};

int game32_start(CPU *c) { fprintf(stderr, "[bmaa] static initialisers done; the engine start is not written yet\n"); return 0; }
void game32_window_size(CPU *c, float *w, float *h) { *w = 1280; *h = 720; }
int game32_windowed(CPU *c) { return 1; }
void game32_frame(CPU *c) {}
int game32_quit_requested(CPU *c) { return 1; }
void game32_mouse_move(CPU *c, float x, float y) {}
void game32_mouse_button(CPU *c, int button, int down, int clicks) {}
void game32_wheel(CPU *c, int delta) {}
void game32_key(CPU *c, int down, uint32_t ch, uint32_t mods) {}
void game32_modifier(CPU *c, int which, int down) {}
