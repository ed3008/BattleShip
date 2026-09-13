# iOS Port — Status (running on device)

Supersedes `docs/ios_port_spike_2026-09-12.md`, which was written when the
engine library compiled and the app did not link. That document is still
worth reading for the build-system findings; everything it lists under
"What's blocked" is now done.

## TL;DR

Super Smash Bros. 64 runs on an iPhone 13 Pro Max (iOS 26.6.1): image,
audio, and a steady frame rate from a Release build. The game's assets are
extracted from the user's ROM on the host at build time and shipped inside
the app bundle.

## What works

End-to-end on device:

1. `cmake -G Xcode -DCMAKE_SYSTEM_NAME=iOS` configures and generates.
2. Torch builds as a **host** macOS tool and extracts `BattleShip.o2r`
   (12 MB) from `baserom.us.z64` during the build.
3. `xcodebuild` signs with a free Personal Team and produces
   `BattleShip.app` (7.4 MB executable in Release, arm64, platform IOS).
4. The app launches through SDL2main's UIKit entry point, comes up
   landscape at the device's native point size, and loads `f3d.o2r` and
   `BattleShip.o2r` from inside the bundle.
5. Audio assets parse and play — 43 music instruments, 47 sequences.
6. `gamecontrollerdb.txt` loads (42 mappings).
7. The game renders and is playable, at a measured 59.91fps sustained over
   166 seconds with no dropped interval.
8. On-screen controls work (drag-anywhere stick, A/B/Z/R/Start), and a
   paired Bluetooth controller works with no extra code — SDL talks to
   GameController natively.

## The six platform bugs, in the order they had to be solved

Each one hid the next, which is why they are worth recording as a chain.

### 1. hidapi has no iOS backend

hidapi selects its macOS backend for any Apple target, which includes
`<IOKit/hid/IOHIDManager.h>` — absent from the iOS SDK. libultraship
already had an Android no-op stub for the same situation; iOS now shares
it. iOS exposes no raw HID layer at all, and controllers arrive through
GameController, which SDL2 handles.

### 2. funchook / TinyCC cannot exist on iOS

`DISABLE_SCRIPTING` is forced on. funchook's CMake resolves
`src/funchook_.c` to nothing for iOS and the *generate* step fails
outright. It could not work regardless: the hook backend needs memory that
is both writable and executable, which iOS never grants.

### 3. Torch was cross-compiled instead of built for the host

`ExternalProject` steps run inside an Xcode script phase, and xcodebuild
exports the target platform into that environment (`SDKROOT`,
`IPHONEOS_DEPLOYMENT_TARGET`, ...). clang honours those over anything CMake
passes, so every Torch object compiled for iOS despite `CMAKE_OSX_SYSROOT`
pointing at the macOS SDK. The resulting binary could not run on the Mac —
the extraction step died with `Killed: 9`.

Fixed by scrubbing those variables for the sub-build's configure and build
steps with `cmake -E env --unset=...`.

**Do not "fix" this with `-DCMAKE_SYSTEM_NAME=Darwin`.** That puts the
sub-build into cross-compiling mode; the link loses its platform-version
load command, `ld` skips the automatic ad-hoc signature, and macOS SIGKILLs
the unsigned arm64 result. It looks like a fix and is not one.

### 4. GetAppBundlePath returned the data container

On iOS it answered `$HOME/Documents` — the writable directory
`GetAppDirectoryPath` already returns. `LocateExistingFile` therefore
probed one directory twice and never looked inside the `.app`, so bundled
read-only resources resolved to the `./<name>` miss value and startup
failed with "The archive at path /./f3d.o2r does not exist". iOS now falls
through to the NSBundle branch, which is correct for it: an iOS bundle is
flat, so `resourcePath` is the `.app` directory.

### 5. SDL was not allowed to own process startup

`SDL_MAIN_HANDLED` was defined for every platform except Android, which
suppresses `SDL_main.h`'s `#define main SDL_main`. iOS needs that rename
for the same structural reason Android does: SDL2main supplies the real
`main()`, which stands up the `UIApplication` and only then calls ours from
inside UIKit's launch cycle. Without it `SDL_Init(SDL_INIT_VIDEO)` failed
with "Application didn't initialize properly...", `SDL_CreateWindow`
returned null, and Gui init tripped an ImGui "No current context"
assertion.

Both halves are required. Because our `main()` kept its name, the linker
also never pulled SDL2main's `main()` out of the static library — nothing
referenced it — so linking SDL2main alone changed nothing.

### 6. No launch screen meant legacy compatibility mode

CMake's stock bundle Info.plist declares no launch screen, and iOS reads
that as an app predating modern screen sizes: the window came back as the
original iPhone's 320x480 points (480x320 landscape) on a 2778x1284
display, regardless of what SDL asked for. `cmake/iOSInfo.plist.in` adds
`UILaunchScreen` plus landscape-only orientations,
`UIRequiresFullScreen`, a hidden status bar,
`UIApplicationSupportsIndirectInputEvents` and `metal` in
`UIRequiredDeviceCapabilities`.

### 7. ucontext links on iOS but does not work

The one that actually kept the screen black, and a correction to the spike
doc. `<ucontext.h>` ships in the iOS SDK, libSystem exports
`getcontext`/`makecontext`/`swapcontext`, and `coroutine_posix.cpp`
compiles and links cleanly for `arm64-apple-ios` — so the hand-written asm
backend was written off as a latent-bug fix rather than a requirement.

**`getcontext()` fails at runtime.** `port_coroutine_create` returned NULL,
the game started with zero coroutines, and the symptom was four lines deep
in the log:

```
FATAL — failed to create game coroutine
port_resume_service_threads: 0 threads registered
  round 0: no progress, breaking
```

Nothing ran the game, so no RSP task was submitted (`osSpTaskStartGo`: 0 on
iOS, 73 on macOS), so no display list reached the renderer. The renderer
was idle, not broken.

iOS now uses the same aarch64 swap Android does. Nothing in that file was
ever Android-specific — mmap/mprotect/munmap/sysconf plus the `.S` shim,
which emits Mach-O symbol names through its `SYM()` macro. The `.S` is
listed explicitly for the iOS executable because the `port/*.cpp` glob does
not match `.S` sources.

**Linking is not evidence of working.** The spike doc said exactly that
about these routines and the lesson still had to be learned twice.

## Method note

Six of the seven were found by instrumenting, not by reasoning. Both
`SDL_Init` and `SDL_CreateWindow` discarded their return values, so the
first visible symptom was an ImGui assertion three layers removed from the
cause. The Metal probes then had to distinguish "drawable is null" from
"the frame functions never run at all" — the original null check lived
inside the function that was not being called, so its silence was
ambiguous. Every wrong turn in this list came from treating a plausible
explanation as a confirmed one.

## Native resolution was tried and reverted

`SDL_WINDOW_ALLOW_HIGHDPI` is deliberately **not** set for iOS. Adding it
broke rendering on device: first launch came up with audio and a black
screen, the next with neither. Reverted; do not re-apply it without the
verification below.

The drawable is 926x428 points on a 2778x1284 display, so iOS upscales by
3x linear. The symptom pattern — the layer resizes but nothing presents —
points at the screen framebuffer or viewport keeping the old dimensions
while the layer changes underneath them.

If someone picks this up: set the flag, then *read the probes before
installing*. `Metal init` must report 2778x1284 and
`SetupScreenFramebuffer` must be pinned to the same number. If those two
disagree, that gap is the bug. The probes for exactly this already exist
and were not consulted the first time round.

It is a sharpness win, not a playability one. At 926x428 the game looks
good and holds 60fps.

## What's left

- **On-device ROM import.** The archive is bundled at build time, which is
  simpler and boots faster. Only needed to hand the app to someone who
  supplies their own ROM — and there is no realistic distribution route for
  this project anyway. A shippable build would need Torch linked statically
  plus `UIDocumentPickerViewController`, the way Android uses
  `libtorch_runner.so` and a SAF picker.
- **Shader compilation hitches** were visible in a Debug build and are gone
  in Release. Metal compiles MSL on demand, so the cost is a one-time
  warmup per shader; steady state measured clean. Precompiling at startup
  would smooth the first seconds if it ever matters.
- **`UIScene` lifecycle.** UIKit warns it will become mandatory.
- The probes added during this work are one-shot and cheap, but they are
  diagnostics and could be dropped or demoted once the port settles.

## Reproduction

Host: macOS 26.5 (arm64), Xcode 26.6, iOS SDK 26.5. Device: iPhone 13 Pro
Max, iOS 26.6.1, Developer Mode enabled.

```bash
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DSSB64_VERSION=us \
      -DSSB64_IOS_DEVELOPMENT_TEAM=<10-char team id>

xcodebuild -project build-ios/ssb64.xcodeproj -target ssb64 \
  -configuration Release -destination 'platform=iOS,id=<device udid>' \
  -parallelizeTargets -allowProvisioningUpdates
```

`-parallelizeTargets` is required: without it Xcode enforces a linear
target order that closes a dependency cycle with genuine file-level
dependencies. The CMake graph is fine.

A free Personal Team is enough. It issues 7-day profiles and can only
provision devices that are physically connected with Developer Mode on —
the "add device IDs manually" route in the error message needs a paid
account.

## Why this exists

The goal is an AR build: camera passthrough with the game anchored in the
room, driven by ARKit and a controller, optionally using LiDAR scene
reconstruction for occlusion. A flat iPhone build was the prerequisite and
is useful on its own. The camera work should build on the
viewport / projection / HUD-anchor split already mapped out in
`docs/widescreen_16_9_plan_2026-05-10.md`, which solves a structurally
similar problem.
