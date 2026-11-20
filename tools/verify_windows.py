#!/usr/bin/env python3
"""Run synthetic Windows boundary fixtures; no game or controller is used."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def windows(path):
    return str(path) if os.name == "nt" else "Z:" + str(path).replace("/", "\\")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--vr-build", type=Path)
    args = parser.parse_args()
    build = args.build.resolve()
    if not (build / "CMakeCache.txt").is_file():
        parser.error("--build must be a configured Windows build directory")
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("ED_RUMBLE_", "ED_VR_", "STEAM_COMPAT_"))
           and k not in ("WINEPREFIX", "WINEARCH", "WINEDLLOVERRIDES")}
    prefix = [] if os.name == "nt" else ["wine"]
    if prefix:
        env.update(WINEPREFIX=str(build / "owned-test-prefix"), WINEARCH="win64",
                   WINEDEBUG="-all", WINEDLLOVERRIDES="dinput8=n,b;winemenubuilder.exe=d")
    runs = []
    with tempfile.TemporaryDirectory(prefix="owned-checks-", dir=build) as directory:
        owned = Path(directory)

        def run(name, executable, arguments=(), result=False):
            if not executable.is_file():
                raise FileNotFoundError(f"Build the fixture first: {executable}")
            output = owned / (name + ".txt")
            command = prefix + [str(executable), *map(str, arguments)]
            if result:
                command.append(windows(output))
            process = subprocess.run(command, env=env, text=True, capture_output=True, timeout=90)
            if process.returncode or (result and (not output.is_file() or output.read_text() != "PASS")):
                raise RuntimeError(f"{name} failed ({process.returncode}):\n{process.stdout}\n{process.stderr}")
            runs.append({"name": name, "returncode": process.returncode})
            print("PASS", name, flush=True)

        for name in ("natural_gain", "motor_ownership_policy", "vr_haptics", "mac_haptics", "mac_controller_check",
                     "native_motion_metadata", "onfoot_haptics", "ship_weapon_haptics", "ship_weapon_motion", "ship_motion_playback", "sco_haptics", "openxr_output"):
            run(name, build / (name + "_verification.exe"))
        run("session_retention", build / "session_retention_verification.exe", [windows(owned / "session-retention")])
        run("ship_motion_ordering", build / "ship_motion_ordering_verification.exe")
        trace = owned / "native-impact-recording"
        trace.mkdir()
        run("native_impact_recording", build / "onfoot_event_trace_verification.exe", [windows(trace)], result=True)
        run("gameinput_adapter", build / "gameinput_adapter_verification.exe", result=True)
        run("mac_output_relay", build / "mac_output_verification.exe", result=True)
        if prefix:
            native = owned / "Mac user's $() `literal` 漢字"
            native.mkdir()
            (native / "ed-macos-haptics.app").mkdir()
            opener = native / "owned-opener"
            opener.write_text("#!/usr/bin/python3\nimport json,pathlib,sys\nargs=sys.argv[1:]\npathlib.Path(args[args.index('--status')+1]).write_text(json.dumps(args))\n")
            opener.chmod(0o755)
            run("mac_native_launch",build / "mac_output_verification.exe",["launch",windows(opener),windows(native)],result=True)
            capture=native / "macos-haptics-status.json"
            deadline=time.monotonic()+3
            while not capture.exists() and time.monotonic()<deadline:
                time.sleep(.01)
            arguments=json.loads(capture.read_text())
            if arguments[:4]!=["-g","-n","-a",str(native / "ed-macos-haptics.app")] or arguments[4:6]!=["--args","--port"] or not 0<int(arguments[6])<65536 or arguments[7:]!=["--token","5a"*16,"--status",str(capture)]:
                raise RuntimeError("Native launcher arguments changed or lost literal path characters")
        backend = windows(build / "verification/backend/ed_rumble_real_xinput9_1_0.dll")
        run("minhook_image_fallback", build / "minhook_image_fallback_verification.exe", [backend])
        for mode in ("patch-restore", "remove-retry", "create-retry", "rip-range", "patch-clearance", "near-retry",
                     "thread-context"):
            run("minhook_image_" + mode.replace("-", "_"), build / "minhook_image_fallback_verification.exe",
                [backend, mode])
        for mode in ("verified", "unknown"):
            run("output_" + mode, build / "xinput_output_verification.exe", [backend, mode], result=True)
            run("crossover_output_" + mode, build / "crossover_output_verification.exe", [backend, mode], result=True)
        run("crossover_output_alias", build / "crossover_output_verification.exe", [backend, "alias"], result=True)
        run("output_shutdown", build / "xinput_output_verification.exe", [backend, "shutdown"], result=True)
        run("crossover_output_shutdown", build / "crossover_output_verification.exe", [backend, "shutdown"], result=True)
        run("page_protection_copy_on_write", build / "page_protection_verification.exe", result=True)
        run("page_protection_readonly_reproduces_fault", build / "page_protection_verification.exe", ["readonly"], result=True)
        run("motor_headroom_frame_slot", build / "motor_headroom_slot_verification.exe")
        run("motor_headroom_foreign_slot", build / "motor_headroom_slot_verification.exe", ["foreign"])
        run("motor_headroom_initialize_failure", build / "motor_headroom_slot_verification.exe", ["initialize"])
        run("motor_headroom_failed_readback", build / "motor_headroom_slot_verification.exe", ["readback"])
        run("motor_headroom_rollback_retry", build / "motor_headroom_slot_verification.exe", ["readback-retry"])
        run("motor_headroom_writecopy", build / "motor_headroom_slot_verification.exe", ["writecopy"])
        run("motor_headroom_writecopy_conflict", build / "motor_headroom_slot_verification.exe", ["writecopy-conflict"])
        run("dinput_controlled", build / "dinput_proxy_verification.exe",
            [windows(build / "verification/dinput/dinput8.dll"), "controlled"], result=True)
        run("dinput_system", build / "dinput_proxy_verification.exe",
            [windows(build / "restoration-dinput8/dinput8.dll"), "system"], result=True)
        run("dinput_crossover", build / "dinput_proxy_verification.exe",
            [windows(build / "crossover-dinput8/dinput8.dll"), "system"], result=True)
        chain = owned / "chain"
        chain.mkdir()
        shutil.copyfile(build / "restoration-dinput8-chain/dinput8.dll", chain / "dinput8.dll")
        shutil.copyfile(build / "verification/dinput/backend/ed_real_dinput8.dll", chain / "ed_rumble_dinput8_previous.dll")
        run("dinput_chain", build / "dinput_proxy_verification.exe", [windows(chain / "dinput8.dll"), "chain"], result=True)
        if args.vr_build:
            vr = args.vr_build.resolve()
            run("vr_mirror", vr / "vr_mirror_verification.exe", [backend, "verified"], result=True)
            run("vr_broker", vr / "vr_broker_verification.exe")
    report = {"passed": True, "scope": "Owned API/controller fixtures only; no game, headset or motor output.", "runs": runs}
    (build / "owned-verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS", len(runs), "synthetic boundary checks")


if __name__ == "__main__":
    main()
