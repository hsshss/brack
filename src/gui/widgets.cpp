#include "widgets.h"

#include <imgui_internal.h>  // ClearActiveID, FindRenderedTextEnd

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace brack::ui {

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 pos, float size, ImU32 col) {
    const float u = size / 24.0f;
    const float w = std::max(1.0f, (icon == Icon::Check ? 3.0f : 1.8f) * u);
    col = tint(col);
    auto P = [&](float x, float y) { return ImVec2(pos.x + x * u, pos.y + y * u); };
    auto rad = [](float deg) { return deg * IM_PI / 180.0f; };
    auto line = [&](float x0, float y0, float x1, float y1) { dl->AddLine(P(x0, y0), P(x1, y1), col, w); };
    auto poly = [&](std::initializer_list<ImVec2> pts, bool closed) {
        for (ImVec2 p : pts) dl->PathLineTo(P(p.x, p.y));
        dl->PathStroke(col, closed ? ImDrawFlags_Closed : 0, w);
    };
    auto circle = [&](float x, float y, float r) { dl->AddCircle(P(x, y), r * u, col, 0, w); };
    auto dot = [&](float x, float y, float r) { dl->AddCircleFilled(P(x, y), r * u, col); };
    auto rect = [&](float x, float y, float rw, float rh, float r) { dl->AddRect(P(x, y), P(x + rw, y + rh), col, r * u, 0, w); };
    auto arc = [&](float cx, float cy, float r, float fromDeg, float toDeg) { dl->PathArcTo(P(cx, cy), r * u, rad(fromDeg), rad(toDeg)); };
    switch (icon) {
        case Icon::None: break;
        case Icon::Rack:
            rect(3, 4, 18, 6, 1.5f);
            rect(3, 14, 18, 6, 1.5f);
            break;
        case Icon::Routing:
            circle(6, 6, 2.5f);
            circle(18, 18, 2.5f);
            dl->PathLineTo(P(8.5f, 6));
            arc(14, 10, 4, -90, 0);
            poly({{18, 15.5f}}, false);
            break;
        case Icon::Sliders:
            line(4, 7, 14, 7);
            line(18, 7, 20, 7);
            line(4, 17, 8, 17);
            line(12, 17, 20, 17);
            circle(16, 7, 2);
            circle(10, 17, 2);
            break;
        case Icon::Log:
            line(5, 6, 19, 6);
            line(5, 12, 19, 12);
            line(5, 18, 14, 18);
            break;
        case Icon::Power:
            line(12, 3, 12, 11);
            arc(12, 13.1f, 8, -44.5f, 224.5f);
            dl->PathStroke(col, 0, w);
            break;
        case Icon::ChevronDown: poly({{6, 9}, {12, 15}, {18, 9}}, false); break;
        case Icon::Plus:
            line(12, 5, 12, 19);
            line(5, 12, 19, 12);
            break;
        case Icon::File:
            poly({{14, 3}, {5, 3}, {5, 21}, {19, 21}, {19, 8}}, true);
            poly({{14, 3}, {14, 8}, {19, 8}}, false);
            break;
        case Icon::Grip:
            for (float y : {7.0f, 12.0f, 17.0f}) {
                dot(9.5f, y, 1.4f);
                dot(14.5f, y, 1.4f);
            }
            break;
        case Icon::Warning:
            poly({{12, 3}, {2, 20}, {22, 20}}, true);
            line(12, 10, 12, 14);
            dot(12, 17.2f, 1.1f);
            break;
        case Icon::Editor:
            rect(3, 5, 18, 14, 2);
            line(3, 9, 21, 9);
            break;
        case Icon::Reload:
            arc(12, 12, 8, -7.1f, -139.8f);
            poly({{4, 8}}, false);
            poly({{4, 4}, {4, 8}, {8, 8}}, false);
            arc(12, 12, 8, 172.9f, 40.2f);
            poly({{20, 16}}, false);
            poly({{20, 20}, {20, 16}, {16, 16}}, false);
            break;
        case Icon::More:
            for (float x : {5.0f, 12.0f, 19.0f}) dot(x, 12, 1.7f);
            break;
        case Icon::Close:
            line(6, 6, 18, 18);
            line(18, 6, 6, 18);
            break;
        case Icon::Search:
            circle(11, 11, 6.5f);
            line(16, 16, 20.5f, 20.5f);
            break;
        case Icon::Copy:
            rect(8, 8, 12, 12, 2);
            poly({{16, 8}, {16, 4}, {4, 4}, {4, 16}, {8, 16}}, false);
            break;
        case Icon::Check: poly({{5, 12.5f}, {9.5f, 17}, {19, 7.5f}}, false); break;
        case Icon::Folder: poly({{3, 5}, {9, 5}, {11, 7}, {21, 7}, {21, 19}, {3, 19}}, true); break;
    }
}

float textWidth(ImFont* font, float size, std::string_view s) {
    return font->CalcTextSizeA(px(size), FLT_MAX, 0, s.data(), s.data() + s.size()).x;
}

std::string shortenPath(std::string_view path, ImFont* font, float size, float width) {
    if (textWidth(font, size, path) <= width) return std::string(path);
    // Cuts fall on UTF-8 character boundaries.
    auto starts = [&](size_t i) { return i == 0 || i >= path.size() || (path[i] & 0xC0) != 0x80; };
    const size_t sep = path.find_last_of("\\/");
    size_t tail = sep == std::string_view::npos ? 0 : sep;
    while (tail < path.size() && textWidth(font, size, std::string("...").append(path.substr(tail))) > width)
        do ++tail; while (!starts(tail));
    size_t head = 0;
    for (size_t next = head; next < tail;) {
        do ++next; while (!starts(next));
        if (textWidth(font, size, std::string(path.substr(0, next)).append("...").append(path.substr(tail))) > width) break;
        head = next;
    }
    return std::string(path.substr(0, head)).append("...").append(path.substr(tail));
}

void drawText(ImVec2 pos, ImFont* font, float size, ImU32 col, std::string_view s) {
    ImGui::GetWindowDrawList()->AddText(font, px(size), pos, tint(col), s.data(), s.data() + s.size());
}

void text(std::string_view s, ImFont* font, float size, ImU32 col) {
    ImGui::PushFont(font, size);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(s.data(), s.data() + s.size());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

Row::Row(float height, float width)
    : origin_(ImGui::GetCursorScreenPos()), height_(px(height)),
      width_(width < 0 ? ImGui::GetContentRegionAvail().x : width), x_(origin_.x), right_(origin_.x + width_) {}

ImVec2 Row::next(float w, float h) {
    const ImVec2 p(x_, origin_.y + std::floor((height_ - h) * 0.5f));
    x_ += w;
    return p;
}

ImVec2 Row::nextRight(float w, float h) {
    right_ -= w;
    return ImVec2(right_, origin_.y + std::floor((height_ - h) * 0.5f));
}

// The line and the gap below it as one item, so that the window's extent ends with an item
// rather than a cursor move (which ImGui reports as an error).
void Row::end(float gapBelow) {
    ImGui::SetCursorScreenPos(origin_);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
    ImGui::Dummy(ImVec2(width_, height_ + px(gapBelow)));
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2(origin_.x, ImGui::GetCursorScreenPos().y));  // the next line starts where this one did
}

namespace {

struct ButtonLayout {
    float width, pad, icon, gap;
    const char* textEnd;
};

ButtonLayout layoutButton(const char* label, const ButtonLook& look, float height, Icon icon, float fontSize) {
    ButtonLayout l{};
    l.textEnd = ImGui::FindRenderedTextEnd(label);
    l.icon = icon == Icon::None ? 0 : px(height >= 40 ? 18.0f : height >= 34 ? 16.0f : 14.0f);
    if (l.textEnd == label) {
        l.width = px(height);
        return l;
    }
    l.pad = px(height >= 36 ? 14.0f : height >= 32 ? 12.0f : 10.0f);
    l.gap = icon == Icon::None ? 0 : px(height >= 40 ? 10.0f : 6.0f);
    l.width = 2 * l.pad + l.icon + l.gap + textWidth(look.bold ? fonts.bold : fonts.sans, fontSize, {label, size_t(l.textEnd - label)});
    return l;
}

bool isLight(ImU32 c) {
    return ((c >> IM_COL32_R_SHIFT) & 0xFF) + ((c >> IM_COL32_G_SHIFT) & 0xFF) + ((c >> IM_COL32_B_SHIFT) & 0xFF) > 3 * 128;
}

}  // namespace

float buttonWidth(const char* label, const ButtonLook& look, float height, Icon icon, float fontSize) {
    return layoutButton(label, look, height, icon, fontSize).width;
}

bool button(const char* label, const ButtonLook& look, float height, Icon icon, float fontSize) {
    const ButtonLayout l = layoutButton(label, look, height, icon, fontSize);
    const float h = px(height), r = px(height >= 36 ? 8.0f : height >= 30 ? 7.0f : 6.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + l.width, p.y + h);
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(l.width, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (look.bg) dl->AddRectFilled(p, q, tint(look.bg), r);
    if (look.border && look.border != look.bg) dl->AddRect(p, q, tint(look.border), r);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        const int a = ImGui::IsItemActive() ? 28 : 14;
        dl->AddRectFilled(p, q, tint(look.bg && isLight(look.bg) ? IM_COL32(0, 0, 0, a) : IM_COL32(255, 255, 255, a)), r);
    }
    float x = l.textEnd == label ? p.x + (l.width - l.icon) * 0.5f : p.x + l.pad;
    if (icon != Icon::None) {
        drawIcon(dl, icon, ImVec2(x, p.y + (h - l.icon) * 0.5f), l.icon, look.text);
        x += l.icon + l.gap;
    }
    if (l.textEnd != label)
        drawText(ImVec2(x, p.y + (h - px(fontSize)) * 0.5f), look.bold ? fonts.bold : fonts.sans, fontSize, look.text,
                 {label, size_t(l.textEnd - label)});
    return clicked;
}

bool dbFader(const char* id, float& db, float minDb, float maxDb, float width, float thumb, ImU32 fill) {
    const float th = px(thumb), h = th + px(12);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, h));
    // The thumb's centre travels between these, so the thumb stays within `width`.
    const float x0 = p.x + th * 0.5f, x1 = p.x + width - th * 0.5f;
    auto xOf = [&](float v) { return x0 + (std::clamp(v, minDb, maxDb) - minDb) / (maxDb - minDb) * (x1 - x0); };
    bool changed = false;
    if (ImGui::IsItemActive()) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        const float v = std::round((minDb + t * (maxDb - minDb)) * 10.0f) / 10.0f;
        changed = v != db;
        db = v;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        ImGui::ClearActiveID();  // or the fader would keep following the (still pressed) mouse
        db = 0.0f;
        changed = true;
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) ImGui::SetTooltip("double-click: 0 dB");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = p.y + h * 0.5f, cx = xOf(db), zx = std::round(xOf(0.0f));
    dl->AddRectFilled(ImVec2(p.x, cy - px(2)), ImVec2(p.x + width, cy + px(2)), tint(col::controlOn), px(2));
    dl->AddRectFilled(ImVec2(p.x, cy - px(2)), ImVec2(cx, cy + px(2)), tint(fill), px(2));
    dl->AddRectFilled(ImVec2(zx, cy - px(7)), ImVec2(zx + std::max(1.0f, std::floor(px(1))), cy + px(7)), tint(col::tick));
    dl->AddCircleFilled(ImVec2(cx, cy), th * 0.5f, tint(col::text));
    return changed;
}

namespace {
float segmentWidth(const char* label) { return 2 * px(10) + textWidth(fonts.bold, 12, label); }
}  // namespace

float segmentedWidth(std::span<const char* const> labels) {
    float w = 2 * px(3);
    for (const char* l : labels) w += segmentWidth(l);
    return w;
}

bool segmented(const char* id, int& selected, std::span<const char* const> labels, ImU32 onBg, ImU32 onText) {
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = segmentedWidth(labels), h = px(34), inner = px(28);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), tint(col::bg), px(7));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), tint(col::border), px(7));
    bool changed = false;
    float x = p.x + px(3);
    for (int i = 0; i < (int)labels.size(); ++i) {
        const float sw = segmentWidth(labels[i]);
        const ImVec2 a(x, p.y + px(3));
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton(labels[i], ImVec2(sw, inner)) && selected != i) {
            selected = i;
            changed = true;
        }
        const bool on = selected == i;
        if (on) dl->AddRectFilled(a, ImVec2(a.x + sw, a.y + inner), tint(onBg), px(5));
        const bool strong = on && onBg != col::controlOn;
        ImFont* f = strong ? fonts.bold : fonts.sans;
        const float tw = textWidth(f, 12, labels[i]);
        drawText(ImVec2(a.x + (sw - tw) * 0.5f, a.y + (inner - px(12)) * 0.5f), f, 12,
                 on ? onText : ImGui::IsItemHovered() ? col::text : col::text2, labels[i]);
        x += sw;
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
    return changed;
}

bool toggleSwitch(const char* id, bool& on) {
    const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + px(40), p.y + px(24));
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(q.x - p.x, q.y - p.y));
    if (clicked) on = !on;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, tint(on ? col::text : col::control), px(12));
    dl->AddRect(p, q, tint(on ? col::text : col::borderTag), px(12));
    dl->AddCircleFilled(ImVec2(p.x + px(on ? 27.0f : 11.0f), p.y + px(12)), px(8), tint(on ? col::onLight : col::text2));
    return clicked;
}

ImVec2 badgeSize(std::string_view s, ImFont* font, float size, float padX) {
    return ImVec2(textWidth(font, size, s) + 2 * px(padX), std::round(px(size * 1.4f)));
}

void badge(ImVec2 pos, std::string_view s, ImFont* font, float size, ImU32 bg, ImU32 border, ImU32 textCol, float padX) {
    const ImVec2 sz = badgeSize(s, font, size, padX), q(pos.x + sz.x, pos.y + sz.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (bg) dl->AddRectFilled(pos, q, tint(bg), px(3));
    if (border) dl->AddRect(pos, q, tint(border), px(3));
    drawText(ImVec2(pos.x + px(padX), pos.y + (sz.y - px(size)) * 0.5f), font, size, textCol, s);
}

void sectionHeader(const char* label, ImU32 dot, const char* hint) {
    Row row(24);
    const ImVec2 d = row.next(px(8), px(8));
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(d.x + px(4), d.y + px(4)), px(4), tint(dot));
    row.gap(8);
    drawText(row.next(textWidth(fonts.bold, 12, label), px(12)), fonts.bold, 12, col::text2, label);
    if (hint) drawText(row.nextRight(textWidth(fonts.sans, 12, hint), px(12)), fonts.sans, 12, col::text3, hint);
    row.end(10);
}

void beginCard(const char* id, float width, ImU32 bg, ImU32 border, ImVec2 padding, float rounding) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(rounding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(padding.x), px(padding.y)));
    ImGui::BeginChild(id, ImVec2(width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void endCard() {
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::EndChild();
}

void spaceBelow(float gap) {
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetItemRectMax().y + px(gap)));
}

}  // namespace brack::ui
