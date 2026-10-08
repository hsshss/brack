#pragma once
// Small drawn controls shared by the GUI's views. Sizes given as parameters are design pixels
// (see px()); positions and measured widths are screen pixels.
#include <imgui.h>

#include <span>
#include <string>
#include <string_view>

#include "theme.h"

namespace brack::ui {

enum class Icon { None, Rack, Routing, Sliders, Log, Power, ChevronDown, Plus, File, Grip, Warning, Editor, Reload, More, Close, Search, Copy, Check, Folder };

// Stroke icons on a 24-unit grid, drawn `size` screen pixels square at `pos`.
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 pos, float size, ImU32 col);

// `col` with the current style alpha applied (BeginDisabled, a dimmed card).
inline ImU32 tint(ImU32 col) { return ImGui::GetColorU32(col); }

float textWidth(ImFont* font, float size, std::string_view s);
void drawText(ImVec2 pos, ImFont* font, float size, ImU32 col, std::string_view s);
// A path shortened to `width` screen pixels by "..." in its middle, keeping its last component whole
// where it fits, so the file or folder name stays readable.
std::string shortenPath(std::string_view path, ImFont* font, float size, float width);
// Text as an ImGui item at the cursor.
void text(std::string_view s, ImFont* font, float size, ImU32 col);

// Lays out one line of fixed height left to right, and from the right edge, centring each item
// vertically. ImGui items go at the cursor that place() sets; end() moves the cursor below the line.
class Row {
public:
    explicit Row(float height, float width = -1);
    ImVec2 next(float w, float h);       // top left of the next w x h box; screen pixels
    ImVec2 nextRight(float w, float h);  // the same, packed against the right edge
    void place(float w, float h) { ImGui::SetCursorScreenPos(next(w, h)); }
    void placeRight(float w, float h) { ImGui::SetCursorScreenPos(nextRight(w, h)); }
    void gap(float g) { x_ += px(g); }
    void gapRight(float g) { right_ -= px(g); }
    float left() const { return x_; }
    float remaining() const { return right_ - x_; }
    float top() const { return origin_.y; }
    float height() const { return height_; }
    void end(float gapBelow = 0);

private:
    ImVec2 origin_;
    float height_, width_, x_, right_;
};

struct ButtonLook {
    ImU32 bg, border, text;
    bool bold = false;
};
namespace look {
constexpr ButtonLook primary{col::text, col::text, col::onLight, true};
constexpr ButtonLook outline{0, col::borderStrong, col::text};
constexpr ButtonLook outlineMuted{0, col::borderStrong, col::text2};
constexpr ButtonLook filled{col::control, col::control, col::text};
constexpr ButtonLook ghost{0, 0, col::text2};
constexpr ButtonLook danger{col::error, col::error, col::onError, true};
constexpr ButtonLook dangerOutline{0, col::errorTag, col::error};
constexpr ButtonLook amber{col::midi, col::midi, col::onMidi, true};
constexpr ButtonLook amberOutline{0, col::warnBorder, col::warn};
}  // namespace look

// A button `height` tall at the cursor, with an icon before the label. The label may hide its
// id after "##"; a button with no label text is a square icon button.
bool button(const char* label, const ButtonLook& look, float height, Icon icon = Icon::None, float fontSize = 13);
float buttonWidth(const char* label, const ButtonLook& look, float height, Icon icon = Icon::None, float fontSize = 13);

// A horizontal fader in dB, `width` screen pixels wide, with a tick at 0 dB. Double-click sets
// 0 dB. Returns true when `db` changed; IsItemActive() tells whether it is being dragged.
bool dbFader(const char* id, float& db, float minDb, float maxDb, float width, float thumb, ImU32 fill);

// Mutually exclusive options side by side. onBg/onText colour the selected one.
bool segmented(const char* id, int& selected, std::span<const char* const> labels, ImU32 onBg = col::controlOn,
               ImU32 onText = col::text);
float segmentedWidth(std::span<const char* const> labels);

bool toggleSwitch(const char* id, bool& on);

// A tag such as a plugin's format; drawn only.
void badge(ImVec2 pos, std::string_view s, ImFont* font, float size, ImU32 bg, ImU32 border, ImU32 textCol, float padX);
ImVec2 badgeSize(std::string_view s, ImFont* font, float size, float padX);

// A list section's heading: a coloured dot, the label, and a hint at the right.
void sectionHeader(const char* label, ImU32 dot, const char* hint = nullptr);

// A rounded panel as tall as its content; always pair with endCard().
void beginCard(const char* id, float width, ImU32 bg, ImU32 border, ImVec2 padding, float rounding = 10);
void endCard();

// Puts the cursor `gap` below the last item, instead of ItemSpacing below it.
void spaceBelow(float gap);

}  // namespace brack::ui
