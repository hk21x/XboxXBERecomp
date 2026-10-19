#pragma once
#include <cstdint>

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

}  // namespace input
