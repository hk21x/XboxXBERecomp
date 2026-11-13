#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/achievements.h"

namespace input {
struct Pad;
struct WheelFeel;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0: only the title's present interval
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
    // 0 from the output's shape, 1 4:3, 2 16:9 (the console's TV setting, at boot), 3 widescreen filling the screen's
    // shape (ultrawide: the title's 3D views widened by its GameInfo code, the frame shown at the screen's shape)
    uint32_t aspect = 0;
    // controls, by input::Control: the physical pad input that drives each console input, and up to two keys (Windows
    // virtual key codes, 0 none) that press it on controller 1
    std::vector<uint8_t> pad_map;    // empty: each input drives itself
    std::vector<std::array<uint16_t, 2>> keys;  // empty: the default keys
    bool keyboard = true;
    // the mouse moves controller 1's right stick, for a title that offers it (GameInfo::mouse_look): the cursor is
    // held in the window while the game has it
    bool mouse_look = true;
    uint32_t mouse_sensitivity = 100;  // percent
    bool mouse_invert_y = false;
    uint32_t monitor = 0;            // 0 the primary display, n the nth display Windows lists
    bool pause_unfocused = false;    // the game stands still, silent, while its window is not in front
    bool show_fps = false;           // frames a second in a corner
    bool sharp_2d = true;            // GameInfo::sharpen_2d: the HUD's, menus' and text's pictures enlarged sharp
    uint32_t msaa = 0;               // 0 none, 1 2 3 MSAA 2x 4x 8x on the scene
    bool fxaa = false;               // FXAA on the finished frame
    bool smaa = false;               // SMAA on the finished frame, in place of FXAA
    uint32_t scale_filter = 0;       // the frame to the screen's size: 0 bilinear, 1 sharp (Catmull-Rom)
    bool alpha_coverage = false;     // with MSAA, alpha tested edges (fences, leaves) get partial coverage
    bool world_textures = false;     // the 3D scene's colour textures enlarged twice on a background thread
    bool low_latency = false;        // a waitable swap chain holding one frame, at the next start
    uint32_t sharpen = 0;            // percent, contrast adaptive sharpening of the finished frame
    uint32_t renderer = 0;           // 0 Direct3D 12, 1 Direct3D 11, at the next start
    uint32_t language = 1;           // the console's language (XC_LANGUAGE, GameInfo::languages), at the next start
    // a steering wheel and its kit (core/wheel.cpp), playing as controller 1: each control on any DirectInput device,
    // named by the device (and its instance, telling two of the same apart), then an axis (0-7: X Y Z Rx Ry Rz,
    // slider 1, slider 2) or a button (0-127). No device: the wheel found by itself (the first driving device; other
    // devices are bound by name). A pedal reads 0 at rest and 1 at full, from those raw values (-32768 to 32767).
    struct WheelBind {
        std::string device, guid;
        int32_t axis = -1, button = -1;
        int32_t rest = 32767, full = -32768;
        bool invert = false;  // steering: turning right reads negative
        bool operator==(const WheelBind&) const = default;
    };
    // the defaults, a Logitech G920/G923/G29: steering X, throttle Y, brake Rz, clutch slider 1, pedals released at the top
    WheelBind wheel_steer{.axis = 0}, wheel_throttle{.axis = 1}, wheel_brake{.axis = 5}, wheel_clutch{.axis = 6};
    uint32_t wheel_range = 900;      // the wheel's rotation lock to lock, as its driver sets it (Logitech G HUB: 900)
    uint32_t wheel_lock = 360;       // degrees of wheel turn for the game's full steering
    uint32_t wheel_deadzone = 0;     // percent of the steering around the centre that reads as straight
    std::vector<WheelBind> wheel_buttons;  // per input::Control: the button that presses it
    std::vector<WheelBind> wheel_gears;    // an H-pattern shifter's positions: R, 1 to 7 (kGears)
    uint32_t ffb_strength = 70;      // force feedback overall, percent, 0 off
    uint32_t ffb_road = 50;          // the game's rumble (kerbs, impacts) felt in the wheel, percent
};
// the wheel buttons of a Logitech G920/G923 (Xbox) for each input::Control, -1 none
int8_t DefaultWheelButton(int c);
constexpr int kGears = 8;  // an H-pattern shifter's positions: R, 1 to 7
const char* GearName(int g);  // "R", "1" ... "7"
// the default keys for input::Control c
std::array<uint16_t, 2> DefaultKeys(int c);
std::string KeyToName(uint16_t vk);  // settings file names of Windows virtual keys
uint16_t KeyFromName(const std::string& name);
// the in-game settings window (F1): open, and drawn each frame inside the ImGui frame (overlay.cpp)
bool OverlayOpen();
void ToggleOverlay();
void DrawOverlay();
void OverlayInit();  // the fonts, after ImGui's context and before a renderer's first frame
// a key pressed while the overlay waits for one to bind (window thread); true takes it
bool OverlayKey(uint16_t vk);

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
void* Monitor();                  // HMONITOR of the monitor setting (the primary display when it is gone)
std::vector<std::string> Monitors();  // the displays' names, in the monitor setting's order from 1
std::vector<Mode> ListModes();        // the monitor setting's display modes, widest first, each size once
bool Paused();                    // pause_unfocused and the window is not in front (Windows)
// F12: the renderers save the next frame, before they draw the interface (app/screenshot.cpp)
void RequestScreenshot();
bool TakeScreenshotRequest();
void SaveScreenshot(std::vector<uint8_t> bgra, uint32_t w, uint32_t h);
bool ScreenshotJustSaved();  // for a few seconds after one is written
void DrawHud(double fps);         // the frame counter, inside the ImGui frame (overlay.cpp)
void FeedPad();                   // controllers to the settings window's navigation, before each ImGui frame
Mode Output();
float RenderScale(uint32_t guest_height);
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV;
// a screen narrower than 16:9 that a title fills (GameInfo::fills_narrow_screens) is a 4:3 TV, widened
bool Widescreen();
// the shape the frame is shown at: 4:3, 16:9, or the screen's when the aspect setting fills it (never under 16:9, but
// for a title filling a screen between 4:3 and 16:9: 16:10 shows the title's 4:3 mode widened, not 16:9 letterboxed)
float FrameAspect();
// the shape the title's 2D is laid out for in the mode the console reports: GameInfo::hud_aspect, 4:3 on a 4:3 TV
float HudAspect();

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
// the same with no window, its progress as lines on stdout for the launcher ("progress <stage> <files done> <files> <bytes
// done> <bytes>", then "ready" or "failed <why>"), for --prepare-code; the process's exit code
int PrepareGameCodeHeadless(const GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// leave the game as closing its window does (the settings window's Quit, a title's own quit item); from any thread
void Quit();
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
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
    const char* label = nullptr;  // the settings window's name for it, else the key
    const char* help = nullptr;   // a line under it in the settings window
    const char* const* choices = nullptr;  // a value's names, ended by nullptr: a dropdown in the settings window
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
    std::vector<achievements::Def> achievements = {};  // core/achievements.h
    // other releases' xbe SHA-1s whose code and data are the recompiled one's, differing only in the signature and
    // certificate (region, version, keys): started without the "different release" question
    std::vector<const char*> same_code = {};
    // a steering wheel's force feedback: what the player's car is doing, each poll of controller 1 (input.h)
    void (*wheel)(input::WheelFeel& feel) = nullptr;
    // the level (0 to 1) of voices heard only through the effect sends, which the mixer plays into the speakers in
    // place of the DSP image's effects: a title whose image applies a volume there (PGR2's radio, at the music
    // volume) reports it here, read on the audio thread at each mix; none is full level
    float (*send_level)() = nullptr;
    // movies step one frame per game frame and the host holds a frame BinkWait says is early (xdk/bink.cpp); off for a
    // title whose movie loop waits on BinkWait itself
    bool bink_frame_hold = true;
    // the shape the title lays out its 2D for (HUD, menus): shown wider, that 2D is squeezed into it (gpu::HudSqueeze)
    float hud_aspect = 16.0f / 9.0f;
    // that 2D also where drawn by a vertex program that only moves its position input to the screen (gpu::
    // ScreenProgram: EoN's HUD and menus, through D3D's pass-through program form), or that multiplies it by a 2D
    // orthographic matrix (gpu::MatrixProgram, OrthoConstants: EoN's driving HUD)
    bool squeeze_screen_programs = false;
    // ultrawide: a textured bar the frame's full height ending on its left or right edge (NightFire's sniper scope: a
    // black 32x32 texture from x 0 to 81 and 559 to 640 beside the sight) keeps that edge, as an untextured one always
    // does (gpu::DrawSpan), so the bars reach the screen's sides; off, they are squeezed with the rest of the 2D
    bool edge_bars = false;
    // ultrawide: an untextured fill ending on the frame's left or right edge (EoN's sniper scope) keeps that edge (gpu::
    // DrawSpan); off, it is squeezed with the rest of the 2D (PGR2's HUD backdrops behind the times and the speed)
    bool edge_fills = true;
    // a draw under a multisample mode asking for more samples than its target was made with (gpu::State::aa_x, aa_y)
    // lands that many samples to a pixel: the console's SetViewport then halves the viewport it gives the vertex
    // programs (c-38, c-37), so the draw lands where the others do. True applies that (Juiced's glow materials set
    // D3DRS_MULTISAMPLERENDERTARGETMODE 4X on its 640x960 scene target around their draws)
    bool sample_scaled_viewport = false;
    // the textures 2D draws use (pretransformed or as above; swizzled, up to 512 texels) are enlarged two or four times
    // with sharp edges (gpu::Sharpen2D, a background thread) and drawn so: text and HUD pictures stay crisp at high
    // resolutions (Settings::sharp_2d turns it off)
    bool sharpen_2d = false;
    // the languages a title has on its disc, as the console's setting numbers them (1 English, 2 Japanese, 3 German,
    // 4 French, 5 Spanish, 6 Italian, 7 Korean, 8 Chinese, 9 Portuguese): the settings window offers them; none, English
    std::vector<uint32_t> languages;
    // a PAL release (the certificate's game regions without North America) runs on a PAL console with PAL 60 Hz on:
    // a title naming its files by the video standard finds the disc's (EoN's movies are _pal and _palw only)
    bool pal_console = false;
    // disc reads paced as a drive of this many MB a second reads (the console's about 6), 0 at once; XBE_DISC_RATE
    // overrides (EoN's driving streams stall for good on reads that complete at once)
    float disc_rate = 0;
    // a file the title asks to open (its Xbox path, as named) is reported missing when this says so: a title that
    // passes over a missing movie skips it (EoN's logos, by a setting)
    bool (*hide_file)(const std::string& xbox_path) = nullptr;
    // the mouse can play controller 1's right stick (Settings::mouse_look): a shooter's camera and aim
    bool mouse_look = false;
    // the title's code takes the mouse's movement itself (input::TakeMouse) rather than as the right stick
    bool mouse_direct = false;
    // the keys each input::Control starts with (two Windows virtual keys, 0 none), else the SDK's (DefaultKeys)
    std::array<uint16_t, 2> (*default_keys)(int c) = nullptr;
    // a texture the title binds (SetTexture, its guest D3DBaseTexture) with its stage's palette (D3DPalette, 0 none), on
    // the title's thread before the draws that use it, and again when the stage's palette is set (SetPalette): the
    // title may change its texels in guest memory (EoN's button pictures), true when it did, and may give the
    // renderers a larger picture of its own for it (gpu::SetReplacement)
    bool (*texture_bound)(uint32_t header, uint32_t palette) = nullptr;
    // the title draws one frame a step of its game, and the frame cap asks for more: frames in between (gpu/interp.cpp), while true
    bool (*step_interpolation)() = nullptr;
    // in-between frames draw at the step's place a draw that samples a target drawn this step by draws that cannot be blended
    // (a reflection pass drawn from a mirrored view each step: blended, what samples it slid against it, back and forth every
    // other step: EoN's driving floor); off, every matched draw is blended
    bool interp_sampled_targets_at_step = false;
    // a vertex stream over a target the GPU drew takes its newest finished copy (a frame or so old) instead of waiting for the GPU
    // (EoN's driving draws a 128x128 target each frame and draws vertices from it: read back at once, the title waited about 7 ms
    // a frame behind the in-between frames, and lost the second). Off, it is read back at once (NightFire's scope)
    bool vertex_read_back_late = false;
    // while true, those frames carry on past the step instead of leading up to it: no delay, for what the player aims
    bool (*step_ahead)() = nullptr;
    // the title draws its world a few vertices at a time with nothing changed between (Agent Under Fire: thousands of draws a
    // frame, nine in ten alike): those runs reach the renderer as one draw, each costing its time per draw (render_thread.cpp)
    bool merge_draws = false;
    // the aspect setting's "from the screen's shape" follows an ultrawide screen instead of stopping at 16:9
    bool auto_fills_screen = false;
    // the title's 3D views follow FrameAspect when it changes (PGR2 reshapes its cameras): in a widescreen boot, a choice
    // between 16:9 and filling the screen applies at once; 4:3 against widescreen still waits for the next start
    bool fill_follows_live = false;
    // while frames run late (slower than 120 a second), each face of a cube map the GPU draws (a car's reflections) is
    // drawn at most this often a second and keeps its last picture in between; frames on time draw every face. 0 draws
    // them every frame always. XBE_CUBE_FACE_HZ overrides it
    float cube_face_hz = 0;
    // the title widens its 3D views to FrameAspect() from its 4:3 mode too (as from 16:9): a screen between 4:3 and 16:9
    // (16:10, the Steam Deck's 1280x800) is filled, the console reporting a 4:3 TV and the 2D kept 4:3 in the middle,
    // where the frame was 16:9 with bars above and below; set once the title's views follow it
    bool fills_narrow_screens = false;
    // a title's music decoded to PCM streams against its other sounds, read on the audio thread at each mix (effects the
    // DSP image would have balanced drown it out): the level of PCM streams and of every other voice; none is full level
    float (*pcm_stream_level)() = nullptr;
    float (*other_voice_level)() = nullptr;
    // the level of the title's 3D voices (its engines and effects placed in the world) against its music and menu
    // sounds, read on the audio thread at each mix; none is full level. Juiced's 3D voices have no headroom where its
    // 2D ones have the library's 6 dB, and a race came out 15 dB over the menus, clipping: a setting there
    float (*voice_3d_level)() = nullptr;
    // the title's 3D sound positions have +x to the left (DirectSound's is to the right): Project Gotham Racing's head relative
    // ones do, so a car passing on the right was heard on the left
    bool sound_x_left = false;
    // a fixed-function stage with no texture reads (0,0,0,0) as its texture, as the hardware's stage with texture mode NONE
    // does, not white: True Crime clears its shadow map's border with untextured lines that select the texture
    bool unbound_texture_zero = false;
    // 2D squeezed as squeeze_screen_programs' also where a program reaches its position from v0 and constants by moves,
    // multiplies and adds alone, through its registers (gpu::FlatProgram): Juiced's 2D is (v0 + c) * c + c, then the
    // viewport's scale and offset, for the HUD, the menus and the full screen effects alike
    bool squeeze_flat_programs = false;
    // how wide a line is drawn, in console pixels (the console's is 1): a title whose lines are fine detail (Juiced's
    // overhead wires, line strips in the scene) keeps them fine at a PC's resolution with less
    float line_width = 1;
};
extern const GameInfo kGame;

// sleeps out the rest of the frame: the title's present interval (vertical blanks a frame at 60 Hz, 0 unpaced)
// within the fps cap
void Pace(uint32_t vblanks = 1);
// frames a second for a title asking for vblanks within cap (0 unlimited); 0 unpaced
// the display rate a present interval counts in: the console's 60, a title drawing faster sets more (its cap still applies)
inline std::atomic<uint32_t> g_pace_hz{60};
inline uint32_t PaceRate(uint32_t cap, uint32_t vblanks) {
    if (!vblanks) return cap;
    uint32_t fps = g_pace_hz.load(std::memory_order_relaxed) / vblanks;
    return cap && cap < fps ? cap : fps;
}

}  // namespace app
