// Cross-thread atomic state for monitor mode stereo 3D
#pragma once

#include <atomic>
#include <cmath>

namespace ue3d {

namespace constants {
    constexpr float DEG_PER_RAD = 57.2957795f;
    constexpr float FOV_MIN = 5.0f;
    constexpr float FOV_MAX = 170.0f;
    constexpr float M00_MIN_THRESHOLD = 0.001f;
    constexpr float CONVERGENCE_SHIFT_CLAMP = 0.1f;       // max NDC convergence shift per eye
    constexpr float INCHES_TO_CM = 2.54f;
    constexpr float DEFAULT_ASPECT = 16.0f / 9.0f;
    constexpr float OVERLAY_FOV_MIN = 30.0f;
    constexpr float DEFAULT_STEREO_DEPTH = 0.105f;        // 63mm IPD / 60cm screen
    constexpr float DEFAULT_CONVERGENCE = 2.17f;           // 65cm viewing dist / 30cm half-screen
}

struct MonitorState {
    static MonitorState& get() {
        static MonitorState instance;
        return instance;
    }

    // all fields are relaxed atomics - one-frame staleness is fine at 60fps

    std::atomic<bool> bMonitorMode{false};

    // stereo calibration (ipd/screen ratio + convergence)
    std::atomic<float> fStereoDepth{constants::DEFAULT_STEREO_DEPTH};
    std::atomic<float> fConvergence{constants::DEFAULT_CONVERGENCE};

    // dynamic depth/convergence from GameFOV (zoom-aware modulation)
    std::atomic<float> fDynDepthMult{1.0f};
    std::atomic<float> fDynConvMult{1.0f};

    // game camera FOV tracking
    std::atomic<float> fGameFOV_FromMatrix{90.0f};
    std::atomic<bool> bFOVCalibrated{false};
    std::atomic<uint32_t> uGameFOV_WriterFrame{0};         // frame stamp: last frame GameFOV wrote fGameFOV_FromMatrix
    std::atomic<float> fDisplayBaseFOV{90.0f};             // unzoomed baseline for overlay sizing

    // HUD depth
    std::atomic<float> fHUDDepthMult{1.0f};                // scene-aware: 0 (menu) to 1 (gameplay)

    // physical setup
    std::atomic<float> fVerticalInches{13.24f};            // 27" 16:9 = 13.24" vertical
    std::atomic<float> fIPD_mm{63.0f};
    std::atomic<float> fViewingDistance_cm{65.0f};

    // aim input (two-source: mouse RMB + gamepad LT)
    std::atomic<bool> bIsAiming{false};
    std::atomic<bool> bMouseAiming{false};
    std::atomic<bool> bGamepadAiming{false};

    // user controls
    std::atomic<float> fGlobalDepthFloor{0.0f};
    std::atomic<float> f3DStrength{1.0f};

    // HUD depth — per-mode sliders [-2, +2], 0=flat
    std::atomic<float> fHUDDepthScale{0.0f};
    std::atomic<float> fADSHUDDepth{0.0f};
    std::atomic<float> fScopeHUDDepth{0.0f};
    std::atomic<float> fCutsceneHUDDepth{0.0f};
    std::atomic<float> fHUDDepthTarget{0.0f};              // EMA-smoothed target (written by GameFOV)

    // HUD size — per-mode multipliers on base UI Size [0.5, 2.0]
    std::atomic<float> fNormalHUDSize{1.0f};
    std::atomic<float> fADSHUDSize{1.0f};
    std::atomic<float> fScopeHUDSize{1.0f};
    std::atomic<float> fCutsceneHUDSize{1.0f};
    std::atomic<float> fHUDSizeTarget{1.0f};               // EMA-smoothed target (written by GameFOV)
    std::atomic<bool> bHUDAutoSize{false};

    // HUD hooks
    std::atomic<bool> bCanvasHUDHook{false};               // enable init_canvas hook (off by default, may crash some games)

    // depth-based auto world-scale (experimental, opt-in)
    std::atomic<bool> bDepthAutoScale{false};              // some games crash with depth enabled

    // cached world_scale for convergence symmetry (written by view_offset, read by projection)
    std::atomic<float> fCachedWorldScale{100.0f};

    // Leia LookAround
    std::atomic<bool> bLeiaLookAroundEnabled{false};    // master enable toggle
    std::atomic<bool> bLeiaTracking{false};              // true when face is tracked
    std::atomic<float> fLeiaHeadX{0.0f};                 // smoothed head offset X (cm, right)
    std::atomic<float> fLeiaHeadY{0.0f};                 // smoothed head offset Y (cm, up)
    std::atomic<float> fLeiaHeadZ{0.0f};                 // smoothed head offset Z (cm, forward)
    std::atomic<float> fLeiaSensitivity{1.0f};           // parallax sensitivity multiplier
    std::atomic<float> fLeiaSmoothing{0.15f};            // EMA alpha (lower = smoother)
    std::atomic<bool> bLeiaAxisX{true};                  // enable horizontal parallax
    std::atomic<bool> bLeiaAxisY{true};                  // enable vertical parallax
    std::atomic<bool> bLeiaAxisZ{false};                 // enable depth parallax (experimental)
    std::atomic<bool> bLeiaInvertX{false};               // flip X parallax direction
    std::atomic<bool> bLeiaInvertY{false};               // flip Y parallax direction
    std::atomic<bool> bLeiaInvertZ{false};               // flip Z depth direction
    std::atomic<float> fLeiaZDepthStrength{1.0f};        // Z: FOV modulation + stereo scaling (0=off, 1.0=normal, 3.0=max)
    std::atomic<float> fLeiaMotionParallax{0.5f};        // motion parallax: near shifts fast, far slow (0=off, 3=max)
    std::atomic<uint32_t> uLeiaFrameCounter{0};          // frames with tracking data
    std::atomic<float> fLeiaDisplayWidthCm{0.0f};        // physical display width from SR::Display
    std::atomic<float> fLeiaDisplayHeightCm{0.0f};       // physical display height from SR::Display

    // per-eye Kooima parallax (each eye gets its own off-axis projection)
    std::atomic<float> fLeiaLeftEyeX{0.0f};              // left eye smoothed offset X (cm)
    std::atomic<float> fLeiaLeftEyeY{0.0f};              // left eye smoothed offset Y (cm)
    std::atomic<float> fLeiaRightEyeX{0.0f};             // right eye smoothed offset X (cm)
    std::atomic<float> fLeiaRightEyeY{0.0f};             // right eye smoothed offset Y (cm)

    // safe Leia reads
    float leia_head_x_safe() const {
        float v = fLeiaHeadX.load(std::memory_order_relaxed);
        return std::isfinite(v) ? v : 0.0f;
    }
    float leia_head_y_safe() const {
        float v = fLeiaHeadY.load(std::memory_order_relaxed);
        return std::isfinite(v) ? v : 0.0f;
    }
    float leia_head_z_safe() const {
        float v = fLeiaHeadZ.load(std::memory_order_relaxed);
        return std::isfinite(v) ? v : 0.0f;
    }
    float leia_left_eye_x_safe() const {
        float v = fLeiaLeftEyeX.load(std::memory_order_relaxed);
        return std::isfinite(v) ? v : 0.0f;
    }
    float leia_left_eye_y_safe() const {
        float v = fLeiaLeftEyeY.load(std::memory_order_relaxed);
        return std::isfinite(v) ? v : 0.0f;
    }
    float leia_right_eye_x_safe() const {
        float v = fLeiaRightEyeX.load(std::memory_order_relaxed);
        return std::isfinite(v) ? v : 0.0f;
    }
    float leia_right_eye_y_safe() const {
        float v = fLeiaRightEyeY.load(std::memory_order_relaxed);
        return std::isfinite(v) ? v : 0.0f;
    }
    float leia_z_depth_strength_safe() const {
        float v = fLeiaZDepthStrength.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 1.0f;
        return (v < 0.0f) ? 0.0f : (v > 3.0f) ? 3.0f : v;
    }
    float leia_motion_parallax_safe() const {
        float v = fLeiaMotionParallax.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 0.5f;
        return (v < 0.0f) ? 0.0f : (v > 3.0f) ? 3.0f : v;
    }
    float leia_sensitivity_safe() const {
        float v = fLeiaSensitivity.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 1.0f;
        return (v < 0.1f) ? 0.1f : (v > 5.0f) ? 5.0f : v;
    }
    float leia_smoothing_safe() const {
        float v = fLeiaSmoothing.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 0.15f;
        return (v < 0.01f) ? 0.01f : (v > 1.0f) ? 1.0f : v;
    }
    float viewing_distance_safe() const {
        float v = fViewingDistance_cm.load(std::memory_order_relaxed);
        if (!std::isfinite(v) || v < 10.0f) return 65.0f;
        return (v > 300.0f) ? 300.0f : v;
    }

    // debug diagnostics
    std::atomic<bool> bForceFlat{false};
    std::atomic<uint32_t> uViewOffsetCalls{0};
    std::atomic<uint32_t> uProjectionCalls{0};
    std::atomic<uint32_t> uSlateHookCalls{0};
    std::atomic<uint32_t> uCanvasHookCalls{0};
    std::atomic<uint32_t> uDebugFrameCount{0};

    // snapshot values (latched once per frame for UI)
    std::atomic<uint32_t> uViewOffsetCallsSnapshot{0};
    std::atomic<uint32_t> uProjectionCallsSnapshot{0};
    std::atomic<uint32_t> uSlateHookCallsSnapshot{0};
    std::atomic<uint32_t> uCanvasHookCallsSnapshot{0};
    std::atomic<float> fLastEyeOffset{0.0f};
    std::atomic<float> fLastConvergenceShift{0.0f};

    // safe reads with NaN/infinity guards
    float stereo_depth_safe(float fallback = constants::DEFAULT_STEREO_DEPTH) const {
        float v = fStereoDepth.load(std::memory_order_relaxed);
        return (std::isfinite(v) && v >= 0.0f) ? v : fallback;
    }

    float convergence_safe(float fallback = constants::DEFAULT_CONVERGENCE) const {
        float v = fConvergence.load(std::memory_order_relaxed);
        return (std::isfinite(v) && v > constants::M00_MIN_THRESHOLD) ? v : fallback;
    }

    // clamped to [0, 2]
    float dyn_depth_safe() const {
        float v = fDynDepthMult.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 1.0f;
        return (v < 0.0f) ? 0.0f : (v > 2.0f) ? 2.0f : v;
    }

    float dyn_conv_safe() const {
        float v = fDynConvMult.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 1.0f;
        return (v < 0.0f) ? 0.0f : (v > 2.0f) ? 2.0f : v;
    }

    float strength_safe() const {
        float v = f3DStrength.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 1.0f;
        return (v < 0.0f) ? 0.0f : (v > 2.0f) ? 2.0f : v;
    }

    // clamped to [0.01, 10000]
    float world_scale_safe() const {
        float v = fCachedWorldScale.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 100.0f;
        return (v < 0.01f) ? 0.01f : (v > 10000.0f) ? 10000.0f : v;
    }

    // clamped to [-2, 2] — bidirectional: negative=popout, 0=flat, positive=into world
    float hud_depth_scale_safe() const {
        float v = fHUDDepthScale.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 0.0f;
        return (v < -2.0f) ? -2.0f : (v > 2.0f) ? 2.0f : v;
    }

    // clamped to [-2, 2] — per-mode HUD depth (ADS)
    float ads_hud_depth_safe() const {
        float v = fADSHUDDepth.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 0.0f;
        return (v < -2.0f) ? -2.0f : (v > 2.0f) ? 2.0f : v;
    }

    // clamped to [-2, 2] — per-mode HUD depth (Scope)
    float scope_hud_depth_safe() const {
        float v = fScopeHUDDepth.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 0.0f;
        return (v < -2.0f) ? -2.0f : (v > 2.0f) ? 2.0f : v;
    }

    // clamped to [-2, 2] — per-mode HUD depth (Cutscene)
    float cutscene_hud_depth_safe() const {
        float v = fCutsceneHUDDepth.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 0.0f;
        return (v < -2.0f) ? -2.0f : (v > 2.0f) ? 2.0f : v;
    }

    // clamped to [-2, 2] — EMA-smoothed per-mode target (computed by GameFOV)
    float hud_depth_target_safe() const {
        float v = fHUDDepthTarget.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 0.0f;
        return (v < -2.0f) ? -2.0f : (v > 2.0f) ? 2.0f : v;
    }

    // clamped to [0.5, 2.0] — per-mode HUD size multipliers
    float normal_hud_size_safe() const {
        float v = fNormalHUDSize.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 1.0f;
        return (v < 0.5f) ? 0.5f : (v > 2.0f) ? 2.0f : v;
    }

    float ads_hud_size_safe() const {
        float v = fADSHUDSize.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 1.0f;
        return (v < 0.5f) ? 0.5f : (v > 2.0f) ? 2.0f : v;
    }

    float scope_hud_size_safe() const {
        float v = fScopeHUDSize.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 1.0f;
        return (v < 0.5f) ? 0.5f : (v > 2.0f) ? 2.0f : v;
    }

    float cutscene_hud_size_safe() const {
        float v = fCutsceneHUDSize.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 1.0f;
        return (v < 0.5f) ? 0.5f : (v > 2.0f) ? 2.0f : v;
    }

    // clamped to [0.5, 2.0] — EMA-smoothed per-mode size (computed by GameFOV)
    float hud_size_target_safe() const {
        float v = fHUDSizeTarget.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 1.0f;
        return (v < 0.5f) ? 0.5f : (v > 2.0f) ? 2.0f : v;
    }

    // clamped to [0, 1] — scene-aware auto EMA from GameFOV
    float hud_depth_mult_safe() const {
        float v = fHUDDepthMult.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 1.0f;
        return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    }

    // clamped to [FOV_MIN, FOV_MAX]
    float display_base_fov_safe() const {
        float v = fDisplayBaseFOV.load(std::memory_order_relaxed);
        if (!std::isfinite(v) || v < constants::FOV_MIN) return 90.0f;
        return (v > constants::FOV_MAX) ? constants::FOV_MAX : v;
    }

    // clamped to [0, 1]
    float global_floor_safe() const {
        float v = fGlobalDepthFloor.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) return 0.0f;
        return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
    }

private:
    MonitorState() = default;
    ~MonitorState() = default;
    MonitorState(const MonitorState&) = delete;
    MonitorState& operator=(const MonitorState&) = delete;
};

} // namespace ue3d
