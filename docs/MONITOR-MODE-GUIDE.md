# Monitor Mode Guide

Play any Unreal Engine 4 or 5 game in stereoscopic 3D on a 3D monitor, autostereoscopic display, or AR glasses — no VR headset needed. UEVR's Monitor Mode hijacks the engine's stereo rendering pipeline to produce two eye views, which VRto3D composites into a side-by-side image on your 3D display.

---

## What You Need

- **VRto3D** — A SteamVR virtual headset driver that puts the 3D output on your monitor. [Get it here](https://github.com/oneup03/VRto3D).
- **SteamVR** — Required as the compositor between UEVR and VRto3D.
- **A 3D-capable display** — 3D TV, 3D monitor, Leia autostereoscopic display, passive/interlaced display, or AR glasses.
- **UEVR Frontend** — The injector app that loads UEVR into your game.

---

## Quick Start

1. **Install VRto3D** — Copy the `vrto3d` folder into your `Steam\steamapps\common\SteamVR\drivers` folder. See the [VRto3D README](https://github.com/oneup03/VRto3D) for details.
2. **Launch SteamVR** — You should see the VRto3D headset window appear on your display.
3. **Launch your UE4/UE5 game** and let it reach the main menu.
4. **Open UEVR Frontend**, select your game from the process list, and click **Inject**.
5. **Open UEVR's menu** in-game (default: Insert key) and go to the **Monitor 3D** tab.
6. **Check "Enable Monitor Mode"** — the status should show "Connected" in green.
7. **Click Calibrate** — this auto-calculates a good starting depth from the game's world scale.
8. You should now see 3D on your display. Adjust **3D Strength** up or down to taste.

---

## Display Setup

These three settings describe your physical setup. Get them right once and they work for every game.

**IPD (mm)** — Your inter-pupillary distance (the spacing between your eyes). The default of 63mm works for most people. If you know yours, enter it. If not, leave it.

**Screen Height (inches)** — The vertical height of your screen, not the diagonal. For a 27" diagonal 16:9 monitor, this is about 13.2 inches. Click the **Detect** button and UEVR will try to read it from your monitor automatically. On Leia displays, it uses factory-calibrated dimensions for best accuracy.

**Viewing Distance (cm)** — How far you sit from the screen. Closer makes the 3D effect stronger. Farther makes it gentler. Measure from your eyes to your screen — most desktop setups are 50-80cm.

These three values together calculate the mathematically correct eye separation and convergence for your specific screen and seating position. If the 3D feels generally "off" across all games, one of these is probably wrong.

---

## 3D Calibration

**VRto3D Depth Buttons** — Five buttons that adjust VRto3D's depth setting:
- **VRto3D++** / **VRto3D+** — Increase depth (bigger / smaller jumps)
- **VRto3D-** / **VRto3D--** — Decrease depth
- **Calibrate** — Auto-calculates depth from the game's world scale. Good starting point for any game.

You can also use keyboard hotkeys: `Ctrl+F3`/`Ctrl+F4` to adjust depth while playing.

**3D Strength** (0.0 – 2.0) — The master intensity control for all 3D on the UEVR side. Think of it as a volume knob. 1.0 is physically correct. Turn it down if the 3D is too intense, up if it feels flat. This multiplies the convergence shift — the horizontal offset between left and right eye views.

**3D Effect bar** — A live readout showing your current depth as a percentage. 100% means full 3D. Watch it drop when you zoom in-game — that's the auto-depth system reducing eye strain.

---

## Auto-Depth

When your game zooms the camera (aiming down sights, using a scope, entering a cutscene), the narrower field of view amplifies the 3D effect and can cause eye strain or doubled images. The auto-depth system automatically reduces the 3D strength during zoom to keep things comfortable.

**Auto-Depth Preset** — Pre-made combinations of all the depth tuning settings:

| Preset | What it does |
|--------|-------------|
| **Comfort** | Maximum eye comfort. Nearly flat when zoomed. Best for long sessions. |
| **Balanced** | Good 3D with comfort. The recommended starting point. |
| **Preserve Depth** | Keeps strong 3D even when zoomed. More immersive but more eye strain during ADS. |
| **Minimal** | Least depth reduction. Almost no auto-flattening. Full 3D nearly all the time. |
| **Custom** | Shown when you've changed any individual setting. |

Click **Reset** to go back to Balanced at any time.

**Min 3D Depth** (0% – 100%) — A safety floor. The auto-depth system will never flatten below this percentage. Set it to 0% for maximum flattening range, 100% to effectively disable auto-depth (always full 3D), or somewhere in between as a compromise.

---

## HUD Controls

Expand **HUD Depth & Size** to access these. Each gameplay mode has its own depth and size sliders, and the system smoothly transitions between them as the game mode changes.

### HUD Depth (per mode, -2.0 to +2.0)

Controls how far in front of or behind the screen the HUD appears in 3D:
- **Negative** = HUD pops out toward you (in front of screen)
- **0** = HUD sits flat at the screen surface
- **Positive** = HUD recedes into the game world (behind screen)

| Slider | When it's active |
|--------|-----------------|
| **HUD 3D Depth** | Normal gameplay |
| **ADS HUD Depth** | Aiming down sights |
| **Scope HUD Depth** | Using a scope |
| **Cutscene HUD Depth** | Cutscenes and conversations |

### HUD Size (per mode, 0.5 to 2.0)

Multiplies the base **UI Size** (found under Runtime Info). 1.0 means no change. Reduce if the HUD feels too big during a specific mode, increase if it's too small.

### Auto HUD Size (Experimental)

When enabled, ignores the per-mode size sliders and auto-scales the HUD based on depth distance. Deeper depth means a larger HUD to fill the screen. Turn this off to use per-mode sliders individually.

### Canvas HUD Hook

**OFF by default.** Only turn this on if the game's HUD uses UE's canvas rendering system and you want depth on those elements. This hooks into the engine's canvas draw calls to add parallax. **Warning: can crash some games on injection.** The overlay HUD depth works without this — only enable Canvas HUD Hook if you need it and the game handles it.

---

## Depth Tuning

Expand **Depth Tuning** for fine-grained control over how the 3D effect behaves during zoom. Most users won't need to touch these — the presets handle it.

### Flattening Curve

**Flattening Strength** (0 – 2.0) — How aggressively the 3D reduces during zoom. If guns still look doubled when you aim down sights, increase this. If the 3D disappears too quickly when zooming, decrease it.

**Gentle or Aggressive** (0.1 – 2.0) — The shape of the reduction curve. Low values give a gradual fade (depth reduces slowly as you zoom more). High values give a quick drop (depth snaps flat early).

**Strong Zoom Falloff** (0 – 2.0) — Extra reduction at high zoom levels. 0 means the same rate everywhere. Higher values add more flattening at extreme zoom (deep scope). Useful if low-zoom scopes feel fine but max zoom gets uncomfortable.

**ADS Starts At** (1.0x – 1.5x) — Zoom below this factor is ignored entirely (stays in Normal mode). Above this factor with the aim button held = ADS mode. This prevents tiny camera movements from triggering the depth system.

### Per-Mode Floors

Minimum 3D depth for each mode. The auto-depth system can flatten down to this level but never below it. Think of these as safety nets — they only matter when the flattening curve would push below them.

| Slider | What it controls |
|--------|-----------------|
| **When Aiming** (0.01 – 0.50) | Floor during ADS. Some 3D is nice for depth perception, but too much doubles the gun model. |
| **When Scoped** (0.01 – 0.30) | Floor during scope. Scopes zoom hard, so this is usually the lowest floor. |
| **During Cutscenes** (0.01 – 0.60) | Floor during cutscenes. A higher floor keeps the cinematic 3D feel. |

### Per-Mode Flattening

These multiply the base flattening strength for each mode:

| Slider | What it controls |
|--------|-----------------|
| **Flatten: Aiming** | Scales flattening during ADS. 1.0 = same as base curve. Higher = more flat. ADS is a shallow zoom, so you may need 3-5+ for visible effect. |
| **Flatten: Scoped** | Scales flattening during scope. Deep zoom is sensitive — 1-2 is usually enough. |
| **Flatten: Cutscene** | Scales flattening during cutscenes. 0 = preserve full cinematic depth. |

---

## Expert Settings

Expand **Expert** for advanced controls. These rarely need changing.

### FOV Tracking

**FOV Tracking** — Auto or Manual. Auto reads the field of view from the engine each frame and works for most games. Switch to Manual only if depth behaves erratically (some games have unusual camera systems).

**Base FOV / Calibrate FOV** — Shows the game's normal (un-zoomed) field of view. Click **Calibrate FOV** while NOT aiming or zoomed to capture it. This is the reference point for zoom detection — if it's wrong, zoom classification may not work correctly.

### Zoom Classification

This section shows a live indicator of what zoom mode you're currently in:

- **Below dead zone** (gray) — Tiny zoom, ignored
- **ADS** (yellow) — Moderate zoom with aim held
- **SCOPE** (red) — Deep zoom with aim held
- Without aim held, any zoom = **Cutscene**

**When to Start Flattening** (1 – 20 deg) — How many degrees the FOV must change before zoom is detected at all. Most users won't need to change this.

**Scope Zoom Threshold** (1.1x – 3.0x) — The boundary between ADS and Scope mode. Zoom above this factor with aim held = Scope. Below = ADS. Aim your different weapons while watching the zone indicator to find the right split.

The zone map is shown below the slider: `[1.0 – dead zone: None] [dead zone – threshold: ADS] [threshold+: Scope]`

### Transition Speeds

**Attack Rate** (3 – 30 Hz) — How fast the 3D reduces when you start zooming. Higher = faster response.

**Release Rate** (3 – 20 Hz) — How fast the 3D recovers when you stop zooming. Higher = quicker snap-back to full 3D.

### FOV Smoothing

**Smooth FOV Transitions** — Smooths raw FOV readings from the engine. Enable this if the depth jitters during gameplay (some games report noisy FOV values).

**FOV Lerp Speed** (0.01 – 1.0) — How much smoothing to apply. Higher = less lag but less smoothing. Lower = very stable but adds slight delay.

### Depth Buffer (Experimental)

**Depth Auto-Scale** — Uses the game's depth buffer to adjust stereo based on scene content. Close objects get more flattening. **Experimental: crashes most UE5 games. Forced off on config load — you must re-enable each session if you want to use it.**

**Depth Sensitivity** (0.1 – 1.0) — How strongly the depth reading affects stereo. Start low.

---

## LookAround (Leia Displays)

For Leia autostereoscopic displays with face tracking. This adds head-tracking parallax — when you move your head, the scene shifts as if you're looking through a window into the game world.

**Important: Enable LookAround after loading into the game, not at the start menu.** The system needs the game's camera running to calibrate properly.

**Enable LookAround** — Turns on face tracking parallax.

**Sensitivity** (0.1 – 5.0) — How much the view shifts when you move. Higher = more dramatic parallax. Start at 1.0 and adjust to taste.

**Smoothing** (0.01 – 1.0) — Filters jitter from the face tracking. Lower = smoother but laggier. Higher = more responsive but may jitter.

**Axis Controls** — Enable or disable each axis independently:
- **X (horizontal)** — View shifts when you lean left/right
- **Y (vertical)** — View shifts when you lean up/down
- **Z (depth)** — View changes when you lean closer/farther. Also affects FOV and stereo intensity.

Each axis can be individually inverted if the direction feels wrong. Z also has a separate **Invert Z Stereo** toggle that flips the stereo scaling direction independently from the Z parallax direction.

**Motion Depth** (0 – 3.0) — Controls the depth-based parallax effect. Near objects shift more than far objects, creating a natural sense of depth when you move. "Near fast, far slow."

**Z Depth** (0 – 3.0) — Only shown when Z axis is enabled. Controls how much leaning forward/back affects the FOV and stereo intensity.

**Recalibrate** — Click if the parallax feels offset. Resets the face tracking origin to your current position.

**Status** shows your tracking state, head position, and display dimensions.

---

## Per-Game Settings

All Monitor Mode settings save automatically per game. When you inject UEVR into a game, it loads that game's saved settings. When you change settings, they persist for that game.

Config files are stored in the UEVR profile folder alongside the game executable — you don't need to manage them manually.

---

## Troubleshooting

| Problem | Likely Cause | What to Do |
|---------|-------------|------------|
| Flat / no 3D effect | Monitor Mode not enabled, or VRto3D not running | Check that **Enable Monitor Mode** is checked and shows "Connected" in green. Make sure SteamVR is running with VRto3D installed. |
| Painful or excessive 3D separation | 3D Strength or VRto3D depth too high | Reduce **3D Strength** slider. Use **VRto3D-** buttons or `Ctrl+F3` to reduce VRto3D depth. Also check **Screen Height** and **Viewing Distance** are accurate. |
| 3D doesn't change when zooming | Base FOV not calibrated | Go to Expert, click **Calibrate FOV** while NOT aiming. If FOV Tracking is on Manual, switch to Auto. |
| Guns look doubled when aiming (ADS) | Not enough flattening during zoom | Open Depth Tuning and increase **Flattening Strength**. Or switch to the **Comfort** preset. |
| Black VRto3D window | SteamVR compositor issue | Check that SteamVR is running and VRto3D is enabled in SteamVR's Manage Add-Ons. Restart SteamVR if needed. |
| VRto3D window on wrong monitor | Wrong display_index | Edit VRto3D's `default_config.json` and set `display_index` to the correct monitor number. |
| HUD at wrong depth | Per-mode sliders need adjustment | Open HUD Depth & Size and adjust the slider for the current mode (Normal, ADS, Scope, or Cutscene). |
| HUD depth not working on game HUD | Game uses canvas rendering | Try enabling **Canvas HUD Hook**. If the game crashes, leave it off — the overlay HUD works without it. |
| LookAround not tracking | Leia face tracking not active | Make sure you enabled LookAround after loading into the game. Click **Recalibrate** if the tracking seems offset. |
| Depth jitters or behaves erratically | Noisy FOV from engine | Enable **Smooth FOV Transitions** in Expert settings. If still bad, try switching FOV Tracking to Manual. |
| Game crashes on injection | UE5 compatibility issue, or Canvas HUD Hook | Disable **Canvas HUD Hook** if enabled. Try injecting at the main menu rather than in-game. |
| Status shows "Waiting..." (not Connected) | VRto3D bridge not established | Make sure SteamVR is running with VRto3D active. VRto3D needs to be receiving frames for the connection to establish. |
