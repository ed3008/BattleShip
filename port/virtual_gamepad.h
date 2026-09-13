#ifndef PORT_VIRTUAL_GAMEPAD_H
#define PORT_VIRTUAL_GAMEPAD_H

/**
 * virtual_gamepad.h — SDL virtual gamepad shared by the touch overlays.
 *
 * Both mobile targets drive the game from on-screen controls, and both do
 * it the same way: attach an SDL virtual joystick carrying an Xbox-360
 * vendor/product signature, then set buttons and axes on it. SDL promotes
 * such a joystick to SDL_GameController, so libultraship's existing
 * SDLButtonToAnyMapping pipeline maps it to OSContPad bits with no changes
 * and no awareness that the input came from a finger.
 *
 * What differs per platform is only where the touches come from and who
 * draws the buttons — Java views on Android, ImGui on iOS.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Attach on first use. Returns false if SDL refused; safe to call often. */
bool port_vgamepad_ensure(void);

/** Set an SDL_GameControllerButton. No-ops if the pad is not attached. */
void port_vgamepad_set_button(int button, bool down);

/** Set an SDL_GameControllerAxis. Value is clamped to [-32768, 32767]. */
void port_vgamepad_set_axis(int axis, int value);

/** Release the pad. Called from PortShutdown. */
void port_vgamepad_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_VIRTUAL_GAMEPAD_H */
