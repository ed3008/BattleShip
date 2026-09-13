// virtual_gamepad.cpp — SDL virtual gamepad shared by the touch overlays.
//
// Extracted from port/android_touch_overlay.cpp, which proved the approach
// on Android. Nothing here is platform-specific; the overlays differ only
// in where touches come from and who draws the buttons.

#include "virtual_gamepad.h"
#include "port_log.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_joystick.h>

#include <atomic>

namespace {

SDL_Joystick* sJoystick = nullptr;
int sDeviceIndex = -1;
std::atomic<bool> sAttachAttempted{ false };

// Xbox-360-style descriptor. SDL2 promotes any virtual joystick carrying
// this vendor/product signature to SDL_GameController, so LUS's existing
// SDLButtonToAnyMapping pipeline picks us up without changes.
constexpr int kAxisCount = SDL_CONTROLLER_AXIS_MAX;     // 6
constexpr int kButtonCount = SDL_CONTROLLER_BUTTON_MAX; // 15

} // namespace

extern "C" {

bool port_vgamepad_ensure(void) {
    if (sJoystick != nullptr) {
        return true;
    }
    // One attempt per process. A second caller while the first is still in
    // SDL must not race into a duplicate attach.
    if (sAttachAttempted.exchange(true)) {
        return sJoystick != nullptr;
    }

    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = kAxisCount;
    desc.nbuttons = kButtonCount;
    desc.vendor_id = 0x045E;
    desc.product_id = 0x028E;
    desc.name = "SSB64 Touch Overlay";

    sDeviceIndex = SDL_JoystickAttachVirtualEx(&desc);
    if (sDeviceIndex < 0) {
        port_log("SSB64: virtual gamepad attach failed: %s\n", SDL_GetError());
        return false;
    }
    sJoystick = SDL_JoystickOpen(sDeviceIndex);
    if (sJoystick == nullptr) {
        port_log("SSB64: virtual gamepad open(%d) failed: %s\n", sDeviceIndex, SDL_GetError());
        SDL_JoystickDetachVirtual(sDeviceIndex);
        sDeviceIndex = -1;
        return false;
    }

    // SDL_GameController normalizes the trigger axes through the standard
    // Xbox-360 mapping ("lefttrigger:a4,righttrigger:a5"): raw -32768 reads
    // as released, raw +32767 as fully pressed. SDL_JoystickAttachVirtual
    // defaults every axis to 0, which lands the triggers at ~50% — LUS would
    // see N64 Z and R held down from launch. Park them explicitly.
    SDL_JoystickSetVirtualAxis(sJoystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    SDL_JoystickSetVirtualAxis(sJoystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);

    port_log("SSB64: virtual gamepad attached (index=%d, instance=%d, %d axes, %d buttons)\n", sDeviceIndex,
             SDL_JoystickInstanceID(sJoystick), kAxisCount, kButtonCount);
    return true;
}

void port_vgamepad_set_button(int button, bool down) {
    if (!port_vgamepad_ensure()) {
        return;
    }
    if (button < 0 || button >= kButtonCount) {
        return;
    }
    SDL_JoystickSetVirtualButton(sJoystick, button, down ? 1 : 0);
}

void port_vgamepad_set_axis(int axis, int value) {
    if (!port_vgamepad_ensure()) {
        return;
    }
    if (axis < 0 || axis >= kAxisCount) {
        return;
    }
    if (value < -32768) {
        value = -32768;
    }
    if (value > 32767) {
        value = 32767;
    }
    SDL_JoystickSetVirtualAxis(sJoystick, axis, (Sint16)value);
}

void port_vgamepad_shutdown(void) {
    // These statics outlive SDL_Quit. Android keeps the process and its
    // globals alive across Activity relaunches, so a stale sJoystick from a
    // previous SDL session would be a use-after-free on the next input.
    if (sJoystick != nullptr) {
        SDL_JoystickClose(sJoystick);
        sJoystick = nullptr;
    }
    if (sDeviceIndex >= 0) {
        SDL_JoystickDetachVirtual(sDeviceIndex);
        sDeviceIndex = -1;
    }
    sAttachAttempted.store(false);
}

} // extern "C"
