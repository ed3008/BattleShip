// android_touch_overlay.cpp — JNI shim over the shared virtual gamepad.
//
// Java's TouchOverlay draws the buttons on top of the SDL surface and
// forwards touch DOWN/UP through the setButton()/setAxis() entry points
// below. Everything past that is platform-neutral and lives in
// port/virtual_gamepad.cpp, which iOS's overlay uses as well.
//
// SDL_JoystickSetVirtualButton/Axis are documented as thread-safe, so the
// Android UI thread may call in directly; SDL_PumpEvents on the SDL thread
// turns the state changes into SDL_CONTROLLERBUTTONDOWN/UP that LUS already
// maps to OSContPad bits.

#if defined(__ANDROID__)

#include <SDL2/SDL.h>

#include "virtual_gamepad.h"

#include <libultraship/libultraship.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>

#include <android/log.h>
#include <jni.h>

#include <atomic>

#define LOG_TAG "ssb64.touch"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)


extern "C" {

// Called from PortShutdown. Android keeps the process and its globals alive
// across Activity relaunches, so the pad must be released here — see the
// note in port/virtual_gamepad.cpp for why a stale handle would bite.
void port_touch_overlay_shutdown(void) {
    port_vgamepad_shutdown();
}

JNIEXPORT void JNICALL
Java_com_jrickey_battleship_TouchOverlay_setButton(
    JNIEnv * /*env*/, jclass /*clazz*/, jint btn, jboolean down) {
    port_vgamepad_set_button((int)btn, down != JNI_FALSE);
}

JNIEXPORT void JNICALL
Java_com_jrickey_battleship_TouchOverlay_setAxis(
    JNIEnv * /*env*/, jclass /*clazz*/, jint axis, jint value) {
    port_vgamepad_set_axis((int)axis, (int)value);
}

// Menu toggle: set an atomic flag here on the Android UI thread; the
// SDL_main thread's PortPushFrame drains it via port_drain_pending_menu_toggle()
// (see port/gameloop.cpp) and calls libultraship's
// Gui->GetMenu()->ToggleVisibility() from the same thread that's running
// the ImGui frame. Pushing SDL_KEYDOWN(F1) was unreliable — Gui.cpp's
// IsKeyPressed(TOGGLE_BTN, false) is an edge detector that needs the
// keydown to land in the right frame slot, and SDL backend event timing
// from a JNI-thread push didn't consistently line up.
static std::atomic<bool> sMenuTogglePending{false};

extern "C" bool port_drain_pending_menu_toggle(void) {
    return sMenuTogglePending.exchange(false);
}

JNIEXPORT void JNICALL
Java_com_jrickey_battleship_TouchOverlay_toggleMenu(
    JNIEnv * /*env*/, jclass /*clazz*/) {
    sMenuTogglePending.store(true);
    LOGI("toggleMenu: queued");
}

// Polled every ~100ms by TouchOverlay.installMenuVisibilityPoller so the
// overlay can hide itself while the menu is up — otherwise the analog
// stick view eats taps on menu items before they reach ImGui.
JNIEXPORT jboolean JNICALL
Java_com_jrickey_battleship_TouchOverlay_isMenuVisible(
    JNIEnv * /*env*/, jclass /*clazz*/) {
    auto ctx = Ship::Context::GetInstance();
    if (!ctx || !ctx->GetWindow() || !ctx->GetWindow()->GetGui()) {
        return JNI_FALSE;
    }
    return ctx->GetWindow()->GetGui()->GetMenuOrMenubarVisible()
        ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"

#endif // __ANDROID__
