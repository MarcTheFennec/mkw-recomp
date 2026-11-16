// The start screen's look (launcher_look.h): its ImGui style and the colours of the settings rows on its Settings page.
// Only the start screen uses it; the in-game menu keeps menu_look_style and the rows' own colours.
#include "ui/launcher/launcher_look.h"

// Every colour ImGui draws by itself is set here, the scrollbar, the text selection and the progress bar too: a default
// left in one corner reads as unfinished.
void launcher_look_style(ImGuiStyle &s) {
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 4; s.FrameRounding = 3; s.GrabRounding = 3; s.TabRounding = 3; s.ChildRounding = 3;
    s.PopupRounding = 3; s.ScrollbarRounding = 3;
    s.WindowPadding = ImVec2(32, 26); s.FramePadding = ImVec2(18, 9); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 1; s.ScrollbarSize = 12; s.TabBarBorderSize = 1; s.TabBarOverlineSize = 2;
    ImVec4 *c = s.Colors;
    auto rgb = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
    ImVec4 accent = rgb(LOOK_ACCENT), raised = rgb(LOOK_RAISED), hover = rgb(LOOK_HOVER),
           pressed = rgb(IM_COL32(64, 72, 90, 255)), clear(0, 0, 0, 0);
    c[ImGuiCol_WindowBg] = rgb(LOOK_PANEL);
    c[ImGuiCol_PopupBg] = rgb(LOOK_PANEL);
    c[ImGuiCol_ChildBg] = clear;
    c[ImGuiCol_Border] = rgb(LOOK_LINE);
    c[ImGuiCol_Text] = rgb(LOOK_TEXT);
    c[ImGuiCol_TextDisabled] = rgb(LOOK_DIM);
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_Button] = raised;
    c[ImGuiCol_ButtonHovered] = hover;
    c[ImGuiCol_ButtonActive] = pressed;
    c[ImGuiCol_FrameBg] = raised;
    c[ImGuiCol_FrameBgHovered] = hover;
    c[ImGuiCol_FrameBgActive] = pressed;
    c[ImGuiCol_Header] = raised;
    c[ImGuiCol_HeaderHovered] = hover;
    c[ImGuiCol_HeaderActive] = pressed;
    c[ImGuiCol_Tab] = clear;
    c[ImGuiCol_TabHovered] = hover;
    c[ImGuiCol_TabSelected] = raised;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = clear;
    c[ImGuiCol_TabDimmedSelected] = raised;
    c[ImGuiCol_TabDimmedSelectedOverline] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = rgb(LOOK_ACCENT_HOT);
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_ScrollbarBg] = clear;
    c[ImGuiCol_ScrollbarGrab] = hover;
    c[ImGuiCol_ScrollbarGrabHovered] = pressed;
    c[ImGuiCol_ScrollbarGrabActive] = rgb(IM_COL32(84, 94, 114, 255));
    c[ImGuiCol_NavCursor] = rgb(LOOK_FOCUS);
    c[ImGuiCol_Separator] = rgb(LOOK_LINE);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.60f);
}

// The selection is a white border: the blue stays for the one main action and for progress.
const OptLook launcher_look_rows = {
    /* select */ LOOK_FOCUS,
    /* select_off */ IM_COL32(255, 255, 255, 110),
    /* divider */ LOOK_LINE,
    /* dim */ LOOK_DIM,
    /* off */ LOOK_OFF,
    /* arrow */ LOOK_DIM,
    /* label */ LOOK_TEXT,
    /* value */ LOOK_TEXT,
    /* value_default */ LOOK_HEADING,
    /* note */ LOOK_WARN,
    /* note_off */ LOOK_WARN,
    /* fill */ LOOK_ROW,
    /* fill_hot */ LOOK_ROW_HOT,
    /* border */ IM_COL32(255, 255, 255, 24),
    /* border_off */ IM_COL32(255, 255, 255, 10),
    /* cell_off */ IM_COL32(255, 255, 255, 10),
    /* group */ LOOK_HEADING,
    /* warn */ LOOK_WARN,
    /* radius */ 3,
    /* quiet */ true,
    /* changed */ LOOK_ACCENT_HOT,
};
