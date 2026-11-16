// ImGui ID conflicts (two widgets with the same ID in one window) logged instead of shown to the player.
#pragma once

// After CreateContext: turns off ImGui's red "Programmer error: conflicting ID" popup, which players took for a fault
// of the game (#437, 16/11/2026). The conflicts are still counted and ui_id_check reports them.
void ui_id_check_init(void);

// Just before ImGui::NewFrame (which resets the count): logs, once per ID, a conflict found under the mouse last frame
// with its window and `where` (the page), so the next mkw.log names the screen to fix.
void ui_id_check(const char *where);
