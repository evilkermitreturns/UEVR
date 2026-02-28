// GameFOV.hpp - Game FOV tracking, zoom detection, and adaptive depth for monitor mode stereo

#pragma once

#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <functional>
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_set>

namespace sdk { class UObject; }

namespace vrmod {

enum class DepthMode : uint8_t {
    None = 0, ADS = 1, Scope = 2, Cutscene = 3
};

enum class DepthPreset : uint8_t {
    Comfort = 0, Balanced = 1, PreserveDepth = 2, Minimal = 3, Custom = 4
};

enum class FovMode : uint8_t {
    Auto = 0,    // Base FOV tracks game FOV when not zooming (default, current behavior)
    Manual = 1   // Base FOV is fixed; user must calibrate manually
};

class GameFOV {
public:
    static GameFOV& get() {
        static GameFOV instance;
        return instance;
    }

    struct Config {
        // Core settings
        bool enabled = true;                  // Master enable
        float base_fov = 90.0f;               // "Normal" game FOV (calibrate!)
        float min_fov = 5.0f;                 // Minimum FOV to accept
        float max_fov = 170.0f;               // Maximum FOV to accept

        // FOV detection mode
        FovMode fov_mode = FovMode::Auto;     // Auto = current behavior, Manual = fixed base FOV

        // Zoom detection
        float zoom_threshold = 5.0f;          // FOV drop to trigger zoom
        bool invert_zoom_detection = false;   // For games that increase FOV on zoom

        // Smoothing
        bool smooth_transitions = true;       // Enable FOV smoothing
        float lerp_speed = 0.3f;              // 0-1, higher = faster response

        // Output scaling
        float fov_multiplier = 1.0f;          // Scale factor for final output

        // VRto3D integration
        bool vrto3d_bridge_enabled = true;    // Enable VRto3D communication
        bool vrto3d_auto_depth = true;        // Auto-adjust depth on zoom
        bool vrto3d_fov_compensation = true;  // Compensate for VRto3D FOV changes

        // Debug
        bool debug_logging = false;           // Verbose output to spdlog

        // Adaptive depth curve
        float depth_base_power = 0.8f;        // Power curve exponent base
        float depth_extra_power = 0.8f;       // Additional power scaling with zoom
        float depth_strength = 1.0f;          // Overall strength multiplier
        float depth_dead_zone = 1.15f;        // No adjustment below this zoom factor
        float ads_min_depth = 0.20f;          // Floor for ADS mode
        float scope_min_depth = 0.05f;        // Floor for Scope mode
        float cutscene_min_depth = 0.30f;     // Floor for Cutscene mode
        float velocity_cutscene_max = 15.0f;        // |vel| < this + long duration = Cutscene
        float cutscene_min_duration = 3.0f;
        float depth_attack_rate = 12.0f;         // Unified: entering any zoom
        float depth_release_rate = 10.0f;        // Unified: exiting any zoom
        float scope_zoom_threshold = 1.4f;       // zoom_factor >= this = Scope floor
        DepthPreset active_preset = DepthPreset::Balanced;
        float stereo_aim_correction = 0.0f;     // Lateral aim correction (zoom-scaled)
        float stereo_aim_base = 0.0f;           // Lateral aim correction (constant base)
        float depth_ws_response = 0.5f;         // Depth influence on flattening [0.1-1.0]
        float ads_strength_mult = 1.0f;         // ADS flattening multiplier [0.0-2.0]
        float scope_strength_mult = 1.0f;       // Scope flattening multiplier [0.0-2.0]
        float cutscene_strength_mult = 1.0f;    // Cutscene flattening multiplier [0.0-2.0]
        float aim_release_timeout = 2.0f;       // Tier 1: seconds after aim release to force reset [1.0-5.0]
    };

    Config& config() { return m_config; }
    const Config& config() const { return m_config; }

    struct State {
        // FOV state
        float game_fov = 90.0f;           // Current game camera FOV
        float current_fov = 90.0f;        // Smoothed FOV value
        float target_fov = 90.0f;         // Target for smoothing
        bool fov_valid = false;           // true if we successfully read FOV

        // Zoom state
        bool is_zooming = false;          // true if zoom detected
        float zoom_factor = 1.0f;         // Magnification (2.0 = 2x zoom)
        float zoom_duration = 0.0f;       // Seconds in current zoom
        std::chrono::steady_clock::time_point zoom_start_time{};

        // Scene context (for VRto3D depth hints)
        bool is_cutscene = false;
        bool is_menu = false;
        bool is_first_person = true;

        // VRto3D state (read from bridge)
        bool vrto3d_connected = false;
        float vrto3d_depth = 0.0f;
        float vrto3d_convergence = 0.0f;
        float vrto3d_fov_adjustment = 0.0f;
        bool vrto3d_auto_depth = false;
        bool vrto3d_profile_loaded = false;

        // Depth classification state
        float fov_velocity = 0.0f;
        float prev_game_fov = 90.0f;
        DepthMode depth_mode = DepthMode::None;
        DepthMode prev_depth_mode = DepthMode::None;
        float depth_mode_duration = 0.0f;
        float target_depth_multiplier = 1.0f;
        float current_depth_multiplier = 1.0f;
        bool player_pawn_valid = true;  // true if controller->get_acknowledged_pawn() != null
        float pending_mode_duration = 0.0f;   // Hysteresis timer for mode transitions
        float fov_velocity_avg = 0.0f;        // Smoothed FOV velocity for cutscene detection
        float prev_floor = 1.0f;              // Previous mode's depth floor for blending
        float floor_blend_timer = 0.0f;       // Floor transition blend timer
        bool auto_calibrated = false;         // Has auto-calibration fired this session?

        // Aim-release recovery (Tier 1) and cooldown (Tier 3)
        bool prev_is_aiming = false;              // Previous frame's aim state for edge detection
        float aim_release_timer = 0.0f;           // Seconds since aim released while in ADS/Scope
        bool aim_release_recovery_active = false;  // True when timer is counting down
        float recovery_cooldown = 0.0f;           // Seconds of zoom suppression after force-reset
        bool recovery_pending = false;            // True from aim-release edge until baseline return

        // Depth readback state (from SceneDepthZ center pixel)
        float center_depth = 0.0f;           // Raw reversed-Z (1=near, 0=far)
        float smoothed_depth = 0.5f;         // Temporally smoothed
        bool depth_readback_active = false;  // True when valid depth data available
        float depth_ws_modifier = 1.0f;      // Non-zoom world_scale modifier [0.9-1.1]

        // Timing
        std::chrono::steady_clock::time_point last_update{std::chrono::steady_clock::now()};
    };

    const State& state() const { return m_state; }

    void update();

    // Returns FOV scale for projection (0.5 = 2x zoom). Thread-safe.
    float get_fov_scale() const;

    bool is_zooming() const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_state.is_zooming;
    }

    float get_zoom_factor() const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_state.zoom_factor;
    }

    // Set base FOV to current game FOV (call at "normal" view)
    void calibrate_base_fov();

    void reset();
    void set_fov_override(float fov_degrees);
    void clear_fov_override();
    void set_scene_context(bool is_cutscene, bool is_menu, bool is_first_person);
    void refresh_vrto3d_status();

    float get_depth_multiplier() const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_state.current_depth_multiplier;
    }
    DepthMode get_depth_mode() const {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_state.depth_mode;
    }

    // World scale injection (called by VR.cpp before update)
    void set_world_scale(float scale) { m_world_scale = scale; }

    // Center-screen depth injection (called by VR.cpp from D3D11Component)
    void set_center_depth(float d) { m_raw_center_depth = d; }

    // Config save callback (called by VR.cpp to register persistence)
    void set_save_callback(std::function<void()> cb) { m_save_callback = std::move(cb); }
    void apply_preset(DepthPreset preset);
    static const char* depth_mode_name(DepthMode m) {
        switch (m) {
            case DepthMode::None: return "Normal";
            case DepthMode::ADS: return "ADS";
            case DepthMode::Scope: return "Scope";
            case DepthMode::Cutscene: return "Cutscene";
            default: return "?";
        }
    }
    static const char* preset_name(DepthPreset p) {
        switch (p) {
            case DepthPreset::Comfort: return "Comfort";
            case DepthPreset::Balanced: return "Balanced";
            case DepthPreset::PreserveDepth: return "Preserve Depth";
            case DepthPreset::Minimal: return "Minimal";
            case DepthPreset::Custom: return "Custom";
            default: return "?";
        }
    }
    static const char* fov_mode_name(FovMode m) {
        switch (m) {
            case FovMode::Auto: return "Auto";
            case FovMode::Manual: return "Manual";
            default: return "?";
        }
    }

private:
    GameFOV() = default;
    ~GameFOV() { shutdown(); }
    GameFOV(const GameFOV&) = delete;
    GameFOV& operator=(const GameFOV&) = delete;

    // Lifecycle
    void initialize();
    void shutdown();

    // Internal methods
    float read_game_camera_fov();
    float read_nested_struct_fov(sdk::UObject* camera_manager, const wchar_t* root_name);
    bool write_nested_struct_fov(sdk::UObject* camera_manager, const wchar_t* root_name, float fov);
    sdk::UObject* get_camera_manager();
    void update_modifier_baseline();
    void update_zoom_state();
    void apply_smoothing(float delta_time);
    void calculate_fov_scale();
    bool check_baseline_return();
    bool check_aim_release_recovery(float dt);
    void update_vrto3d_bridge();
    void debug_log(const char* fmt, ...);

    void classify_depth_mode(float dt);
    void calculate_target_depth(float dt);
    void smooth_depth_multiplier(float dt);
    void update_depth_state(float dt);

    // Member variables
    mutable std::shared_mutex m_mutex;
    Config m_config{};
    State m_state{};

    bool m_initialized = false;
    float m_cached_fov_scale = 1.0f;
    int m_frames_at_baseline = 0;          // For stuck FOV detection
    std::optional<float> m_fov_override;   // Optional manual FOV override

    // World scale (fed by VR.cpp)
    float m_world_scale = 1.0f;

    // Center-screen depth (fed by D3D11Component via VR.cpp)
    float m_raw_center_depth = 0.0f;

    // Baseline modifier tracking — grows during stable gameplay, used to detect orphans
    std::vector<std::wstring> m_baseline_modifier_names;

    // Pending modifiers waiting for stabilization before baseline promotion
    struct PendingModifier {
        std::wstring name;
        int frames_at_none = 0;  // Consecutive frames at DepthMode::None since first seen
    };
    std::vector<PendingModifier> m_pending_modifiers;
    static constexpr int BASELINE_STABILIZE_FRAMES = 30;  // ~0.5s at 60fps

    // FOV override after orphan neutralization — returns base_fov until game recovers or aim clears
    bool m_neutralize_override_active = false;

    // Minimum override duration — prevents false-positive auto-clear.
    // After canary neutralization (Alpha=0), GetFOVAngle may briefly read base_fov
    // (modifier stops applying), triggering premature clear. 2s minimum ensures stability.
    float m_override_active_duration = 0.0f;
    static constexpr float OVERRIDE_MIN_DURATION_SECS = 2.0f;

    // Names of modifiers we've neutralized — prevents re-promotion to baseline
    std::unordered_set<std::wstring> m_neutralized_modifier_names;

    // Names of modifiers the canary confirmed ALIVE — event-managed, never promote to baseline
    std::unordered_set<std::wstring> m_canary_alive_names;

    // ──── Canary Probe: Orphaned Modifier Detection ────
    // Directly tests whether the game is actively writing to a camera modifier's Alpha.
    // Set bDisabled=true (blocks UE's UpdateAlpha interp), write canary Alpha, wait 3 frames.
    // If Alpha restored → ALIVE (game actively managing). If canary persists → ORPHANED → neutralize.

    enum class ProbePhase : uint8_t {
        IDLE = 0,      // No non-baseline modifier present
        WAIT = 1,      // Counting frames before next probe
        WRITTEN = 2    // Canary written, waiting for readback
    };

    struct CanaryProbeState {
        ProbePhase phase = ProbePhase::IDLE;
        int frame_counter = 0;
        int attempt_number = 0;
        float canary_alpha = 0.0f;
        float original_alpha = 0.0f;
        bool original_disabled = false;
        sdk::UObject* target_modifier = nullptr;
        bool orphan_detected = false;  // Signal for update() Phase 3 to activate override

        static constexpr int WAIT_FRAMES = 120;       // ~2s between probes
        static constexpr int READBACK_FRAMES = 3;      // Frames to wait after writing
        static constexpr float CANARY_DELTA = 0.001f;  // Subtract from current Alpha
    };
    CanaryProbeState m_canary_probe{};

    // Post-menu recalibration: detects when gameplay FOV differs from initial calibration
    int m_recalibration_frames = 0;
    bool m_recalibrated_this_level = false;

    // Four-layer recalibration guard (Bug 8 fix)
    float m_transition_holdoff_timer = 0.0f;            // Layer 1: seconds since last level transition
    bool m_fov_has_varied_since_transition = false;      // Layer 2: game FOV produced 2+ distinct values
    float m_first_fov_after_transition = 0.0f;           // Layer 2: first stable FOV reading post-transition
    static constexpr float TRANSITION_HOLDOFF_SECS = 5.0f;  // Layer 1: wait 5s after transition
    static constexpr float FOV_VARIANCE_THRESHOLD = 1.0f;   // Layer 2: readings must differ by > 1°

    // Cached camera_manager pointer for level-transition detection
    sdk::UObject* m_last_camera_manager = nullptr;

    // Config save callback (fed by VR.cpp)
    std::function<void()> m_save_callback;

    // Delta-check: cache last values sent to bridge to avoid redundant writes
    struct CachedBridgeState {
        float game_fov = 0.0f;
        float base_fov = 0.0f;
        float fov_scale = 1.0f;
        float zoom_factor = 1.0f;
        float depth_multiplier = 1.0f;
        float convergence_mult = 1.0f;
        float world_scale = 1.0f;
        bool is_zooming = false;
        bool is_valid = false;
        uint8_t zoom_mode = 0;
        uint8_t scene = 0;
        bool dirty = true;   // Force first write
        int idle_frames = 0; // Counter for periodic timestamp refresh
    };
    CachedBridgeState m_cached_bridge{};
};

} // namespace vrmod
