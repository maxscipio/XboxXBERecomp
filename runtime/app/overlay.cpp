// the host's settings window, F1 (or Back and Start together on a controller): display, graphics, audio, controllers and
// the game's own options. Changes are saved to the settings file as they are made; display mode, resolution, render
// scale and audio apply at once, the aspect ratio at the next start.
// A section list on the left and one full-width row per setting, in classic Steam's dark olive. Made for a controller:
// LB/RB change section, up/down rows, left/right change a row's value, A presses, B closes.
// Off unless GameInfo::settings_window is set. The window keeps ImGui's style and font scale to itself, a title's own
// ui is drawn after it as before.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <iterator>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include "app/app.h"
#include "app/classic_theme.h"
#include "audio/audio.h"
#include "core/input.h"
#include "core/log.h"

namespace app {

namespace {

std::atomic<bool> g_open{false};
std::atomic<bool> g_focus_first{false};  // select the page's first row

using namespace classic;
constexpr float kBaseFont = 15;  // pixels at 720 lines; the window scales with the screen

ImFont* g_font = nullptr;  // Tahoma, as classic Steam; null without it
ImFont* g_bold = nullptr;
const char* g_help = nullptr;  // the selected row's line for the footer
bool g_prompt_change = false;  // the selected row changes with left and right
bool g_dirty = false;          // a setting changed that is not in the file yet

// saved once nothing is held: a slider dragged or stepped does not write the file every frame
void Changed() { g_dirty = true; }

void SetHelp(const char* help) { g_help = help; }

// the arrows on any of a controller or the keyboard, repeating while held; held a second a press counts for `fast`
int LeftRight(bool repeat, int fast = 1) {
    int dir = 0;
    float held = 0;
    auto press = [&](ImGuiKey key, int d) {
        if (ImGui::IsKeyPressed(key, repeat)) dir = d;
        if (ImGui::IsKeyDown(key)) held = std::max(held, ImGui::GetKeyData(key)->DownDuration);
    };
    press(ImGuiKey_LeftArrow, -1), press(ImGuiKey_GamepadDpadLeft, -1);
    press(ImGuiKey_RightArrow, 1), press(ImGuiKey_GamepadDpadRight, 1);
    return held > 1 ? dir * fast : dir;
}

bool StepHeld() {
    return ImGui::IsMouseDown(0) || ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow) ||
           ImGui::IsKeyDown(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadRight);
}

// one setting's row: the label on the left, the value's area on the right half; selected, it takes the accent
struct Row {
    ImRect rect, value;
    bool pressed, hovered, selected;
    ImU32 text;
};

Row RowBegin(const char* label, const char* help) {
    float w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetFrameHeight() + ImGui::GetFontSize() * 0.3f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    Row r;
    r.pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
    r.hovered = ImGui::IsItemHovered();
    r.selected = ImGui::IsItemFocused();
    if (g_focus_first) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        g.NavDisableHighlight = false;
        g_focus_first = false;
        r.selected = true;
    }
    r.rect = ImRect(pos, ImVec2(pos.x + w, pos.y + h));
    float pad = ImGui::GetStyle().FramePadding.x;
    r.value = ImRect(ImVec2(pos.x + w * 0.5f, pos.y + 2), ImVec2(pos.x + w - pad, pos.y + h - 2));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (r.selected) dl->AddRectFilled(r.rect.Min, r.rect.Max, kSelect);
    else if (r.hovered) dl->AddRectFilled(r.rect.Min, r.rect.Max, kHover);
    r.text = r.selected ? kWhite : kText;
    const char* end = ImGui::FindRenderedTextEnd(label);
    dl->AddText(ImVec2(pos.x + pad, pos.y + (h - ImGui::GetFontSize()) * 0.5f), r.text, label, end);
    if (r.selected || r.hovered) SetHelp(help);
    return r;
}

// a sunken field, as the value of a row
void Field(const ImRect& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(r.Min, r.Max, kField);
    dl->AddLine(r.Min, ImVec2(r.Max.x, r.Min.y), kDark);
    dl->AddLine(r.Min, ImVec2(r.Min.x, r.Max.y), kDark);
    dl->AddLine(ImVec2(r.Min.x, r.Max.y - 1), ImVec2(r.Max.x, r.Max.y - 1), kLight);
    dl->AddLine(ImVec2(r.Max.x - 1, r.Min.y), ImVec2(r.Max.x - 1, r.Max.y), kLight);
}

void ValueText(const Row& r, const char* text, float align = 0.5f) {
    ImVec2 size = ImGui::CalcTextSize(text);
    ImVec2 pos(r.value.Min.x + (r.value.GetWidth() - size.x) * align, r.value.Min.y + (r.value.GetHeight() - size.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(ImVec2(std::max(pos.x, r.value.Min.x), pos.y), kText, text);
}

bool RowCheck(const char* label, bool* v, const char* help = nullptr) {
    Row r = RowBegin(label, help);
    bool changed = r.pressed;
    if (r.selected) g_prompt_change = true;
    if (r.selected && LeftRight(false)) changed = true;
    if (changed) *v = !*v;
    Field(r.value);
    ValueText(r, *v ? "On" : "Off");
    if (changed) Changed();
    return changed;
}

// short lists only: each step applies at once, and a held arrow does not repeat
bool RowChoice(const char* label, int* v, const char* const* names, int n, const char* help = nullptr) {
    Row r = RowBegin(label, help);
    int step = 0;
    if (r.pressed) step = ImGui::GetIO().MousePos.x < r.value.GetCenter().x && ImGui::GetIO().MousePos.x >= r.value.Min.x ? -1 : 1;
    if (r.selected) {
        g_prompt_change = true;
        if (int d = LeftRight(false)) step = d;
    }
    if (step) *v = (*v + step + n) % n;
    Field(r.value);
    ValueText(r, names[std::clamp(*v, 0, n - 1)]);
    float pad = ImGui::GetStyle().FramePadding.x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = r.value.Min.y + (r.value.GetHeight() - ImGui::GetFontSize()) * 0.5f;
    dl->AddText(ImVec2(r.value.Min.x + pad, y), kAccent, "<");
    dl->AddText(ImVec2(r.value.Max.x - pad - ImGui::CalcTextSize(">").x, y), kAccent, ">");
    if (step) Changed();
    return step != 0;
}

// a setting that takes one of a list of values; a value the file holds that is not in the list joins it as it is
bool RowValues(const char* label, uint32_t* v, std::vector<std::pair<uint32_t, std::string>> list, const char* help = nullptr) {
    int at = -1;
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].first == *v) at = int(i);
    if (at < 0) {
        list.push_back({*v, std::to_string(*v)});
        at = int(list.size()) - 1;
    }
    std::vector<const char*> names;
    for (const auto& e : list) names.push_back(e.second.c_str());
    if (!RowChoice(label, &at, names.data(), int(names.size()), help)) return false;
    *v = list[at].first;
    return true;
}

bool RowSlider(const char* label, int* v, int lo, int hi, int step, const char* suffix = "", const char* help = nullptr) {
    Row r = RowBegin(label, help);
    bool active = ImGui::IsItemActive();
    int was = *v;
    ImGuiIO& io = ImGui::GetIO();
    if (active && ImGui::GetCurrentContext()->ActiveIdSource == ImGuiInputSource_Mouse && io.MouseClickedPos[0].x >= r.value.Min.x) {
        float t = std::clamp((io.MousePos.x - r.value.Min.x) / r.value.GetWidth(), 0.0f, 1.0f);
        *v = std::clamp(lo + int(std::lround(t * (hi - lo) / step)) * step, lo, hi);
    }
    if (r.selected) {
        g_prompt_change = true;
        if (int d = LeftRight(true, 5)) *v = std::clamp(*v + d * step, lo, hi);
    }
    Field(r.value);
    float t = hi > lo ? std::clamp(float(*v - lo) / float(hi - lo), 0.0f, 1.0f) : 0;
    ImRect bar(r.value.Min + ImVec2(1, 1), r.value.Max - ImVec2(1, 1));
    ImGui::GetWindowDrawList()->AddRectFilled(bar.Min, ImVec2(bar.Min.x + bar.GetWidth() * t, bar.Max.y), r.selected ? kSelect : kHead);
    ValueText(r, (std::to_string(*v) + suffix).c_str());
    if (*v != was) Changed();
    return *v != was;
}

// a value with no set range: left and right step it, pressing the row types it
bool RowNumber(const char* label, uint32_t* v, const char* help = nullptr) {
    Row r = RowBegin(label, help);
    if (r.pressed) ImGui::OpenPopup(label);
    bool changed = false;
    if (r.selected) {
        g_prompt_change = true;
        if (int d = LeftRight(true, 10)) {
            *v = uint32_t(std::max(int64_t(*v) + d, int64_t(0)));
            changed = true;
        }
    }
    Field(r.value);
    ValueText(r, std::to_string(*v).c_str());
    ImGui::SetNextWindowPos(ImVec2(r.value.Min.x, r.value.Max.y));
    ImGui::SetNextWindowSize(ImVec2(r.value.GetWidth(), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    if (ImGui::BeginPopup(label)) {
        int n = int(*v);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputInt("##number", &n)) {
            *v = uint32_t(std::max(n, 0));
            changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    if (changed) Changed();
    return changed;
}

// a long list, or one a step at a time would be costly (a resolution): pressing the row opens it. Between
// RowListBegin returning true and ImGui::EndPopup, one ListItem per entry
bool RowListBegin(const char* label, const char* current, const char* help = nullptr) {
    Row r = RowBegin(label, help);
    if (r.pressed) ImGui::OpenPopup(label);
    Field(r.value);
    ValueText(r, current);
    ImGui::GetWindowDrawList()->AddText(ImVec2(r.value.Max.x - ImGui::GetStyle().FramePadding.x - ImGui::CalcTextSize("v").x,
                                               r.value.Min.y + (r.value.GetHeight() - ImGui::GetFontSize()) * 0.5f),
                                        kAccent, "v");
    ImGui::SetNextWindowPos(ImVec2(r.value.Min.x, r.value.Max.y));
    ImGui::SetNextWindowSizeConstraints(ImVec2(r.value.GetWidth(), 0), ImVec2(r.value.GetWidth(), ImGui::GetFontSize() * 18));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().FramePadding);
    bool open = ImGui::BeginPopup(label);
    ImGui::PopStyleVar();
    return open;
}

bool ListItem(const char* name, bool chosen) {
    bool picked = ImGui::Selectable(name, chosen);
    if (chosen) ImGui::SetItemDefaultFocus();
    return picked;
}

bool RowButton(const char* label, const char* value = nullptr, const char* help = nullptr) {
    Row r = RowBegin(label, help);
    if (value) ValueText(r, value, 1.0f);
    return r.pressed;
}

// a live reading, not selectable: the label and a bar
void RowMeter(const char* label, float fraction, const char* text) {
    float h = ImGui::GetFrameHeight();
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    float pad = ImGui::GetStyle().FramePadding.x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(pos.x + pad, pos.y + (h - ImGui::GetFontSize()) * 0.5f), kDim, label);
    ImRect bar(ImVec2(pos.x + w * 0.5f, pos.y + 2), ImVec2(pos.x + w - pad, pos.y + h - 2));
    Field(bar);
    dl->AddRectFilled(bar.Min + ImVec2(1, 1), ImVec2(bar.Min.x + 1 + (bar.GetWidth() - 2) * std::clamp(fraction, 0.0f, 1.0f), bar.Max.y - 1), kHead);
    ImVec2 size = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(bar.GetCenter().x - size.x * 0.5f, bar.GetCenter().y - size.y * 0.5f), kText, text);
}

void Section(const char* title) {
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.4f));
    float pad = ImGui::GetStyle().FramePadding.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    if (g_bold) ImGui::PushFont(g_bold);
    ImGui::TextColored(Col(kAccent), "%s", title);
    if (g_bold) ImGui::PopFont();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(pos, ImVec2(pos.x + w, pos.y), kDark);
    dl->AddLine(ImVec2(pos.x, pos.y + 1), ImVec2(pos.x + w, pos.y + 1), kLight & 0x60ffffff);
    ImGui::Dummy(ImVec2(0, 3));
}

void Note(const char* text) {
    float pad = ImGui::GetStyle().FramePadding.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - pad);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// the window's monitor's modes, widest first, each size once
std::vector<Mode> Modes() {
    std::vector<Mode> modes;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY), &mi)) return modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++) {
        if (dm.dmBitsPerPel < 32 || dm.dmPelsHeight < 480) continue;
        Mode m{dm.dmPelsWidth, dm.dmPelsHeight};
        if (std::none_of(modes.begin(), modes.end(), [&](const Mode& o) { return o.width == m.width && o.height == m.height; }))
            modes.push_back(m);
    }
    std::sort(modes.begin(), modes.end(), [](const Mode& a, const Mode& b) {
        return a.width != b.width ? a.width > b.width : a.height > b.height;
    });
    return modes;
}

void Display(Settings& s) {
    bool apply = false;
    const char* modes[] = {"Fullscreen", "Borderless window", "Window"};
    int mode = int(s.display_mode);
    if (RowChoice("Display mode", &mode, modes, 3)) {
        s.display_mode = DisplayMode(mode);
        apply = true;
    }
    static std::vector<Mode> list = Modes();
    std::string current = s.width && s.height ? std::to_string(s.width) + " x " + std::to_string(s.height) : "Desktop";
    if (RowListBegin("Resolution", current.c_str())) {
        if (ListItem("Desktop", !s.width)) {
            s.width = s.height = 0;
            apply = true;
        }
        for (const Mode& m : list) {
            std::string name = std::to_string(m.width) + " x " + std::to_string(m.height);
            if (ListItem(name.c_str(), s.width == m.width && s.height == m.height)) {
                s.width = m.width;
                s.height = m.height;
                apply = true;
            }
        }
        ImGui::EndPopup();
    }
    RowCheck("Vertical sync", &s.vsync);
    int aspect = int(std::min<uint32_t>(s.aspect, kAspectCount - 1));
    const char* aspects[] = {"From the screen's shape", "4:3", "16:9 (widescreen)", "Fill the screen (ultrawide)"};
    if (RowChoice("Aspect ratio", &aspect, aspects, int(kAspectCount),
                  "Applies at the next start (the game reads its TV setting when it boots)."))
        s.aspect = uint32_t(aspect);
    RowCheck("Console look", &s.console_look,
             "The picture as the console drew it: 4:3 at its own resolution, with bars on a wide screen. Overrides aspect ratio "
             "and render scale; applies at the next start.");
    if (apply) {
        Changed();
        ApplyDisplay();
    }
}

void Graphics(Settings& s) {
    int percent = int(s.render_scale);
    if (RowSlider("Render scale", &percent, 25, 500, 25, "%",
                  "Percent of the console's own resolution: 100% is 480 lines (640 x 480 at 4:3, 854 x 480 at 16:9, 1120 x 480 "
                  "at 21:9), 225% is 1080 lines, 450% is 2160. Above the screen's lines it is downsampled, which also smooths "
                  "edges; applies at once."))
        s.render_scale = uint32_t(percent);
    RowValues("Anisotropic filtering", &s.aniso, {{0, "As the game asks"}, {2, "2x"}, {4, "4x"}, {8, "8x"}, {16, "16x"}});
    RowValues("Antialiasing (MSAA)", &s.msaa, {{0, "Off"}, {1, "MSAA 2x"}, {2, "MSAA 4x"}, {3, "MSAA 8x"}},
              "Smooths the edges of the 3D scene at once (Direct3D 12). A render scale above the screen's lines also smooths edges.");
    RowCheck("Smooth alpha edges", &s.alpha_coverage,
             "With MSAA on: softens fences, leaves and other cut-out textures, which MSAA alone misses. A dark outline on some is possible.");
    int edge = s.smaa ? 2 : s.fxaa ? 1 : 0;
    const char* edge_names[] = {"Off", "FXAA", "SMAA"};
    if (RowChoice("Edge smoothing", &edge, edge_names, 3,
                  "On the finished frame, HUD included. SMAA keeps more detail than FXAA and costs a little more.")) {
        s.fxaa = edge == 1;
        s.smaa = edge == 2;
    }
    int filter = int(s.scale_filter % 2);
    const char* filter_names[] = {"Bilinear", "Sharp"};
    if (RowChoice("Scaling filter", &filter, filter_names, 2, "How the frame reaches the screen's size when the render scale's lines differ from the screen's."))
        s.scale_filter = uint32_t(filter);
    int sharp = int(s.sharpen);
    if (RowSlider("Sharpening", &sharp, 0, 100, 5, "%", "Contrast adaptive sharpening of the finished frame; applies at once."))
        s.sharpen = uint32_t(sharp);
    RowCheck("CRT filter", &s.crt,
             "The frame as a CRT television showed the console's lines (Direct3D 12). Replaces edge smoothing, sharpening and the "
             "scaling filter; best with Console look.");
    if (s.crt) {
        auto slider = [](const char* label, uint32_t* v, const char* tip) {
            int i = int(*v);
            if (RowSlider(label, &i, 0, 100, 5, "%", tip)) *v = uint32_t(i);
        };
        slider("CRT scanlines", &s.crt_scanlines, "The dark gaps between the console's lines.");
        slider("CRT beam softness", &s.crt_softness, "How far a pixel's light bleeds sideways: a CRT's pixels are not sharp squares.");
        slider("CRT phosphor mask", &s.crt_mask, "The red, green and blue dots of the screen. Shows best at 1440 lines and up.");
        slider("CRT curved glass", &s.crt_curve, "Bends the picture's edges and darkens the corners.");
    }
    RowCheck("Low latency", &s.low_latency,
             "Applies at the next start. Holds one finished frame instead of several: less input lag, but a slow frame costs more.");
    RowValues("Frame rate cap", &s.fps_cap,
              {{30, "30"}, {60, "60"}, {120, "120"}, {144, "144"}, {240, "240"}, {0, "Unlimited"}, {kFpsCapRefresh, "Monitor refresh rate"}},
              "The game sets its own pace; the cap only limits it further.");
    RowCheck("Custom textures", &s.custom_textures, "Images in textures\\replacement beside the exe replace the game's.");
    RowCheck("Shader cache", &s.shader_cache, "Compiled shaders kept for the next run. Applies at the next start.");
    RowCheck("Pre-compile shaders", &s.precompile_shaders,
             "Prepares at launch the pipelines earlier runs used, so they do not stutter in the game. Needs the shader cache. Applies at the next start.");
}

void Audio(Settings& s) {
    const char* outputs[] = {"Automatic", "Stereo", "Surround (5.1)", "Mono"};
    int out = int(std::min(s.audio_output, audio::kOutputMono));
    if (RowChoice("Speakers", &out, outputs, 4, "Automatic follows the speaker setup Windows reports.")) {
        s.audio_output = uint32_t(out);
        audio::SetOutputMode(s.audio_output);
    }
}

void Controller(Settings& s) {
    Section("Controllers");
    for (int port = 0; port < 4; port++) {
        std::string name = input::DeviceName(port);
        Note(("Controller " + std::to_string(port + 1) + ": " + (name.empty() ? std::string("not connected") : name)).c_str());
    }
    Section("Sticks");
    for (int i = 0; i < 2; i++) {
        int dz = int(s.stick_deadzone[i]), sens = int(s.stick_sensitivity[i]);
        if (RowSlider(i ? "Right stick dead zone" : "Left stick dead zone", &dz, 0, 50, 1, "%",
                      "How far the stick moves before it counts, on top of the game's own dead zone. Raise it if a "
                      "worn stick drifts when let go."))
            s.stick_deadzone[i] = uint32_t(dz);
        if (RowSlider(i ? "Right stick sensitivity" : "Left stick sensitivity", &sens, 25, 300, 5, "%",
                      "Below 100% the stick turns or moves slower at full throw; above, it reaches full speed sooner."))
            s.stick_sensitivity[i] = uint32_t(sens);
    }
    if (kGame.mouse_look) {
        Section("Mouse");
        RowCheck("Mouse look", &s.mouse_look, "The mouse plays controller 1's right stick: the camera and aim. The pointer stays in the window while the game has it.");
        int sens = int(s.mouse_sensitivity);
        if (RowSlider("Mouse sensitivity", &sens, 10, 1000, 10, "%")) s.mouse_sensitivity = uint32_t(sens);
        RowCheck("Invert vertical look", &s.mouse_invert_y);
    }
    Section("Test");
    input::Pad pad;
    for (int port = 0; port < 4 && !pad.connected; port++) pad = input::PollRaw(port);
    if (!pad.connected) {
        Note("No controller connected.");
        return;
    }
    // as the game gets them: the dead zone and sensitivity above are in these
    auto meter = [](const char* label, int16_t v) {
        RowMeter(label, (v + 32768) / 65535.0f, (std::to_string(int(std::lround(v / 327.67f))) + "%").c_str());
    };
    meter("Left stick, across", pad.lx);
    meter("Left stick, up", pad.ly);
    meter("Right stick, across", pad.rx);
    meter("Right stick, up", pad.ry);
}

void GameOptions(Settings&) {
    if (kGame.options.empty()) {
        Note("No options for this game.");
        return;
    }
    for (const Option& o : kGame.options) {
        const char* name = o.label ? o.label : o.key;
        if (o.flag) {
            RowCheck(name, o.flag, o.help);
        } else if (o.value) {
            if (o.choices) {
                int n = 0, v = int(*o.value);
                while (o.choices[n]) n++;
                if (n && RowChoice(name, &v, o.choices, n, o.help)) *o.value = uint32_t(v);
            } else {
                RowNumber(name, o.value, o.help);
            }
        }
    }
}

// Quit asks first: the window closes as with its own close button (Alt+F4 does the same)
void QuitPage(Settings&) {
    if (RowButton("Quit game", nullptr, "Progress since the game last saved is lost.")) ImGui::OpenPopup("Quit?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGui::GetStyle().FramePadding * 2);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImGui::GetStyle().FramePadding);
    if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted("Quit the game? Progress since the game last saved is lost.");
        float w = ImGui::GetFontSize() * 7;
        if (ImGui::Button("Quit", ImVec2(w, 0))) {
            LOGR("quit from the settings window");
            Quit();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(w, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

struct Page {
    const char* name;
    void (*draw)(Settings&);
};
const Page kPages[] = {{"Display", Display},       {"Graphics", Graphics}, {"Audio", Audio},
                       {"Controller", Controller}, {"Game", GameOptions},  {"Quit", QuitPage}};

std::string TitleText() {
    std::string out;
    for (const wchar_t* t = kGame.title; t && *t; t++) out += *t < 128 ? char(*t) : '?';
    return out;
}

void Prompts(float right, float y) {
    std::string text;
    if (g_prompt_change) text += "[<>] Change    ";
    text += "[A] Select    [LB/RB] Section    [B] Close";
    ImGui::GetWindowDrawList()->AddText(ImVec2(right - ImGui::CalcTextSize(text.c_str()).x, y), kDim, text.c_str());
}

// the window's own style and type size, the title's put back after it
struct Look {
    ImGuiStyle saved;
    float scale;
    ImFont* font;
    explicit Look(float k) : saved(ImGui::GetStyle()), scale(ImGui::GetIO().FontGlobalScale), font(g_font ? g_font : ImGui::GetFont()) {
        ImGuiStyle s;
        s.WindowPadding = ImVec2(0, 0);
        s.FramePadding = ImVec2(8, 4);
        s.ItemSpacing = ImVec2(6, 2);
        s.ItemInnerSpacing = ImVec2(6, 4);
        s.CellPadding = ImVec2(4, 1);
        s.ScrollbarSize = 14;
        s.GrabMinSize = 12;
        s.DisplaySafeAreaPadding = ImVec2(0, 0);
        Apply(s);
        s.ScaleAllSizes(k);
        ImGui::GetStyle() = s;
        ImGui::GetIO().FontGlobalScale = kBaseFont * k / font->FontSize;
        ImGui::PushFont(font);
    }
    ~Look() {
        ImGui::GetIO().FontGlobalScale = scale;
        ImGui::PopFont();
        ImGui::GetStyle() = saved;
    }
};

void DrawWindow(Settings& s) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    Look look(std::max(1.0f, vp->WorkSize.y / 720.0f));
    ImGuiContext& g = *ImGui::GetCurrentContext();
    constexpr int kCount = int(std::size(kPages));
    static int page = 0;
    static bool busy = false;  // last frame a list was open: its B or Escape was for that

    bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    bool close = back && !popup && !busy;
    busy = popup;
    if (!popup) {
        int d = (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false) ? 1 : 0) -
                (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false) ? 1 : 0);
        if (d) {
            page = (page + d + kCount) % kCount;
            g_focus_first = true;
        }
    }

    // on a very wide screen the window keeps a readable shape; placed again when the screen's size changes while it is
    // open (a new resolution from the Display page)
    static ImVec2 placed_for{0, 0};
    const bool resized = vp->WorkSize.x != placed_for.x || vp->WorkSize.y != placed_for.y;
    placed_for = vp->WorkSize;
    const ImGuiCond place = resized ? ImGuiCond_Always : ImGuiCond_Appearing;
    float height = vp->WorkSize.y * 0.8f, width = std::min(vp->WorkSize.x * 0.6f, height * 1.5f);
    ImGui::SetNextWindowPos(vp->GetCenter(), place, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height), place);
    ImGui::SetNextWindowBgAlpha(1);
    bool open = !close;
    const float em = ImGui::GetFontSize();
    if (ImGui::Begin("##settings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                                ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const float pad = em * 0.6f, head_h = em * 2.1f, foot_h = em * 3.4f, side_w = em * 9.5f;

        // the header: the game's name and the close button
        dl->AddRectFilled(origin, origin + ImVec2(size.x, head_h), kHead);
        dl->AddLine(origin + ImVec2(0, head_h), origin + ImVec2(size.x, head_h), kDark);
        if (g_bold) ImGui::PushFont(g_bold);
        dl->AddText(origin + ImVec2(pad, (head_h - em) * 0.5f), kWhite, (TitleText() + " - Settings").c_str());
        if (g_bold) ImGui::PopFont();
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::SetCursorScreenPos(origin + ImVec2(size.x - head_h, 0));
        if (ImGui::InvisibleButton("##close", ImVec2(head_h, head_h))) open = false;
        ImU32 close_col = ImGui::IsItemHovered() ? kWhite : kText;
        ImVec2 c = origin + ImVec2(size.x - head_h * 0.5f, head_h * 0.5f);
        float x = em * 0.3f;
        dl->AddLine(c - ImVec2(x, x), c + ImVec2(x, x), close_col, 1.5f);
        dl->AddLine(c + ImVec2(-x, x), c + ImVec2(x, -x), close_col, 1.5f);
        ImGui::PopItemFlag();

        // the sections
        ImGui::SetCursorScreenPos(origin + ImVec2(0, head_h + 1));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kSide);
        ImGui::BeginChild("##sections", ImVec2(side_w, size.y - head_h - 1), 0, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
        ImGui::Dummy(ImVec2(0, em * 0.3f));
        for (int i = 0; i < kCount; i++) {
            ImGui::PushID(i);
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 row(side_w, em * 1.9f);
            if (ImGui::InvisibleButton("##section", row) && page != i) {
                page = i;
                g_focus_first = true;
            }
            ImDrawList* side = ImGui::GetWindowDrawList();
            if (page == i) side->AddRectFilled(pos, pos + row, kSelect);
            else if (ImGui::IsItemHovered()) side->AddRectFilled(pos, pos + row, kHover);
            side->AddText(pos + ImVec2(pad, (row.y - em) * 0.5f), page == i ? kWhite : kText, kPages[i].name);
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // the page; a controller's selection always stays in it
        g_help = nullptr;
        g_prompt_change = false;
        ImGui::SetCursorScreenPos(origin + ImVec2(side_w, head_h + 1));
        ImVec2 page_size(size.x - side_w, size.y - head_h - 1 - foot_h);
        ImGui::PushID(page);  // each page's own scroll and selection
        ImGui::BeginChild("##page", page_size);
        ImGuiWindow* page_window = ImGui::GetCurrentWindow();
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && g.NavWindow != page_window) {
            ImGui::SetNavWindow(page_window);
            if (!page_window->NavLastIds[0]) g_focus_first = true;
            else g.NavId = page_window->NavLastIds[0];
        }
        ImGui::Dummy(ImVec2(0, em * 0.2f));
        kPages[page].draw(s);
        ImGui::Dummy(ImVec2(0, em * 0.5f));
        ImGui::EndChild();
        ImGui::PopID();
        g_focus_first = false;  // a page with no rows

        // the footer: the selected row's help and the controller's buttons
        ImVec2 foot = origin + ImVec2(side_w, size.y - foot_h);
        dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(foot, origin + size, kHead);
        dl->AddLine(foot, foot + ImVec2(size.x - side_w, 0), kDark);
        if (g_help) dl->AddText(nullptr, 0, foot + ImVec2(pad, em * 0.35f), kText, g_help, nullptr, size.x - side_w - pad * 2);
        Prompts(origin.x + size.x - pad, origin.y + size.y - em * 1.45f);
    }
    ImGui::End();
    if (!open) ToggleOverlay();
}

}  // namespace

bool OverlayOpen() { return g_open; }

void ToggleOverlay() {
    g_open = !g_open;
    g_focus_first = g_open.load();
    LOGR("settings window %s", g_open ? "opened" : "closed");
}

void Quit() { PostMessageW(static_cast<HWND>(Window()), WM_CLOSE, 0, 0); }

void OverlayInit() {
    if (!kGame.settings_window) return;
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();  // the title's text keeps ImGui's own face
    // Tahoma, as classic Steam, at the size the desktop's height wants; ImGui's face on a machine without it
    wchar_t dir[MAX_PATH];
    UINT len = GetWindowsDirectoryW(dir, MAX_PATH);
    std::filesystem::path fonts = std::filesystem::path(std::wstring(dir, len)) / L"Fonts";
    float px = std::round(kBaseFont * std::max(1.0f, float(Desktop().height) / 720.0f));
    std::error_code ec;
    if (std::filesystem::exists(fonts / L"tahoma.ttf", ec)) g_font = io.Fonts->AddFontFromFileTTF((fonts / L"tahoma.ttf").string().c_str(), px);
    if (std::filesystem::exists(fonts / L"tahomabd.ttf", ec)) g_bold = io.Fonts->AddFontFromFileTTF((fonts / L"tahomabd.ttf").string().c_str(), px);
}

// the controller on ImGui's navigation while the window is open: the left stick moves as the d-pad does. X and Y stay out
// (ImGui's window switcher and on screen keyboard). Back and Start together open and close it.
void FeedPad() {
    if (!kGame.settings_window || !ImGui::GetCurrentContext()) return;
    enum : uint32_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kA = 16, kB = 32, kL1 = 64, kR1 = 128 };
    struct Map {
        uint32_t bit;
        ImGuiKey key;
    };
    static const Map kMap[] = {{kUp, ImGuiKey_GamepadDpadUp},   {kDown, ImGuiKey_GamepadDpadDown}, {kLeft, ImGuiKey_GamepadDpadLeft},
                               {kRight, ImGuiKey_GamepadDpadRight}, {kA, ImGuiKey_GamepadFaceDown},  {kB, ImGuiKey_GamepadFaceRight},
                               {kL1, ImGuiKey_GamepadL1},       {kR1, ImGuiKey_GamepadR1}};
    static uint32_t last = 0;
    static bool combo = false;
    uint32_t held = 0;
    bool both = false;
    for (int port = 0; port < 4; port++) {
        input::Pad p = input::PollRaw(port);
        if (!p.connected) continue;
        constexpr int kStick = 16000;
        if (p.buttons & 1 || p.ly > kStick) held |= kUp;
        if (p.buttons & 2 || p.ly < -kStick) held |= kDown;
        if (p.buttons & 4 || p.lx < -kStick) held |= kLeft;
        if (p.buttons & 8 || p.lx > kStick) held |= kRight;
        if (p.analog[0]) held |= kA;
        if (p.analog[1]) held |= kB;
        if (p.analog[5]) held |= kL1;
        if (p.analog[4]) held |= kR1;
        both |= (p.buttons & 0x10) && (p.buttons & 0x20);
    }
    if (both && !combo) ToggleOverlay();
    combo = both;
    ImGuiIO& io = ImGui::GetIO();
    if (g_open) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    if (!g_open) held = 0;
    for (const Map& m : kMap)
        if (bool(held & m.bit) != bool(last & m.bit)) io.AddKeyEvent(m.key, held & m.bit);
    last = held;
}

void DrawUi(double fps) {
    if (kGame.settings_window && g_open) {
        DrawWindow(Get());
        if (g_dirty && !StepHeld()) {
            g_dirty = false;
            Save();
        }
    } else if (g_dirty) {
        g_dirty = false;
        Save();
    }
    if (kGame.ui) kGame.ui(fps);
}

}  // namespace app
