#pragma once
// Classic Steam's dark olive, shared by the settings window (overlay.cpp) and the launcher (launcher/win/screen.cpp):
// every colour here, so the two stay one look
#include <imgui.h>

namespace classic {

constexpr ImU32 kWindow = IM_COL32(62, 70, 55, 255), kSide = IM_COL32(46, 52, 41, 255), kHead = IM_COL32(76, 88, 68, 255),
                kField = IM_COL32(40, 45, 35, 255), kHover = IM_COL32(76, 88, 68, 255), kSelect = IM_COL32(149, 136, 49, 255),
                kAccent = IM_COL32(196, 181, 80, 255), kText = IM_COL32(216, 222, 211, 255), kDim = IM_COL32(160, 170, 149, 255),
                kWhite = IM_COL32(255, 255, 255, 255), kLight = IM_COL32(136, 145, 128, 255), kDark = IM_COL32(41, 44, 33, 255),
                kGood = IM_COL32(150, 200, 120, 255), kBad = IM_COL32(235, 130, 105, 255);

inline ImVec4 Col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// the style's colours and square, etched edges; sizes are the caller's
inline void Apply(ImGuiStyle& s) {
    s.WindowBorderSize = s.PopupBorderSize = s.FrameBorderSize = 1;
    s.ChildBorderSize = 0;
    s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0;
    ImVec4* c = s.Colors;
    ImGui::StyleColorsDark(&s);
    c[ImGuiCol_Text] = Col(kText);
    c[ImGuiCol_TextDisabled] = Col(kDim);
    c[ImGuiCol_WindowBg] = Col(kWindow);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = Col(kSide);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
    c[ImGuiCol_Border] = Col(kDark);
    c[ImGuiCol_BorderShadow] = ImVec4(Col(kLight).x, Col(kLight).y, Col(kLight).z, 0.35f);
    c[ImGuiCol_FrameBg] = Col(kField);
    c[ImGuiCol_FrameBgHovered] = Col(kField);
    c[ImGuiCol_FrameBgActive] = Col(kField);
    c[ImGuiCol_CheckMark] = Col(kAccent);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = Col(kHead);
    c[ImGuiCol_ScrollbarBg] = Col(kField);
    c[ImGuiCol_ScrollbarGrab] = Col(kHead);
    c[ImGuiCol_ScrollbarGrabHovered] = c[ImGuiCol_ScrollbarGrabActive] = Col(kLight);
    c[ImGuiCol_Button] = Col(kHead);
    c[ImGuiCol_ButtonHovered] = Col(kLight);
    c[ImGuiCol_ButtonActive] = c[ImGuiCol_Header] = c[ImGuiCol_HeaderHovered] = c[ImGuiCol_HeaderActive] = Col(kSelect);
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = Col(kDark);
    c[ImGuiCol_ResizeGrip] = Col(kHead);
    c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_ResizeGripActive] = Col(kAccent);
    c[ImGuiCol_PlotHistogram] = c[ImGuiCol_PlotHistogramHovered] = Col(kSelect);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0.12f);
    c[ImGuiCol_TableHeaderBg] = Col(kHead);
    c[ImGuiCol_TableBorderLight] = c[ImGuiCol_TableBorderStrong] = Col(kDark);
    c[ImGuiCol_NavHighlight] = Col(kAccent);
    c[ImGuiCol_NavWindowingHighlight] = Col(kAccent);
}

}  // namespace classic
