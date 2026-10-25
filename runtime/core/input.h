#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// every input of the console controller, each 0 to 1: buttons, triggers, and each stick direction on its own
enum Control : int {
    kUp, kDown, kLeft, kRight, kStart, kBack, kLThumb, kRThumb,  // the digital buttons, as Pad::buttons bits
    kA, kB, kX, kY, kBlack, kWhite, kLT, kRT,                    // the analog buttons, as Pad::analog
    kLSLeft, kLSRight, kLSUp, kLSDown, kRSLeft, kRSRight, kRSUp, kRSDown,
    kControls
};
const char* ControlName(int c);  // settings file name, "dpad_up" ...
const char* ControlLabel(int c); // shown to the player, "D-pad up" ...
int ControlByName(const char* name);  // -1 when unknown

void Init();  // window thread, before boot
// the controller on that port, then GameInfo::pad
Pad Poll(int port);
// raw mouse movement, from the window thread (WM_INPUT): read into controller 1's right stick (Settings::mouse_look)
void MouseMoved(int dx, int dy);
// the mouse plays the right stick now: mouse look on, the game window in front, the settings window shut
bool MouseLooking();
// for a title taking the mouse itself (GameInfo::mouse_direct): the movement since the last call as a rate, in full
// stick deflections (2400 counts a second at 100% sensitivity is 1), up positive, the settings' sensitivity and invert
// applied; false and nothing taken while the mouse is not looking
bool TakeMouse(float& x, float& y);
// what controller 1 was last played with: true for the keyboard and mouse (a bound key or mouse button pressed, the
// mouse moved while it looks; the keyboard alone when no controller is plugged in), false for a controller or wheel
// (a button pressed, a stick pushed half way); a title's button pictures may follow it
bool UsingKeyboard();
// the keys controller 1's control c is bound to: the settings', else the defaults (0 none; mouse buttons VK_LBUTTON ..)
std::array<uint16_t, 2> ControlKeys(int c);
// the mouse for a title's own menus, while it looks (MouseLooking): the wheel's notches since the last call (up
// positive), and the buttons held (1 left, 2 right, 4 middle)
int TakeWheel();
uint32_t MouseButtons();
void MouseWheel(int delta);  // window thread, WHEEL_DELTA (120) a notch
void Rumble(int port, uint16_t left, uint16_t right);
// the physical controls held on any controller, a bit per Control, from the last polls (the settings window's
// navigation)
uint32_t HeldControls();

// a steering wheel and the rest of a rig through DirectInput (core/wheel.cpp): every game controller DirectInput lists
// but Xbox pads (SDL's), each control bound to one of them (app::Settings::WheelBind), merged into controller 1
constexpr int kGearUnknown = -100;
// what the game tells the force feedback and the shifter, each poll of controller 1 (GameInfo::wheel)
struct WheelFeel {
    bool racing = false;     // the player drives: the wheel's forces follow the car, else a light centring
    float speed = 0;         // m/s
    int gear = kGearUnknown;  // the player's gear: -1 reverse, 0 neutral, 1 first ...; kGearUnknown when not known
    int shift_up = -1, shift_down = -1;  // the console controls (Control) that change gear, for an H-pattern shifter
};
struct WheelDevice {  // one device, as the settings window shows it
    std::string name, guid;  // DirectInput's product name, the instance GUID as text
    bool ffb = false;        // it offers force feedback
    bool feedback = false;   // the forces go to it (the steering's device)
    int axes_count = 0, buttons_count = 0, povs_count = 0;
    uint8_t axis_mask = 0;   // the axes it has, a bit each in the order below (a pedal box may have Z, Rz and a slider)
    int32_t axes[8] = {};    // raw, -32768 to 32767: X Y Z Rx Ry Rz slider 1, slider 2
    std::vector<int> pressed;  // buttons held
    int pov = -1;            // its first hat in hundredths of a degree, -1 centred
};
struct WheelInfo {
    std::vector<WheelDevice> devices;
    bool active = false;     // some binding reads a device: the rig plays as controller 1
    std::string effects;     // the force feedback effects made
    float steer = 0, throttle = 0, brake = 0, clutch = 0;  // as the game gets them: steer -1 to 1, pedals 0 to 1
    int gear_selected = kGearUnknown, gear_game = kGearUnknown;  // the shifter's position and the car's gear
};
void WheelInit(void* hwnd);  // with the game window (force feedback needs it): starts the wheel thread
void WheelSetup();           // the bindings again from app::Get(), after the settings window changed them
// the rig's console inputs, 0 to 1 each in Control order; false when no binding reads a device
bool WheelRead(float* v);
void WheelFeedback(const WheelFeel& feel, uint16_t rumble_left, uint16_t rumble_right);
WheelInfo Wheel();           // the settings window's view
void WheelTestForce(float level);  // -1 left to 1 right, for half a second

}  // namespace input
