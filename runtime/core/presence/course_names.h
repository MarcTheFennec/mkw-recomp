// The game's own English course names from the player's RomFS (course_names.c), for the Discord status.
#pragma once
#include <stddef.h>

// The name of the course whose file names say `internal` ("MooMooFarm", a battle's "Volcano") into out: 1 when the
// game's texts have it, 0 otherwise (out untouched). battle: the internal name is a battle course's. Presence thread.
int course_name(const char *internal, int battle, char *out, size_t cap);
