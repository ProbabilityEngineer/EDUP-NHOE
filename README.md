# EDUP - NHOE

**Native Haptics Odyssey Expanded** restores the vibration Elite Dangerous already authored and adds new feedback for Odyssey actions and other mapped gameplay cues. The runtime handles haptic output through gamepads and compatible VR frontends. Elite retains control of input, bindings and event timing.

The current public test preview is **0.12.1-preview.5**. The changelog tracks development previews separately. [Public preview](https://github.com/Fenris159/EDUP-NHOE/releases/tag/v0.12.1-preview.5) · [Changelog](CHANGELOG.md)

## Download and install

- [Windows x64 preview](https://github.com/Fenris159/EDUP-NHOE/releases/download/v0.12.1-preview.5/EDUP-NHOE-0.12.1-preview.5-windows-x64.zip)
- [Linux / Proton x64 preview](https://github.com/Fenris159/EDUP-NHOE/releases/download/v0.12.1-preview.5/EDUP-NHOE-0.12.1-preview.5-linux-proton-x64.zip)
- [Mac / CrossOver gamepad preview](https://github.com/Fenris159/EDUP-NHOE/releases/download/v0.12.1-preview.5/EDUP-NHOE-0.12.1-preview.5-macos-crossover-x64.zip)
- [All releases and checksums](https://github.com/Fenris159/EDUP-NHOE/releases)

Close the game and launcher, back up matching mod files, and copy the **contents** of `Copy-to-game` beside `EliteDangerous64.exe`. Keep the support folder intact and retain your existing settings. Follow the ZIP's `INSTALL.txt` for your platform and check its instructions for existing DirectInput proxies.

[Windows instructions](docs/INSTALL-WINDOWS.txt) · [Linux / Proton instructions](docs/INSTALL-LINUX.txt) · [Mac / CrossOver instructions](docs/INSTALL-MAC.txt) · [Optional OpenXR registration](docs/OPENXR.txt)

Linux/Proton uses the documented launch wrapper and DLL override. In CrossOver, set `dinput8` to **Native then Builtin**. Open the bundled Mac app once for its silent capability check, then launch Elite normally. The companion requires macOS 11.3 or later and is ad hoc signed. Follow the Mac guide if macOS asks for approval. OpenXR users register the layer once before use.

## Developer roadmap

### Native restoration

The game still dispatches the original Motion effects. EDUP - NHOE restores the gain and quiet main-motor output on the supported build, preserving the authored envelopes, conditional mutes and stops. Familiar ship and vehicle cues, such as docking and collision, are useful checks with added effects disabled.

| Area | Implemented | Next milestone |
| --- | --- | --- |
| Authored output | Restores the shared Motion path and preserves silence for an authored zero. [Runtime](src/restoration_runtime.cpp) | Test faint portions, fades and stops on hardware. Confirm repeatable playback with all added effects disabled. |
| Ship impacts | Diagnostic associations cover 27 impact, collision and friction event routes linked to 17 authored sources. The game controls when those effects play and stop. [Native motion metadata](include/edtrace/native_motion_routes.hpp) | Match the three unresolved Corvette routes to repeatable gameplay actions. Confirm local shield and hull damage ownership before adding damage feedback. |
| Game compatibility | The runtime checks the exact supported game version before applying its in-memory changes. [Testing guide](docs/TESTING.md) | Validate each new Elite build and retest familiar authored effects before enabling it. |

### Odyssey and supplemental effects

These features use new haptic patterns keyed to mapped gameplay cues. An existing cue can supply timing for a new pattern even when the game supplied no vibration for that action.

| Area | Implemented | Next milestone |
| --- | --- | --- |
| On-foot movement | Mapped running and sprinting contacts, jump and landing impulses, and jetpack ignition, thrust and cutoff. Walking and unknown gait stay quiet. [On-foot effects](src/onfoot_haptics.cpp) | Verify the inferred walk/run boundary and emitter matching on different surfaces. Add surface coverage once a local running contact can be identified reliably. |
| On-foot interactions | Patterns for mapped weapon fire, reloads, melee, shield toggles and selected completion cues. [Weapon patterns](include/edtrace/onfoot_profiles.hpp) | Map more tools, including scanning, cutting and sampling, from distinct local start, completion and cancellation cues. Verify silence for nearby players and stale events. |
| Elevators | Movement cues are pending. [Testing guide](docs/TESTING.md) | Find motion start and stop cues shared by station and fleet carrier elevators. Verify silence during the door cycle. |
| SRVs | The Scorpion adds a light right-trigger turret pulse and a longer left-trigger and body pulse for missile ejection. [On-foot and mounted effects](src/onfoot_haptics.cpp) | Confirm shots, launches and vehicle exit on hardware. Identify local shield-break cues for the Scorpion and Scarab. Check the Rhino for authored Motion before designing a profile. |
| Ship hardpoints | Main-motor feedback for mapped weapons, including held beams and individual multicannon shots. Matching native stops end sustained beam output. [Ship weapon effects](src/ship_weapon_haptics.cpp) | Test release timing and simultaneous hardpoints in combat. Extend the catalog with confirmed player-attached cues and tune patterns by weapon family and size. |
| Supercruise Overcharge | A 2.5-second uneven shake on engagement and a smooth 2.5-second slowdown on disengagement. [SCO effects](src/sco_haptics.cpp) | Check both transitions against the game's exported state timing, including focus changes and ordinary supercruise jumps. |

### Output and compatibility

| Route | Implemented | Next milestone |
| --- | --- | --- |
| Windows and Proton gamepads | GameInput carries separate main and trigger channels when the matched device and transport expose them. XInput provides main-motor fallback. [GameInput output](src/gameinput_output.cpp) | Verify each motor across supported devices and transports. Isolate the Proton capability and delivery gap with a game-independent GameInput test. |
| CrossOver gamepads | The gamepad-only build has a native macOS companion and GameInput/XInput fallback. A tester felt XInput rumble in an Intel bottle. ARM64EC/FEX remains experimental. [Mac installation and diagnostics](docs/INSTALL-MAC.txt) | Compare GameInput and XInput in the same bottle. Diagnose companion startup and verify each native actuator plus fallback on Intel/Rosetta and ARM64EC/FEX. |
| SteamVR and legacy Meta | The adapters blend trigger contributions into the corresponding hand. Detection is automatic once the adapter loads. [Windows installation](docs/INSTALL-WINDOWS.txt) | Test per-hand playback, quieting on focus loss and handoff on physical headsets. |
| OpenXR | Uses the application's existing negotiated Touch Pro profile and bound trigger outputs when available. Other profiles use per-hand output. [OpenXR setup](docs/OPENXR.txt) | Verify hand and trigger localization on suitable hardware and confirm per-hand output on profiles with fewer endpoints. |

## Settings

The supplied `ed-onfoot-haptics.ini` enables added effects at 60%:

```ini
[OnFoot]
Enabled=1
Strength=60

[SCO]
Enabled=1
Strength=60

[ShipCombat]
Enabled=1
Strength=60

[Controller]
Output=auto
```

`Strength` ranges from 0 to 100. Set `Enabled=0` in `[OnFoot]`, `[SCO]`, and `[ShipCombat]` to play the original authored effects only. Keep your configuration when upgrading. To enable ship weapon feedback in an older configuration, add the `[ShipCombat]` section shown above.

`Output=auto` keeps GameInput available and enables native discovery in the Mac package. For a two-motor compatibility check, set `Output=xinput` and restart Elite. This setting changes haptic output only.

## Compatibility and testing

The supported executable version is `2026.10.23.334925`, SHA-256:

```text
ed5a8f8cd10f9a80d3bda5badd7ba1d4fcfc15996c8c7e018852cb170e264b63
```

Unsupported versions leave restoration disabled. Four-motor output depends on what the controller, connection and runtime expose. Steam Input can present a virtual device with fewer capabilities. Linux HID access uses existing system permissions.

Test one action at a time. Include the package version, platform and runtime version, controller and connection, Steam Input state, and which motors or hands you felt. Use the testing guide for focused cases and the Mac guide for session status files. [Testing guide](docs/TESTING.md) · [Report feedback](https://github.com/Fenris159/EDUP-NHOE/issues/new/choose)

## Build and contribute

Run the fast native fixtures with CMake 3.20+, Ninja and a C++17 compiler.

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel 2
ctest --test-dir build/native --output-on-failure
```

For a roadmap item, start with one repeatable gameplay case, document the evidence for the local emitter and its stop behavior, and add coverage for the outcome. Record synthetic API checks and felt hardware output separately. Protect existing game input, runtime selection and ownership when changing a haptic route. [Platform build and packaging guide](BUILDING.md) · [Contribution guide](CONTRIBUTING.md)

Third-party components retain their included terms. [Dependency notices](THIRD-PARTY.md)
