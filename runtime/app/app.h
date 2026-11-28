#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };
inline constexpr uint32_t kFpsCapRefresh = 1;  // fps_cap: the monitor's refresh rate

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_scale = 200;     // percent of the console's own resolution: 100 is its 480 lines (640 x 480 at 4:3, 854 x 480 at 16:9, 1120 x 480 at 21:9)
    uint32_t fps_cap = 60;           // 0 unlimited, kFpsCapRefresh the monitor's refresh rate
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    bool shader_cache = true;        // compiled shaders and pipelines kept in <exe dir>/shader_cache for the next run
    // the pipelines earlier runs used are made at launch when the driver lacks them (gpu/d3d12.cpp), behind a window
    // that shows itself when this takes a moment; off, the game makes them as it draws (needs shader_cache)
    bool precompile_shaders = true;
    bool custom_textures = true;     // images in textures\replacement\ beside the exe replace the game's
    bool dump_textures = false;      // each texture once into textures\dumps\ beside the exe
    uint32_t texture_dump_format = 0;  // 0 PNG, 1 DDS
    uint32_t aspect = 0;             // 0 from the output's shape, 1 4:3, 2 16:9, 3 fill the screen (kAspects)
    uint32_t msaa = 0;               // 0 none, 1 2 3 MSAA 2x 4x 8x on the scene (kMsaa)
    bool fxaa = false;               // FXAA on the finished frame
    bool smaa = false;               // SMAA on the finished frame, in place of FXAA
    bool upscale_videos = true;      // video frames and stills: bicubic and deinterlaced instead of bilinear
    uint32_t scale_filter = 0;       // the frame to the screen's size: 0 bilinear, 1 sharp (Catmull-Rom)
    bool alpha_coverage = false;     // with MSAA, alpha tested edges (fences, leaves) get partial coverage
    bool low_latency = false;        // a waitable swap chain holding one frame, at the next start
    uint32_t sharpen = 0;            // percent, contrast adaptive sharpening of the finished frame
    // the console's picture: 4:3 at the console's resolution (bars on a wide screen), whatever the aspect and render scale
    // settings say; a title that picks its own shape is left alone
    bool console_look = false;
    // the finished frame as a CRT television showed the console's lines, each in percent (Direct3D 12); replaces FXAA,
    // sharpening and the scaling filter
    bool crt = false;
    uint32_t crt_scanlines = 60;     // the dark gaps between the console's lines
    uint32_t crt_softness = 50;      // how far a pixel's light bleeds sideways
    uint32_t crt_mask = 30;          // the red, green and blue phosphor dots
    uint32_t crt_curve = 0;          // curved glass: bent edges, dark corners
    uint32_t audio_output = 0;         // audio::kOutputAuto, Stereo, Surround or Mono
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
    // a controller's sticks, left [0] and right [1], before the game reads them: a dead zone (percent of the throw from
    // the centre that reads as centred, the rest stretched to the full throw; the game's own dead zone still follows)
    // and a sensitivity (percent: 50 gives half the deflection, so half the turning speed at full throw; above 100 full
    // deflection comes sooner). 0 and 100 leave the stick as it reads
    uint32_t stick_deadzone[2] = {0, 0};
    uint32_t stick_sensitivity[2] = {100, 100};
    // the mouse moves controller 1's right stick, for a title that offers it (GameInfo::mouse_look): the cursor is
    // held in the window while the game has it
    bool mouse_look = true;
    uint32_t mouse_sensitivity = 100;  // percent
    bool mouse_invert_y = false;
};

Settings& Get();
void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
Mode CachedOutput();  // Output, looked up again only when the resolution setting or the window's monitor changes: cheap every frame
float RenderScale(uint32_t guest_height);
// the output is 3:2 or wider, or the aspect setting asks for 16:9: the console reports a widescreen TV and the frame is
// shown 16:9, else 4:3 as on a 4:3 TV. A title reads the TV's shape once, at boot.
bool Widescreen();
inline constexpr const char* kAspects[] = {"auto", "4:3", "16:9", "screen"};
inline constexpr uint32_t kAspectCount = 4;
inline constexpr const char* kMsaa[] = {"off", "msaa2", "msaa4", "msaa8"};
inline constexpr uint32_t kMsaaCount = 4;
// A title that picks the shown aspect ratio itself sets this (any thread): width over height of the picture the frame
// is fitted into, black bars around it; 0 follows the output, see Widescreen
inline std::atomic<float> g_aspect{0};
// With g_aspect set wider than 16:9: the title widens its 3D views to it, and the renderer keeps the 2D in a centred
// 16:9 part as for FillsScreen, instead of stretching everything across the picture
inline std::atomic<bool> g_wide_2d{false};
// A title that draws its 3D for 4:3 whatever the shown aspect sets this: the renderer narrows the vertex program and
// fixed function 3D of the screen shaped targets about the middle by 4/3 over Aspect(), and the stretch to the picture
// widens it back, with more of the world at the sides instead of a stretched one. Its 2D is laid out for 4:3 too, so
// at 16:9 (with g_wide_2d) the 2D is kept in a centred 4:3 part. D3D12 backend only.
inline std::atomic<bool> g_widen_3d{false};
// A title whose 2D (HUD, menus) is laid out for 4:3 even though its 3D is widened sets this: at any aspect the renderer
// keeps the 2D in a centred 4:3 part (needs g_wide_2d or the fill the screen setting), so gauges and text stay in
// proportion. D3D12 backend only.
inline std::atomic<bool> g_hud_4x3{false};
inline std::atomic<bool> g_native_render{false};  // render at the console's resolution, whatever the render scale
// g_aspect, else the output's shape when the aspect setting fills the screen (never under 16:9), else 16:9 when
// Widescreen, else 4:3
float Aspect();
// the aspect setting fills the screen (ultrawide) and the title has not picked a shape itself, or has asked for the same
// with g_wide_2d: the renderer stretches the screen shaped targets across and keeps the 2D in a centred 16:9 part. A
// title widens its own 3D views.
bool FillsScreen();

std::filesystem::path ExeDir();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
std::string Sha1(const std::vector<uint8_t>& data);  // lower case hex
// the recompiled code, <exe>.dll, built from the game in assets when missing or out of date (game_code.cpp) and
// loaded; false after telling the player why the game cannot start, or when they chose not to start it yet
bool PrepareGameCode(const GameFiles& game);
// the window's icon from the xbe's own title image ($$XTIMAGE, title_icon.cpp); the default icon stays without one
void SetTitleIcon(void* hwnd, const std::vector<uint8_t>& xbe_file);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
// the shader pre-compile's window (precompile.cpp), for the renderer at launch; it has its own thread and shows itself
// only once the work takes a moment. new_driver: the pipelines were ready for another driver than this one
void OpenPrecompileWindow(uint32_t total, bool new_driver);
void PrecompileStep();         // one more done, any thread
bool PrecompileSkipped();      // Skip was pressed: the rest compile as the game meets them
void ClosePrecompileWindow();  // and with "Don't show this again" ticked, turns precompile_shaders off and saves
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
    // the settings window's row: a name and a line of help in its footer; a value with choices (null terminated) is a list
    const char* label = nullptr;
    const char* help = nullptr;
    const char* const* choices = nullptr;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    // the game's first presented frame onward: true keeps the game waiting, the ui still drawn each frame
    bool (*hold)();
    // the host folder of the console's hard disk (its partitions, the saves on E:), given the folder's name: hdd, or
    // hdd2 and on for a second copy (Instance). Once, before boot; null keeps it beside the exe
    std::filesystem::path (*hard_disk)(const std::wstring& folder);
    // xbe file name, SHA-1: other releases the executable runs, each from its own [xbe] named config
    std::vector<std::pair<const char*, const char*>> releases;
    // the game's own dead zone on each stick axis, as a part of full throw: the stick sensitivity scales the throw past it
    float stick_deadzone = 0;
    // the host's settings window: F1, or Back and Start together on a controller. Off for a title that draws its own
    // settings in ui
    bool settings_window = false;
    // the mouse can play controller 1's right stick (Settings::mouse_look): a shooter's camera and aim
    bool mouse_look = false;
    // the key that saves the frame as a PNG (Windows virtual key; 0 off); a title that uses F12 itself moves it
    uint32_t screenshot_key = 0x7B;  // VK_F12
};
extern const GameInfo kGame;

// the settings window (overlay.cpp). DrawUi is the renderers' each frame inside the ImGui frame, UiLock held: the
// window, then GameInfo::ui. FeedPad runs after the backends' NewFrame and before ImGui::NewFrame.
bool OverlayOpen();
// F12 (GameInfo::screenshot_key): the renderers save the next frame, before they draw the interface (screenshot.cpp)
void RequestScreenshot();
bool TakeScreenshotRequest();
void SaveScreenshot(std::vector<uint8_t> bgra, uint32_t w, uint32_t h);  // bgra: rows top down, w * h * 4 bytes
bool ScreenshotJustSaved();  // for a few seconds after one is written
void ToggleOverlay();  // any thread
void OverlayInit();    // window thread, after ImGui's context
void FeedPad();
void DrawUi(double fps);
void Quit();  // closes the game window, as its close button does

// sleeps out the rest of the frame for the fps cap and for the console's pace: the swap interval counts vertical
// blanks of the 60 Hz the console ran at (2 is 30 frames a second); a title's own frame rate, when set, replaces it
void Pace(uint32_t interval = 1);
void SetEngineFps(uint32_t fps);
uint32_t RefreshRate();  // the game window's monitor's refresh rate in Hz, 0 when unreadable

// A title that steps its game once per Swap sets these (any thread): the steps per second Pace holds to instead of
// the fps cap (0: the cap), and how many steps make one shown frame (1: every step is shown). Above the console's 60 it
// also calls SetEngineFps with the rate, or Pace holds the steps back to 60. Such a title takes its rate from the fps
// cap and the refresh rate itself (see Settings::fps_cap), as nothing else slows or speeds its game.
inline std::atomic<uint32_t> g_step_rate{0};
inline std::atomic<uint32_t> g_show_every{1};

// Blanks per second that D3DDevice_BlockUntilVerticalBlank waits on: the console's 60, or the title's own rate.
inline std::atomic<uint32_t> g_vblank_rate{60};

}  // namespace app
