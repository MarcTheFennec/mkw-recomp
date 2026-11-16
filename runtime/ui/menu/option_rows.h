// Settings pages in two panes (ui/menu/option_rows.cpp): option rows on the left, the selected (controller) or hovered (mouse)
// row's description on the right (whichever the player used last: the mouse's row or the cursor's, never both). Each
// row shows a setting of the cfg table (cfg.h) and changes it through cfg_set: A or a click steps it (toggle, next
// value), left and right step it. A row marks the default value, says "after restart" when the setting is read at start
// (the in-game menu; the start screen marks a changed value instead, OptLook::quiet), and is greyed out with a note
// when an environment variable fixes it.
//
//   opt_begin("##screen", scale);
//   opt_toggle("MKW_FULLSCREEN", UI_MENU_FULLSCREEN, UI_MENU_FULLSCREEN_DESC);
//   opt_end();
//
// key: the setting's MKW_ name, which must have a cfg_meta row. Present path only (ImGui).
#pragma once
#include "imgui.h"

extern "C" {
#include "ui_text.h"
}

// The rows' colours and corner radius. The in-game menu's are the default; the start screen draws the same rows in its
// own palette (ui/launcher/launcher_look.h).
struct OptLook {
    ImU32 select, select_off;   // the border of the row under the cursor, of one greyed out; group headings: group
    ImU32 divider;              // the line between the rows and the description
    ImU32 dim, off;             // descriptions and header cells; a greyed-out row's text
    ImU32 arrow;                // a choice's arrows at rest
    ImU32 label, value, value_default;
    ImU32 note, note_off;       // the note under a value, under a greyed-out row's label
    ImU32 fill, fill_hot;       // a row's fill at rest, under the cursor
    ImU32 border, border_off;   // a row's border at rest, greyed out
    ImU32 cell_off;             // the border of a binding cell that takes no input
    ImU32 group, warn;          // opt_group's heading, opt_text's warning
    float radius;               // a row's corners, at 1080 lines
    // The start screen: no "default" or "after restart" notes (everything there applies when the game starts); a value
    // that is not the default is drawn bright with a dot of changed after its label, a default one dim.
    bool quiet;
    ImU32 changed;
};
// The rows' look from now on; NULL: the in-game menu's. Kept by pointer: l must outlive the rows drawn with it.
void opt_look(const OptLook *l);

void opt_begin(const char *id, float scale);
void opt_end(void);
// The height the next pages leave free under their panes (the menu's bottom row).
void opt_reserve_bottom(float h);
// The next page's first row takes the controller's cursor (opening the menu, switching tabs).
void opt_focus_first(void);
// The row of key (or button id) takes the cursor, scrolled into view, when it is next drawn.
void opt_focus_key(const char *key);
// The next row is greyed out like one the environment fixes, with note under its label and in its description.
void opt_disable_next(UiTextId note);
// The rows from now on hang indented under the one before (a preset's rows), until opt_indent(false) or the page's end.
void opt_indent(bool on);

// Page text that is not a row: a group heading in the accent colour, a wrapped paragraph (warn: the warning colour).
void opt_group(UiTextId title);
void opt_text(UiTextId text, bool warn = false);

// The return value of every row: true when it changed the setting this frame.
// inverted: the row shows "on" while the key is 0 (a setting whose key names the opposite).
bool opt_toggle(const char *key, UiTextId label, UiTextId desc, bool inverted = false);
// One name per value of the key's cfg_meta values list, in its order. When the key has no fixed default (an empty
// def), names[0] is the automatic choice (the key unset) and names[1..] follow the values. note: a short line under
// the value in place of the "default" mark (NULL: none).
bool opt_choice(const char *key, UiTextId label, const UiTextId *names, int n, UiTextId desc,
                const char *note = nullptr);
// A choice over a list the caller builds (devices, window sizes): values[i] is written, names[i] shown; "" means the key
// unset. cur: the index in effect, -1 to find cfg_value(key) in values (no match: the value shown as it is).
bool opt_choice_list(const char *key, UiTextId label, const char *const *values, const char *const *names, int n,
                     UiTextId desc, int cur = -1, const char *note = nullptr);
// A row that runs an action: true on A or a click. id: unique on the page and not a setting ("##copy_info"); value:
// shown on the right ("" none).
bool opt_button(const char *id, UiTextId label, UiTextId desc, const char *value = "");
// lo..hi in steps of step (left and right held for a second go ten times as fast); fmt formats the value ("%.0f %%").
bool opt_slider(const char *key, UiTextId label, float lo, float hi, UiTextId desc, float step = 1,
                const char *fmt = "%g");

// Binding rows (remapping on the Controls tab): a label and three cells, each selectable on its own (left / right move between them).
// opt_bind_header: the column titles above them. opt_bind_row: key is the setting's name without MKW_ (BIND_A; the
// description names it when the environment sets it); a disabled cell shows its text greyed and takes no cursor; note
// (NULL none) follows the description. Returns the cell A / Enter / a click activated (0..2), 10 + the cell Delete,
// Backspace or X cleared, or -1. opt_focus_key("<key>#<cell>") puts the cursor on a cell.
void opt_bind_header(UiTextId c0, UiTextId c1, UiTextId c2);
void opt_test_scroll(float fraction);   // test hooks: every page scrolled to that fraction of its height
void opt_bind_icon(int cell, int pad, int kind);   // before opt_bind_row: cell shows that pad button's icon
int opt_bind_row(const char *key, const char *label, UiTextId desc, const char *const cells[3], const bool enabled[3],
                 const char *note = nullptr);

// mkw.ini is written a moment after the last change (a held slider would rewrite it every frame) and when the menu
// closes. opt_save_tick: on every frame the menu lays out; force: write now if anything is pending. 1 while a write is
// pending.
int opt_save_tick(int force);
// A change made outside a row (Reset, F10): mkw.ini is written as after a row's change.
void opt_save_soon(void);
