# runtime/ui/menu

The in-game menu (F1) and what it shares with the start screen: look, settings tabs and rows, control remapping.

| File | What it is |
|---|---|
| `menu.cpp`, `menu.h` | Menu core: ImGui state, event queue, controller navigation, Vulkan back end and pages; no window system |
| `menu_notice.cpp`, `menu_notice.h` | `menu_notice`: a timed line at the top of the picture, menu open or not (missing controllers) |
| `menu_sdl.cpp` | Event adapter for the SDL3 window |
| `settings_tabs.cpp`, `settings_tabs.h` | The settings tabs (bar and rows of each tab), shared by the menu and the start screen |
| `menu_look.cpp`, `menu_look.h` | Colours and sizes of the menu, and the fonts it shares with the start screen (embedded Roboto) |
| `option_rows.cpp`, `option_rows.h` | Two-pane settings pages: rows of the `cfg.h` table on the left, description on the right (from SavageWindRecomp, MIT); their colours are the menu's unless a caller passes its own (`opt_look`) |
| `binds_page.cpp`, `binds_page.h` | Remapping rows of the controls tab (`input_map.h`, `MKW_BIND_*` lines) |
| `id_check.cpp`, `id_check.h` | ImGui ID conflicts written to `mkw.log` (window and page) instead of ImGui's red popup, for the menu and the start screen |
