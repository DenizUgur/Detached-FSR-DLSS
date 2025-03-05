import re
import os
import sys
import json
import copy
import time
import random
import signal
import argparse
import subprocess
from glob import glob
from datetime import datetime, timezone

# Windows specific imports
import win32gui
import win32api
import win32process
import win32con
import ctypes

# Other
import numpy as np
import pyautogui
import matplotlib.pyplot as plt
from skimage import img_as_float, io
# pylint: disable-next=E0611
from skimage.metrics import (
    peak_signal_noise_ratio,
    structural_similarity,
    mean_squared_error,
)

__doc__ = """
This script is used to run FSR tests. It starts the renderer and upscaler processes
and collects metrics from them and the GPU. The metrics are then printed to the
console in JSON format.
"""
pyautogui.FAILSAFE = False
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
FSR_DIR = os.path.join(SCRIPT_DIR, "bin")
TSR_SHARED_BUFFER_COUNT = 10

UPSCALERS = [
    "Native",
    "Point",
    "Bilinear",
    "Bicubic",
    "FSR1",
    "FSR2",
    "FSR3Upscale",
    "FSR3",
    "DLSSUpscale",
    "DLSS",
]

# List of upscalers that require jitter, otherwise it should be disabled
NEEDS_JITTER = ["FSR2", "FSR3Upscale", "FSR3", "DLSSUpscale", "DLSS"]

# List of upscalers that has Frame Generation
FRAME_GENERATION = ["FSR3", "DLSS"]

DLSS_MODES = [
    "Performance",
    "Balanced",
    "Quality",
    "UltraPerformance",
]

# Get the default config
with open(
    os.path.join(SCRIPT_DIR, "samples/fsr/config/fsrconfig.json"), "r", encoding="utf-8"
) as f:
    config = json.load(f)


with open(
    os.path.join(SCRIPT_DIR, "framework/cauldron/framework/config/cauldronconfig.json"),
    "r",
    encoding="utf-8",
) as f:
    framework_config = json.load(f)


def utcnow_iso8601():
    return datetime.now(timezone.utc).isoformat()


# Prepare the config
def get_config(mode, opts):
    tmp = copy.deepcopy(config)
    tmp_framework = copy.deepcopy(framework_config)

    # Apply mode specific settings
    tmp["FidelityFX FSR"]["TSR"]["Mode"] = mode

    # Apply present resolution settings
    tmp["FidelityFX FSR"]["Presentation"]["Width"] = opts.present_res[0]
    tmp["FidelityFX FSR"]["Presentation"]["Height"] = opts.present_res[1]

    # Apply FPS settings
    tmp["FidelityFX FSR"]["FPSLimiter"]["UseReflex"] = False  # Not operational
    tmp["FidelityFX FSR"]["FPSLimiter"]["TargetFPS"] = opts.fps
    tmp["FidelityFX FSR"]["FPSLimiter"][
        "UseGPULimiter"
    ] = False  # Messes with GPU metrics

    # Set the scene exposure
    tmp["FidelityFX FSR"]["Content"]["SceneExposure"] = opts.scene_exposure

    # Apply scene settings
    if opts.scene == "Sponza":
        tmp["FidelityFX FSR"]["Content"]["Scenes"] = [
            "../media/SponzaNew/MainSponza.gltf"
        ]
        tmp["FidelityFX FSR"]["Content"]["Camera"] = "PhysCamera003"
    elif opts.scene == "Brutalism":
        tmp["FidelityFX FSR"]["Content"]["Scenes"] = [
            "../media/Brutalism/BrutalistHall.gltf"
        ]
        tmp["FidelityFX FSR"]["Content"]["Camera"] = "persp9_Orientation"
    elif opts.scene == "Bistro":
        tmp["FidelityFX FSR"]["Content"]["Scenes"] = [
            "../media/BistroInterior/BistroInrterior.gltf"
        ]
        tmp["FidelityFX FSR"]["Content"][
            "Camera"
        ] = "Camera_6"  # 5 is visually better but 6 is more GPU intensive
    else:
        raise ValueError("Invalid scene")

    # Apply reduced motion settings
    if opts.reduced_motion:
        tmp["FidelityFX FSR"]["Content"]["ParticleSpawners"] = []
        tmp["FidelityFX FSR"]["TSR"]["RenderModules"]["Default"].remove(
            "AnimatedTexturesRenderModule"
        )
        tmp["FidelityFX FSR"]["TSR"]["RenderModules"]["Renderer"].remove(
            "AnimatedTexturesRenderModule"
        )

    # Apply the animation configuration
    spd = 0.002 * 60 / opts.fps
    if opts.scene == "Sponza":
        tmp["FidelityFX FSR"]["Content"]["Animation"] = {
            "Enabled": not opts.disable_animation,
            "p": 12.0,
            "q": 3.0,
            "xo": 2.0,
            "yo": 3.0,
            "zo": 0.0,
            "spd": spd,
            "lx": -8.0,
            "ly": 2.0,
            "lz": 0.0,
        }
    elif opts.scene == "Brutalism":
        tmp["FidelityFX FSR"]["Content"]["Animation"] = {
            "Enabled": not opts.disable_animation,
            "p": 10.0,
            "q": 3.0,
            "xo": 1.0,
            "yo": 5.0,
            "zo": -20.0,
            "spd": spd,
            "lx": 30.0,
            "ly": 2.0,
            "lz": -30.0,
        }
    elif opts.scene == "Bistro":
        tmp["FidelityFX FSR"]["Content"]["Animation"] = {
            "Enabled": not opts.disable_animation,
            "p": 3,
            "q": 1,
            "xo": -0.05,
            "yo": 3.8,
            "zo": -0.9,
            "spd": spd,
            "lx": 12.0,
            "ly": 3.0,
            "lz": -3.5,
        }

    # Remove content on upscaler if running in detached mode
    if mode == "Upscaler":
        del tmp["FidelityFX FSR"]["Content"]["Scenes"]
        del tmp["FidelityFX FSR"]["Content"]["Camera"]
        del tmp["FidelityFX FSR"]["Content"]["Animation"]
        del tmp["FidelityFX FSR"]["Content"]["DiffuseIBL"]
        del tmp["FidelityFX FSR"]["Content"]["SpecularIBL"]

    # Apply upscaler settings
    tmp["FidelityFX FSR"]["TSR"]["Upscaler"] = UPSCALERS.index(opts.upscaler)

    # Apply render settings
    tmp["FidelityFX FSR"]["Render"] = {
        "EnableJitter": opts.upscaler in NEEDS_JITTER,
        "InitialRenderWidth": opts.render_res[0],
        "InitialRenderHeight": opts.render_res[1],
    }

    # Apply DLSS settings
    if "DLSS" in opts.upscaler:
        tmp["FidelityFX FSR"]["TSR"]["RenderModuleOverrides"]["Default"][
            "DLSSUpscaleRenderModule"
        ]["mode"] = (DLSS_MODES.index(opts.dlssMode) + 1)
        tmp["FidelityFX FSR"]["TSR"]["RenderModuleOverrides"]["Upscaler"][
            "DLSSUpscaleRenderModule"
        ]["mode"] = (DLSS_MODES.index(opts.dlssMode) + 1)

    # Apply streaming settings
    if opts.stream and mode in ("Default", "Upscaler"):
        tmp["FidelityFX FSR"]["Stream"] = {
            "Enabled": True,
            "Host": "https://localhost",
            "Port": 4443,
            "Name": "live",
        }
    else:
        del tmp["FidelityFX FSR"]["Stream"]

    # Modify the allocations
    if mode == "Upscaler":
        tmp_framework["Cauldron"]["Allocations"] = {
            "UploadHeapSize": 41943040,  # / 10
            "DynamicBufferPoolSize": 7864320,
            "GPUSamplerViewCount": 300,
            "GPUResourceViewCount": 6000,  # / 10
            "CPUResourceViewCount": 5000,  # / 10
            "CPURenderViewCount": 100,
            "CPUDepthViewCount": 100,
        }

    return {
        "sample": tmp,
        "framework": tmp_framework,
    }


def apply_config(config_data):
    with open(
        os.path.join(FSR_DIR, "configs/fsrconfig.json"), "w", encoding="utf-8"
    ) as config_file:
        json.dump(config_data["sample"], config_file, indent=4)
    with open(
        os.path.join(FSR_DIR, "configs/cauldronconfig.json"), "w", encoding="utf-8"
    ) as config_file:
        json.dump(config_data["framework"], config_file, indent=4)


def parse_args():
    parser = argparse.ArgumentParser(description="Run FSR tests")
    parser.add_argument(
        "--render-res",
        type=int,
        nargs=2,
        default=[1920, 1080],
        help="Render resolution (width height)",
    )
    parser.add_argument(
        "--present-res",
        type=int,
        nargs=2,
        default=[2560, 1440],
        help="Present resolution (width height)",
    )
    parser.add_argument(
        "--dlssMode",
        type=str,
        default="Performance",
        choices=DLSS_MODES,
        help="DLSS mode to use",
    )
    parser.add_argument(
        "--fps",
        type=int,
        default=60,
        help="The FPS to run the renderer at",
    )
    parser.add_argument(
        "--reduced-motion",
        action="store_true",
        default=False,
        help="Enable reduced motion mode, disables animated textures and particles",
    )
    parser.add_argument(
        "--benchmark",
        type=int,
        default=-1,
        help="Enable benchmark mode, sets the test duration in seconds",
    )
    parser.add_argument(
        "--scene",
        type=str,
        default="Sponza",
        choices=["Sponza", "Brutalism", "Bistro"],
        help="Scene to use",
    )
    parser.add_argument(
        "--scene-exposure",
        type=float,
        default=1.355,
        help="Scene exposure",
    )
    parser.add_argument(
        "--upscaler",
        type=str,
        default="DLSSUpscale",
        choices=UPSCALERS,
        help="Upscaler to use",
    )
    parser.add_argument(
        "--skip-renderer",
        action="store_true",
        default=False,
        help="Skip the renderer process",
    )
    parser.add_argument(
        "--skip-upscaler",
        action="store_true",
        default=False,
        help="Skip the upscaler process",
    )
    parser.add_argument(
        "--use-default",
        action="store_true",
        default=False,
        help="Use the default config, without decoupling the upscaler",
    )
    parser.add_argument(
        "--screenshot",
        type=str,
        choices=["image", "video"],
        help="Take a screenshot or video",
    )
    parser.add_argument(
        "--stream",
        action="store_true",
        default=False,
        help="Stream the upscaled content over Media-over-QUIC. Launches moq-relay.exe (unless --disable-supplementary is set) and configures the upscaler to stream the content",
    )
    parser.add_argument(
        "--hide-ui",
        action="store_true",
        default=False,
        help="Hide the UI, indirectly enabled by --stream",
    )
    parser.add_argument(
        "--disable-animation",
        action="store_true",
        default=False,
        help="Disable the animation for the camera",
    )
    parser.add_argument(
        "--disable-supplementary",
        action="store_true",
        default=False,
        help="Disable supplementary processes",
    )
    parser.add_argument(
        "--use-debug-build",
        action="store_true",
        default=False,
        help="Use the debug build of FidelityFX FSR",
    )
    parser.add_argument(
        "--structured-logs",
        action="store_true",
        default=False,
        help="Enable structured logs",
    )
    parser.add_argument(
        "--compare",
        type=str,
        choices=["ssim", "psnr", "mse"],
        help="Enable comparison with the reference image, will run the same configuration but without upscaler",
    )
    parser.add_argument(
        "--compare-fps",
        type=int,
        default=60,
        help="The FPS to run the comparison upscaler",
    )
    parser.add_argument(
        "--switch-default-mode",
        action="store_true",
        default=False,
        help="In compare mode, switch the default mode when running the reference image",
    )
    parser.add_argument(
        "--compare-with",
        type=str,
        help="Compare with the given upscaler",
        default="Native",
        choices=UPSCALERS,
    )
    parser.add_argument(
        "--enable-cursor-jitter",
        action="store_true",
        default=False,
        help="Enable cursor jitter",
    )
    return parser.parse_args()


def find_window_by_pid(pid, window_title="FidelityFX FSR"):
    def enum_windows_callback(hwnd, windows):
        if win32gui.IsWindowVisible(hwnd) and win32gui.IsWindowEnabled(hwnd):
            _, found_pid = win32process.GetWindowThreadProcessId(hwnd)
            if found_pid == pid:
                title = win32gui.GetWindowText(hwnd)
                if window_title is None or title == window_title:
                    windows.append(hwnd)

    windows = []
    win32gui.EnumWindows(enum_windows_callback, windows)
    return windows[0] if windows else None


def focus_by_pid(pid):
    hwnd = find_window_by_pid(pid)
    if hwnd:
        ctypes.windll.user32.SetForegroundWindow(hwnd)
        ctypes.windll.user32.ShowWindow(hwnd, win32con.SW_RESTORE)
        return True
    return False


def close_by_pid(pid):
    hwnd = find_window_by_pid(pid)
    if hwnd:
        win32api.PostMessage(hwnd, win32con.WM_QUIT, 0, 0)
        return True
    return False


def get_process_args(
    mode, screenshot_mode=None, duration=0, has_fg=False, hide_ui=False
):
    if screenshot_mode == "video":
        screenshot = "-screenshot-for-video"
    elif screenshot_mode == "image":
        screenshot = "-screenshot"
    else:
        screenshot = ""

    process_args = [
        (
            screenshot if mode != "Renderer" else ""
        ),  # Always ignore screenshot for renderer
        "-hide-ui" if hide_ui else "",
        "-displaymode",
        "DISPLAYMODE_LDR",
        "-benchmark",
        "json",
    ]

    if duration > 0:
        if mode != "Default" and has_fg:
            # BUG: Upscaler gets blocked for buffer count. Running for 10 more frames to avoid this.
            # Weird thing is that this is only needed for frame generation upscalers.
            #! Might only be needed if TSR_SHARED_BUFFER_COUNT (m_BufferIndex) is more than 1
            # For video, we set it to 1 anyway.
            # duration += TSR_SHARED_BUFFER_COUNT
            pass
        process_args.append(f"duration={duration}")
    return process_args


def main(opts):
    # Default process arguments
    exe_name = (
        "FFX_FSR_NATIVE_DX12D.exe"
        if opts.use_debug_build
        else "FFX_FSR_NATIVE_DX12.exe"
    )

    # If streaming is enabled, launch the moq-relay process
    opts.hide_ui = opts.hide_ui or opts.stream
    if opts.stream and not opts.disable_supplementary:
        relay = subprocess.Popen(
            [
                os.path.join(FSR_DIR, "moq-relay.exe"),
                "--bind",
                "[::]:4443",
                "--tls-cert",
                "cert/localhost.crt",
                "--tls-key",
                "cert/localhost.key",
                "--dev",
            ],
            cwd=FSR_DIR,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
        )
        if relay.poll() is not None:
            raise ValueError("Failed to start the relay process")
        if opts.structured_logs:
            print("RELAY_PID", relay.pid)
            sys.stdout.flush()
        else:
            print(f"Relay PID: {relay.pid}")

        web = subprocess.Popen(
            ["npm", "run", "preview"],
            shell=True,
            cwd=os.path.join(SCRIPT_DIR, "demo"),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
        )
        if web.poll() is not None:
            raise ValueError("Failed to start the web server process")
        if opts.structured_logs:
            print("WEB_PID", web.pid)
            sys.stdout.flush()
        else:
            print(f"Web PID: {web.pid}")
            print("Web server started at https://localhost:3000")

    # For Native rendering, match the render resolution to the present resolution
    if opts.upscaler == "Native":
        if opts.render_res != opts.present_res:
            if opts.structured_logs:
                raise ValueError(
                    "Native rendering is enabled, but the render resolution does not match the present resolution."
                )
            print(
                "Warning: Native rendering is enabled, but the render resolution does not match the present resolution. The render resolution will be set to the present resolution."
            )
            opts.render_res = opts.present_res

    # Create the renderer process
    mode = "Default" if opts.use_default else "Renderer"
    renderer_config = get_config(mode, opts)
    apply_config(renderer_config)
    if not opts.skip_renderer:
        renderer = subprocess.Popen(
            [
                os.path.join(FSR_DIR, exe_name),
                *get_process_args(
                    mode,
                    screenshot_mode=opts.screenshot,
                    duration=opts.benchmark * opts.fps,
                    has_fg=opts.upscaler in FRAME_GENERATION,
                    hide_ui=opts.hide_ui,
                ),
            ],
            cwd=FSR_DIR,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
        )
        if opts.structured_logs:
            print("RENDERER_PID", renderer.pid)
            sys.stdout.flush()
        else:
            print(f"Renderer PID: {renderer.pid}")

        # Wait until the renderer is ready
        os.set_blocking(renderer.stdout.fileno(), False)
        while True:
            line = renderer.stdout.readline()
            if b"Running" in line:
                break
            time.sleep(0.1)
    else:
        input("Press Enter to continue...")

    # Default mode implies skipping the upscaler
    skip_upscaler = opts.skip_upscaler
    if opts.use_default:
        opts.skip_upscaler = True

    # Wait until the renderer is ready
    if not opts.use_default:
        time.sleep(2)

        # Create the upscaler process
        upscaler_config = get_config("Upscaler", opts)
        apply_config(upscaler_config)
        # We do not encapsulate config creation in the following
        # if statement because we may use Visual Studio to debug the upscaler

    if not opts.skip_upscaler:
        upscaler = subprocess.Popen(
            [
                os.path.join(FSR_DIR, exe_name),
                *get_process_args(
                    "Upscaler",
                    screenshot_mode=opts.screenshot,
                    duration=opts.benchmark * opts.fps,
                    has_fg=opts.upscaler in FRAME_GENERATION,
                    hide_ui=opts.hide_ui,
                ),
            ],
            cwd=FSR_DIR,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
        )
        if opts.structured_logs:
            print("UPSCALER_PID", upscaler.pid)
            sys.stdout.flush()
        else:
            print(f"Upscaler PID: {upscaler.pid}")

        # Wait until the upscaler is ready
        os.set_blocking(upscaler.stdout.fileno(), False)
        while True:
            line = upscaler.stdout.readline()
            if b"Running" in line:
                break
            time.sleep(0.1)

    # Register signal handlers
    def cleanup(sig, _, non_zero=False):
        if not opts.structured_logs:
            print()
            print("Cleaning up...")

        # Try to gracefully close the processes
        if not opts.skip_renderer and renderer.poll() is None:
            close_by_pid(renderer.pid)

            # Wait for 5 seconds for the renderer to close
            start_time = time.time()
            while renderer.poll() is None and time.time() - start_time < 5:
                time.sleep(0.1)

        if not opts.skip_upscaler and upscaler.poll() is None:
            close_by_pid(upscaler.pid)

            # Wait for 5 seconds for the upscaler to close
            start_time = time.time()
            while upscaler.poll() is None and time.time() - start_time < 5:
                time.sleep(0.1)

        # Kill the processes if they are still running
        if not opts.skip_renderer and renderer.poll() is None:
            renderer.kill()

        if not opts.skip_upscaler and upscaler.poll() is None:
            upscaler.kill()

        # Close the relay process
        if opts.stream and not opts.disable_supplementary:
            relay.kill()
            web.kill()

        # Exit with the appropriate code
        if non_zero:
            sys.exit(1)

        # Get the return codes
        return_codes = {
            "renderer": renderer.returncode if not opts.skip_renderer else None,
            "upscaler": upscaler.returncode if not opts.skip_upscaler else None,
        }

        if not opts.structured_logs:
            print("Return codes:")
            print(json.dumps(return_codes, indent=4))

        if not opts.compare:
            for value in return_codes.values():
                if value is not None and value != 0:
                    sys.exit(value)
            sys.exit(0)

        if opts.compare and sig is not None:
            print(
                "In comparison mode, exiting is not allowed. You must wait for the test to finish."
            )

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    if not skip_upscaler:
        # Focus the renderer or upscaler window
        assert focus_by_pid(renderer.pid if opts.use_default else upscaler.pid), (
            "Could not focus the window. "
            "Please make sure the window is visible and not minimized."
        )

        if opts.enable_cursor_jitter:
            # In case focus fails, try manual focus
            pyautogui.moveTo(40, 20)
            pyautogui.click()
            pyautogui.moveTo(120, 20)
            pyautogui.click()

    dots = 0
    start_time = time.time()
    if opts.structured_logs:
        print("TEST_START", utcnow_iso8601())
        sys.stdout.flush()
    while True:
        # Check if the renderer is still running
        if not opts.skip_renderer and renderer.poll() is not None:
            break

        # Check if the upscaler is still running
        if not opts.skip_upscaler and upscaler.poll() is not None:
            break

        # If mouse is within jiggling range, move it
        cursor_pos = pyautogui.position()
        if (
            opts.enable_cursor_jitter
            and cursor_pos[0] > 500
            and cursor_pos[1] > 500
            and cursor_pos[0] < 900
            and cursor_pos[1] < 900
        ):
            # Jiggle the mouse to prevent the screen from turning off
            pyautogui.moveTo(600, 600)
            pyautogui.moveRel(
                100,
                random.randint(0, 100),
                duration=0.25,
                tween=pyautogui.easeInOutQuad,
            )

        # Bail out if test is taking too long
        if (
            opts.benchmark > 0
            and time.time() - start_time > opts.benchmark + 15
            and opts.screenshot != "video"
        ):
            cleanup(None, None, non_zero=True)
            break

        # Check telemetry
        processes = []
        if not opts.skip_renderer:
            processes.append(renderer)
        if not opts.skip_upscaler:
            processes.append(upscaler)

        while processes:
            p = processes[-1]
            line = p.stdout.readline()
            if not line:
                processes.pop()
                continue

            data = re.search(b"<TELEMETRY>(.*)</TELEMETRY>", line, re.IGNORECASE)
            if not data:
                continue

            if opts.structured_logs:
                print(f"TELEMETRY_{p.pid}", line.decode().strip())
                sys.stdout.flush()
            else:
                values = json.loads(data.group(1).decode().strip())
                print(
                    f"Got telemetry for {p.pid}: min={values['delta_ms']:.4f}ms{'.' * dots + ' ' * (4 - dots)}",
                    end="\r",
                )

        if not opts.structured_logs and opts.benchmark > 0:
            print(
                f"Waiting for process(es) to finish{'.' * dots + ' ' * (4 - dots)}",
                end="\r" if not opts.skip_upscaler else "\n",
            )
        dots = (dots + 1) % 4
        time.sleep(0.5)

    if opts.structured_logs:
        print("TEST_END", utcnow_iso8601())
        sys.stdout.flush()
    else:
        print()
        print("Process(es) finished")
    cleanup(None, None)

    return renderer.pid if opts.use_default else upscaler.pid


def load_image_for_pid(pid):
    image_path = glob(os.path.join(FSR_DIR, "benchmark", f"*_{pid}_*.jpg"))
    assert len(image_path) == 1, "Could not find the image"
    return img_as_float(io.imread(image_path[0]))


if __name__ == "__main__":
    args = parse_args()

    if args.compare:
        assert (
            args.screenshot != "video"
        ), "Cannot use screenshot for video in comparison mode"

    if args.skip_renderer and args.skip_upscaler:
        raise ValueError("Both renderer and upscaler cannot be skipped")

    # Run with the requested arguments
    test_pid = main(args)
    sys.stdout.flush()

    if args.compare:
        # Run the same configuration without the upscaler
        compare_args = parse_args()
        compare_args.upscaler = args.compare_with
        compare_args.fps = args.compare_fps
        if compare_args.switch_default_mode:
            compare_args.use_default = not args.use_default
        if compare_args.upscaler == "Native":
            compare_args.render_res = compare_args.present_res
        ref_pid = main(compare_args)

        # Load the images
        test_image = load_image_for_pid(test_pid)
        ref_image = load_image_for_pid(ref_pid)

        # Calculate the metrics
        if args.compare == "ssim":
            metric = structural_similarity(
                test_image, ref_image, data_range=1, channel_axis=2
            )
        elif args.compare == "psnr":
            metric = peak_signal_noise_ratio(test_image, ref_image, data_range=1)
        elif args.compare == "mse":
            metric = mean_squared_error(test_image, ref_image)
        else:
            raise ValueError("Invalid metric")

        # Print the metric
        if not args.structured_logs:
            print(f"{args.compare.upper()}: {metric:.6f}")
        else:
            print(f"METRIC_{args.compare.upper()}", metric)
            sys.stdout.flush()
            exit(0)

        # Calculate the difference image
        diff = np.abs(test_image - ref_image)
        ref_image[diff > 0.02] = 1

        # Show the difference
        fig, ax = plt.subplots(figsize=(16 * 1.5, 9 * 1.5))
        ax.imshow(ref_image)
        plt.title(f"{args.compare.upper()} = {metric:.6f}")
        fig.tight_layout()
        plt.show()
