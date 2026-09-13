// touch_overlay_ios.cpp — on-screen controls for iOS.
//
// Android draws its overlay with Java views on top of the SDL surface and
// pushes touches down through JNI. iOS has no equivalent layer here, and
// does not need one: SDL already delivers multi-touch, and ImGui is already
// running a frame every tick. So this file does both halves in C++ and
// feeds the same shared virtual gamepad (port/virtual_gamepad.cpp) that the
// Android path uses, which means LUS's existing SDLButtonToAnyMapping
// pipeline and any user remapping apply unchanged.
//
// Multi-touch is the reason this reads SDL_FINGER* events rather than
// ImGui's own IO. SDL synthesizes a single mouse cursor from touch, so an
// ImGui-button overlay could only ever register one contact at a time —
// useless for a fighting game where the stick and a button are held
// together. SDL_AddEventWatch sees every finger without any change to who
// pumps the event loop.
//
// Layout (landscape):
//
//     left half: drag anywhere for the analog stick
//                                              [Z]      [R]
//                                              [B]      [A]
//                                                 [Start]

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE

#include "virtual_gamepad.h"
#include "port_log.h"

#include <SDL2/SDL.h>
#include <imgui.h>

#include <libultraship/libultraship.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <ship/window/gui/GuiWindow.h>

#include <memory>

#include <array>
#include <atomic>
#include <cmath>

namespace {

// Normalized (0..1) screen-space layout. Kept in normalized units so the
// overlay follows the drawable rather than any assumed pixel size.
struct Circle {
    float cx, cy, r;
    bool contains(float x, float y) const {
        const float dx = x - cx, dy = y - cy;
        return dx * dx + dy * dy <= r * r;
    }
};

constexpr float kBtnR = 0.075f;
constexpr Circle kBtnA{ 0.93f, 0.70f, kBtnR };
constexpr Circle kBtnB{ 0.80f, 0.78f, kBtnR };
constexpr Circle kBtnZ{ 0.80f, 0.30f, kBtnR };
constexpr Circle kBtnR_{ 0.93f, 0.22f, kBtnR };
constexpr Circle kBtnStart{ 0.87f, 0.94f, 0.055f };

// The stick is a drag zone, not a fixed pad: the first touch in the left
// half becomes the centre and the finger's offset from it drives the axes.
// Travel is the distance that reads as full deflection.
constexpr float kStickZoneRight = 0.45f;
constexpr float kStickTravel = 0.13f;

struct Finger {
    SDL_FingerID id;
    bool active;
    // Stick ownership: only the finger that started the drag steers it.
    bool ownsStick;
    float originX, originY;
    float x, y;
};

std::array<Finger, 8> sFingers{};
std::atomic<bool> sInstalled{ false };

// Live state, published for the drawing pass.
std::atomic<bool> sStickActive{ false };
std::atomic<int> sStickOriginX{ 0 }, sStickOriginY{ 0 };
std::atomic<int> sStickCurX{ 0 }, sStickCurY{ 0 };
std::atomic<unsigned> sButtonMask{ 0 };

enum ButtonBit { kA = 1u << 0, kB = 1u << 1, kZ = 1u << 2, kR = 1u << 3, kStart = 1u << 4 };

Finger* findFinger(SDL_FingerID id) {
    for (auto& f : sFingers) {
        if (f.active && f.id == id) {
            return &f;
        }
    }
    return nullptr;
}

Finger* acquireFinger(SDL_FingerID id) {
    for (auto& f : sFingers) {
        if (!f.active) {
            f = Finger{ id, true, false, 0.f, 0.f, 0.f, 0.f };
            return &f;
        }
    }
    return nullptr;
}

void applyButtons(unsigned mask) {
    port_vgamepad_set_button(SDL_CONTROLLER_BUTTON_A, (mask & kA) != 0);
    port_vgamepad_set_button(SDL_CONTROLLER_BUTTON_B, (mask & kB) != 0);
    port_vgamepad_set_button(SDL_CONTROLLER_BUTTON_START, (mask & kStart) != 0);
    // Z and R are digital on an N64 pad but LUS reads them from trigger
    // axes, so they are driven to the rails rather than pressed.
    port_vgamepad_set_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, (mask & kZ) ? 32767 : -32768);
    port_vgamepad_set_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, (mask & kR) ? 32767 : -32768);
}

unsigned buttonsUnder(float x, float y) {
    unsigned m = 0;
    if (kBtnA.contains(x, y)) m |= kA;
    if (kBtnB.contains(x, y)) m |= kB;
    if (kBtnZ.contains(x, y)) m |= kZ;
    if (kBtnR_.contains(x, y)) m |= kR;
    if (kBtnStart.contains(x, y)) m |= kStart;
    return m;
}

// Recompute everything from the fingers currently down. Simpler and more
// robust than tracking per-finger deltas: a lost UP event can strand a
// button, a full recompute cannot.
void republish() {
    unsigned mask = 0;
    bool stick = false;
    float sx = 0.f, sy = 0.f, ox = 0.f, oy = 0.f;

    for (const auto& f : sFingers) {
        if (!f.active) {
            continue;
        }
        if (f.ownsStick) {
            stick = true;
            ox = f.originX;
            oy = f.originY;
            sx = f.x;
            sy = f.y;
        } else {
            mask |= buttonsUnder(f.x, f.y);
        }
    }

    applyButtons(mask);
    sButtonMask.store(mask);

    if (stick) {
        const float dx = (sx - ox) / kStickTravel;
        const float dy = (sy - oy) / kStickTravel;
        const float mag = std::sqrt(dx * dx + dy * dy);
        float nx = dx, ny = dy;
        if (mag > 1.f) { // clamp to the unit circle, not the square
            nx /= mag;
            ny /= mag;
        }
        port_vgamepad_set_axis(SDL_CONTROLLER_AXIS_LEFTX, (int)(nx * 32767.f));
        port_vgamepad_set_axis(SDL_CONTROLLER_AXIS_LEFTY, (int)(ny * 32767.f));
        sStickOriginX.store((int)(ox * 10000));
        sStickOriginY.store((int)(oy * 10000));
        sStickCurX.store((int)(sx * 10000));
        sStickCurY.store((int)(sy * 10000));
    } else {
        port_vgamepad_set_axis(SDL_CONTROLLER_AXIS_LEFTX, 0);
        port_vgamepad_set_axis(SDL_CONTROLLER_AXIS_LEFTY, 0);
    }
    sStickActive.store(stick);
}

int SDLCALL eventWatch(void* /*userdata*/, SDL_Event* e) {
    switch (e->type) {
        case SDL_FINGERDOWN: {
            Finger* f = acquireFinger(e->tfinger.fingerId);
            if (f == nullptr) {
                break;
            }
            f->x = f->originX = e->tfinger.x;
            f->y = f->originY = e->tfinger.y;
            // Left half steers, unless another finger already owns the
            // stick — a second left-side touch is ignored rather than
            // fighting the first for control.
            if (e->tfinger.x < kStickZoneRight && !sStickActive.load()) {
                f->ownsStick = true;
            }
            republish();
            break;
        }
        case SDL_FINGERMOTION: {
            Finger* f = findFinger(e->tfinger.fingerId);
            if (f == nullptr) {
                break;
            }
            f->x = e->tfinger.x;
            f->y = e->tfinger.y;
            republish();
            break;
        }
        case SDL_FINGERUP: {
            Finger* f = findFinger(e->tfinger.fingerId);
            if (f != nullptr) {
                f->active = false;
            }
            republish();
            break;
        }
        default:
            break;
    }
    return 0; // watchers do not consume events
}

ImU32 col(unsigned mask, unsigned bit) {
    return (mask & bit) ? IM_COL32(255, 255, 255, 190) : IM_COL32(255, 255, 255, 70);
}

void drawCircle(ImDrawList* dl, const Circle& c, const ImVec2& size, const char* label, unsigned mask, unsigned bit) {
    const ImVec2 p{ c.cx * size.x, c.cy * size.y };
    const float r = c.r * size.y;
    dl->AddCircleFilled(p, r, IM_COL32(0, 0, 0, 60));
    dl->AddCircle(p, r, col(mask, bit), 0, 3.0f);
    const ImVec2 t = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x - t.x * 0.5f, p.y - t.y * 0.5f), col(mask, bit), label);
}

void drawControls(); // defined below, after the event plumbing

// LUS drives every registered GuiWindow once per ImGui frame, which is the
// only place ImGui::GetForegroundDrawList() is valid. Draw() is overridden
// rather than DrawElement() so the overlay ignores the show/hide plumbing —
// the controls are the only way to play and must never disappear.
class TouchOverlayWindow final : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;
    void Draw() override {
        drawControls();
    }
    void InitElement() override {
    }
    void DrawElement() override {
    }
    void UpdateElement() override {
    }
};

std::shared_ptr<TouchOverlayWindow> sWindow;

} // namespace

extern "C" {

void port_touch_overlay_ios_init(void) {
    if (sInstalled.exchange(true)) {
        return;
    }
    port_vgamepad_ensure();
    SDL_AddEventWatch(eventWatch, nullptr);

    auto ctx = Ship::Context::GetInstance();
    if (ctx && ctx->GetWindow() && ctx->GetWindow()->GetGui()) {
        sWindow = std::make_shared<TouchOverlayWindow>("gTouchOverlay", "Touch Overlay");
        ctx->GetWindow()->GetGui()->AddGuiWindow(sWindow);
    } else {
        port_log("SSB64: iOS touch overlay — no Gui yet, controls will not draw\n");
    }
    port_log("SSB64: iOS touch overlay installed\n");
}

void port_touch_overlay_ios_shutdown(void) {
    if (!sInstalled.exchange(false)) {
        return;
    }
    SDL_DelEventWatch(eventWatch, nullptr);
    port_vgamepad_shutdown();
}

} // extern "C"

namespace {

void drawControls() {
    if (!sInstalled.load()) {
        return;
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (dl == nullptr) {
        return;
    }
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    if (size.x <= 0.f || size.y <= 0.f) {
        return;
    }

    const unsigned mask = sButtonMask.load();
    drawCircle(dl, kBtnA, size, "A", mask, kA);
    drawCircle(dl, kBtnB, size, "B", mask, kB);
    drawCircle(dl, kBtnZ, size, "Z", mask, kZ);
    drawCircle(dl, kBtnR_, size, "R", mask, kR);
    drawCircle(dl, kBtnStart, size, "START", mask, kStart);

    // The stick only appears where the finger put it.
    if (sStickActive.load()) {
        const ImVec2 o{ sStickOriginX.load() / 10000.f * size.x, sStickOriginY.load() / 10000.f * size.y };
        const ImVec2 c{ sStickCurX.load() / 10000.f * size.x, sStickCurY.load() / 10000.f * size.y };
        dl->AddCircle(o, kStickTravel * size.y, IM_COL32(255, 255, 255, 70), 0, 3.0f);
        dl->AddCircleFilled(c, kStickTravel * size.y * 0.45f, IM_COL32(255, 255, 255, 150));
    }
}

} // namespace

#endif /* __APPLE__ && TARGET_OS_IPHONE */
