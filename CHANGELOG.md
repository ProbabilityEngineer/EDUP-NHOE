# Changelog

## 0.12.1-preview.7 (unreleased)

- Keep the amplitude cap for the two authored main-motor channels through a verified audio callback when CrossOver cannot allocate the regular motor-sender hook.
- Include the original MinHook status and the selected headroom fallback in the startup status.

## 0.12.1-preview.6 (unreleased)

- Let the Mac audio output slot use either verified XInput entry point from the loader import or export lookup. Restore the exact binding on shutdown.
- Add a Mac output mode that tries Core Haptics with XInput fallback. It skips GameInput and preserves the game's controller input.
- Report the startup stage, motor headroom hook error, and GameInput attempt status in the existing diagnostics.

## 0.12.1-preview.5

- Give SCO engagement a 2.5-second uneven shake that settles as the ship stabilizes. Disengagement uses a smooth 2.5-second slowdown.
- Release supplemental beam rumble when a matching native stop targets the active playing instance, including stops that give the audio a fade.

## 0.12.1-preview.4 (unreleased)

- Add main-motor feedback for additional ship weapons, including beam and pulse lasers, multicannons, plasma weapons and launchers.
- Require a registered player-attached hardpoint for supplemental ship feedback. Use actual shot cues for multicannons and silence overlapping callbacks when their firing order is ambiguous.
- Enable ship feedback at 60% in new preview installs. Existing configurations can add the separate ShipCombat section.
- Wait for the Mac capability checker to finish launching before enumerating controllers. Show the detected device and allow a retry.

## 0.12.1-preview.3 (unreleased)

- Add a 300 ms main-motor surge when Supercruise Overcharge engages and a 150 ms jolt when it disengages within supercruise. The proxy reads the game's exported SCO state.
- Enable SCO feedback at 60% by default. The INI has separate SCO enable and strength settings.

## 0.12.1-preview.2 (unreleased)

- Connect 24 named native impact, collision and friction event mappings plus three unresolved Corvette-specific routes to 17 existing authored Motion sources in optional diagnostic recordings.
- Record selected self shield/hull strength and collision/friction parameters for support analysis, preserving the manual ten-minute observation limit.
- Consume each recording request before opening a trace so an exit or failed cleanup cannot restart it on relaunch.
- Deliver quiet authored main-motor frames that the original PC converter rounded to silence. Extend the response to zero below the native cutoff and keep the original response above it.
- Verify recorder forwarding and metadata using owned fixtures, including global parameter IDs and observation expiry.

## 0.12.0-preview.1

- Add a universal arm64/x86_64 macOS GameController/Core Haptics companion with automatic launch and independent handle/trigger output.
- Select native output conservatively for one native Xbox controller, one XInput gamepad and four usable actuator engines; preserve fallback for unavailable or ambiguous devices.
- Add authenticated loopback transport, bounded pulses, source/peer expiry, controller-generation fencing and quiet provider handoffs.
- Add a no-vibration capability check, native status reporting and a playback-failure backoff. Keep controller input and graphics configuration unchanged.
- Require macOS CI, a signed universal native app and all three platform bundles before publication. New native four-motor delivery still needs physical testing.

## 0.11.0-preview.1

- Add a minimal Mac/CrossOver gamepad package with five copyable files and one installation guide. Physical Mac output remains untested; ARM64EC/FEX is experimental.
- Route the Mac profile through verified game-side XInput bindings, preserving system XInput code and controller input APIs. Wait for audio initialization and stop restoration on foreign slot ownership.
- Preserve native GameInput capabilities on Mac, with XInput fallback and an optional startup-only `Output=xinput` setting for compatibility troubleshooting on all platforms.
- Retain the same authored Motion correction and Odyssey/Scorpion patterns, including disabled walking contacts and enabled running/sprinting.
- Always assemble Windows, Linux/Proton and Mac/CrossOver bundles together and validate their checksums before publication.

## 0.10.1-preview.1

- Disable walking contacts while retaining running/sprinting through a native speed gate; gameplay confirmation remains pending.
- Keep four-channel GameInput output, XInput fallback and capability-aware VR output routing.
- Fix fallback focus/shutdown stops, stale-frame submission ordering and bounded queue draining.
- Reject queued inactive effects across readiness changes and preserve authored Motion when additions are disabled.
- Preserve native API behavior for unrelated controller indices and the application's OpenXR instance negotiation.
- Provide separate Windows and Linux/Proton manual-copy previews with checksums and installation instructions.
