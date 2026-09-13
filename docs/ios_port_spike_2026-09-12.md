# iOS Port — Compile Spike (2026-09-12, updated 2026-09-13)

> **Superseded by `docs/ios_port_status_2026-09-13.md`.** The game now
> runs on device. Everything listed below under "What's blocked" is
> done; the build-system findings are still accurate and worth reading.
> Note in particular that this document's conclusion about ucontext on
> iOS was wrong — it links but fails at runtime, which is what kept the
> screen black.

## TL;DR

Every translation unit of the port — the decomp, the `port/` layer, and
libultraship including the Fast3D **Metal** backend — compiles for
`arm64-apple-ios` against Xcode 26.6 / iOS SDK 26.5. Torch runs as a host
tool during the build and extracts the user's ROM into a 12 MB
`BattleShip.o2r`, so the asset pipeline works end to end.

**The app does not link yet.** Four symbols remain unresolved, all
platform-specific leftovers in libultraship (see "What's blocked"). There is
no running game, and nothing has been installed on a device.

Targeting **device** (`-sdk iphoneos`), not the simulator: the simulator has
no ARKit and no camera, so it can never host the actual goal. It was only
ever a way to dodge code signing.

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

### Torch as a host tool (the environment trap)

`ExternalProject_Add(TorchExternal)` builds Torch to run on *this Mac* during
the build. Under an iOS parent it produced an iOS binary instead, and the
extraction step died with `Killed: 9`.

Two wrong diagnoses came first, both worth recording. Setting
`-DCMAKE_SYSTEM_NAME=Darwin` to force a host build put CMake into
cross-compiling mode; with a blanked deployment target the link lost its
platform-version load command, `ld` skipped the automatic ad-hoc signature,
and macOS SIGKILLs unsigned arm64 binaries. Removing that fixed signing in an
isolated test but not in the real build — and a stale-objects theory did not
survive a clean rebuild either.

The actual cause: ExternalProject's steps run inside an Xcode script phase,
and xcodebuild exports the target platform into that environment (`SDKROOT`,
`IPHONEOS_DEPLOYMENT_TARGET`, `PLATFORM_NAME`, ...). clang honours those over
anything CMake passes, so every object compiled for iOS despite
`CMAKE_OSX_SYSROOT` pointing at the macOS SDK. A relink in a clean shell said
it plainly: *"building for 'macOS', but linking in object file built for
'iOS'"*.

The fix scrubs those variables for the sub-build's configure and build steps
via `cmake -E env --unset=...`. Verified on three axes: `vtool -show-build`
reports `platform MACOS`, `codesign` reports `adhoc,linker-signed`, and the
binary runs.

### Discord Rich Presence and the self-updater are off on iOS

Both were measured, not assumed — the first full compile pass proved that
most `#if !defined(__ANDROID__)` sites are harmless on iOS, so only what
actually breaks is excluded.

- discord-rpc does not build: `src/discord_register_osx.m` imports
  `<AppKit/AppKit.h>`, which the iOS SDK does not ship.
- `Updater.cpp` shells out through `system()`, marked `__API_UNAVAILABLE` on
  iOS.

Rather than repeat a two-platform test at each of the four CMake sites and
two C++ guards, the conditions are named once —`SSB64_NO_DISCORD` and
`SSB64_NO_SELF_UPDATER`, defined for Android *and* iOS. Android's behaviour
is unchanged; the guards that used to test `__ANDROID__` now test the macro
that is defined for Android anyway.

### Two more `system()` call sites

- `port/bridge/lbreloc_byteswap.cpp` created its texture-dump directory by
  spawning `mkdir`. Replaced with `std::filesystem::create_directories`,
  which also removes the separate Windows branch.
- `port/first_run.cpp` spawns Torch through a shell for on-device
  extraction. iOS gets an explicit branch that fails with a clear message:
  the archive is extracted on the host at build time, so the path should be
  unreachable, and reporting a silent success would be worse.

### PLATFORM is overridable (libultraship)

`cmake/ios-toolchain-populate.cmake` hard-coded `PLATFORM=OS64COMBINED`.
COMBINED is Xcode-only — the leetal toolchain hard-fails under any other
generator — and it also drags in an x86_64 simulator slice that nothing here
needs. It now defaults to the same value but respects a caller-provided one.

## What's blocked

### 1. Four unresolved symbols (the only thing between here and a binary)

```
Ship::CoreAudioAudioPlayer::CoreAudioAudioPlayer(Ship::AudioSettings)
Ship::CoreAudioAudioPlayer::~CoreAudioAudioPlayer()
_isNativeMacOSFullscreenActive
_toggleNativeMacOSFullscreen
```

Both live in the libultraship submodule and need opposite treatments.

**CoreAudio** — `src/ship/audio/CoreAudioAudioPlayer.cpp` exists and the
frameworks are already linked for iOS (`src/CMakeLists.txt:91` covers
`Darwin OR iOS`); the *source file* just is not in the iOS build. iOS has
CoreAudio and AudioToolbox, so compiling it should be enough.

**macOS fullscreen** — `src/ship/utils/macUtils.mm` is Cocoa, genuinely
inapplicable to iOS, and is called unguarded from `src/fast/backends/
gfx_sdl2.cpp:239,240,697`. iOS has no windowed-fullscreen concept (an app is
always fullscreen), so those call sites need guarding rather than a port.

### 2. App bundle and signing

Nothing has been installed anywhere. The target still produces a bare
executable: no `MACOSX_BUNDLE`, no `Info.plist`, no bundle identifier, and
no development team. Runtime data (`BattleShip.o2r`, `gamecontrollerdb.txt`,
`config.yml`, the CSS PNGs) is staged next to the binary as on desktop and
has to become bundle resources instead. Installing on a device additionally
needs a provisioning profile.

### 3. Nothing has been run

Compiling and linking say nothing about behaviour. The coroutine backend in
particular rides on deprecated ucontext routines that are present and link
cleanly but have never executed here.

### Notes on the build system

- **Dependency cycle (resolved, no code change).** Xcode reported
  `GenerateF3DO2R → ssb64_game → GenerateRelocArtifacts → TorchExternal →
  GenerateF3DO2R`. None of those edges is a real `add_dependencies`; Xcode
  was enforcing a linear target order because *"Parallelize build for
  command-line builds"* is off, and the artificial edges closed a loop with
  genuine file-level dependencies. Building with `-parallelizeTargets`
  avoids it. Worth knowing before anyone tries to "fix" the CMake graph.
- **Ninja is not a usable alternative.** It dies far earlier: SDL2's `.m`
  sources are classified as Objective-C++ and receive `-std=gnu++2a`, which
  is invalid in Objective-C mode.
- **The toolchain is included mid-configure** (`libultraship/CMakeLists.txt:63`)
  rather than passed as `CMAKE_TOOLCHAIN_FILE`. A consequence:
  `-DPLATFORM=SIMULATORARM64` sets the architecture list but leaves
  `CMAKE_OSX_SYSROOT` on the device SDK, so a simulator build links device
  `.tbd` stubs and fails. Device builds are self-consistent; simulator
  builds would need the sysroot passed explicitly.

## Reproduction

Host: macOS 26.5 (arm64), Xcode 26.6 / Build 17F113, iOS SDK 26.5,
CMake + Ninja from Homebrew.

```bash
# macOS baseline (works, runs — 840+ frames of the attract demo)
cmake -S . -B build-us -GNinja -DSSB64_VERSION=us
cmake --build build-us -j

# iOS: configure + generate the Xcode project
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DSSB64_VERSION=us

# iOS device: compiles fully, still four symbols short of linking.
# -parallelizeTargets is required — without it Xcode reports a dependency
# cycle. Signing is off because nothing is installed yet.
xcodebuild -project build-ios/ssb64.xcodeproj -target ssb64 \
  -configuration Debug -sdk iphoneos -parallelizeTargets \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO
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
