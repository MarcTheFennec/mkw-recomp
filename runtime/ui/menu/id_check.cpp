// ImGui ID conflicts logged to mkw.log instead of ImGui's popup (id_check.h). The count is ImGui's own (ItemAdd in
// imgui.cpp: HoveredIdPreviousFrameItemCount), which it keeps with ConfigDebugHighlightIdConflicts off.
#include "imgui.h"
#include "imgui_internal.h"   // GImGui: HoveredIdPreviousFrame and its item count, HoveredWindow

#include "ui/menu/id_check.h"

extern "C" void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void ui_id_check_init(void) { ImGui::GetIO().ConfigDebugHighlightIdConflicts = false; }

void ui_id_check(const char *where) {
    static ImGuiID seen[16];   // per process: a conflict under a resting mouse would log every frame
    static int n;
    const ImGuiContext &g = *GImGui;
    if (g.HoveredIdPreviousFrameItemCount < 2 || !g.HoveredIdPreviousFrame) return;
    for (int i = 0; i < n; i++)
        if (seen[i] == g.HoveredIdPreviousFrame) return;
    if (n < (int)(sizeof seen / sizeof *seen)) seen[n++] = g.HoveredIdPreviousFrame;
    rt_log("ui: %d widgets share the ImGui ID %#x in window \"%s\" (%s): give each its own (PushID or a ##suffix)",
           g.HoveredIdPreviousFrameItemCount, g.HoveredIdPreviousFrame, g.HoveredWindow ? g.HoveredWindow->Name : "?",
           where);
}
