# Monitor Mode Architecture Map

> UI → State → Pipeline field trace for UEVR's Monitor 3D stereo rendering system.
> Reference document — trace any UI control to its effect in the rendering pipeline.

**Last updated**: 2026-03-10 (v2.2, post dead-code cleanup)

---

## Data Flow Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           VR.cpp (UI Layer)                            │
│  Monitor 3D Tab: sliders, checkboxes, buttons, presets, status display │
└────────────────┬───────────────────────────────────┬────────────────────┘
                 │ writes                            │ reads (status)
                 ▼                                   │
┌────────────────────────────────────┐               │
│   MonitorState (atomic hub)        │               │
│   UE3D_MonitorState.hpp            │◄──────────────┘
│   Relaxed atomics, safe helpers    │
└──┬──────────┬──────────┬───────────┘
   │          │          │
   │ reads    │ reads    │ reads + writes
   ▼          ▼          ▼
┌────────┐ ┌────────┐ ┌─────────────┐    ┌─────────────────┐
│FFakeSte│ │Overlay │ │ GameFOV.cpp │───▶│ UE3D_Bridge.cpp │
│reoHook │ │Comp.cpp│ │ (zoom/depth)│    │ (shared memory) │
└────────┘ └────────┘ └─────────────┘    └────────┬────────┘
   │          │                                    │
   │ projection matrix [2][0]                      │ 256-byte protocol
   │ eye separation (view offset)                  ▼
   │ viewport split                          ┌───────────┐
   ▼                                         │  VRto3D   │
  UE4/5 Render Pipeline                      │  (SBS out)│
                                             └───────────┘
```

---

## Section 1: Display Setup

### IPD (mm)
| Stage | Location | Detail |
|-------|----------|--------|
| **UI** | VR.cpp:2875 | `SliderFloat("IPD (mm)", 55-75)` → `ms.fIPD_mm` |
| **Persist** | VR.cpp:2132 save / 1967 load | Key: `ue3d_ipd_mm` |
| **Compute** | VR.cpp:1486 | `stereo_depth = ipd_cm / screen_w_cm` → `ms.fStereoDepth` |
| **Consume** | FFakeStereoHook:4848 | `stereo_depth_safe()` → eye separation magnitude |
| **Consume** | FFakeStereoHook:5220 | `stereo_depth_safe()` → convergence shift `[2][0]` |
| **Consume** | OverlayComponent:235,934 | `stereo_depth_safe()` → overlay quad offset |

### Screen Height (inches)
| Stage | Location | Detail |
|-------|----------|--------|
| **UI** | VR.cpp:2880 | `SliderFloat("Screen Height", 5-40)` → `ms.fVerticalInches` |
| **Detect** | VR.cpp:2884-2900 | "Detect" button: Leia factory dims or EDID fallback |
| **Persist** | VR.cpp:2133 save / 1970 load | Key: `ue3d_vert_inches` |
| **Compute** | VR.cpp:1482-1486 | `screen_w_cm = vert_cm × 16/9` → feeds `stereo_depth` and `convergence` |

### Viewing Distance (cm)
| Stage | Location | Detail |
|-------|----------|--------|
| **UI** | VR.cpp:2913 | `SliderFloat("Viewing Distance", 30-150)` → `ms.fViewingDistance_cm` |
| **Persist** | VR.cpp:2134 save / 1973 load | Key: `ue3d_view_dist_cm` |
| **Compute** | VR.cpp:1487 | `convergence = view_dist / (screen_w × 0.5)` → `ms.fConvergence` |
| **Consume** | FFakeStereoHook:5328 | `viewing_distance_safe()` → Leia parallax calibration |

### Derived: Stereo Depth & Convergence
These are **not UI controls** — they're computed from IPD + screen size + viewing distance:

| Field | Formula | Written | Consumed |
|-------|---------|---------|----------|
| `fStereoDepth` | `ipd_cm / screen_w_cm` | VR.cpp:1490 | FFakeStereo (eye sep, convergence), Overlay (quad offset) |
| `fConvergence` | `view_dist / (screen_w × 0.5)` | VR.cpp:1493 | FFakeStereo (convergence denominator), Overlay (depth calc) |

---

## Section 2: 3D Calibration

### Enable Monitor Mode
| Stage | Location | Detail |
|-------|----------|--------|
| **UI** | VR.cpp:2804 | `Checkbox("Enable Monitor Mode")` → `ms.bMonitorMode` |
| **Persist** | VR.cpp:2131 save / 1966 load | Key: `ue3d_monitor_mode` |
| **Consume** | FFakeStereoHook (21 locations) | Master gate for all stereo operations |
| **Consume** | GameFOV.cpp, D3D11/12Component, OverlayComponent, OpenXR.cpp | Pipeline-wide gate |

### VRto3D Depth Buttons (bridge commands)
| Button | VR.cpp Line | Bridge Method | Protocol Field |
|--------|-------------|---------------|----------------|
| VRto3D++ | 2928 | `request_depth_big_increase()` | `auto_depth_request = 6` |
| VRto3D+ | 2931 | `request_depth_increase()` | `auto_depth_request = 4` |
| Calibrate | 2934 | `request_calibration()` | `auto_depth_request = 2` |
| VRto3D- | 2937 | `request_depth_decrease()` | `auto_depth_request = 3` |
| VRto3D-- | 2940 | `request_depth_big_decrease()` | `auto_depth_request = 5` |

Commands sent via shared memory → VRto3D reads in `hmd_device_driver.cpp`.

### 3D Strength
| Stage | Location | Detail |
|-------|----------|--------|
| **UI** | VR.cpp:2946 | `SliderFloat("3D Strength", 0-2)` → `ms.f3DStrength` |
| **Persist** | VR.cpp:2135 save / 1976 load | Key: `ue3d_3d_strength` |
| **Consume** | FFakeStereoHook:4848 | `strength_safe()` → multiplies eye separation |
| **Consume** | FFakeStereoHook:5220 | `strength_safe()` → multiplies convergence shift |
| **Consume** | OverlayComponent:237 | `strength_safe()` → overlay stereo scale |

### 3D Effect Bar (read-only)
| Stage | Location | Detail |
|-------|----------|--------|
| **UI** | VR.cpp:2953 | `ProgressBar` showing `st.current_depth_multiplier` |
| **Source** | GameFOV.cpp:316 | Written to `ms.fDynDepthMult` every frame |

---

## Section 3: Auto-Depth

### Preset Selector
| Stage | Location | Detail |
|-------|----------|--------|
| **UI** | VR.cpp:2971 | `Combo("Auto-Depth Preset")` → `cfg.active_preset` |
| **Persist** | VR.cpp:2108 save / 1924 load | Key: `gamefov_preset` |
| **Consume** | GameFOV.cpp:1543-1602 | `apply_preset()` — sets all curve params per preset |

Presets: Comfort, Balanced, Preserve Depth, Minimal, Custom.

### Min 3D Depth (Global Floor)
| Stage | Location | Detail |
|-------|----------|--------|
| **UI** | VR.cpp:2999 | `SliderFloat("Min 3D Depth", 0-100%)` → `ms.fGlobalDepthFloor` |
| **Persist** | VR.cpp:2136 save / 1979 load | Key: `ue3d_global_depth_floor` |
| **Consume** | GameFOV.cpp | `global_floor_safe()` → absolute floor for depth multiplier |

---

## Section 4: HUD Depth & Size

### Per-Mode HUD Controls

| Mode | Depth UI | Depth Field | Size UI | Size Field |
|------|----------|-------------|---------|------------|
| Normal | VR.cpp:3012 | `fHUDDepthScale` | VR.cpp:3023 | `fNormalHUDSize` |
| ADS | VR.cpp:3035 | `fADSHUDDepth` | VR.cpp:3042 | `fADSHUDSize` |
| Scope | VR.cpp:3050 | `fScopeHUDDepth` | VR.cpp:3057 | `fScopeHUDSize` |
| Cutscene | VR.cpp:3065 | `fCutsceneHUDDepth` | VR.cpp:3072 | `fCutsceneHUDSize` |

All depth fields: range [-2, +2], negative = popout.
All size fields: range [0.5, 2.0], multiplier on base size.

**Data flow**: UI slider → MonitorState atomic → GameFOV.cpp selects per-mode → EMA smooth → `fHUDDepthTarget` / `fHUDSizeTarget` → FFakeStereoHook `init_canvas()` + OverlayComponent.

| Computed Field | Writer | Formula |
|----------------|--------|---------|
| `fHUDDepthTarget` | GameFOV.cpp:354 | EMA: `cur += (mode_target - cur) × 0.08` |
| `fHUDSizeTarget` | GameFOV.cpp:374 | EMA: `cur += (mode_target - cur) × 0.08` |
| `fHUDDepthMult` | GameFOV.cpp:340 | Derived from depth mode for overlay scaling |

**Config persistence**: All 8 fields saved/loaded (keys: `ue3d_hud_depth_scale`, `ue3d_ads_hud_depth`, etc.)

### Auto HUD Size
| Stage | Location | Detail |
|-------|----------|--------|
| **UI** | VR.cpp:3079 | `Checkbox("Auto HUD Size")` → `ms.bHUDAutoSize` |
| **Persist** | VR.cpp:2148 save / 2032 load | Key: `ue3d_hud_auto_size` |
| **Consume** | GameFOV.cpp:374 | Overrides per-mode size: `size = 1.0 + |depth| × 0.15` |

### Canvas HUD Hook
| Stage | Location | Detail |
|-------|----------|--------|
| **UI** | VR.cpp:3105 | `Checkbox("Canvas HUD Hook")` → `ms.bCanvasHUDHook` |
| **Persist** | VR.cpp:2149 save / 2035 load | Key: `ue3d_canvas_hud_hook` |
| **Effect** | VR.cpp:3098,3108 | Calls `set_init_canvas_hook_enabled()` on FFakeStereoHook |
| **Consume** | FFakeStereoHook `init_canvas()` | Intercepts UE canvas draw for per-eye HUD depth |

---

## Section 5: Depth Tuning

### Flattening Curve Parameters

| Parameter | UI Line | Config Field | Consumed In | Purpose |
|-----------|---------|-------------|-------------|---------|
| Flattening Strength | 3131 | `depth_strength` | GameFOV:1421 | Overall power curve multiplier |
| Gentle or Aggressive | 3138 | `depth_base_power` | GameFOV:1413 | Base exponent of power curve |
| Strong Zoom Falloff | 3143 | `depth_extra_power` | GameFOV:1413 | Extra exponent at deep zoom |
| ADS Starts At | 3148 | `depth_dead_zone` | GameFOV:1333,1346,1402 | Zoom factor below which = Normal mode |

All persisted (keys: `gamefov_depth_strength`, `gamefov_depth_base_power`, etc.)
Changing any sets `active_preset = Custom`.

**Math** (GameFOV.cpp `calculate_target_depth()`):
```
zoom_norm = (zoom_factor - dead_zone) / (8.0 - dead_zone)
power = base_power + zoom_norm × extra_power
effective_power = power × depth_strength × mode_mult
target = pow(fov_scale, effective_power)
```

### Per-Mode Floors

| Floor | UI Line | Config Field | Consumed In |
|-------|---------|-------------|-------------|
| When Aiming | 3163 | `ads_min_depth` | GameFOV:1447,1457 |
| When Scoped | 3168 | `scope_min_depth` | GameFOV:1448,1458 |
| During Cutscenes | 3173 | `cutscene_min_depth` | GameFOV:1449,1459 |

### Per-Mode Flattening Multipliers

| Multiplier | UI Line | Config Field | Consumed In |
|------------|---------|-------------|-------------|
| Flatten: Aiming | 3186 | `ads_strength_mult` | GameFOV:1416 |
| Flatten: Scoped | 3192 | `scope_strength_mult` | GameFOV:1417 |
| Flatten: Cutscene | 3198 | `cutscene_strength_mult` | GameFOV:1418 |

Range [-2, 100]. Persisted (keys: `ue3d_ads_str_mult`, etc.)

---

## Section 6: Expert

### FOV Tracking
| Control | UI Line | Field | Consumed In |
|---------|---------|-------|-------------|
| FOV Tracking combo | 3222 | `cfg.fov_mode` | GameFOV:1006 (Auto vs Manual tolerance) |
| Base FOV display | 3231 | `cfg.base_fov` | GameFOV: zoom detection baseline |
| Calibrate FOV button | 3234 | calls `gfov.calibrate_base_fov()` | GameFOV: resets base_fov to current |

### Zoom Classification
| Control | UI Line | Field | Consumed In |
|---------|---------|-------|-------------|
| When to Start Flattening | 3264 | `cfg.zoom_threshold` | GameFOV:939-940 (hysteresis) |
| Scope Zoom Threshold | 3271 | `cfg.scope_zoom_threshold` | GameFOV:1325 (ADS vs Scope boundary) |

Zone map: `[1.0—dead_zone: Normal] [dead_zone—scope_threshold: ADS] [scope_threshold+: Scope]`

### Transition Speeds
| Control | UI Line | Field | Consumed In |
|---------|---------|-------|-------------|
| Attack Rate | 3282 | `cfg.depth_attack_rate` | GameFOV:1486 (zoom-in speed) |
| Release Rate | 3286 | `cfg.depth_release_rate` | GameFOV:1486 (zoom-out speed) |

### FOV Smoothing
| Control | UI Line | Field | Consumed In |
|---------|---------|-------|-------------|
| Smooth FOV Transitions | 3294 | `cfg.smooth_transitions` | GameFOV:243 (gate) |
| FOV Lerp Speed | 3299 | `cfg.lerp_speed` | GameFOV:975 (EMA alpha) |

### Depth Buffer (Experimental)
| Control | UI Line | Field | Consumed In |
|---------|---------|-------|-------------|
| Depth Auto-Scale | 3310 | `ms.bDepthAutoScale` | D3D11/12Component, GameFOV |
| Depth Sensitivity | 3323 | `cfg.depth_ws_response` | GameFOV:1430,1528 |

**Note**: `bDepthAutoScale` is **forced OFF on config load** (UE5 crash safety, lesson 95). User must re-enable each session.

---

## Section 7: LookAround (Leia Eye Tracking)

### Controls

| Control | UI Line | Field | Consumed In |
|---------|---------|-------|-------------|
| Enable LookAround | 3346 | `ms.bLeiaLookAroundEnabled` | FFakeStereo (parallax gate), UE3D_Bridge (data read gate) |
| Sensitivity | 3352 | `ms.fLeiaSensitivity` | FFakeStereo:5295 (parallax magnitude) |
| Smoothing | 3359 | `ms.fLeiaSmoothing` | UE3D_Bridge (EMA alpha for tracking data) |
| X axis enable | 3369 | `ms.bLeiaAxisX` | FFakeStereo, UE3D_Bridge (per-axis gate) |
| Y axis enable | 3373 | `ms.bLeiaAxisY` | FFakeStereo, UE3D_Bridge (per-axis gate) |
| Z axis enable | 3377 | `ms.bLeiaAxisZ` | FFakeStereo (Z parallax: FOV + stereo) |
| Invert X | 3386 | `ms.bLeiaInvertX` | FFakeStereo (sign flip) |
| Invert Y | 3392 | `ms.bLeiaInvertY` | FFakeStereo (sign flip) |
| Invert Z | 3398 | `ms.bLeiaInvertZ` | FFakeStereo (sign flip) |
| Motion Depth | 3405 | `ms.fLeiaMotionParallax` | FFakeStereo (near/far depth scaling) |
| Z Depth | 3415 | `ms.fLeiaZDepthStrength` | FFakeStereo (Z stereo + FOV modulation) |
| Invert Z Stereo | 3424 | `ms.bLeiaInvertZStereo` | FFakeStereo (separate Z stereo sign) |
| Recalibrate button | 3429 | calls `bridge.reset_leia_calibration()` | UE3D_Bridge (resets zero reference) |

All persisted (keys: `ue3d_leia_*`).

### Tracking Data (written by pipeline, displayed in UI)

| Field | Writer | Source |
|-------|--------|--------|
| `bLeiaTracking` | UE3D_Bridge.cpp:369 | Set true when valid tracking frame arrives |
| `uLeiaFrameCounter` | UE3D_Bridge.cpp:370 | Incremented per valid frame |
| `fLeiaHeadX/Y/Z` | UE3D_Bridge.cpp:362-364 | EMA-smoothed center-eye position |
| `fLeiaLeftEyeX/Y` | UE3D_Bridge.cpp:365-366 | Per-eye Kooima offset (left) |
| `fLeiaRightEyeX/Y` | UE3D_Bridge.cpp:367-368 | Per-eye Kooima offset (right) |
| `fLeiaDisplayWidthCm` | UE3D_Bridge.cpp:376 | Leia SR::Display factory calibration |
| `fLeiaDisplayHeightCm` | UE3D_Bridge.cpp:379 | Leia SR::Display factory calibration |

### LookAround Parallax Pipeline
```
LeiaSR Runtime → 3DGameBridge (predict())
  → shared memory (ue3d_protocol.h leia_* fields)
  → UE3D_Bridge::read_leia_eye_data()
    → EMA smoothing (per m_leia_smooth_* state)
    → dynamic calibration (zero reference)
    → MonitorState atomics (fLeiaHeadX/Y/Z, fLeiaLeftEyeX/Y, fLeiaRightEyeX/Y)
      → FFakeStereoHook::calculate_stereo_projection_matrix()
        → projection[2][0], [2][1] (per-eye parallax)
        → projection[3][0], [3][1] (center-eye motion parallax)
```

---

## Section 8: Status (Read-Only Diagnostics)

### Always-Visible Summary (line 2836-2866)
| Display | Source | Updated By |
|---------|--------|------------|
| Mode name | `gfov.get_depth_mode()` | GameFOV classification |
| Zoom factor | `st.zoom_factor` | GameFOV FOV tracking |
| Aim status | `ms.bIsAiming` | GameFOV two-source aim detection |
| Depth multiplier | `st.current_depth_multiplier` | GameFOV adaptive depth |
| Effective strength | `cfg.depth_strength × mode_mult` | Computed in UI display code |

### Status TreeNode (line 3456-3527)

| Section | Fields Displayed | Source |
|---------|-----------------|--------|
| Stereo | `stereo_depth_safe()`, `convergence_safe()` | Computed from IPD/screen/dist |
| Mode | `depth_mode`, `bIsAiming` | GameFOV classification |
| Dynamic | `dyn_depth_safe()`, `dyn_conv_safe()` | GameFOV per-frame modulation |
| FOV State | `game_fov`, `base_fov`, `current_fov`, `fov_scale`, `fov_valid`, `fov_mode`, `bFOVCalibrated` | GameFOV tracking |
| Zoom State | `is_zooming`, `zoom_factor`, `zoom_duration`, `player_pawn_valid` | GameFOV classification |
| Depth Engine | `depth_mode`, `depth_mode_duration`, `fov_velocity`, `target_depth_multiplier`, `current_depth_multiplier`, `active_preset` | GameFOV adaptive depth |
| VRto3D Bridge | `vrto3d_connected`, `vrto3d_profile_loaded` | UE3D_Bridge shared memory |

### Debug Diagnostics (visible when Debug checkbox ON)

| Display | Field | Written By |
|---------|-------|------------|
| Force Flat toggle | `ms.bForceFlat` | UI (zeros all stereo for isolation) |
| ViewOffset calls/frame | `ms.uViewOffsetCallsSnapshot` | FFakeStereoHook (latched per-frame) |
| Projection calls/frame | `ms.uProjectionCallsSnapshot` | FFakeStereoHook |
| Slate Hook calls/frame | `ms.uSlateHookCallsSnapshot` | FFakeStereoHook |
| Canvas Hook calls/frame | `ms.uCanvasHookCallsSnapshot` | FFakeStereoHook |
| Last Eye Offset | `ms.fLastEyeOffset` | FFakeStereoHook:4848 |
| Last Conv Shift | `ms.fLastConvergenceShift` | FFakeStereoHook:5220 |

---

## Section 9: Rendering Pipeline Consumption Summary

### FFakeStereoRenderingHook.cpp — Hook Functions

| Hook | Primary MonitorState Reads | Output |
|------|---------------------------|--------|
| `adjust_view_rect()` | `bMonitorMode` | SBS viewport split (left/right half) |
| `calculate_stereo_view_offset()` | `bMonitorMode`, `bForceFlat`, `stereo_depth_safe()`, `dyn_depth_safe()`, `strength_safe()`, `fCachedWorldScale` | Eye separation (3D right vector × magnitude) |
| `calculate_stereo_projection_matrix()` | `bMonitorMode`, `bForceFlat`, `stereo_depth_safe()`, `convergence_safe()`, `dyn_conv_safe()`, `strength_safe()`, all Leia fields | `[2][0]` convergence shift + Leia parallax |
| `init_canvas()` | `bMonitorMode`, `stereo_depth_safe()`, `convergence_safe()`, `hud_depth_target_safe()` | Canvas HUD per-eye depth offset |
| `is_stereo_enabled()` | `bMonitorMode` | Returns true (Invariant I1) |

### OverlayComponent.cpp — UEVR Overlay HUD

| Function | MonitorState Reads | Output |
|----------|-------------------|--------|
| Quad positioning | `bMonitorMode`, `stereo_depth_safe()`, `convergence_safe()`, `strength_safe()`, `hud_depth_mult_safe()` | Per-eye overlay quad X offset |
| Size adjustment | `hud_size_target_safe()` | Overlay quad scale |

### GameFOV.cpp — Depth/Zoom Engine

| Function | MonitorState Reads | MonitorState Writes |
|----------|-------------------|---------------------|
| `update()` | `bMonitorMode`, `bIsAiming`, all HUD depth/size fields, `bHUDAutoSize`, `fGlobalDepthFloor`, `bDepthAutoScale` | `fDynDepthMult`, `fDynConvMult`, `fHUDDepthTarget`, `fHUDSizeTarget`, `fHUDDepthMult`, `fDisplayBaseFOV` |
| `classify_depth_mode()` | (internal state) | `depth_mode` in GameFOV::State |
| `calculate_target_depth()` | (config fields) | `target_depth_multiplier` in State |

### UE3D_Bridge.cpp — Shared Memory

| Function | MonitorState Reads | MonitorState Writes |
|----------|-------------------|---------------------|
| `update_all()` | `stereo_depth_safe()` (for hint) | Protocol fields via shared memory |
| `read_leia_eye_data()` | `fLeiaSmoothing`, axis enables/inverts | `fLeiaHeadX/Y/Z`, per-eye offsets, `bLeiaTracking`, `uLeiaFrameCounter`, display dims |

---

## Section 10: Config Persistence Map

### GameFOV Config Keys (20 keys)

| Config Key | Field | Type |
|------------|-------|------|
| `gamefov_base_fov` | `cfg.base_fov` | float |
| `gamefov_fov_mode` | `cfg.fov_mode` | int (enum) |
| `gamefov_zoom_threshold` | `cfg.zoom_threshold` | float |
| `gamefov_invert_zoom` | `cfg.invert_zoom_detection` | bool |
| `gamefov_depth_strength` | `cfg.depth_strength` | float |
| `gamefov_preset` | `cfg.active_preset` | int (enum) |
| `gamefov_depth_base_power` | `cfg.depth_base_power` | float |
| `gamefov_depth_extra_power` | `cfg.depth_extra_power` | float |
| `gamefov_depth_dead_zone` | `cfg.depth_dead_zone` | float |
| `gamefov_ads_min_depth` | `cfg.ads_min_depth` | float |
| `gamefov_scope_min_depth` | `cfg.scope_min_depth` | float |
| `gamefov_cutscene_min_depth` | `cfg.cutscene_min_depth` | float |
| `gamefov_smooth` | `cfg.smooth_transitions` | bool |
| `gamefov_lerp_speed` | `cfg.lerp_speed` | float |
| `gamefov_fov_comp` | `cfg.vrto3d_fov_compensation` | bool |
| `gamefov_scope_zoom_threshold` | `cfg.scope_zoom_threshold` | float |
| `gamefov_depth_attack_rate` | `cfg.depth_attack_rate` | float |
| `gamefov_depth_release_rate` | `cfg.depth_release_rate` | float |
| `gamefov_depth_ws_response` | `cfg.depth_ws_response` | float |

### MonitorState Keys (28 keys)

| Config Key | Field | Notes |
|------------|-------|-------|
| `ue3d_monitor_mode` | `bMonitorMode` | |
| `ue3d_ipd_mm` | `fIPD_mm` | |
| `ue3d_vert_inches` | `fVerticalInches` | |
| `ue3d_view_dist_cm` | `fViewingDistance_cm` | |
| `ue3d_3d_strength` | `f3DStrength` | |
| `ue3d_global_depth_floor` | `fGlobalDepthFloor` | |
| `ue3d_depth_auto_scale` | `bDepthAutoScale` | **Forced OFF on load** (UE5 safety) |
| `ue3d_hud_depth_scale` | `fHUDDepthScale` | |
| `ue3d_ads_hud_depth` | `fADSHUDDepth` | |
| `ue3d_scope_hud_depth` | `fScopeHUDDepth` | |
| `ue3d_cutscene_hud_depth` | `fCutsceneHUDDepth` | |
| `ue3d_normal_hud_size` | `fNormalHUDSize` | |
| `ue3d_ads_hud_size` | `fADSHUDSize` | |
| `ue3d_scope_hud_size` | `fScopeHUDSize` | |
| `ue3d_cutscene_hud_size` | `fCutsceneHUDSize` | |
| `ue3d_hud_auto_size` | `bHUDAutoSize` | |
| `ue3d_canvas_hud_hook` | `bCanvasHUDHook` | |
| `ue3d_ads_str_mult` | `cfg.ads_strength_mult` | |
| `ue3d_scope_str_mult` | `cfg.scope_strength_mult` | |
| `ue3d_cut_str_mult` | `cfg.cutscene_strength_mult` | |
| `ue3d_leia_look_enabled` | `bLeiaLookAroundEnabled` | |
| `ue3d_leia_sensitivity` | `fLeiaSensitivity` | |
| `ue3d_leia_smoothing` | `fLeiaSmoothing` | |
| `ue3d_leia_axis_x/y/z` | `bLeiaAxisX/Y/Z` | |
| `ue3d_leia_inv_x/y/z` | `bLeiaInvertX/Y/Z` | |
| `ue3d_leia_inv_z_stereo` | `bLeiaInvertZStereo` | |
| `ue3d_leia_z_depth_strength` | `fLeiaZDepthStrength` | |
| `ue3d_leia_motion_parallax` | `fLeiaMotionParallax` | |

---

## File Reference

| File | Role in Monitor Mode |
|------|---------------------|
| `src/mods/VR.cpp` | UI layer: all controls, config save/load, stereo calibration |
| `src/mods/vr/ue3d/UE3D_MonitorState.hpp` | Atomic state hub: all fields + safe helpers |
| `src/mods/vr/FFakeStereoRenderingHook.cpp` | Rendering: eye separation, convergence, viewport, HUD hook |
| `src/mods/vr/GameFOV.cpp` | Brain: FOV tracking, zoom classification, depth engine, HUD selection |
| `src/mods/vr/GameFOV.hpp` | Config struct + State struct definitions |
| `src/mods/vr/OverlayComponent.cpp` | UEVR overlay: per-eye quad positioning and sizing |
| `src/mods/vr/UE3D_Bridge.cpp` | Shared memory: protocol writes, Leia eye data reads |
| `src/mods/vr/UE3D_Bridge.hpp` | Bridge API: update_all, depth commands, getters |
| `src/mods/vr/ue3d_protocol.h` | 256-byte packed struct (Invariant I7) |
| `src/mods/vr/runtimes/OpenXR.cpp` | OpenXR runtime: fov_scale zoom, projection caching |
| `src/mods/vr/D3D11Component.cpp` | DX11 frame submission + depth readback |
| `src/mods/vr/D3D12Component.cpp` | DX12 frame submission + depth readback |
