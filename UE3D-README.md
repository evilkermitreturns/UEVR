# UEVR - Monitor Mode Fork

Fork of [praydog/UEVR](https://github.com/praydog/UEVR) with monitor mode — comfort features for playing in stereo 3D on a 3D monitor with [VRto3D](https://github.com/TheEvilKermit/VRto3D).

## What Monitor Mode Does

UEVR + VRto3D already give you stereo 3D on a monitor. That works fine on its own. The problem is that the 3D depth stays the same no matter what the game is doing — so when you aim down sights the gun looks doubled, scoped views are uncomfortably deep, and cutscenes can feel off. Monitor mode shifts to parallel projection and auto-adjusts the depth based on what the game's camera is doing, so you're not stuck with one depth setting that looks wrong half the time.

```
UE Game -> UEVR (stereo rendering) -> SteamVR -> VRto3D (SBS output) -> 3D Monitor
```

## What It Adds

- **Auto depth adjustment** — flattens depth when the game zooms so scopes and ADS don't look cross-eyed
- **Game state detection** — figures out if you're aiming, scoping, or in a cutscene and adjusts accordingly
- **Per-mode depth** — separate 3D strength for ADS, Scope, and Cutscene, so cutscenes can be subtle while gameplay pops
- **Stereo calibration** — enter your monitor size and viewing distance, it calculates the rest
- **HUD depth** — per-mode depth and size sliders for the HUD overlay so it sits at a comfortable depth
- **VRto3D bridge** — shared memory link so depth changes happen in both programs at once
- **Stuck camera fix** — detects and cleans up camera modifiers that get stuck after conversations or cinematics
- **Auto FOV tracking** — re-detects FOV after menu changes so depth classification doesn't get confused
- **Depth presets** — Comfort, Balanced, Preserve Depth, Minimal, or Custom
- **Own UI tab** — "Monitor 3D" sidebar tab, separate from VR settings

## Setup

1. Install [VRto3D](https://github.com/TheEvilKermit/VRto3D) (the monitor mode fork) and register it with SteamVR
2. Launch SteamVR — VRto3D should show as the active headset
3. Launch your UE4/5 game
4. Inject UEVR (this fork's build)
5. Open the UEVR overlay (Insert key)
6. Click the **Monitor 3D** tab on the left sidebar
7. Enter your screen height and viewing distance
8. Click **Calibrate** to sync with VRto3D
9. Use **VRto3D+** / **VRto3D-** buttons to dial in the 3D strength

You should see side-by-side 3D in VRto3D's headset window. Switch your monitor to 3D mode and you're set.

## Menu Guide

The Monitor 3D tab follows a setup-flow layout — controls are in the order you'd use them when setting up a game.

### Display Setup

| Control | What it does |
|---------|-------------|
| IPD (mm) | Your eye spacing. Most people are 60-68mm. |
| Screen Height | Your monitor's screen height in inches (not diagonal). Click Detect to auto-read from Windows. |
| Viewing Distance | How far you sit from the screen in cm. |

These three values feed the stereo calibration math. Get them roughly right and the 3D will look correct for your setup.

### 3D Calibration

| Control | What it does |
|---------|-------------|
| VRto3D++ / + / - / -- | Adjust VRto3D's depth (eye separation). Start with Calibrate, then fine-tune with +/-. |
| Calibrate | Auto-calculate depth from your display measurements. Do this first. |
| 3D Strength | Overall stereo strength on the UEVR side (0 = off, 1 = normal, 2 = exaggerated). |
| Sync Depth to VRto3D | Sends depth multiplier to VRto3D so both programs stay in sync during zoom. |

### Auto-Depth

| Control | What it does |
|---------|-------------|
| Preset | Comfort (nearly flat when zoomed), Balanced (recommended), Preserve Depth (keeps strong 3D), Minimal (least reduction), Custom. |
| Min 3D Depth | Floor for auto-depth flattening. 0% = can go fully flat, 100% = never flattens. |

### HUD Depth & Size

Per-mode depth and size for the HUD overlay. Each mode (Normal, ADS, Scope, Cutscene) has its own depth slider [-2 to +2] and size slider [0.5x to 2.0x].

| Control | What it does |
|---------|-------------|
| HUD 3D Depth | Depth during normal gameplay. Negative = popout, 0 = flat, positive = into the world. |
| Canvas HUD Hook | Hooks the engine's canvas system for HUD depth. OFF by default — may crash some games. |
| Auto HUD Size | Experimental. Auto-scales HUD size based on depth distance. |

### Depth Tuning

Fine-tuning for the adaptive depth curve. Most people won't need to touch these.

| Control | What it does |
|---------|-------------|
| Flattening Strength | How aggressively depth reduces during zoom. Increase if guns look doubled in ADS. |
| Per-Mode Floors | Minimum depth for ADS, Scope, and Cutscene separately. |
| Flatten: Aiming / Scoped / Cutscene | Per-mode multipliers [-2 to 100]. 1 = default, higher = more flat, negative = popout. |

### Expert

| Control | What it does |
|---------|-------------|
| FOV Tracking | Auto (reads from engine) or Manual (you calibrate). Auto works for most games. |
| Calibrate FOV | Press while NOT zoomed to capture your game's normal FOV. |
| Zoom thresholds | Where ADS and Scope zones start. Watch the live zoom indicator while aiming to tune. |
| Transition speeds | How fast depth ramps down (attack) and recovers (release). |
| Depth Auto-Scale | Experimental depth buffer mode. Crashes most UE5 games. |

### Status

Live telemetry — FOV readings, zoom state, depth engine, VRto3D bridge connection. Useful for debugging weird behavior.

## Known Limitations

- **16:9 only** — no ultrawide 3D monitors exist yet
- **SteamVR required** — the pipeline goes through SteamVR even though there's no headset
- **Shader compatibility** — some UE4/5 games have stereo rendering issues. Same limitations as base UEVR.
- **AHUD games** — games using Unreal's AHUD class (Bloodstained, Asterigos, Sinking City, Styx) bypass the HUD depth system. 3D still works, HUD depth won't.
- **Depth buffer (experimental)** — crashes most UE5 games. Behind an opt-in toggle.

## Building From Source

Requires Visual Studio 2025 with C++23 support. CMake for build generation.

```bash
# Generate build files
cmake -B build -G "Visual Studio 18 2025"

# Build
"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" build/uevr-proj.sln -p:Configuration=Release -m -verbosity:minimal
```

Output: `build/bin/uevr/UEVRBackend.dll`

## Credits

- [praydog](https://github.com/praydog) for UEVR — the foundation this is built on
- [oneup03](https://github.com/oneup03) for VRto3D — the virtual headset driver that makes monitor output possible
- [Asxcvbn](https://github.com/Asxcvbn) for convergence and depth controls in OpenXR mode ([PR #372](https://github.com/praydog/UEVR/pull/372))
- The UEVR and VRto3D communities for testing and feedback
