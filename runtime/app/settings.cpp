// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.shader_cache = t["shader_cache"].value_or(s.shader_cache);
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    std::string aspect = t["aspect"].value_or(std::string(kAspects[std::min(s.aspect, kAspectCount - 1)]));
    for (uint32_t i = 0; i < kAspectCount; i++)
        if (aspect == kAspects[i]) s.aspect = i;
    std::string msaa = t["msaa"].value_or(std::string(kMsaa[std::min(s.msaa, kMsaaCount - 1)]));
    for (uint32_t i = 0; i < kMsaaCount; i++)
        if (msaa == kMsaa[i]) s.msaa = i;
    s.fxaa = t["fxaa"].value_or(s.fxaa);
    s.sharpen = std::min<uint32_t>(t["sharpen"].value_or(s.sharpen), 100);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    s.audio_output = std::min(t["audio_output"].value_or(s.audio_output), audio::kOutputMono);
    for (int i = 0; i < 2; i++) {
        const std::string side = i ? "right_stick_" : "left_stick_";
        s.stick_deadzone[i] = std::min<uint32_t>(t[side + "deadzone"].value_or(s.stick_deadzone[i]), 50);
        s.stick_sensitivity[i] = std::clamp<uint32_t>(t[side + "sensitivity"].value_or(s.stick_sensitivity[i]), 25, 300);
    }
    s.mouse_look = t["mouse_look"].value_or(s.mouse_look);
    s.mouse_sensitivity = std::clamp<uint32_t>(t["mouse_sensitivity"].value_or(s.mouse_sensitivity), 10, 1000);
    s.mouse_invert_y = t["mouse_invert_y"].value_or(s.mouse_invert_y);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
    audio::SetOutputMode(s.audio_output);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"shader_cache", s.shader_cache},
                  {"custom_textures", s.custom_textures},
                  {"dump_textures", s.dump_textures},
                  {"texture_dump_format", int64_t(s.texture_dump_format)},
                  {"aspect", kAspects[std::min(s.aspect, kAspectCount - 1)]},
                  {"msaa", kMsaa[std::min(s.msaa, kMsaaCount - 1)]},
                  {"fxaa", s.fxaa},
                  {"sharpen", int64_t(s.sharpen)},
                  {"join_ip", s.join_ip},
                  {"audio_output", int64_t(s.audio_output)},
                  {"left_stick_deadzone", int64_t(s.stick_deadzone[0])},
                  {"right_stick_deadzone", int64_t(s.stick_deadzone[1])},
                  {"left_stick_sensitivity", int64_t(s.stick_sensitivity[0])},
                  {"right_stick_sensitivity", int64_t(s.stick_sensitivity[1])}};
    if (kGame.mouse_look) {
        t.insert("mouse_look", s.mouse_look);
        t.insert("mouse_sensitivity", int64_t(s.mouse_sensitivity));
        t.insert("mouse_invert_y", s.mouse_invert_y);
    }
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

// written beside the real file and renamed over it, so a crash mid save keeps the old settings
void Save() {
    std::filesystem::path tmp = File();
    tmp += ".tmp";
    std::ofstream f(tmp);
    f << Table() << "\n";
    f.close();
    if (f) MoveFileExW(tmp.c_str(), File().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

// Output for every frame: the desktop mode lookup reads the registry, so it runs again only when the resolution
// setting or the window's monitor changes
Mode CachedOutput() {
    struct Key {
        uint32_t width;
        uint32_t height;
        HMONITOR monitor;
        bool operator==(const Key&) const = default;
    };
    static std::mutex lock;
    static Key key{};
    static Mode mode{};
    const Settings& s = Get();
    Key now{s.width, s.height, MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY)};
    std::lock_guard<std::mutex> guard(lock);
    if (!(now == key) || !mode.height) {
        key = now;
        mode = Output();
    }
    return mode;
}

// asked per frame and per HUD quad: CachedOutput costs a monitor lookup, the registry only on a change
bool Widescreen() {
    if (float a = g_aspect; a > 0) return a >= 1.5f;
    const Settings& s = Get();
    if (s.aspect) return s.aspect >= 2;
    Mode m = CachedOutput();
    return m.width * 2 >= m.height * 3;  // auto: 3:2 or wider
}

float Aspect() {
    if (float a = g_aspect; a > 0) return a;
    if (Get().aspect == 3) {
        Mode m = CachedOutput();
        return m.height ? std::max(16.0f / 9.0f, float(m.width) / float(m.height)) : 16.0f / 9.0f;
    }
    return Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
}

bool FillsScreen() { return g_aspect.load() <= 0 && Get().aspect == 3; }

float RenderScale(uint32_t guest_height) {
    if (g_native_render) return 1.0f;
    float h = CachedOutput().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
