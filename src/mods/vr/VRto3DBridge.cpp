/*
 * VRto3DBridge.cpp - UEVR <-> VRto3D Communication Bridge Implementation
 *
 * VERSION: 3.3 (matches uevr_vrto3d_protocol.h)
 */

#include "VRto3DBridge.hpp"
#include "GameFOV.hpp"  // For DepthMode enum
#include "ue3d/UE3D_MonitorState.hpp"  // For stereo_depth_safe()

#include <cstdarg>
#include <cstring>
#include <algorithm>
#include <cmath>

// Protocol hardening: validate float before writing to shared memory
static inline float safe_float(float v, float fallback, float lo, float hi) {
    if (!std::isfinite(v)) return fallback;
    return std::clamp(v, lo, hi);
}

#include <spdlog/spdlog.h>

namespace vrmod {

// ============================================================================
// DEBUG HELPER
// ============================================================================

void VRto3DBridge::debug_log(const char* fmt, ...) const {
    if (!m_config.debug_logging) return;

    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    spdlog::info("[VRto3DBridge] {}", buffer);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

bool VRto3DBridge::init() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_data != nullptr) {
        debug_log("Already initialized");
        return true;
    }

    // Create or open shared memory
    m_mapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(UEVR_VRto3D_SharedData),
        VRTO3D_SHARED_MEM_NAME
    );

    if (m_mapping == nullptr) {
        spdlog::error("[VRto3DBridge] Failed to create shared memory: {}", GetLastError());
        return false;
    }

    bool already_exists = (GetLastError() == ERROR_ALREADY_EXISTS);

    m_data = static_cast<UEVR_VRto3D_SharedData*>(
        MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UEVR_VRto3D_SharedData))
    );

    if (m_data == nullptr) {
        spdlog::error("[VRto3DBridge] Failed to map shared memory: {}", GetLastError());
        CloseHandle(m_mapping);
        m_mapping = nullptr;
        return false;
    }

    // Initialize if we created it (VRto3D might have created it first)
    if (!already_exists) {
        std::memset(m_data, 0, sizeof(UEVR_VRto3D_SharedData));
    } else {
        // Validate struct_size from existing shared memory to detect version mismatch
        if (m_data->struct_size != 0 && m_data->struct_size != sizeof(UEVR_VRto3D_SharedData)) {
            spdlog::warn("[VRto3DBridge] Shared memory struct_size mismatch: expected {}, got {}",
                sizeof(UEVR_VRto3D_SharedData), m_data->struct_size);
        }
    }

    // Always set our header
    m_data->magic = VRTO3D_BRIDGE_MAGIC;
    m_data->version = VRTO3D_BRIDGE_VERSION;
    m_data->struct_size = sizeof(UEVR_VRto3D_SharedData);
    m_data->flags = FLAG_MULTIPLIER_MODE | FLAG_SCENE_AWARE | FLAG_FOV_COMP | FLAG_MODIFIERS | FLAG_AIM_CORRECTION;

    // Initialize to safe defaults
    m_data->depth_multiplier = 1.0f;
    m_data->convergence_multiplier = 1.0f;
    m_data->world_scale = 1.0f;
    m_data->stereo_aim_correction = 0.0f;
    m_data->stereo_aim_base = 0.0f;
    m_data->stereo_depth_hint = 0.0f;

    spdlog::info("[VRto3DBridge] v3.3 initialized shared memory '{}' ({})",
        VRTO3D_SHARED_MEM_NAME, already_exists ? "existing" : "new");

    return true;
}

void VRto3DBridge::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_data != nullptr) {
        // Signal we're disconnecting
        m_data->is_valid = 0;
        m_data->is_zooming = 0;
        m_data->auto_depth_request = 0;

        UnmapViewOfFile(m_data);
        m_data = nullptr;
    }

    if (m_mapping != nullptr) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }

    spdlog::info("[VRto3DBridge] Shutdown complete");
}

// ============================================================================
// CONNECTION STATUS
// ============================================================================

bool VRto3DBridge::is_vrto3d_connected() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return false;
    return m_data->vrto3d_connected != 0;
}

bool VRto3DBridge::is_vrto3d_listener_enabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return false;
    return m_data->vrto3d_listener_enabled != 0;
}

bool VRto3DBridge::is_vrto3d_applying_depth() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return false;
    return m_data->vrto3d_auto_depth_active != 0;
}

bool VRto3DBridge::is_vrto3d_profile_loaded() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return false;
    return m_data->vrto3d_profile_loaded != 0;
}

// ============================================================================
// v3.1: PROFILE MODIFIERS
// ============================================================================

bool VRto3DBridge::has_profile_modifiers() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return false;
    return m_data->has_modifiers != 0;
}

ProfileModifiers VRto3DBridge::get_modifiers() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    ProfileModifiers mods{};

    if (!m_data || m_data->has_modifiers == 0) {
        return mods;  // All zeroes = no overrides
    }

    mods.active = true;
    mods.depth_strength = m_data->mod_depth_strength;
    mods.depth_min_floor = m_data->mod_depth_min_floor;
    mods.ads_floor = m_data->mod_ads_floor;
    mods.scope_floor = m_data->mod_scope_floor;
    mods.cutscene_floor = m_data->mod_cutscene_floor;
    mods.base_power = m_data->mod_base_power;
    mods.extra_power = m_data->mod_extra_power;
    mods.dead_zone = m_data->mod_dead_zone;
    mods.transition_speed = m_data->mod_transition_speed;
    mods.zoom_threshold = m_data->mod_zoom_threshold;
    mods.base_fov_override = m_data->mod_base_fov_override;

    return mods;
}

// ============================================================================
// WRITE METHODS
// ============================================================================

void VRto3DBridge::update_fov_state(
    float game_fov, float base_fov, float fov_scale, float zoom_factor,
    bool is_zooming, bool is_valid, ZoomMode zoom_mode
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return;

    m_data->game_fov = safe_float(game_fov, 90.0f, 1.0f, 179.0f);
    m_data->base_fov = safe_float(base_fov, 90.0f, 1.0f, 179.0f);
    m_data->fov_scale = safe_float(fov_scale, 1.0f, 0.01f, 2.0f);
    m_data->zoom_factor = safe_float(zoom_factor, 1.0f, 0.1f, 100.0f);
    m_data->is_zooming = is_zooming ? 1 : 0;
    m_data->is_valid = is_valid ? 1 : 0;
    m_data->zoom_mode = static_cast<uint8_t>(zoom_mode);

    update_timestamp();
}

void VRto3DBridge::update_depth_multiplier(float multiplier, bool is_auto_depth) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return;

    m_data->depth_multiplier = std::clamp(multiplier, m_config.min_depth_multiplier, 1.0f);
    m_data->auto_depth_request = is_auto_depth ? 1 : 0;

    debug_log("Depth multiplier: %.3f (auto=%d)", m_data->depth_multiplier, is_auto_depth);
}

void VRto3DBridge::update_scene(SceneType scene, float world_scale) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return;

    m_data->scene_type = static_cast<uint8_t>(scene);
    m_data->world_scale = world_scale;
}

void VRto3DBridge::set_profile_info(const std::string& profile_name, const std::string& exe_name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return;

    std::strncpy(m_data->uevr_profile_name, profile_name.c_str(),
        sizeof(m_data->uevr_profile_name) - 1);
    m_data->uevr_profile_name[sizeof(m_data->uevr_profile_name) - 1] = '\0';

    std::strncpy(m_data->game_exe_name, exe_name.c_str(),
        sizeof(m_data->game_exe_name) - 1);
    m_data->game_exe_name[sizeof(m_data->game_exe_name) - 1] = '\0';
}

void VRto3DBridge::update_timing(float frametime) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return;

    m_data->uevr_frametime = frametime;
    update_timestamp();
}

void VRto3DBridge::update_all(
    float game_fov, float base_fov, float fov_scale, float zoom_factor,
    bool is_zooming, bool is_valid, ZoomMode zoom_mode,
    float depth_multiplier, SceneType scene,
    float world_scale,
    float convergence_mult
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return;

    // FOV state (validated — prevent NaN/infinity from reaching VRto3D)
    m_data->game_fov = safe_float(game_fov, 90.0f, 1.0f, 179.0f);
    m_data->base_fov = safe_float(base_fov, 90.0f, 1.0f, 179.0f);
    m_data->fov_scale = safe_float(fov_scale, 1.0f, 0.01f, 2.0f);
    m_data->zoom_factor = safe_float(zoom_factor, 1.0f, 0.1f, 100.0f);
    m_data->is_zooming = is_zooming ? 1 : 0;
    m_data->is_valid = is_valid ? 1 : 0;
    m_data->zoom_mode = static_cast<uint8_t>(zoom_mode);

    // Depth control (validated)
    m_data->depth_multiplier = safe_float(depth_multiplier, 1.0f, m_config.min_depth_multiplier, 1.0f);
    m_data->convergence_multiplier = safe_float(convergence_mult, 1.0f, 0.1f, 5.0f);
    m_data->auto_depth_request = (depth_multiplier < 0.99f) ? 1 : 0;
    m_data->scene_type = static_cast<uint8_t>(scene);
    m_data->world_scale = safe_float(world_scale, 1.0f, 0.01f, 10000.0f);

    // v3.2: Aim correction (from config, not a per-frame dynamic value)
    m_data->stereo_aim_correction = m_config.stereo_aim_correction;
    m_data->stereo_aim_base = m_config.stereo_aim_base;

    // Monitor mode flag
    m_data->monitor_mode = m_monitor_mode ? 1 : 0;

    // v3.3: Stereo depth hint — tell VRto3D our stereo_depth so overlay IPD matches game
    if (m_monitor_mode) {
        m_data->stereo_depth_hint = safe_float(
            ue3d::MonitorState::get().stereo_depth_safe(), 0.0f, 0.0f, 2.0f);
    }

    // Timing
    update_timestamp();

    debug_log("update_all: fov=%.1f scale=%.3f zoom=%d depth_mult=%.3f conv_mult=%.3f mode=%d ws=%.3f",
        game_fov, fov_scale, is_zooming ? 1 : 0, depth_multiplier, convergence_mult,
        static_cast<int>(zoom_mode), world_scale);
}

void VRto3DBridge::update_timestamp() {
    // Called with lock held
    // MUST use GetTickCount64() - the protocol spec requires it, and VRto3D's
    // receiver checks freshness using GetTickCount64(). Using steady_clock here
    // would produce values from a different time source, breaking staleness checks.
    m_data->uevr_timestamp = GetTickCount64();
    m_data->uevr_frame_count = ++m_frame_count;
}

// ============================================================================
// VRTO3D COMMANDS (via auto_depth_request field)
// ============================================================================

void VRto3DBridge::request_calibration() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_data) {
        m_data->auto_depth_request = 2;
        m_data->command_seq = ++m_command_seq;
        debug_log("Calibration requested (seq=%d)", m_command_seq);
    }
}

void VRto3DBridge::request_depth_decrease() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_data) {
        m_data->auto_depth_request = 3;
        m_data->command_seq = ++m_command_seq;
        debug_log("Depth decrease requested (seq=%d)", m_command_seq);
    }
}

void VRto3DBridge::request_depth_increase() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_data) {
        m_data->auto_depth_request = 4;
        m_data->command_seq = ++m_command_seq;
        debug_log("Depth increase requested (seq=%d)", m_command_seq);
    }
}

void VRto3DBridge::request_depth_big_decrease() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_data) {
        m_data->auto_depth_request = 5;
        m_data->command_seq = ++m_command_seq;
        debug_log("Big depth decrease requested (seq=%d)", m_command_seq);
    }
}

void VRto3DBridge::request_depth_big_increase() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_data) {
        m_data->auto_depth_request = 6;
        m_data->command_seq = ++m_command_seq;
        debug_log("Big depth increase requested (seq=%d)", m_command_seq);
    }
}

void VRto3DBridge::request_depth_huge_decrease() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_data) {
        m_data->auto_depth_request = 8;
        m_data->command_seq = ++m_command_seq;
        debug_log("Huge depth decrease requested (seq=%d)", m_command_seq);
    }
}

void VRto3DBridge::request_depth_huge_increase() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_data) {
        m_data->auto_depth_request = 9;
        m_data->command_seq = ++m_command_seq;
        debug_log("Huge depth increase requested (seq=%d)", m_command_seq);
    }
}

// ============================================================================
// READ METHODS
// ============================================================================

float VRto3DBridge::get_vrto3d_depth() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return 0.0f;
    return m_data->vrto3d_depth;
}

float VRto3DBridge::get_vrto3d_convergence() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return 0.0f;
    return m_data->vrto3d_convergence;
}

float VRto3DBridge::get_vrto3d_fov() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return 0.0f;
    return m_data->vrto3d_fov;
}

float VRto3DBridge::get_vrto3d_fov_adjustment() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return 0.0f;
    return m_data->vrto3d_fov_adjustment;
}

// ============================================================================
// CALCULATED VALUES
// ============================================================================

float VRto3DBridge::get_compensated_fov_scale(float raw_fov_scale) const {
    if (!m_config.use_fov_compensation) {
        return raw_fov_scale;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data || !m_data->vrto3d_connected) {
        return raw_fov_scale;
    }

    // VRto3D's convergence can affect the effective FOV
    // Compensate for this so zoom magnification stays accurate
    float fov_adjustment = m_data->vrto3d_fov_adjustment;

    if (std::abs(fov_adjustment) < 0.001f) {
        return raw_fov_scale;
    }

    // Adjust our scale based on VRto3D's FOV change
    float compensation = 1.0f - (fov_adjustment / 90.0f);
    compensation = std::clamp(compensation, 0.8f, 1.2f);

    return raw_fov_scale * compensation;
}

// ============================================================================
// v3.1: ADAPTIVE DEPTH CURVE
// ============================================================================

float VRto3DBridge::calculate_zoom_depth_multiplier(
    float fov_scale, float zoom_factor, DepthMode mode
) const {
    /*
     * ADAPTIVE DEPTH CURVE (v3.1)
     *
     * Research basis:
     *   - Shibata 2011: comfort zone is +/-1 deg disparity
     *   - Nonlinear compression recommended by multiple papers
     *   - Power-law preserves depth near fixation, compresses extremes
     *
     * Algorithm:
     *   1. Check dead zone (Bernhard 2014: don't correct within comfort zone)
     *   2. Normalize zoom depth (0.0 at dead zone, 1.0 at extreme 8x zoom)
     *   3. Calculate adaptive power (increases with zoom depth)
     *   4. Apply power curve: mult = pow(fov_scale, power)
     *   5. Apply strength scaling
     *   6. Enforce per-mode floor
     *
     * The curve is gentler at low zoom (preserves depth enjoyment)
     * and increasingly aggressive at high zoom (prevents VAC discomfort).
     */

    // No zoom = no depth change
    if (fov_scale >= 1.0f || zoom_factor <= 1.0f) {
        return 1.0f;
    }

    // Dead zone: don't correct minor FOV changes
    // Bernhard 2014: adjusting within comfort zone adds fusion overhead
    if (zoom_factor < m_config.depth_dead_zone) {
        return 1.0f;
    }

    // Normalize zoom depth: 0.0 at dead zone, 1.0 at extreme zoom (8x)
    const float extreme_zoom = 8.0f;
    float zoom_depth = (zoom_factor - m_config.depth_dead_zone) /
                       (extreme_zoom - m_config.depth_dead_zone);
    zoom_depth = std::clamp(zoom_depth, 0.0f, 1.0f);

    // Adaptive power: increases with zoom depth
    // At dead zone boundary: power = base_power (gentle)
    // At extreme zoom: power = base_power + extra_power (aggressive)
    float power = m_config.depth_base_power +
                  zoom_depth * m_config.depth_extra_power;

    // Core power-law calculation
    // fov_scale is <1.0 when zoomed, so pow(fov_scale, power) is also <1.0
    float mult = std::pow(fov_scale, power);

    // Apply overall strength scaling
    // strength=1.0: use full curve output
    // strength=0.5: halve the reduction (mult closer to 1.0)
    if (m_config.depth_strength < 1.0f) {
        // Lerp between 1.0 (no change) and calculated mult
        mult = 1.0f + m_config.depth_strength * (mult - 1.0f);
    } else if (m_config.depth_strength > 1.0f) {
        // More aggressive: apply additional power
        mult = std::pow(mult, m_config.depth_strength);
    }

    // Per-mode floor: minimum depth depends on what we're doing
    float floor = m_config.min_depth_multiplier;
    switch (mode) {
        case DepthMode::ADS:
            floor = (std::max)(floor, m_config.ads_min_depth);
            break;
        case DepthMode::Scope:
            floor = (std::max)(floor, m_config.scope_min_depth);
            break;
        case DepthMode::Cutscene:
            floor = (std::max)(floor, m_config.cutscene_min_depth);
            break;
        default:
            // DepthMode::None shouldn't reach here, but use scope floor as safety
            floor = (std::max)(floor, m_config.scope_min_depth);
            break;
    }

    mult = (std::max)(mult, floor);

    debug_log("adaptive_depth: zoom=%.2fx fov_scale=%.3f power=%.2f mult=%.3f floor=%.2f mode=%d",
        zoom_factor, fov_scale, power, mult, floor, static_cast<int>(mode));

    return mult;
}

// Backward-compatible v3.0 overload: uses Scope mode as default
float VRto3DBridge::calculate_zoom_depth_multiplier(float fov_scale) const {
    float zoom_factor = (fov_scale > 0.001f) ? (1.0f / fov_scale) : 1.0f;
    return calculate_zoom_depth_multiplier(fov_scale, zoom_factor, DepthMode::Scope);
}

} // namespace vrmod
