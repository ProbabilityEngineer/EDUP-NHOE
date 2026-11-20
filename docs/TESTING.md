# Preview feedback

Report the package version, operating system, Proton version if applicable, controller model, USB/Bluetooth/wireless connection, Steam Input state, and VR frontend/runtime if used. For Mac, include CPU, CrossOver version, bottle engine (Rosetta or ARM64EC/FEX), selected Controller Output mode, restoration-status.json and macos-haptics-status.json from the newest session. The native four-motor route needs physical testing; its package is gamepad only. Open the native app manually for a capability check that sends no vibration. Test each trigger separately while CrossOver has focus and check that fallback returns if the companion is unavailable. Multiple visible controllers deliberately prevent automatic native selection.

Test one action at a time and report what you felt:

1. Original ship feedback during a familiar authored event, such as docking or undocking.
2. Walking, running and sprinting separately on the same surface. Walking should be quiet; running/sprinting should retain mapped contacts.
3. A short weapon burst and release, then a reload. Include weapon type and whether the feedback stopped.
4. A short jetpack boost, native cutoff and landing separately.
5. Scorpion turret fire and missile launch separately, noting main motors versus triggers or hands.
6. Focus loss/regain and disabling added effects. Background output should stop, and disabling additions should preserve any active original authored effect.
7. In a SCO-capable ship already in supercruise, engage SCO, wait, then disengage. Check for a rough shake that settles over 2.5 seconds and a smooth slowdown of the same length. Verify both effects are independent of the Odyssey setting. Ordinary jumps and regaining focus during SCO should add no SCO effect. The game exports the detection state to Status.json, so its write cadence affects timing.
8. Fire a short beam burst in combat, then release while the ship is otherwise steady. Report the beam size and whether vibration stops with the beam or continues into the audio tail.

For VR, identify whether the output was per hand or separately localized at a trigger. Stock SteamVR and legacy Meta output blend trigger contributions into the corresponding hand. OpenXR trigger separation requires the application's negotiated profile and bound endpoints.

The preview now sends quiet positive main-motor amplitudes that previously became zero at the PC converter's cutoff. The response joins the first native motor command above the cutoff. A zero source amplitude still stops that motor, and hardware may have its own minimum perceptible intensity. When testing a familiar authored cue, report whether quiet portions or fade-outs feel different and whether feedback stops when the cue ends.

Known preview gaps include surface coverage, the inferred walk/run threshold, broader tool/combat mapping, and physical validation across VR frontends. A missing effect should be reported with its exact action and context.

Native ship impact, collision and friction effects use the shared restored Motion output when the game dispatches them. The diagnostic mapping covers 27 native outer events and 17 existing main-motor sources, including authored sustained-impact and friction stops. Three Corvette-specific associations retain an unresolved gameplay role. Incoming shield-hit ownership is still unresolved.

When a support test explicitly enables the optional bounded recorder, its queue records now identify these known native routes and their play/stop source associations. They also retain selected `shield_strength_self`, `hull_strength_self`, `collision_velocity` and `friction_velocity` requests for timing comparisons. A recorded route is not proof that its nested source executed or that the affected ship was yours; ownership remains `unknown`. The self-strength parameter scale still needs live verification, and targeted-ship journal shield values must not be used as your own shield readings.

Each manually started observation stops within ten minutes and must end before the next test is started. The recorder consumes the request before recording so a leftover marker cannot reopen it on relaunch. Removing the marker cancels the current observation. The time limit applies to each observation window.

For the small fixed railgun and medium fixed cannon, test one discharge at a
time with `[ShipCombat]` enabled and a nonzero strength. The audio-only
firing variants can now call the matching authored Motion at its native
gain. Charging the railgun and nearby impacts should leave the linked
firing counters unchanged. The session status counts accepted and failed Motion posts in
`ship_motion_posts` and `ship_motion_rejected`. Feel the effect on hardware
to confirm delivery.
