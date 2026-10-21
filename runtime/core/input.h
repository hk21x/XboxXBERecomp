#pragma once
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
void Rumble(int port, uint16_t left, uint16_t right);
// the physical controls held on any controller, a bit per Control, from the last polls (the settings window's
// navigation)
uint32_t HeldControls();

// a steering wheel through DirectInput (core/wheel.cpp), merged into controller 1
// what the game tells the force feedback, each poll of controller 1 (GameInfo::wheel)
struct WheelFeel {
    bool racing = false;  // the player drives: the wheel's forces follow the car, else a light centring
    float speed = 0;      // m/s
};
struct WheelInfo {
    bool connected = false, ffb = false;
    std::string name, effects;  // effects: the force feedback ones made
    int axes_count = 8, buttons_count = 0;
    int32_t axes[8] = {};       // raw, -32768 to 32767: X Y Z Rx Ry Rz slider 1, slider 2
    std::vector<int> pressed;   // buttons held
    int pov = -1;               // hat in hundredths of a degree, -1 centred
    float steer = 0, throttle = 0, brake = 0, clutch = 0;  // as the game gets them: steer -1 to 1, pedals 0 to 1
};
void WheelInit(void* hwnd);  // window thread, with the game window (force feedback needs it)
// the wheel's console inputs, 0 to 1 each in Control order; false when there is none
bool WheelRead(float* v);
void WheelFeedback(const WheelFeel& feel, uint16_t rumble_left, uint16_t rumble_right);
WheelInfo Wheel();           // the settings window's view
void WheelTestForce(float level);  // -1 left to 1 right, for half a second

}  // namespace input
