// GameFOV.cpp - Dynamic Game FOV Passthrough for UEVR

#include <algorithm>
#include <cmath>
#include <cstdarg>

#include "GameFOV.hpp"
#include "VRto3DBridge.hpp"
#include "ue3d/UE3D_MonitorState.hpp"

#include <sdk/APlayerCameraManager.hpp>
#include <sdk/APlayerController.hpp>
#include <sdk/UClass.hpp>
#include <sdk/UEngine.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/FProperty.hpp>
#include <sdk/FStructProperty.hpp>

#include <spdlog/spdlog.h>

extern uint32_t g_frame_count;  // FFakeStereoRenderingHook.cpp

namespace vrmod {

void GameFOV::debug_log(const char* fmt, ...) {
    if (!m_config.debug_logging) return;

    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    spdlog::info("[GameFOV] {}", buffer);
}

void GameFOV::initialize() {
    if (m_initialized) {
        debug_log("initialize() called but already initialized");
        return;
    }

    spdlog::info("[GameFOV] Initializing Game FOV Passthrough");

    if (m_config.vrto3d_bridge_enabled) {
        auto& bridge = VRto3DBridge::get();
        bridge.config().enabled = true;
        bridge.config().debug_logging = m_config.debug_logging;

        if (bridge.init()) {
            spdlog::info("[GameFOV] VRto3D bridge initialized");
        } else {
            spdlog::warn("[GameFOV] VRto3D bridge init failed (VRto3D may not be running)");
        }
    }

    m_initialized = true;
    spdlog::info("[GameFOV] Initialization complete (enabled={})", m_config.enabled);
}

void GameFOV::shutdown() {
    if (!m_initialized) return;

    spdlog::info("[GameFOV] Shutting down...");

    if (m_config.vrto3d_bridge_enabled) {
        VRto3DBridge::get().shutdown();
    }

    m_initialized = false;
    spdlog::info("[GameFOV] Shutdown complete");
}

void GameFOV::update() {
    // Phase 1: Ensure initialized (needs exclusive lock for first-time init)
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        if (!m_initialized) {
            initialize();
        }
    }

    // Phase 2: Read game FOV WITHOUT lock (UESDK reflection is the expensive part)
    float game_fov_reading = 0.0f;
    bool using_override = false;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        using_override = m_fov_override.has_value();
        if (using_override) {
            game_fov_reading = m_fov_override.value();
        }
    }
    if (!using_override) {
        game_fov_reading = read_game_camera_fov();  // No lock needed
    }

    // Phase 3: State mutation under exclusive lock
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);

        // --- Neutralization FOV override ---
        // Clear on aim input FIRST (ADS cycles the camera, fixing stuck FOV naturally)
        if (m_neutralize_override_active && !using_override) {
            const bool aim_clear = ue3d::MonitorState::get().bIsAiming.load(std::memory_order_relaxed);
            if (aim_clear) {
                m_neutralize_override_active = false;
                m_override_active_duration = 0.0f;
                debug_log("FOV OVERRIDE: Cleared by aim input (ADS will cycle camera)");
            }
        }
        // Replace stuck reading with base_fov, or auto-clear if game recovered.
        // Uses signed fov_diff (not abs) to match zoom exit logic: game FOV wider
        // than base gives negative diff, which is always < threshold = "not zoomed".
        if (m_neutralize_override_active && !using_override) {
            float fov_diff = m_config.base_fov - game_fov_reading;
            if (game_fov_reading > 0.0f
                && fov_diff < m_config.zoom_threshold * 0.5f
                && m_override_active_duration >= OVERRIDE_MIN_DURATION_SECS) {
                m_neutralize_override_active = false;
                m_override_active_duration = 0.0f;
                debug_log("FOV OVERRIDE: Cleared — game FOV returned to base (%.1f)", game_fov_reading);
            } else if (game_fov_reading > 0.0f) {
                debug_log("FOV OVERRIDE: Active — game=%.1f, using base=%.1f",
                    game_fov_reading, m_config.base_fov);
                game_fov_reading = m_config.base_fov;
            }
        }

        auto now = std::chrono::steady_clock::now();
        float delta_time = std::chrono::duration<float>(now - m_state.last_update).count();
        m_state.last_update = now;
        delta_time = std::clamp(delta_time, 0.0001f, 0.1f);

        // Track override active duration (for minimum-duration false-positive guard)
        if (m_neutralize_override_active) {
            m_override_active_duration += delta_time;
        }

        // Layer 1+2: Advance transition holdoff timer and track FOV variance
        if (m_transition_holdoff_timer < TRANSITION_HOLDOFF_SECS + 1.0f) {
            m_transition_holdoff_timer += delta_time;
        }
        // Layer 2: Track whether game FOV has varied since transition
        if (!m_fov_has_varied_since_transition && game_fov_reading > 0.0f) {
            if (m_first_fov_after_transition <= 0.0f) {
                m_first_fov_after_transition = game_fov_reading;  // Capture first reading
            } else if (std::abs(game_fov_reading - m_first_fov_after_transition) > FOV_VARIANCE_THRESHOLD) {
                m_fov_has_varied_since_transition = true;
                debug_log("FOV variance detected: first=%.1f, now=%.1f (delta=%.1f)",
                    m_first_fov_after_transition, game_fov_reading,
                    std::abs(game_fov_reading - m_first_fov_after_transition));
            }
        }

        // Tier 3: Decrement recovery cooldown (suppresses zoom re-entry after force-reset)
        if (m_state.recovery_cooldown > 0.0f)
            m_state.recovery_cooldown -= delta_time;

        if (!m_config.enabled) {
            m_state.fov_valid = false;
            m_state.is_zooming = false;
            m_state.zoom_factor = 1.0f;
            m_cached_fov_scale = 1.0f;
            m_frames_at_baseline = 0;
            return;
        }

        if (using_override) {
            m_state.game_fov = game_fov_reading;
            m_state.fov_valid = true;
            debug_log("Using FOV override: %.1f", m_state.game_fov);
        } else if (game_fov_reading > 0.0f) {
            m_state.game_fov = game_fov_reading;
            m_state.fov_valid = true;
        } else {
            m_state.fov_valid = false;
            debug_log("Failed to read game FOV");
        }

        // Auto-calibrate base FOV on first valid reading if still at default.
        // New users often forget to press "Calibrate FOV", leading to bad zoom detection.
        if (m_state.fov_valid && !m_state.auto_calibrated
            && std::abs(m_config.base_fov - 90.0f) < 0.1f
            && m_state.game_fov > 0.0f && std::abs(m_state.game_fov - 90.0f) > 1.0f) {
            m_config.base_fov = m_state.game_fov;
            m_state.current_fov = m_state.game_fov;
            m_state.target_fov = m_state.game_fov;
            m_state.auto_calibrated = true;
            spdlog::info("[GameFOV] Auto-calibrated base FOV to {:.1f} (first launch)", m_config.base_fov);
        }
        if (m_state.fov_valid) m_state.auto_calibrated = true;

        // Post-menu recalibration: if gameplay FOV stabilizes to something different from
        // base_fov (e.g., 59.8 vs 54.4 from cinematic camera at menu), update base_fov.
        // Fires once per level, requires 60 frames (~1s) of stable non-zoom gameplay.
        // Four-layer guard prevents capturing uninitialized camera values (Bug 8).
        if (m_state.auto_calibrated && !m_recalibrated_this_level
            && m_state.fov_valid && m_state.game_fov > 0.0f
            && m_state.depth_mode == DepthMode::None
            && m_state.player_pawn_valid
            && !ue3d::MonitorState::get().bIsAiming.load(std::memory_order_relaxed)
            && std::abs(m_state.game_fov - m_config.base_fov) > 3.0f
            // Layer 1: Wait 5s after level transition (camera needs time to initialize)
            && m_transition_holdoff_timer >= TRANSITION_HOLDOFF_SECS
            // Layer 2: FOV must have varied (uninitialized camera returns constant DefaultFOV)
            && m_fov_has_varied_since_transition
            // Layer 4: Sanity range — reject extreme values
            && m_state.game_fov >= 40.0f && m_state.game_fov <= 130.0f) {

            // Layer 3: Reject values near APlayerCameraManager::DefaultFOV (90deg)
            // During load, GetFOVAngle() returns DefaultFOV before game camera runs.
            // Read the property dynamically — games can override it.
            bool rejected_by_default_fov = false;
            auto* camera_manager = get_camera_manager();
            if (camera_manager) {
                auto dfov_ptr = (float*)camera_manager->get_property_data(L"DefaultFOV");
                float default_fov = dfov_ptr ? *dfov_ptr : 90.0f;
                if (std::abs(m_state.game_fov - default_fov) < 1.0f) {
                    rejected_by_default_fov = true;
                    debug_log("Recalibration blocked: game_fov=%.1f matches DefaultFOV=%.1f",
                        m_state.game_fov, default_fov);
                }
            }

            if (!rejected_by_default_fov) {
                m_recalibration_frames++;
                if (m_recalibration_frames >= 60) {
                    float old_base = m_config.base_fov;
                    m_config.base_fov = m_state.game_fov;
                    m_state.current_fov = m_state.game_fov;
                    m_state.target_fov = m_state.game_fov;
                    m_recalibrated_this_level = true;
                    spdlog::info("[GameFOV] Recalibrated base FOV: {:.1f} -> {:.1f} (gameplay differs from initial)",
                        old_base, m_config.base_fov);
                }
            }
        } else if (m_state.auto_calibrated && !m_recalibrated_this_level) {
            m_recalibration_frames = 0;
        }

        // If can't read FOV, reset scale (bridge update is outside lock)
        if (!m_state.fov_valid) {
            m_cached_fov_scale = 1.0f;
        } else {
            // Track FOV velocity for scene classification
            if (delta_time > 0.0001f) {
                m_state.fov_velocity = (m_state.game_fov - m_state.prev_game_fov) / delta_time;
                // Smoothed velocity for cutscene detection -- dampens noise
                m_state.fov_velocity_avg += 0.15f * (m_state.fov_velocity - m_state.fov_velocity_avg);
            }
            m_state.prev_game_fov = m_state.game_fov;

            // Check for recovery (stuck FOV fix): baseline return > aim-release timeout
            if (!check_baseline_return() && !check_aim_release_recovery(delta_time)) {
                update_zoom_state();

                if (m_config.smooth_transitions) {
                    apply_smoothing(delta_time);
                } else {
                    m_state.current_fov = m_state.target_fov;
                }

                calculate_fov_scale();

                update_depth_state(delta_time);
                classify_depth_mode(delta_time);

                calculate_target_depth(delta_time);
            }

            // Always run depth smoothing (even after baseline return)
            // so the transition back to 1.0 is gradual, not a single-frame snap.
            smooth_depth_multiplier(delta_time);

            // Grow modifier baseline during stable normal gameplay.
            // New modifiers go to pending first; promoted to baseline after
            // BASELINE_STABILIZE_FRAMES consecutive frames at None.
            // This prevents event modifiers (conversation, cinematic) from being
            // captured in the 1-frame window before FOV drops.
            if (m_state.depth_mode == DepthMode::None
                && m_state.player_pawn_valid) {
                update_modifier_baseline();
            } else {
                // Mode went non-None — clear pending (they didn't stabilize)
                if (!m_pending_modifiers.empty()) {
                    debug_log("MODIFIER PENDING: Cleared %d pending (mode=%s)",
                        (int)m_pending_modifiers.size(), depth_mode_name(m_state.depth_mode));
                    m_pending_modifiers.clear();
                }
            }

            // Canary probe: act on orphan detection from Phase 2 (read_game_camera_fov)
            if (m_canary_probe.orphan_detected) {
                m_canary_probe.orphan_detected = false;
                m_neutralize_override_active = true;
                m_override_active_duration = 0.0f;

                // Reset zoom state (override will mask game_fov to base)
                m_state.is_zooming = false;
                m_state.current_fov = m_config.base_fov;
                m_state.target_fov = m_config.base_fov;
                m_state.zoom_factor = 1.0f;
                m_state.zoom_duration = 0.0f;
                m_cached_fov_scale = 1.0f;
                m_frames_at_baseline = 0;
                m_state.depth_mode_duration = 0.0f;
                m_state.floor_blend_timer = 0.0f;

                spdlog::info("[GameFOV] Canary orphan fix: activated FOV override");
            }
        }

        if (m_config.debug_logging) {
            static int counter = 0;
            if (++counter % 60 == 0) {
                spdlog::info("[GameFOV] fov={:.1f} scale={:.3f} zoom={:.1f}x mode={} dmult={:.3f} vrto3d={}",
                    m_state.game_fov, m_cached_fov_scale, m_state.zoom_factor,
                    depth_mode_name(m_state.depth_mode), m_state.current_depth_multiplier,
                    m_state.vrto3d_connected ? 1 : 0);
            }
        }
    }

    // Phase 4: Bridge update outside main lock (bridge has its own mutex)
    // Safe because only the game thread calls update() and writes m_state/m_config
    update_vrto3d_bridge();

    // Phase 5: Write to MonitorState atomics (monitor mode only)
    auto& ms = ue3d::MonitorState::get();
    if (ms.bMonitorMode.load(std::memory_order_relaxed)) {
        ms.fDynDepthMult.store(m_state.current_depth_multiplier, std::memory_order_relaxed);

        // Convergence tracks depth uniformly (fixed blend 0.70).
        // Per-mode differentiation handled upstream by per-mode strength
        // multipliers (which change depth_mult itself — convergence follows).
        const float conv_mult = std::pow(m_state.current_depth_multiplier, 0.70f);
        ms.fDynConvMult.store(conv_mult, std::memory_order_relaxed);

        // Game camera FOV (primary writer, every frame)
        if (m_state.fov_valid && m_state.game_fov > 5.0f && m_state.game_fov < 170.0f) {
            ms.fGameFOV_FromMatrix.store(m_state.game_fov, std::memory_order_relaxed);
            ms.uGameFOV_WriterFrame.store(g_frame_count, std::memory_order_relaxed);
            if (!ms.bFOVCalibrated.load(std::memory_order_relaxed)) {
                ms.bFOVCalibrated.store(true, std::memory_order_relaxed);
            }
        }

        // Display base FOV (stable, unzoomed)
        ms.fDisplayBaseFOV.store(m_config.base_fov, std::memory_order_relaxed);

        // Dynamic HUD depth mult: scene-aware EMA toward mode-based target
        // Controls overlay distance flattening. All modes now user-controlled → mult stays 1.0
        {
            float cur_hud = ms.fHUDDepthMult.load(std::memory_order_relaxed);
            cur_hud += (1.0f - cur_hud) * 0.08f; // Gentle EMA toward 1.0
            ms.fHUDDepthMult.store(cur_hud, std::memory_order_relaxed);
        }

        // Per-mode HUD depth target: EMA-smoothed transition between mode sliders
        // Each mode has its own user-controlled slider [-2, +2]
        {
            float target;
            switch (m_state.depth_mode) {
                case DepthMode::ADS:      target = ms.ads_hud_depth_safe(); break;
                case DepthMode::Scope:    target = ms.scope_hud_depth_safe(); break;
                case DepthMode::Cutscene: target = ms.cutscene_hud_depth_safe(); break;
                default:                  target = ms.hud_depth_scale_safe(); break;
            }
            float cur = ms.fHUDDepthTarget.load(std::memory_order_relaxed);
            cur += (target - cur) * 0.08f;
            ms.fHUDDepthTarget.store(cur, std::memory_order_relaxed);
        }

        // Per-mode HUD size target: EMA-smoothed transition between mode size multipliers
        // When auto-size ON: compensate size based on depth distance (experimental)
        {
            float size_target;
            if (ms.bHUDAutoSize.load(std::memory_order_relaxed)) {
                float depth = std::abs(ms.fHUDDepthTarget.load(std::memory_order_relaxed));
                size_target = 1.0f + depth * 0.15f; // ~15% per depth unit
            } else {
                switch (m_state.depth_mode) {
                    case DepthMode::ADS:      size_target = ms.ads_hud_size_safe(); break;
                    case DepthMode::Scope:    size_target = ms.scope_hud_size_safe(); break;
                    case DepthMode::Cutscene: size_target = ms.cutscene_hud_size_safe(); break;
                    default:                  size_target = ms.normal_hud_size_safe(); break;
                }
            }
            float cur_sz = ms.fHUDSizeTarget.load(std::memory_order_relaxed);
            cur_sz += (size_target - cur_sz) * 0.08f;
            ms.fHUDSizeTarget.store(cur_sz, std::memory_order_relaxed);
        }

        // Periodic diagnostic (every ~5 seconds at 60fps)
        static uint32_t diag_counter = 0;
        if (++diag_counter % 300 == 0) {
            spdlog::info("[UE3D] stereo={:.4f} conv={:.2f} dyn_d={:.3f} dyn_c={:.3f} fov={:.1f} hud={:.2f} str={:.2f}",
                ms.stereo_depth_safe(), ms.convergence_safe(),
                ms.dyn_depth_safe(), ms.dyn_conv_safe(),
                ms.fGameFOV_FromMatrix.load(std::memory_order_relaxed),
                ms.hud_depth_mult_safe(), ms.strength_safe());
        }
    }
}

float GameFOV::read_game_camera_fov() {
    // Uses UESDK reflection to call APlayerCameraManager::GetFOVAngle().
    // UESDK dumps and regenerates these classes from the game's reflection system at runtime.
    try {
        auto engine = sdk::UEngine::get();
        if (!engine) {
            debug_log("read_fov: UEngine::get() returned null");
            return 0.0f;
        }

        auto world = engine->get_world();
        if (!world) {
            debug_log("read_fov: engine->get_world() returned null");
            return 0.0f;
        }

        auto gameplay_statics = sdk::UGameplayStatics::get();
        if (!gameplay_statics) {
            debug_log("read_fov: UGameplayStatics::get() returned null");
            return 0.0f;
        }

        auto controller = gameplay_statics->get_player_controller(world, 0);
        if (!controller) {
            debug_log("read_fov: get_player_controller() returned null");
            return 0.0f;
        }

        // Cache pawn validity for cutscene detection -- during cutscenes,
        // many UE games switch to a cinematic camera and the pawn becomes null.
        sdk::UObject* cached_pawn = nullptr;
        {
            cached_pawn = (sdk::UObject*)controller->get_acknowledged_pawn();
            m_state.player_pawn_valid = (cached_pawn != nullptr);
        }

        auto camera_manager = controller->get_player_camera_manager();
        if (!camera_manager) {
            debug_log("read_fov: get_player_camera_manager() returned null");
            return 0.0f;
        }

        static auto camera_manager_class = sdk::APlayerCameraManager::static_class();
        if (!camera_manager_class) {
            debug_log("read_fov: APlayerCameraManager::static_class() returned null");
            return 0.0f;
        }

        static auto get_fov_func = camera_manager_class->find_function(L"GetFOVAngle");
        if (!get_fov_func) {
            debug_log("read_fov: find_function(GetFOVAngle) returned null");
            return 0.0f;
        }

        struct { float ReturnValue; } params{};
        camera_manager->process_event(get_fov_func, &params);

        // ──── Canary Probe state machine ────
        // Tests whether the game is actively writing to a non-baseline modifier's Alpha.
        // ORPHANED verdict → neutralize (Alpha=0, bDisabled, one-shot FOV stomp, signal override).
        {
            // Resolve ModifierList property (one-time, cached for lifetime)
            static sdk::FProperty* s_probe_mod_prop = nullptr;
            static bool s_probe_prop_resolved = false;
            if (!s_probe_prop_resolved) {
                s_probe_prop_resolved = true;
                auto cam_class = camera_manager->get_class();
                if (cam_class) {
                    s_probe_mod_prop = cam_class->find_property(L"ModifierList");
                }
            }

            // Find first non-baseline modifier with Alpha > 0.01
            sdk::UObject* candidate_mod = nullptr;
            std::wstring candidate_name;

            if (s_probe_mod_prop) {
                struct FakeArray { sdk::UObject** data; int32_t num; int32_t max; };
                auto* arr = s_probe_mod_prop->get_data<FakeArray>(camera_manager);
                if (arr && arr->num > 0) {
                    for (int32_t i = 0; i < arr->num && i < 16; ++i) {
                        auto* mod = arr->data[i];
                        if (!mod) continue;
                        auto mod_class = mod->get_class();
                        if (!mod_class) continue;
                        auto name = mod_class->get_fname().to_string();

                        // Skip baseline modifiers
                        bool in_baseline = false;
                        for (const auto& bn : m_baseline_modifier_names) {
                            if (bn == name) { in_baseline = true; break; }
                        }
                        if (in_baseline) continue;

                        auto alpha_ptr = (float*)mod->get_property_data(L"Alpha");
                        if (!alpha_ptr || *alpha_ptr <= 0.01f) continue;

                        candidate_mod = mod;
                        candidate_name = std::move(name);
                        break;
                    }
                }
            }

            auto& probe = m_canary_probe;

            switch (probe.phase) {
            case ProbePhase::IDLE:
                if (candidate_mod) {
                    probe.phase = ProbePhase::WAIT;
                    probe.frame_counter = CanaryProbeState::WAIT_FRAMES;
                    probe.attempt_number = 0;
                    probe.target_modifier = nullptr;
                    spdlog::info("[GameFOV] CANARY: Non-baseline modifier detected, starting {}f delay",
                        CanaryProbeState::WAIT_FRAMES);
                }
                break;

            case ProbePhase::WAIT:
                if (!candidate_mod) {
                    probe.phase = ProbePhase::IDLE;
                    probe.target_modifier = nullptr;
                    spdlog::info("[GameFOV] CANARY: Non-baseline modifier gone, returning to IDLE");
                    break;
                }

                probe.frame_counter--;
                if (probe.frame_counter <= 0) {
                    // Time to write canary
                    auto alpha_ptr = (float*)candidate_mod->get_property_data(L"Alpha");
                    if (!alpha_ptr) {
                        probe.frame_counter = CanaryProbeState::WAIT_FRAMES;
                        break;
                    }

                    probe.original_alpha = *alpha_ptr;
                    probe.original_disabled = candidate_mod->get_bool_property(L"bDisabled");
                    probe.canary_alpha = probe.original_alpha - CanaryProbeState::CANARY_DELTA;
                    probe.target_modifier = candidate_mod;
                    probe.attempt_number++;

                    // Write: bDisabled=true (blocks UpdateAlpha), then canary Alpha
                    candidate_mod->set_bool_property(L"bDisabled", true);
                    *alpha_ptr = probe.canary_alpha;

                    probe.phase = ProbePhase::WRITTEN;
                    probe.frame_counter = CanaryProbeState::READBACK_FRAMES;

                    spdlog::info("[GameFOV] CANARY PROBE #{}: wrote canary={:.4f} (orig={:.4f}) bDisabled=true (was {})",
                        probe.attempt_number, probe.canary_alpha, probe.original_alpha,
                        probe.original_disabled ? "true" : "false");
                }
                break;

            case ProbePhase::WRITTEN:
            {
                // Validate target still in modifier stack (pointer identity check)
                bool target_valid = false;
                if (s_probe_mod_prop) {
                    struct FakeArray { sdk::UObject** data; int32_t num; int32_t max; };
                    auto* arr = s_probe_mod_prop->get_data<FakeArray>(camera_manager);
                    if (arr) {
                        for (int32_t i = 0; i < arr->num && i < 16; ++i) {
                            if (arr->data[i] == probe.target_modifier) {
                                target_valid = true;
                                break;
                            }
                        }
                    }
                }

                if (!target_valid) {
                    spdlog::info("[GameFOV] CANARY PROBE #{}: target modifier REMOVED during probe!",
                        probe.attempt_number);
                    probe.phase = ProbePhase::IDLE;
                    probe.target_modifier = nullptr;
                    break;
                }

                probe.frame_counter--;
                if (probe.frame_counter <= 0) {
                    // Readback
                    auto alpha_ptr = (float*)probe.target_modifier->get_property_data(L"Alpha");
                    float readback_alpha = alpha_ptr ? *alpha_ptr : -1.0f;
                    bool readback_disabled = probe.target_modifier->get_bool_property(L"bDisabled");

                    bool alpha_restored = (alpha_ptr && std::abs(readback_alpha - probe.canary_alpha) > 0.0005f);

                    const char* verdict = alpha_restored
                        ? "ALIVE (game restored Alpha)"
                        : "ORPHANED (canary persisted)";

                    spdlog::info("[GameFOV] CANARY RESULT #{}: {} | a={:.4f} canary={:.4f} orig={:.4f} | bD: read={} orig={}",
                        probe.attempt_number, verdict,
                        readback_alpha, probe.canary_alpha, probe.original_alpha,
                        readback_disabled ? "T" : "F",
                        probe.original_disabled ? "T" : "F");

                    if (alpha_restored) {
                        // ALIVE: conversation still active — restore everything, probe again later
                        probe.target_modifier->set_bool_property(L"bDisabled", probe.original_disabled);
                        if (alpha_ptr) *alpha_ptr = probe.original_alpha;

                        // Mark this modifier class as event-managed — prevents baseline promotion
                        // (without this, the modifier lingers at DepthMode::None after conversation
                        // ends, accumulates 30 pending frames, and gets baselined — making the
                        // canary blind to all subsequent conversations with the same modifier class)
                        auto alive_class = probe.target_modifier->get_class();
                        if (alive_class) {
                            m_canary_alive_names.insert(alive_class->get_fname().to_string());
                        }

                        probe.phase = ProbePhase::WAIT;
                        probe.frame_counter = CanaryProbeState::WAIT_FRAMES;
                        probe.target_modifier = nullptr;
                    } else {
                        // ORPHANED: nobody writing — remove modifier from camera stack
                        bool removed = false;

                        // Primary: call UE's RemoveCameraModifier UFunction
                        {
                            static sdk::UFunction* s_remove_func = nullptr;
                            static bool s_remove_resolved = false;
                            if (!s_remove_resolved) {
                                s_remove_resolved = true;
                                auto cam_class = camera_manager->get_class();
                                if (cam_class) {
                                    s_remove_func = cam_class->find_function(L"RemoveCameraModifier");
                                    if (s_remove_func) {
                                        spdlog::info("[GameFOV] Found RemoveCameraModifier UFunction");
                                    } else {
                                        spdlog::warn("[GameFOV] RemoveCameraModifier UFunction not found — will use TArray surgery");
                                    }
                                }
                            }

                            if (s_remove_func) {
                                struct {
                                    sdk::UObject* ModifierToRemove;
                                    bool ReturnValue;
                                } params{};
                                params.ModifierToRemove = probe.target_modifier;
                                camera_manager->process_event(s_remove_func, &params);
                                removed = params.ReturnValue;
                                spdlog::info("[GameFOV] RemoveCameraModifier returned {}", removed);
                            }
                        }

                        // Fallback: direct TArray surgery if UFunction unavailable or failed
                        if (!removed && s_probe_mod_prop) {
                            struct FakeArray { sdk::UObject** data; int32_t num; int32_t max; };
                            auto* arr = s_probe_mod_prop->get_data<FakeArray>(camera_manager);
                            if (arr) {
                                for (int32_t i = arr->num - 1; i >= 0; --i) {
                                    if (arr->data[i] == probe.target_modifier) {
                                        for (int32_t j = i; j < arr->num - 1; ++j) {
                                            arr->data[j] = arr->data[j + 1];
                                        }
                                        arr->num--;
                                        removed = true;
                                        spdlog::info("[GameFOV] TArray surgery: removed modifier at index {}", i);
                                        break;
                                    }
                                }
                            }
                        }

                        // Verify removal by walking the array
                        if (removed && s_probe_mod_prop) {
                            struct FakeArray { sdk::UObject** data; int32_t num; int32_t max; };
                            auto* arr = s_probe_mod_prop->get_data<FakeArray>(camera_manager);
                            bool still_there = false;
                            if (arr) {
                                for (int32_t i = 0; i < arr->num && i < 16; ++i) {
                                    if (arr->data[i] == probe.target_modifier) {
                                        still_there = true;
                                        break;
                                    }
                                }
                            }
                            if (still_there) {
                                spdlog::warn("[GameFOV] CANARY FIX: Modifier still in stack after removal!");
                                removed = false;
                            }
                        }

                        // Track neutralized name (prevents re-promotion to baseline)
                        auto mod_class = probe.target_modifier->get_class();
                        if (mod_class) {
                            m_neutralized_modifier_names.insert(mod_class->get_fname().to_string());
                        }

                        if (removed) {
                            // Post-removal: stomp baked FOV values to base
                            write_nested_struct_fov(camera_manager, L"ViewTarget", m_config.base_fov);
                            write_nested_struct_fov(camera_manager, L"CameraCachePrivate", m_config.base_fov);
                            spdlog::info("[GameFOV] CANARY FIX: Removed orphan + stomped ViewTarget/CachePrivate to {:.1f}", m_config.base_fov);
                        } else {
                            // Removal failed — stomp immediately as fallback
                            write_nested_struct_fov(camera_manager, L"ViewTarget", m_config.base_fov);
                            write_nested_struct_fov(camera_manager, L"CameraCachePrivate", m_config.base_fov);
                            spdlog::warn("[GameFOV] CANARY FIX: Removal failed — Alpha=0 + bDisabled + stomp as fallback");
                            if (alpha_ptr) *alpha_ptr = 0.0f;
                            probe.target_modifier->set_bool_property(L"bDisabled", true);
                        }

                        // Signal Phase 3 to activate override + reset zoom state
                        probe.orphan_detected = true;
                        probe.phase = ProbePhase::IDLE;
                        probe.target_modifier = nullptr;
                    }
                }
            }
                break;
            }
        }

        if (params.ReturnValue > 1.0f && params.ReturnValue < 179.0f) {
            return params.ReturnValue;
        } else {
            debug_log("read_fov: Invalid FOV value %.1f", params.ReturnValue);
            return 0.0f;
        }

    } catch (const std::exception& e) {
        debug_log("read_fov: Exception: %s", e.what());
        return 0.0f;
    } catch (...) {
        debug_log("read_fov: Unknown exception");
        return 0.0f;
    }
}

float GameFOV::read_nested_struct_fov(sdk::UObject* camera_manager, const wchar_t* root_name) {
    // Reads: camera_manager->root_name.POV.FOV
    // e.g. CameraCache.POV.FOV or ViewTarget.POV.FOV
    // Returns 0.0f if any property in the chain doesn't exist.
    try {
        auto cam_class = camera_manager->get_class();
        if (!cam_class) return 0.0f;

        auto root_prop = cam_class->find_property(root_name);
        if (!root_prop) return 0.0f;

        // Root is an inline struct — pointer to its data within camera_manager
        void* root_data = (void*)((uintptr_t)camera_manager + root_prop->get_offset());

        auto root_struct = ((sdk::FStructProperty*)root_prop)->get_struct();
        if (!root_struct) return 0.0f;

        auto pov_prop = root_struct->find_property(L"POV");
        if (!pov_prop) return 0.0f;

        void* pov_data = (void*)((uintptr_t)root_data + pov_prop->get_offset());

        auto pov_struct = ((sdk::FStructProperty*)pov_prop)->get_struct();
        if (!pov_struct) return 0.0f;

        auto fov_prop = pov_struct->find_property(L"FOV");
        if (!fov_prop) return 0.0f;

        float fov = *fov_prop->get_data<float>(pov_data);
        return (fov > 1.0f && fov < 179.0f) ? fov : 0.0f;
    } catch (...) {
        return 0.0f;
    }
}

bool GameFOV::write_nested_struct_fov(sdk::UObject* camera_manager, const wchar_t* root_name, float fov) {
    // Writes: camera_manager->root_name.POV.FOV = fov
    // e.g. ViewTarget.POV.FOV or CameraCachePrivate.POV.FOV
    // Returns true if write succeeded.
    try {
        auto cam_class = camera_manager->get_class();
        if (!cam_class) return false;

        auto root_prop = cam_class->find_property(root_name);
        if (!root_prop) return false;

        void* root_data = (void*)((uintptr_t)camera_manager + root_prop->get_offset());

        auto root_struct = ((sdk::FStructProperty*)root_prop)->get_struct();
        if (!root_struct) return false;

        auto pov_prop = root_struct->find_property(L"POV");
        if (!pov_prop) return false;

        void* pov_data = (void*)((uintptr_t)root_data + pov_prop->get_offset());

        auto pov_struct = ((sdk::FStructProperty*)pov_prop)->get_struct();
        if (!pov_struct) return false;

        auto fov_prop = pov_struct->find_property(L"FOV");
        if (!fov_prop) return false;

        float* fov_ptr = fov_prop->get_data<float>(pov_data);
        if (!fov_ptr) return false;

        *fov_ptr = fov;
        return true;
    } catch (...) {
        return false;
    }
}

sdk::UObject* GameFOV::get_camera_manager() {
    auto gameplay_statics = sdk::UGameplayStatics::get();
    if (!gameplay_statics) return nullptr;

    auto engine = sdk::UEngine::get();
    if (!engine) return nullptr;
    auto world = engine->get_world();
    if (!world) return nullptr;

    auto controller = gameplay_statics->get_player_controller(world, 0);
    if (!controller) return nullptr;

    auto* cm = (sdk::UObject*)controller->get_player_camera_manager();

    // Level transition detection: if camera_manager pointer changed, clear baseline
    if (cm && cm != m_last_camera_manager) {
        if (m_last_camera_manager != nullptr) {
            debug_log("Camera manager changed (%p -> %p), clearing modifier baseline",
                (void*)m_last_camera_manager, (void*)cm);
            m_baseline_modifier_names.clear();
            m_pending_modifiers.clear();
            m_neutralize_override_active = false;
            m_override_active_duration = 0.0f;
            m_neutralized_modifier_names.clear();
            m_canary_alive_names.clear();
            m_recalibrated_this_level = false;
            m_recalibration_frames = 0;
            m_transition_holdoff_timer = 0.0f;
            m_fov_has_varied_since_transition = false;
            m_first_fov_after_transition = 0.0f;
            m_canary_probe = CanaryProbeState{};
        }
        m_last_camera_manager = cm;
    }

    return cm;
}

void GameFOV::update_modifier_baseline() {
    // Pending-promotion baseline: new modifier class names must persist for
    // BASELINE_STABILIZE_FRAMES consecutive frames at DepthMode::None before
    // being promoted to the baseline set. This prevents event modifiers
    // (e.g., conversation) from being captured in the 1-frame window before
    // the FOV drop triggers a mode change.
    auto* camera_manager = get_camera_manager();
    if (!camera_manager) return;

    auto cam_class = camera_manager->get_class();
    if (!cam_class) return;
    auto mod_prop = cam_class->find_property(L"ModifierList");
    if (!mod_prop) return;

    struct FakeArray { sdk::UObject** data; int32_t num; int32_t max; };
    auto* arr = mod_prop->get_data<FakeArray>(camera_manager);
    if (!arr || arr->num <= 0) return;

    // Build set of current modifier names for fast lookup.
    // Skip modifiers with Alpha <= 0 — they've been neutralized and shouldn't
    // pollute the baseline (prevents Bug 5: re-promotion of zeroed modifiers).
    std::vector<std::wstring> current_names;
    for (int32_t i = 0; i < arr->num && i < 16; ++i) {
        auto* mod = arr->data[i];
        if (!mod) continue;
        auto mod_class = mod->get_class();
        if (!mod_class) continue;

        // Skip modifiers we've neutralized or confirmed as event-managed by canary
        // (neutralized: Alpha-zeroed orphans. canary_alive: confirmed game-managed during
        // conversation — must not be promoted or they'll blind the canary to future conversations)
        auto mod_name = mod_class->get_fname().to_string();
        if (m_neutralized_modifier_names.count(mod_name)) continue;
        if (m_canary_alive_names.count(mod_name)) continue;

        current_names.push_back(std::move(mod_name));
    }

    // Add unknown modifiers to pending
    for (const auto& name : current_names) {
        bool in_baseline = false;
        for (const auto& bn : m_baseline_modifier_names) {
            if (bn == name) { in_baseline = true; break; }
        }
        if (in_baseline) continue;

        bool in_pending = false;
        for (auto& pm : m_pending_modifiers) {
            if (pm.name == name) { in_pending = true; break; }
        }
        if (!in_pending) {
            m_pending_modifiers.push_back({name, 0});
        }
    }

    // Increment counters for pending modifiers still in stack, remove those that disappeared
    int promoted = 0;
    for (auto it = m_pending_modifiers.begin(); it != m_pending_modifiers.end(); ) {
        // Verify modifier is still present in current stack
        bool still_present = false;
        for (const auto& cn : current_names) {
            if (cn == it->name) { still_present = true; break; }
        }
        if (!still_present) {
            it = m_pending_modifiers.erase(it);
            continue;
        }

        it->frames_at_none++;
        if (it->frames_at_none >= BASELINE_STABILIZE_FRAMES) {
            m_baseline_modifier_names.push_back(std::move(it->name));
            it = m_pending_modifiers.erase(it);
            promoted++;
        } else {
            ++it;
        }
    }

    if (promoted > 0) {
        debug_log("MODIFIER BASELINE: +%d promoted, total %d known-good modifiers",
            promoted, (int)m_baseline_modifier_names.size());
    }
}

void GameFOV::update_zoom_state() {
    // Tier 3: During recovery cooldown, suppress zoom re-entry.
    // Prevents stale SDK readings from immediately re-triggering zoom.
    if (m_state.recovery_cooldown > 0.0f) {
        m_state.is_zooming = false;
        m_state.zoom_factor = 1.0f;
        m_state.target_fov = m_config.base_fov;
        m_state.zoom_duration = 0.0f;
        return;
    }

    // Hysteresis: use different thresholds for entering vs exiting zoom.
    // Without it, FOV hovering near the threshold causes rapid flicker.
    //   Enter: FOV must drop 5deg below base (harder to enter)
    //   Exit:  FOV only needs to be within 2.5deg of base (easier to exit)

    float fov_diff = m_config.base_fov - m_state.game_fov;

    // Handle inverted detection (rare, but some games work backwards)
    if (m_config.invert_zoom_detection) {
        fov_diff = -fov_diff;
    }

    bool was_zooming = m_state.is_zooming;

    float enter_threshold = m_config.zoom_threshold;
    float exit_threshold = m_config.zoom_threshold * 0.5f;

    if (!m_state.is_zooming) {
        // Not zooming - check if we should start
        m_state.is_zooming = (fov_diff > enter_threshold);
    } else {
        // Zooming - check if we should stop
        m_state.is_zooming = (fov_diff > exit_threshold);
    }

    if (m_state.is_zooming) {
        m_state.zoom_factor = std::max(m_config.base_fov, ue3d::constants::FOV_MIN) / std::max(m_state.game_fov, m_config.min_fov);
        m_state.target_fov = std::clamp(m_state.game_fov, m_config.min_fov, m_config.max_fov);

        if (!was_zooming) {
            m_state.zoom_start_time = std::chrono::steady_clock::now();
            debug_log("Zoom STARTED: fov=%.1f factor=%.1fx", m_state.game_fov, m_state.zoom_factor);
        }
        m_state.zoom_duration = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - m_state.zoom_start_time).count();
    } else {
        m_state.zoom_factor = 1.0f;
        m_state.target_fov = m_config.base_fov;
        m_state.zoom_duration = 0.0f;

        if (was_zooming) {
            debug_log("Zoom ENDED");
        }
    }
}

void GameFOV::apply_smoothing(float delta_time) {
    // Frame-rate independent exponential smoothing:
    // t = 1 - (1 - lerp_speed)^(dt * 60) makes smoothing consistent regardless of fps.

    float t = 1.0f - std::pow(1.0f - m_config.lerp_speed, delta_time * 60.0f);
    t = std::clamp(t, 0.0f, 1.0f);

    m_state.current_fov = m_state.current_fov + (m_state.target_fov - m_state.current_fov) * t;

    // Snap to target when very close (prevents endless tiny adjustments)
    if (std::abs(m_state.current_fov - m_state.target_fov) < 0.5f) {
        m_state.current_fov = m_state.target_fov;
    }
}

void GameFOV::calculate_fov_scale() {
    // FOV scale = current_fov / base_fov. E.g. 45deg/90deg = 0.5 = 2x zoom.

    if (m_state.is_zooming && m_state.current_fov < m_config.base_fov) {
        float raw_scale = m_state.current_fov / std::max(m_config.base_fov, ue3d::constants::FOV_MIN);
        m_cached_fov_scale = std::clamp(raw_scale, 0.05f, 1.0f);
    } else {
        m_cached_fov_scale = 1.0f;
    }
}

bool GameFOV::check_baseline_return() {
    // Stuck FOV fix: conversations/terminals temporarily change game FOV.
    // When they end, our zoom state can get stuck. Track consecutive frames at
    // baseline and force-reset after threshold.
    //   Auto mode:   2.0deg tolerance, 10 frames (best for most games)
    //   Manual mode:  0.5deg tolerance, 30 frames (tighter, for user-fixed base FOV)

    float diff_from_base = std::abs(m_state.game_fov - m_config.base_fov);

    float tolerance = (m_config.fov_mode == FovMode::Manual) ? 0.5f : 2.0f;
    int frame_threshold = (m_config.fov_mode == FovMode::Manual) ? 30 : 10;

    if (diff_from_base < tolerance) {
        m_frames_at_baseline++;

        if (m_frames_at_baseline > frame_threshold) {
            // Force reset zoom state
            bool was_zooming = m_state.is_zooming;

            m_state.is_zooming = false;
            m_state.current_fov = m_config.base_fov;
            m_state.target_fov = m_config.base_fov;
            m_state.zoom_factor = 1.0f;
            m_state.zoom_duration = 0.0f;
            m_cached_fov_scale = 1.0f;

            // Set depth targets so smooth_depth_multiplier() can
            // gradually transition back to 1.0 (prevents snap-back).
            m_state.target_depth_multiplier = 1.0f;
            m_state.depth_mode = DepthMode::None;
            m_state.depth_mode_duration = 0.0f;
            m_state.floor_blend_timer = 0.0f;

            // Clear aim-release recovery state (baseline return resolved it)
            m_state.aim_release_recovery_active = false;
            m_state.aim_release_timer = 0.0f;
            m_state.recovery_pending = false;

            if (was_zooming && m_frames_at_baseline == frame_threshold + 1) {
                debug_log("STUCK FOV FIX: Baseline detected, forced reset (mode=%s)",
                    fov_mode_name(m_config.fov_mode));
            }

            return true;  // Signal we handled it
        }
    } else {
        m_frames_at_baseline = 0;
    }

    return false;  // Normal processing should continue
}

// Tier 1: Aim-release timeout recovery.
// When bIsAiming goes false while classified as ADS/Scope, start a timer.
// If FOV hasn't naturally recovered after aim_release_timeout seconds,
// force-reset classification — the SDK is returning stale zoomed values.
bool GameFOV::check_aim_release_recovery(float dt) {
    const bool is_aiming = ue3d::MonitorState::get().bIsAiming.load(std::memory_order_relaxed);
    const bool was_aiming = m_state.prev_is_aiming;
    m_state.prev_is_aiming = is_aiming;

    // Edge detect: aim released while in ADS or Scope
    if (was_aiming && !is_aiming
        && (m_state.depth_mode == DepthMode::ADS || m_state.depth_mode == DepthMode::Scope)) {
        m_state.aim_release_recovery_active = true;
        m_state.aim_release_timer = 0.0f;
        m_state.recovery_pending = true;  // Persist until baseline return
        debug_log("RECOVERY: Aim released in %s — starting %.1fs timer",
            depth_mode_name(m_state.depth_mode), m_config.aim_release_timeout);
    }

    // Cancel if player re-aims
    if (is_aiming && m_state.aim_release_recovery_active) {
        m_state.aim_release_recovery_active = false;
        m_state.aim_release_timer = 0.0f;
        debug_log("RECOVERY: Player re-aimed — timer cancelled");
    }

    // Re-trigger: zoom persists after cooldown with no aim (stuck SDK)
    if (!m_state.aim_release_recovery_active && m_state.recovery_pending
        && m_state.is_zooming && !is_aiming
        && m_state.recovery_cooldown <= 0.0f) {
        m_state.aim_release_recovery_active = true;
        m_state.aim_release_timer = 0.0f;
        debug_log("RECOVERY: Zoom persists without aim (pending) — restarting %.1fs timer",
            m_config.aim_release_timeout);
    }

    if (!m_state.aim_release_recovery_active) return false;

    m_state.aim_release_timer += dt;
    if (m_state.aim_release_timer < m_config.aim_release_timeout) return false;

    // Timer expired — force reset
    spdlog::info("[GameFOV] RECOVERY: Aim released but FOV stuck for {:.1f}s. Forcing reset.",
        m_state.aim_release_timer);

    m_state.is_zooming = false;
    m_state.current_fov = m_config.base_fov;
    m_state.target_fov = m_config.base_fov;
    m_state.zoom_factor = 1.0f;
    m_state.zoom_duration = 0.0f;
    m_cached_fov_scale = 1.0f;
    m_frames_at_baseline = 0;

    m_state.target_depth_multiplier = 1.0f;
    m_state.depth_mode = DepthMode::None;
    m_state.depth_mode_duration = 0.0f;
    m_state.floor_blend_timer = 0.0f;

    m_state.aim_release_recovery_active = false;
    m_state.aim_release_timer = 0.0f;
    m_state.recovery_cooldown = 1.0f;  // Tier 3: suppress zoom re-entry for 1s
    m_state.recovery_pending = false;  // Recovery completed — prevent re-trigger cycle

    return true;
}

void GameFOV::update_vrto3d_bridge() {
    if (!m_config.vrto3d_bridge_enabled) return;

    auto& bridge = VRto3DBridge::get();

    // Sync bridge config with our config (local struct writes, not shared memory)
    bridge.config().use_fov_compensation = m_config.vrto3d_fov_compensation;
    bridge.config().debug_logging = m_config.debug_logging;
    bridge.config().depth_base_power = m_config.depth_base_power;
    bridge.config().depth_extra_power = m_config.depth_extra_power;
    bridge.config().depth_strength = m_config.depth_strength;
    bridge.config().depth_dead_zone = m_config.depth_dead_zone;
    bridge.config().ads_min_depth = m_config.ads_min_depth;
    bridge.config().scope_min_depth = m_config.scope_min_depth;
    bridge.config().cutscene_min_depth = m_config.cutscene_min_depth;
    bridge.config().stereo_aim_correction = m_config.stereo_aim_correction;
    bridge.config().stereo_aim_base = m_config.stereo_aim_base;

    // Always send the smoothed multiplier (not just when zooming) so the
    // transition back to 1.0 is gradual rather than a single-frame snap.
    float depth_multiplier = 1.0f;
    if (m_config.vrto3d_auto_depth) {
        depth_multiplier = m_state.current_depth_multiplier;
    }

    SceneType scene = SceneType::Normal;
    if (m_state.is_cutscene) {
        scene = SceneType::Cutscene;
    } else if (m_state.is_menu) {
        scene = SceneType::Menu;
    }

    ZoomMode zoom_mode = ZoomMode::None;
    if (m_state.is_zooming) {
        switch (m_state.depth_mode) {
            case DepthMode::Scope: zoom_mode = ZoomMode::Scope; break;
            case DepthMode::ADS:   zoom_mode = ZoomMode::AimDownSights; break;
            default: zoom_mode = (m_state.zoom_factor >= 2.0f) ? ZoomMode::Scope : ZoomMode::AimDownSights; break;
        }
    }

    // Scene-based convergence multiplier for VRto3D:
    //   Cutscene: 1.15 = tighter convergence (more intimate)
    //   Menu:     0.7  = looser convergence (easier to read UI)
    float convergence_mult = 1.0f;
    if (m_state.is_cutscene) {
        convergence_mult = 1.15f;
    } else if (m_state.is_menu) {
        convergence_mult = 0.7f;
    }

    // Only write to shared memory if something changed
    constexpr float EPSILON = 0.001f;
    bool changed = m_cached_bridge.dirty
        || m_cached_bridge.is_zooming != m_state.is_zooming
        || m_cached_bridge.is_valid != m_state.fov_valid
        || m_cached_bridge.zoom_mode != static_cast<uint8_t>(zoom_mode)
        || m_cached_bridge.scene != static_cast<uint8_t>(scene)
        || std::abs(m_cached_bridge.game_fov - m_state.game_fov) > EPSILON
        || std::abs(m_cached_bridge.fov_scale - m_cached_fov_scale) > EPSILON
        || std::abs(m_cached_bridge.depth_multiplier - depth_multiplier) > EPSILON
        || std::abs(m_cached_bridge.convergence_mult - convergence_mult) > EPSILON
        || std::abs(m_cached_bridge.zoom_factor - m_state.zoom_factor) > EPSILON
        || std::abs(m_cached_bridge.base_fov - m_config.base_fov) > EPSILON
        || std::abs(m_cached_bridge.world_scale - m_world_scale * m_state.depth_ws_modifier) > EPSILON;

    if (changed) {
        bridge.update_all(
            m_state.game_fov,
            m_config.base_fov,
            m_cached_fov_scale,
            m_state.zoom_factor,
            m_state.is_zooming,
            m_state.fov_valid,
            zoom_mode,
            depth_multiplier,
            scene,
            m_world_scale * m_state.depth_ws_modifier,
            convergence_mult
        );

        m_cached_bridge.game_fov = m_state.game_fov;
        m_cached_bridge.base_fov = m_config.base_fov;
        m_cached_bridge.fov_scale = m_cached_fov_scale;
        m_cached_bridge.zoom_factor = m_state.zoom_factor;
        m_cached_bridge.is_zooming = m_state.is_zooming;
        m_cached_bridge.is_valid = m_state.fov_valid;
        m_cached_bridge.zoom_mode = static_cast<uint8_t>(zoom_mode);
        m_cached_bridge.scene = static_cast<uint8_t>(scene);
        m_cached_bridge.depth_multiplier = depth_multiplier;
        m_cached_bridge.convergence_mult = convergence_mult;
        m_cached_bridge.world_scale = m_world_scale * m_state.depth_ws_modifier;
        m_cached_bridge.dirty = false;
        m_cached_bridge.idle_frames = 0;
    } else {
        // Periodic timestamp refresh to prevent staleness timeout on receiver side
        if (++m_cached_bridge.idle_frames >= 30) {  // ~500ms at 60fps
            bridge.update_timing(0.0f);  // Just refreshes timestamp
            m_cached_bridge.idle_frames = 0;
        }
    }

    // Read VRto3D's state back (always, since VRto3D may update independently)
    m_state.vrto3d_connected = bridge.is_vrto3d_connected();
    m_state.vrto3d_depth = bridge.get_vrto3d_depth();
    m_state.vrto3d_convergence = bridge.get_vrto3d_convergence();
    m_state.vrto3d_fov_adjustment = bridge.get_vrto3d_fov_adjustment();
    m_state.vrto3d_auto_depth = bridge.is_vrto3d_listener_enabled();
    m_state.vrto3d_profile_loaded = bridge.is_vrto3d_profile_loaded();
}

float GameFOV::get_fov_scale() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_cached_fov_scale;
}

void GameFOV::calibrate_base_fov() {
    float current = read_game_camera_fov();

    std::unique_lock<std::shared_mutex> lock(m_mutex);

    if (current > 0.0f) {
        m_config.base_fov = current;

        // Reset state to prevent immediate zoom trigger
        m_state.current_fov = current;
        m_state.target_fov = current;
        m_state.is_zooming = false;
        m_state.zoom_factor = 1.0f;
        m_state.zoom_duration = 0.0f;
        m_cached_fov_scale = 1.0f;
        m_frames_at_baseline = 0;
        m_cached_bridge.dirty = true;  // Force next bridge write

        // Clear recovery state
        m_state.aim_release_recovery_active = false;
        m_state.aim_release_timer = 0.0f;
        m_state.recovery_cooldown = 0.0f;
        m_state.recovery_pending = false;

        // Clear modifier baseline — recalibration means new reference state
        m_baseline_modifier_names.clear();
        m_pending_modifiers.clear();
        m_neutralize_override_active = false;
        m_override_active_duration = 0.0f;
        m_neutralized_modifier_names.clear();
        m_canary_alive_names.clear();
        m_recalibrated_this_level = false;
        m_recalibration_frames = 0;
        m_transition_holdoff_timer = 0.0f;
        m_fov_has_varied_since_transition = false;
        m_first_fov_after_transition = 0.0f;
        m_last_camera_manager = nullptr;
        m_canary_probe = CanaryProbeState{};

        spdlog::info("[GameFOV] Calibrated base FOV to {:.1f} degrees", m_config.base_fov);

        // Auto-save to UEVR profile so calibration persists across restarts
        lock.unlock();
        if (m_save_callback) m_save_callback();
    } else {
        spdlog::warn("[GameFOV] Calibration failed - could not read game FOV");
    }
}

void GameFOV::reset() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    bool vrto3d_was_enabled = m_config.vrto3d_bridge_enabled;
    auto saved_preset = m_config.active_preset;

    m_config = Config{};
    m_state = State{};
    m_fov_override.reset();
    m_cached_fov_scale = 1.0f;
    m_frames_at_baseline = 0;
    m_raw_center_depth = 0.0f;
    m_cached_bridge.dirty = true;  // Force next bridge write

    // Clear modifier baseline — full reset means fresh start
    m_baseline_modifier_names.clear();
    m_pending_modifiers.clear();
    m_neutralize_override_active = false;
    m_override_active_duration = 0.0f;
    m_neutralized_modifier_names.clear();
    m_canary_alive_names.clear();
    m_recalibrated_this_level = false;
    m_recalibration_frames = 0;
    m_transition_holdoff_timer = 0.0f;
    m_fov_has_varied_since_transition = false;
    m_first_fov_after_transition = 0.0f;
    m_last_camera_manager = nullptr;
    m_canary_probe = CanaryProbeState{};

    // Restore persistent settings
    m_config.vrto3d_bridge_enabled = vrto3d_was_enabled;
    m_config.active_preset = saved_preset;

    spdlog::info("[GameFOV] Reset to defaults");
}

void GameFOV::set_scene_context(bool is_cutscene, bool is_menu, bool is_first_person) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_state.is_cutscene = is_cutscene;
    m_state.is_menu = is_menu;
    m_state.is_first_person = is_first_person;
}

void GameFOV::set_fov_override(float fov_degrees) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_fov_override = std::clamp(fov_degrees, m_config.min_fov, m_config.max_fov);
}

void GameFOV::clear_fov_override() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_fov_override.reset();
}

void GameFOV::refresh_vrto3d_status() {
    if (!m_config.vrto3d_bridge_enabled) return;

    auto& bridge = VRto3DBridge::get();

    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_state.vrto3d_connected = bridge.is_vrto3d_connected();
    m_state.vrto3d_depth = bridge.get_vrto3d_depth();
    m_state.vrto3d_convergence = bridge.get_vrto3d_convergence();
    m_state.vrto3d_fov_adjustment = bridge.get_vrto3d_fov_adjustment();
}

// Zoom-factor-based scene classification. Velocity-based classification
// oscillated during scope zoom because FOV velocity amplifies noise.
// zoom_factor (= base_fov / game_fov) is inherently stable.
void GameFOV::classify_depth_mode(float dt) {
    m_state.prev_depth_mode = m_state.depth_mode;

    if (!m_state.is_zooming) {
        m_state.depth_mode = DepthMode::None;
        m_state.depth_mode_duration = 0.0f;
        m_state.floor_blend_timer = 0.0f;
        return;
    }

    // P1: Aim input - the single most reliable ADS indicator
    // Combine mouse (RMB) and gamepad (LT) sources — either triggers aim.
    // Written to separate atomics to prevent spoofed gamepad from stomping mouse input.
    auto& ms_aim = ue3d::MonitorState::get();
    const bool is_aiming = ms_aim.bMouseAiming.load(std::memory_order_relaxed)
                        || ms_aim.bGamepadAiming.load(std::memory_order_relaxed);
    ms_aim.bIsAiming.store(is_aiming, std::memory_order_relaxed);

    // Classify by ZOOM FACTOR (stable) + AIM INPUT (disambiguates)

    // Heavy zoom: aim disambiguates Scope vs Cutscene
    if (m_state.zoom_factor >= m_config.scope_zoom_threshold) {
        if (is_aiming) {
            m_state.depth_mode = DepthMode::Scope;
        } else {
            m_state.depth_mode = DepthMode::Cutscene;
        }
    }
    // Moderate zoom: aim input disambiguates ADS vs cinematic
    else if (m_state.zoom_factor >= m_config.depth_dead_zone) {
        if (is_aiming) {
            m_state.depth_mode = DepthMode::ADS;
        } else {
            m_state.depth_mode = DepthMode::Cutscene;
        }
    }
    // Below dead zone: pawn-based cutscene detection (strong signal, no dead_zone guard)
    else if (!m_state.player_pawn_valid && m_state.zoom_duration > 0.5f) {
        m_state.depth_mode = DepthMode::Cutscene;
    }
    // Below dead zone with pawn valid: insignificant zoom, don't classify.
    // Prevents stuck Cutscene mode when FOV returns to near-baseline after a cutscene.
    else if (m_state.zoom_factor < m_config.depth_dead_zone) {
        m_state.depth_mode = DepthMode::None;
    }
    // Slow FOV drift cutscene detection (only fires above dead_zone)
    else if (m_state.depth_mode != DepthMode::ADS
             && m_state.depth_mode != DepthMode::Scope
             && std::abs(m_state.fov_velocity_avg) < m_config.velocity_cutscene_max
             && m_state.zoom_duration > m_config.cutscene_min_duration) {
        m_state.depth_mode = DepthMode::Cutscene;
    }
    // Default: aim disambiguates (only fires above dead_zone)
    else if (m_state.depth_mode == DepthMode::None) {
        if (is_aiming) {
            m_state.depth_mode = DepthMode::ADS;
        } else {
            m_state.depth_mode = DepthMode::Cutscene;
        }
    }
    // else: keep current mode (hysteresis)

    // Mode hysteresis: require 0.2s hold before locking a new mode.
    // Prevents jitter when zoom hovers at ADS/Scope boundary.
    constexpr float MODE_HYSTERESIS_TIME = 0.2f;
    if (m_state.depth_mode != m_state.prev_depth_mode) {
        m_state.pending_mode_duration += dt;
        if (m_state.pending_mode_duration < MODE_HYSTERESIS_TIME) {
            // Revert to previous mode - not held long enough
            m_state.depth_mode = m_state.prev_depth_mode;
        } else {
            // Held long enough - commit the mode change
            m_state.pending_mode_duration = 0.0f;
            m_state.depth_mode_duration = 0.0f;
            m_state.floor_blend_timer = 0.0f;
            debug_log("Mode: %s -> %s (zoom=%.1fx pawn=%d held=%.2fs)",
                depth_mode_name(m_state.prev_depth_mode),
                depth_mode_name(m_state.depth_mode),
                m_state.zoom_factor, m_state.player_pawn_valid ? 1 : 0,
                MODE_HYSTERESIS_TIME);
        }
    } else {
        m_state.pending_mode_duration = 0.0f;
        m_state.depth_mode_duration += dt;
    }
}

void GameFOV::calculate_target_depth(float dt) {
    if (!m_state.is_zooming || m_config.depth_strength <= 0.0f) {
        m_state.target_depth_multiplier = 1.0f;
        return;
    }

    float zoom = m_state.zoom_factor;

    // Soft edge around dead zone -- fade band prevents stutter when
    // hovering near the threshold.
    constexpr float DEAD_ZONE_FADE = 0.05f;  // Width of the fade band
    float dz = m_config.depth_dead_zone;
    if (zoom < dz - DEAD_ZONE_FADE) {
        m_state.target_depth_multiplier = 1.0f;
        return;
    }

    // Normalize: 0 at dead_zone, 1 at ~8x
    float zoom_norm = (zoom - dz) / (8.0f - dz);
    zoom_norm = std::clamp(zoom_norm, 0.0f, 1.0f);

    // Adaptive power curve with per-mode strength multiplier
    float power = m_config.depth_base_power + zoom_norm * m_config.depth_extra_power;
    float mode_mult = 1.0f;
    switch (m_state.depth_mode) {
        case DepthMode::ADS:      mode_mult = m_config.ads_strength_mult; break;
        case DepthMode::Scope:    mode_mult = m_config.scope_strength_mult; break;
        case DepthMode::Cutscene: mode_mult = m_config.cutscene_strength_mult; break;
        default: break;
    }
    power *= m_config.depth_strength * mode_mult;

    float fov_scale = std::clamp(m_cached_fov_scale, 0.01f, 1.0f);

    // Depth-aware power adjustment (when SceneDepthZ readback is active).
    // UE reversed-Z: 1.0 = near, 0.0 = far.
    // Near targets need MORE flattening (boost power), far targets need LESS.
    float depth_adjust = 1.0f;
    if (m_state.depth_readback_active) {
        float response = m_config.depth_ws_response;
        depth_adjust = 1.0f + (m_state.smoothed_depth - 0.5f) * response;
        depth_adjust = std::clamp(depth_adjust, 0.6f, 1.4f);
    }

    float mult = std::pow(fov_scale, power * depth_adjust);

    // Within the fade band, blend from 1.0 toward curve
    if (zoom < dz) {
        float fade_t = (zoom - (dz - DEAD_ZONE_FADE)) / DEAD_ZONE_FADE;
        fade_t = std::clamp(fade_t, 0.0f, 1.0f);
        mult = 1.0f + (mult - 1.0f) * fade_t;  // Blend: 1.0 at bottom, full curve at top
    }

    // Per-mode floor
    float target_floor = 0.0f;
    switch (m_state.depth_mode) {
        case DepthMode::ADS:      target_floor = m_config.ads_min_depth; break;
        case DepthMode::Scope:    target_floor = m_config.scope_min_depth; break;
        case DepthMode::Cutscene: target_floor = m_config.cutscene_min_depth; break;
        default: break;
    }

    // Floor blending: smooth transition between mode floors to prevent "pop"
    constexpr float FLOOR_BLEND_DURATION = 0.3f;
    if (m_state.depth_mode != m_state.prev_depth_mode && m_state.floor_blend_timer == 0.0f) {
        m_state.prev_floor = (m_state.prev_depth_mode == DepthMode::None) ? 1.0f :
            (m_state.prev_depth_mode == DepthMode::ADS) ? m_config.ads_min_depth :
            (m_state.prev_depth_mode == DepthMode::Scope) ? m_config.scope_min_depth :
            m_config.cutscene_min_depth;
    }
    if (m_state.floor_blend_timer < FLOOR_BLEND_DURATION) {
        m_state.floor_blend_timer += std::clamp(dt, 0.0f, FLOOR_BLEND_DURATION);
        float blend_t = std::clamp(m_state.floor_blend_timer / FLOOR_BLEND_DURATION, 0.0f, 1.0f);
        target_floor = m_state.prev_floor + (target_floor - m_state.prev_floor) * blend_t;
    } else {
        m_state.floor_blend_timer = FLOOR_BLEND_DURATION;  // Done blending
    }

    // Global floor - user escape valve when auto-classification is too aggressive
    const float global_floor = ue3d::MonitorState::get().global_floor_safe();
    if (global_floor > target_floor) {
        target_floor = global_floor;
    }

    m_state.target_depth_multiplier = (std::max)(mult, target_floor);
}

// Unified depth smoothing. Per-mode rates caused visible stepping when mode
// flipped. Unified attack/release eliminates rate-switching jitter.
void GameFOV::smooth_depth_multiplier(float dt) {
    float target = m_state.target_depth_multiplier;
    float current = m_state.current_depth_multiplier;

    bool attacking = (target < current);
    // Single rate pair - no mode-dependent jitter source
    float rate = attacking ? m_config.depth_attack_rate : m_config.depth_release_rate;

    // Scopes snap-zoom faster than ADS, so allow a snappier attack response.
    if (attacking && m_state.depth_mode == DepthMode::Scope) {
        rate *= 1.2f;
    }

    float alpha = 1.0f - std::exp(-rate * dt);
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    m_state.current_depth_multiplier = current + (target - current) * alpha;

    if (std::abs(m_state.current_depth_multiplier - target) < 0.001f) {
        m_state.current_depth_multiplier = target;
    }
}

void GameFOV::update_depth_state(float dt) {
    auto& ms = ue3d::MonitorState::get();
    bool enabled = ms.bDepthAutoScale.load(std::memory_order_relaxed);

    if (!enabled || m_raw_center_depth <= 0.0f || m_raw_center_depth > 1.0f) {
        m_state.depth_readback_active = false;
        // Smoothly return world_scale modifier to neutral
        if (std::abs(m_state.depth_ws_modifier - 1.0f) > 0.001f) {
            float alpha = 1.0f - std::exp(-3.0f * dt);
            m_state.depth_ws_modifier += alpha * (1.0f - m_state.depth_ws_modifier);
        } else {
            m_state.depth_ws_modifier = 1.0f;
        }
        return;
    }

    m_state.depth_readback_active = true;
    m_state.center_depth = m_raw_center_depth;

    // Temporal smoothing (5Hz EMA)
    float alpha = 1.0f - std::exp(-5.0f * dt);
    m_state.smoothed_depth += alpha * (m_state.center_depth - m_state.smoothed_depth);

    // Non-zoom world_scale modifier for auto-convergence (oneup03's ask).
    // Only active when NOT zooming - zoomed path uses depth in calculate_target_depth.
    if (!m_state.is_zooming) {
        float response = m_config.depth_ws_response * 0.2f;  // Scale down for ws path
        float target_mod = 1.0f + (0.5f - m_state.smoothed_depth) * response;
        target_mod = std::clamp(target_mod, 0.9f, 1.1f);

        float ws_alpha = 1.0f - std::exp(-3.0f * dt);
        m_state.depth_ws_modifier += ws_alpha * (target_mod - m_state.depth_ws_modifier);
    } else {
        // During zoom, world_scale stays neutral - depth_multiplier handles it
        float ws_alpha = 1.0f - std::exp(-8.0f * dt);
        m_state.depth_ws_modifier += ws_alpha * (1.0f - m_state.depth_ws_modifier);
    }
}

void GameFOV::apply_preset(DepthPreset preset) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_config.active_preset = preset;

    switch (preset) {
        case DepthPreset::Comfort:
            m_config.depth_strength = 1.2f; m_config.depth_base_power = 1.0f;
            m_config.depth_extra_power = 1.0f; m_config.depth_dead_zone = 1.10f;
            m_config.scope_min_depth = 0.05f; m_config.ads_min_depth = 0.10f;
            m_config.cutscene_min_depth = 0.20f;
            m_config.scope_zoom_threshold = 1.3f;  // Earlier Scope = more flattening
            m_config.ads_strength_mult = 1.0f; m_config.scope_strength_mult = 1.2f;
            m_config.cutscene_strength_mult = 0.8f;
            break;
        case DepthPreset::Balanced:
            m_config.depth_strength = 1.0f; m_config.depth_base_power = 0.8f;
            m_config.depth_extra_power = 0.8f; m_config.depth_dead_zone = 1.15f;
            m_config.scope_min_depth = 0.05f; m_config.ads_min_depth = 0.20f;
            m_config.cutscene_min_depth = 0.30f;
            m_config.scope_zoom_threshold = 1.4f;  // Default - good balance
            m_config.ads_strength_mult = 1.0f; m_config.scope_strength_mult = 1.0f;
            m_config.cutscene_strength_mult = 1.0f;
            break;
        case DepthPreset::PreserveDepth:
            m_config.depth_strength = 0.7f; m_config.depth_base_power = 0.6f;
            m_config.depth_extra_power = 0.5f; m_config.depth_dead_zone = 1.25f;
            m_config.scope_min_depth = 0.15f; m_config.ads_min_depth = 0.35f;
            m_config.cutscene_min_depth = 0.50f;
            m_config.scope_zoom_threshold = 1.6f;  // Later Scope = keeps depth longer
            m_config.ads_strength_mult = 0.8f; m_config.scope_strength_mult = 1.0f;
            m_config.cutscene_strength_mult = 0.6f;
            break;
        case DepthPreset::Minimal:
            // Least depth reduction - preserves the 3D feel with minimal flattening
            m_config.depth_strength = 0.5f;
            m_config.depth_base_power = 0.5f;
            m_config.depth_extra_power = 0.3f;
            m_config.depth_dead_zone = 1.25f;
            m_config.scope_min_depth = 0.15f;
            m_config.ads_min_depth = 0.40f;
            m_config.cutscene_min_depth = 0.50f;
            m_config.scope_zoom_threshold = 1.5f;
            m_config.ads_strength_mult = 0.8f; m_config.scope_strength_mult = 0.8f;
            m_config.cutscene_strength_mult = 0.6f;
            break;
        case DepthPreset::Custom:
            break;  // Don't change anything
    }
    spdlog::info("[GameFOV] Applied preset: {}", preset_name(preset));
}

} // namespace vrmod
