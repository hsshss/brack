#pragma once
// The GUI's colours, fonts and ImGui style.
#include <imgui.h>

#include <cstdint>

namespace brack::ui {

constexpr ImU32 rgb(uint32_t hex, uint32_t alpha = 255) {
    return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, alpha);
}

namespace col {
constexpr ImU32 bg = rgb(0x0E0E11);
constexpr ImU32 header = rgb(0x0A0A0C);
constexpr ImU32 transport = rgb(0x121215);
constexpr ImU32 card = rgb(0x16161A);
constexpr ImU32 raised = rgb(0x1E1E23);
constexpr ImU32 control = rgb(0x24242A);
constexpr ImU32 controlOn = rgb(0x2A2A31);
constexpr ImU32 line = rgb(0x222228);
constexpr ImU32 border = rgb(0x26262C);
constexpr ImU32 borderStrong = rgb(0x34343B);
constexpr ImU32 borderTag = rgb(0x3A3A42);
constexpr ImU32 text = rgb(0xECE9E2);
constexpr ImU32 text2 = rgb(0xA9A8B1);
constexpr ImU32 text3 = rgb(0x8A8994);
constexpr ImU32 faint = rgb(0x5A5A63);
constexpr ImU32 tick = rgb(0x4A4A53);
constexpr ImU32 onLight = rgb(0x141413);

constexpr ImU32 midi = rgb(0xFFB020);
constexpr ImU32 midiText = rgb(0xFFCB6B);
constexpr ImU32 midiPill = rgb(0x2A2213);
constexpr ImU32 onMidi = rgb(0x1A1408);
constexpr ImU32 audio = rgb(0x4FD1A5);
constexpr ImU32 audioText = rgb(0x7FE0BF);
constexpr ImU32 onAudio = rgb(0x0B1A15);
constexpr ImU32 runningBg = rgb(0x13201B);
constexpr ImU32 runningBorder = rgb(0x2D5E4C);
constexpr ImU32 clip = rgb(0xE6463C);

constexpr ImU32 error = rgb(0xFF8A7E);
constexpr ImU32 errorBg = rgb(0x1B1416);
constexpr ImU32 errorBorder = rgb(0x4A2724);
constexpr ImU32 errorTag = rgb(0x5A2E2A);
constexpr ImU32 onError = rgb(0x1B0F0D);
constexpr ImU32 warn = rgb(0xF2D88A);
constexpr ImU32 warnBg = rgb(0x2A2312);
constexpr ImU32 warnBorder = rgb(0x4A3D1A);
constexpr ImU32 pendingBg = rgb(0x221D10);
constexpr ImU32 pendingNote = rgb(0xC9B57A);

constexpr ImU32 logInfoTag = rgb(0x6A6A74);
constexpr ImU32 logWarnTag = rgb(0xE8C547);
constexpr ImU32 logWarnRow = rgb(0x1A170D);
constexpr ImU32 logErrorRow = rgb(0x1B1213);
}  // namespace col

struct Fonts {
    ImFont* sans = nullptr;
    ImFont* bold = nullptr;
    ImFont* mono = nullptr;  // numbers and ids
};
extern Fonts fonts;

// Loads the fonts and sets up the style. `dpiScale`: the window's content scale.
void setupStyle(float dpiScale);

// A length of the design (CSS pixels of the mockups) in screen pixels.
inline float px(float v) { return v * ImGui::GetStyle().FontScaleDpi; }

}  // namespace brack::ui
