// no DirectInput: no steering wheels or rigs past what SDL reads as a controller (macOS)
#include "core/input.h"

namespace input {

void WheelInit(void*) {}
void WheelSetup() {}
bool WheelRead(float*, bool* steers) {
    if (steers) *steers = false;
    return false;
}
void WheelFeedback(const WheelFeel&, uint16_t, uint16_t) {}
WheelInfo Wheel() { return {}; }
void WheelTestForce(float) {}

}  // namespace input
