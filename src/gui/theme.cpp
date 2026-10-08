#include "theme.h"

#include <filesystem>
#include <initializer_list>

#include "util/common.h"

namespace brack::ui {

Fonts fonts;

namespace {

// The first of `files` that exists and loads.
ImFont* addFirst(std::initializer_list<const char*> files, const ImFontConfig& cfg, const char** loaded = nullptr) {
    for (const char* f : files) {
        if (!std::filesystem::exists(pathFromUtf8(f))) continue;
        if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(f, cfg.SizePixels, &cfg)) {
            if (loaded) *loaded = f;
            return font;
        }
    }
    return nullptr;
}

void loadFonts() {
    ImFontConfig cfg;
    cfg.SizePixels = 13.0f;
    // Device and port names are often Japanese; use system fonts that cover them.
#if defined(_WIN32)
    const auto sans = {"C:\\Windows\\Fonts\\YuGothM.ttc", "C:\\Windows\\Fonts\\meiryo.ttc", "C:\\Windows\\Fonts\\msgothic.ttc",
                       "C:\\Windows\\Fonts\\segoeui.ttf"};
    const auto bold = {"C:\\Windows\\Fonts\\YuGothB.ttc", "C:\\Windows\\Fonts\\meiryob.ttc"};
    const auto mono = {"C:\\Windows\\Fonts\\consola.ttf"};
#elif defined(__APPLE__)
    const auto sans = {"/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc"};
    const auto bold = {"/System/Library/Fonts/ヒラギノ角ゴシック W6.ttc"};
    const auto mono = {"/System/Library/Fonts/Menlo.ttc"};
#else
    const auto sans = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                       "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc"};
    const auto bold = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc",
                       "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Bold.ttc"};
    const auto mono = {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/dejavu/DejaVuSansMono.ttf"};
#endif
    const char* sansFile = nullptr;
    fonts.sans = addFirst(sans, cfg, &sansFile);
    if (!fonts.sans) fonts.sans = ImGui::GetIO().Fonts->AddFontDefault(&cfg);
    fonts.bold = addFirst(bold, cfg);
    if (!fonts.bold) fonts.bold = fonts.sans;
    fonts.mono = addFirst(mono, cfg);
    if (fonts.mono && sansFile) {  // names shown in the monospace font may be Japanese too
        ImFontConfig merge = cfg;
        merge.MergeMode = true;
        addFirst({sansFile}, merge);
    }
    if (!fonts.mono) fonts.mono = fonts.sans;
}

ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

}  // namespace

void setupStyle(float dpiScale) {
    loadFonts();
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.FontSizeBase = 13.0f;
    s.WindowPadding = ImVec2(10, 8);
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.PopupRounding = 8;
    s.FrameRounding = 7;
    s.FrameBorderSize = 1;
    s.FramePadding = ImVec2(10, 10.5f);
    s.ItemSpacing = ImVec2(8, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.ScrollbarSize = 12;
    s.ScrollbarRounding = 6;
    s.GrabRounding = 4;
    s.CellPadding = ImVec2(8, 4);
    s.SeparatorTextBorderSize = 1;
    s.ScaleAllSizes(dpiScale);
    s.FontScaleDpi = dpiScale;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = v4(col::text);
    c[ImGuiCol_TextDisabled] = v4(col::text3);
    c[ImGuiCol_WindowBg] = v4(col::bg);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v4(col::card);
    c[ImGuiCol_Border] = v4(col::borderStrong);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = v4(col::bg);
    c[ImGuiCol_FrameBgHovered] = v4(rgb(0x141418));
    c[ImGuiCol_FrameBgActive] = v4(rgb(0x141418));
    c[ImGuiCol_Button] = v4(col::bg);  // the arrow of a combo; Brack's own buttons are drawn
    c[ImGuiCol_ButtonHovered] = v4(col::raised);
    c[ImGuiCol_ButtonActive] = v4(col::controlOn);
    c[ImGuiCol_Header] = v4(col::control);
    c[ImGuiCol_HeaderHovered] = v4(col::raised);
    c[ImGuiCol_HeaderActive] = v4(col::controlOn);
    c[ImGuiCol_CheckMark] = v4(col::text);
    c[ImGuiCol_SliderGrab] = v4(col::text);
    c[ImGuiCol_SliderGrabActive] = v4(col::text);
    c[ImGuiCol_Separator] = v4(col::line);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = v4(col::controlOn);
    c[ImGuiCol_ScrollbarGrabHovered] = v4(col::borderStrong);
    c[ImGuiCol_ScrollbarGrabActive] = v4(col::borderTag);
    c[ImGuiCol_TableHeaderBg] = v4(col::card);
    c[ImGuiCol_TableBorderStrong] = v4(col::border);
    c[ImGuiCol_TableBorderLight] = v4(col::line);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TextLink] = v4(col::text);
    c[ImGuiCol_TextSelectedBg] = v4(rgb(0x4FD1A5, 70));
    c[ImGuiCol_DragDropTarget] = v4(col::text2);
    c[ImGuiCol_NavCursor] = v4(col::text2);
    c[ImGuiCol_ModalWindowDimBg] = v4(rgb(0x08080A, 180));
}

}  // namespace brack::ui
