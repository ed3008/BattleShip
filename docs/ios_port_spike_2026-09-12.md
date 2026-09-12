# iOS Port — Compile Spike (2026-09-12)

## TL;DR

`libultraship.a` (388 MB fat, x86_64 + arm64, 310 objects) builds clean for
the **iOS Simulator** SDK against Xcode 26.6 / iOS SDK 26.5, including the
Fast3D **Metal** backend (`gfx_metal.o`, `gfx_metal_shader.o`). The top-level
CMake graph configures and generates an Xcode project for `CMAKE_SYSTEM_NAME=iOS`.

**The engine library compiles; the `ssb64` app target does not build yet and
there is no running game.** The Torch asset pipeline and the whole
app-bundle/resource story are unwritten. Device (`iphoneos`) builds have not
been attempted at all — only `iphonesimulator`.

## What works

- Top-level CMake configures + generates for iOS once `DISABLE_SCRIPTING` is
  forced on (see "Changes landed"). First configure takes ~560s (FetchContent
  clones); subsequent ones ~17s.
- libultraship's pre-existing iOS scaffolding works essentially unmodified:
  `cmake/dependencies/ios.cmake` and `cmake/ios-toolchain-populate.cmake`
  (leetal/ios-cmake, `PLATFORM=OS64COMBINED`) were already in the tree.
- All FetchContent dependencies cross-compile cleanly: SDL2 2.32.10
  (`libSDL2d.a`, fat x86_64 + arm64, linked as `SDL2::SDL2-static` +
  `SDL2main`), spdlog, libzip, tinyxml2, nlohmann_json, prism, stb, ImGui.
- metal-cpp (`single-header-metal-cpp` @ `macOS13_iOS16`) compiles against
  the iOS 26.5 SDK. `libImGui.a` (11 MB) carries 35 Metal symbols from
  `imgui_impl_metal.mm`, so the ObjC++/ARC path is good.
- Fast3D's Metal backend compiles for iOS unmodified. This was the single
  largest unknown going in and it is now retired: `gfx_metal.cpp` and
  `gfx_metal_shader.cpp` both produce objects. Note that the shipping macOS
  build already runs on Metal (`Window/Backend/Name = Metal` in
  `BattleShip.cfg.json`), and its shaders are built at runtime via
  `gfx_metal_build_shader` — runtime MSL compilation is a system service on
  iOS, not self-mapped executable memory, so it does not collide with the
  JIT ban.
- `port/coroutine_aarch64.S` assembles for `arm64-apple-ios16.0` with no
  directive changes. Its `.type` / `.size` / `.note.GNU-stack` directives
  were already `__ELF__`-guarded.

### ucontext is available on iOS — the coroutine backend already works

An earlier draft of this document claimed the iOS SDK does not ship
`<ucontext.h>` and that the coroutine layer was therefore the port's first
hard blocker. **That was wrong.** Measured instead of assumed:

- `<ucontext.h>` exists in the iPhoneOS 26.5 and iPhoneSimulator 26.5 SDKs.
  It guards on `_XOPEN_SOURCE` exactly like the macOS header and carries no
  `__API_UNAVAILABLE(ios)` annotation.
- `_getcontext` / `_makecontext` / `_swapcontext` are exported by the iOS
  `libSystem.tbd`; a test program compiles, links, and imports them from
  `/usr/lib/libSystem.B.dylib` for `arm64-apple-ios`.
- `port/coroutine_posix.cpp` compiles unmodified for `arm64-apple-ios16.0`
  and for `x86_64-apple-ios16.0-simulator`, defining all seven public API
  symbols. It already handles Apple via its own `_XOPEN_SOURCE 600` block.
- Its guard (`!_WIN32 && !__ANDROID__`) already admits iOS, and
  `coroutine_android.cpp` (`__ANDROID__`-gated) yields an empty translation
  unit there — 0 symbols. So exactly one backend compiles on iOS, with no
  collision and no CMake source filtering needed.

The concern about the `port/` glob pulling in two conflicting backends was
unfounded for the same reason.

Caveat: these routines are deprecated, and linking is not proof of correct
runtime behaviour. Until the game actually runs on a device this is
"compiles and resolves", not "works". If it does misbehave, the asm backend
in `coroutine_android.cpp` is platform-neutral apart from its `__ANDROID__`
guard (it only uses mmap/mprotect/munmap/sysconf) and could be generalised
to cover iOS.

## Changes landed

Three files, two repos, on branch `agent/ios-port` in both. The
superproject carries the asm fix, the CMake gates and this document; the
hidapi change lives in the `libultraship` submodule and the superproject
commit bumps the gitlink to it.

| File | Change |
|------|--------|
| `port/coroutine_aarch64.S` | `SYM()` macro for the Mach-O leading-underscore symbol prefix |
| `CMakeLists.txt` | `enable_language(ASM)` and `DISABLE_SCRIPTING` extended to iOS |
| `libultraship/cmake/dependencies/common.cmake` | hidapi stub extended from Android-only to Android + iOS |

### Mach-O symbol prefix

Mach-O prefixes every C symbol with `_`; ELF does not. The `.S` defined
`port_coroutine_swap` / `port_coroutine_trampoline_aarch64` and called
`port_coroutine_trampoline_c` unprefixed, so all three would have failed to
link against clang-compiled C on Apple targets. Verified by assembling the
file for three triples and reading `nm`:

| Target | Symbol emitted |
|--------|----------------|
| `arm64-apple-ios16.0` | `_port_coroutine_swap` |
| `arm64-apple-macos13` | `_port_coroutine_swap` |
| `aarch64-unknown-linux-gnu` | `port_coroutine_swap` (unchanged — no Android regression) |

This is a latent-bug fix that stands on its own: the names were wrong for
*any* Apple target, including the macOS build, and would have bitten whoever
first tried to use the asm backend there. It is **not** load-bearing for the
iOS port — see "ucontext is available on iOS" below. Keep it because it is
correct and because the asm backend is the fallback if the deprecated
ucontext path ever misbehaves.

### DISABLE_SCRIPTING on iOS

funchook's CMake resolves `src/funchook_.c` to nothing for an iOS target and
the **generate** step fails outright (`No SOURCES given to target:
funchook-static`). Even if it configured, the hook backend installs code by
making memory writable and executable; the macOS build gets that through an
rwx `__TEXT` maxprot re-sign with JIT entitlements, and iOS has no
equivalent. Same disposition as Android.

### hidapi stub

hidapi selects its macOS backend for an Apple target, which includes
`<IOKit/hid/IOHIDManager.h>` — absent from the iOS SDK. iOS exposes no raw
HID layer to apps; controllers arrive via GameController, which SDL2 already
handles, and a Raphnet USB adapter cannot be attached to an iPhone anyway.
The Android no-op stub in `common.cmake` was already the right shape, so iOS
now shares it rather than getting a second one.

## What's blocked

### 1. Torch cannot cross-compile (blocks all asset targets)

`TorchExternal` / `ExtractAssets` / `GenerateBattleShipO2R` would build Torch
*for iOS* and then try to execute it on the host during the build. Identical
to the problem the Android spike hit — see
`docs/android_port_spike_2026-05-01.md`, which gated
`ExternalProject_Add(TorchExternal)` off for the same reason.

For the first bring-up the cheapest route is to **not run Torch in the iOS
build at all** and stage the `BattleShip.o2r` already produced by the macOS
build into the app bundle. That defers the entire on-device extraction story
(which on Android needs `libtorch_runner.so` + a SAF picker; on iOS it would
need Torch linked statically + `UIDocumentPickerViewController`). Only worth
building once the game actually runs.

### 2. App target / bundle unwritten

`CMakeLists.txt` has 15 `CMAKE_SYSTEM_NAME STREQUAL "Android"` gates. Two are
now handled. The ones the `ssb64` target will hit:

- `:818` / `:908` — Discord RPC is fetched and its include dir added on any
  non-Android target. Network/desktop only; exclude on iOS.
- `:855` — Android emits a SHARED `libmain.so` for `SDLActivity`; desktop
  emits an executable. iOS needs an executable inside an app bundle
  (`MACOSX_BUNDLE`) with an `Info.plist`, bundle identifier and, for a
  device build, a development team + signing identity.
- `:1038` — runtime data (`gamecontrollerdb.txt`, `config.yml`, CSS PNGs)
  is copied next to the binary on desktop. On iOS these must be bundle
  resources.
- `:534` / `:129` — window-icon generation and `USE_OPENGLES` are desktop /
  Android concerns; iOS wants neither (Metal only).

### 3. Device build unattempted

Everything above is `-sdk iphonesimulator` with `CODE_SIGNING_ALLOWED=NO`.
Simulator-arm64 and device-arm64 are different platform triples: a clean
simulator build is *not* evidence that the device build links. Device
requires a provisioning profile and an Apple Developer team, and is where
ARKit can first be exercised — the simulator has no ARKit and no camera.

## Reproduction

Host: macOS 26.5 (arm64), Xcode 26.6 / Build 17F113, iOS SDK 26.5,
CMake + Ninja from Homebrew.

```bash
# macOS baseline (works, runs — 840+ frames of the attract demo)
cmake -S . -B build-us -GNinja -DSSB64_VERSION=us
cmake --build build-us -j

# iOS: configure + generate Xcode project
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DSSB64_VERSION=us

# iOS: engine library only (the app target does not build yet)
xcodebuild -project build-ios/ssb64.xcodeproj -target libultraship \
  -configuration Debug -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO
```

macOS host dependencies came from Homebrew: `cmake ninja sdl2 glew libzip
tinyxml2 nlohmann-json spdlog fmt`. Note that current Homebrew resolves
`sdl2` to **sdl2-compat** (SDL2 API over SDL3); the macOS baseline builds and
runs with it, but it is worth remembering if input or audio behaves oddly.
The iOS build is unaffected — it compiles real SDL2 2.32.10 from source.

## Context

This spike exists because the end goal is an AR build for iPhone: camera
passthrough with the game anchored in the room, driven by ARKit and a
Bluetooth controller, optionally using LiDAR scene reconstruction for
occlusion. None of that is started. Getting the existing port to run flat on
an iPhone is a prerequisite and is useful on its own; the AR camera work
should build on the viewport / projection / HUD-anchor split already mapped
out in `docs/widescreen_16_9_plan_2026-05-10.md`, which solves a
structurally similar problem (the 3D camera changing while screen-space 2D
must stay correct).
