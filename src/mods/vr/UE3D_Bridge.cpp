// UE3D_Bridge.cpp - Shared memory bridge to VRto3D

#include "UE3D_Bridge.hpp"
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

void UE3D_Bridge::debug_log(const char* fmt, ...) const {
    if (!m_config.debug_logging) return;

    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    spdlog::info("[UE3D_Bridge] {}", buffer);
}

bool UE3D_Bridge::init() {
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
        sizeof(UE3D_SharedData),
        VRTO3D_SHARED_MEM_NAME
    );

    if (m_mapping == nullptr) {
        spdlog::error("[UE3D_Bridge] Failed to create shared memory: {}", GetLastError());
        return false;
    }

    bool already_exists = (GetLastError() == ERROR_ALREADY_EXISTS);

    m_data = static_cast<UE3D_SharedData*>(
        MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UE3D_SharedData))
    );

    if (m_data == nullptr) {
        spdlog::error("[UE3D_Bridge] Failed to map shared memory: {}", GetLastError());
        CloseHandle(m_mapping);
        m_mapping = nullptr;
        return false;
    }

    // Initialize if we created it (VRto3D might have created it first)
    if (!already_exists) {
        std::memset(m_data, 0, sizeof(UE3D_SharedData));
    } else {
        // Detect version mismatch
        if (m_data->struct_size != 0 && m_data->struct_size != sizeof(UE3D_SharedData)) {
            spdlog::warn("[UE3D_Bridge] Shared memory struct_size mismatch: expected {}, got {}",
                sizeof(UE3D_SharedData), m_data->struct_size);
        }
    }

    m_data->magic = VRTO3D_BRIDGE_MAGIC;
    m_data->version = VRTO3D_BRIDGE_VERSION;
    m_data->struct_size = sizeof(UE3D_SharedData);
    m_data->flags = FLAG_MULTIPLIER_MODE | FLAG_SCENE_AWARE | FLAG_FOV_COMP;

    m_data->depth_multiplier = 1.0f;
    m_data->world_scale = 1.0f;
    m_data->stereo_depth_hint = 0.0f;

    spdlog::info("[UE3D_Bridge] v4.0 initialized shared memory '{}' ({})",
        VRTO3D_SHARED_MEM_NAME, already_exists ? "existing" : "new");

    return true;
}

void UE3D_Bridge::shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_data != nullptr) {
        // Signal we're disconnecting
        m_data->is_valid = 0;
        m_data->auto_depth_request = 0;

        UnmapViewOfFile(m_data);
        m_data = nullptr;
    }

    if (m_mapping != nullptr) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }

    spdlog::info("[UE3D_Bridge] Shutdown complete");
}

bool UE3D_Bridge::is_vrto3d_connected() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return false;
    return m_data->vrto3d_connected != 0;
}

bool UE3D_Bridge::is_vrto3d_profile_loaded() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return false;
    return m_data->vrto3d_profile_loaded != 0;
}

void UE3D_Bridge::refresh_timestamp() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return;
    update_timestamp();
}

void UE3D_Bridge::update_all(
    float game_fov, float base_fov, float fov_scale, float zoom_factor,
    bool is_zooming, bool is_valid, ZoomMode zoom_mode,
    float world_scale
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return;

    m_data->fov_scale = safe_float(fov_scale, 1.0f, 0.01f, 2.0f);
    m_data->zoom_factor = safe_float(zoom_factor, 1.0f, 0.1f, 100.0f);
    m_data->is_valid = is_valid ? 1 : 0;
    m_data->zoom_mode = static_cast<uint8_t>(zoom_mode);

    // depth_multiplier, scene_type: kept in protocol struct for byte layout (Invariant I7), not written.
    // Depth commands (auto_depth_request 2-6) are sent via request_calibration/depth_* methods.
    m_data->world_scale = safe_float(world_scale, 1.0f, 0.01f, 10000.0f);

    m_data->monitor_mode = m_monitor_mode ? 1 : 0;

    // Stereo depth hint for overlay IPD matching
    if (m_monitor_mode) {
        m_data->stereo_depth_hint = safe_float(
            ue3d::MonitorState::get().stereo_depth_safe(), 0.0f, 0.0f, 2.0f);
    }

    update_timestamp();

    debug_log("update_all: scale=%.3f mode=%d ws=%.3f",
        fov_scale, static_cast<int>(zoom_mode), world_scale);
}

void UE3D_Bridge::update_timestamp() {
    // Must use GetTickCount64 to match VRto3D's staleness checks
    m_data->uevr_timestamp = GetTickCount64();
    m_data->uevr_frame_count = ++m_frame_count;
}

void UE3D_Bridge::request_calibration() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_data) {
        m_data->auto_depth_request = 2;
        m_data->command_seq = ++m_command_seq;
        debug_log("Calibration requested (seq=%d)", m_command_seq);
    }
}

void UE3D_Bridge::request_depth_decrease() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_data) {
        m_data->auto_depth_request = 3;
        m_data->command_seq = ++m_command_seq;
        debug_log("Depth decrease requested (seq=%d)", m_command_seq);
    }
}

void UE3D_Bridge::request_depth_increase() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_data) {
        m_data->auto_depth_request = 4;
        m_data->command_seq = ++m_command_seq;
        debug_log("Depth increase requested (seq=%d)", m_command_seq);
    }
}

void UE3D_Bridge::request_depth_big_decrease() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_data) {
        m_data->auto_depth_request = 5;
        m_data->command_seq = ++m_command_seq;
        debug_log("Big depth decrease requested (seq=%d)", m_command_seq);
    }
}

void UE3D_Bridge::request_depth_big_increase() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_data) {
        m_data->auto_depth_request = 6;
        m_data->command_seq = ++m_command_seq;
        debug_log("Big depth increase requested (seq=%d)", m_command_seq);
    }
}


float UE3D_Bridge::get_vrto3d_depth() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_data) return 0.0f;
    return m_data->vrto3d_depth;
}

// ---- Leia LookAround ----

void UE3D_Bridge::update_leia_tracking() {
    // Per-frame call (Lesson 142: NOT gated by bridge dirty check)
    auto& ms = ue3d::MonitorState::get();
    if (!ms.bLeiaLookAroundEnabled.load(std::memory_order_relaxed)) return;
    if (!is_initialized()) return;

    read_leia_eye_data();
}

void UE3D_Bridge::reset_leia_calibration() {
    m_leia_calibrated = false;
    m_leia_smooth_x = 0.0f;
    m_leia_smooth_y = 0.0f;
    m_leia_smooth_z = 0.0f;
    m_leia_ref_x = 0.0f;
    m_leia_ref_y = 0.0f;
    m_leia_ref_z = 0.0f;
    m_leia_ref_lx = 0.0f; m_leia_ref_ly = 0.0f;
    m_leia_ref_rx = 0.0f; m_leia_ref_ry = 0.0f;
    m_leia_smooth_lx = 0.0f; m_leia_smooth_ly = 0.0f;
    m_leia_smooth_rx = 0.0f; m_leia_smooth_ry = 0.0f;
    m_leia_last_frame = 0;

    auto& ms = ue3d::MonitorState::get();
    ms.fLeiaHeadX.store(0.0f, std::memory_order_relaxed);
    ms.fLeiaHeadY.store(0.0f, std::memory_order_relaxed);
    ms.fLeiaHeadZ.store(0.0f, std::memory_order_relaxed);
    ms.fLeiaLeftEyeX.store(0.0f, std::memory_order_relaxed);
    ms.fLeiaLeftEyeY.store(0.0f, std::memory_order_relaxed);
    ms.fLeiaRightEyeX.store(0.0f, std::memory_order_relaxed);
    ms.fLeiaRightEyeY.store(0.0f, std::memory_order_relaxed);
    ms.bLeiaTracking.store(false, std::memory_order_relaxed);
    ms.uLeiaFrameCounter.store(0, std::memory_order_relaxed);

    spdlog::info("[UE3D_Bridge] Leia calibration reset");
}

void UE3D_Bridge::read_leia_eye_data() {
    // Read Leia eye positions from shared memory (written by 3DGameBridge)
    // Pipeline: shared mem -> NaN guard -> center eye -> calibrate -> coord convert -> gate -> smooth -> MonitorState

    auto& ms = ue3d::MonitorState::get();

    // Lock-free read of shared memory Leia fields
    // (3DGameBridge writes atomically per-field, we tolerate one-frame tearing)
    if (!m_data) {
        ms.bLeiaTracking.store(false, std::memory_order_relaxed);
        return;
    }

    // Check if 3DGameBridge is writing Leia data
    bool tracking = m_data->leia_tracking_active != 0;
    uint32_t frame = m_data->leia_frame_counter;

    if (!tracking || frame == 0) {
        ms.bLeiaTracking.store(false, std::memory_order_relaxed);
        return;
    }

    // Skip if no new data since last read
    if (frame == m_leia_last_frame) return;
    m_leia_last_frame = frame;

    // Read raw eye positions (Leia coords: mm, X=right, Y=up, Z=backward)
    float lx = m_data->leia_left_eye_x;
    float ly = m_data->leia_left_eye_y;
    float lz = m_data->leia_left_eye_z;
    float rx = m_data->leia_right_eye_x;
    float ry = m_data->leia_right_eye_y;
    float rz = m_data->leia_right_eye_z;

    // NaN guard (I8)
    if (!std::isfinite(lx) || !std::isfinite(ly) || !std::isfinite(lz) ||
        !std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(rz)) {
        return;  // skip bad frame, keep last good values
    }

    // Center eye (average both eyes for head translation)
    float cx = (lx + rx) * 0.5f;
    float cy = (ly + ry) * 0.5f;
    float cz = (lz + rz) * 0.5f;

    // Dynamic calibration: first tracked frame = zero reference
    if (!m_leia_calibrated) {
        m_leia_ref_x = cx;
        m_leia_ref_y = cy;
        m_leia_ref_z = cz;
        // Per-eye references (Kooima: each eye gets its own zero)
        m_leia_ref_lx = lx; m_leia_ref_ly = ly;
        m_leia_ref_rx = rx; m_leia_ref_ry = ry;
        m_leia_calibrated = true;
        spdlog::info("[UE3D_Bridge] Leia: calibrated zero reference ({:.1f}, {:.1f}, {:.1f}) mm",
            cx, cy, cz);
    }

    // Offset from zero reference (still in Leia mm coords)
    float dx_mm = cx - m_leia_ref_x;
    float dy_mm = cy - m_leia_ref_y;
    float dz_mm = cz - m_leia_ref_z;

    // Leia -> UE coordinate conversion: (-Z, X, Y), mm/10 for cm
    // Leia: X=right, Y=up, Z=backward
    // UE:   X=forward, Y=right, Z=up
    // So: UE_X = -Leia_Z, UE_Y = Leia_X, UE_Z = Leia_Y
    // For parallax we only need horizontal (UE_Y = Leia_X) and vertical (UE_Z = Leia_Y)
    float head_h_cm = dx_mm / 10.0f;   // horizontal offset (Leia X -> cm)
    float head_v_cm = dy_mm / 10.0f;   // vertical offset (Leia Y -> cm)
    float head_d_cm = -dz_mm / 10.0f;  // depth offset (-Leia Z -> cm, forward positive)

    // Per-axis gating
    if (!ms.bLeiaAxisX.load(std::memory_order_relaxed)) head_h_cm = 0.0f;
    if (!ms.bLeiaAxisY.load(std::memory_order_relaxed)) head_v_cm = 0.0f;
    if (!ms.bLeiaAxisZ.load(std::memory_order_relaxed)) head_d_cm = 0.0f;

    // EMA smoothing
    float alpha = ms.leia_smoothing_safe();
    m_leia_smooth_x = alpha * head_h_cm + (1.0f - alpha) * m_leia_smooth_x;
    m_leia_smooth_y = alpha * head_v_cm + (1.0f - alpha) * m_leia_smooth_y;
    m_leia_smooth_z = alpha * head_d_cm + (1.0f - alpha) * m_leia_smooth_z;

    // Per-eye Kooima parallax: each eye's delta from its own reference
    // Leia X = horizontal, Y = vertical (same axes as center-eye)
    float left_h_cm  = (lx - m_leia_ref_lx) / 10.0f;
    float left_v_cm  = (ly - m_leia_ref_ly) / 10.0f;
    float right_h_cm = (rx - m_leia_ref_rx) / 10.0f;
    float right_v_cm = (ry - m_leia_ref_ry) / 10.0f;

    // Per-axis gating (same gates as center-eye)
    if (!ms.bLeiaAxisX.load(std::memory_order_relaxed)) { left_h_cm = 0.0f; right_h_cm = 0.0f; }
    if (!ms.bLeiaAxisY.load(std::memory_order_relaxed)) { left_v_cm = 0.0f; right_v_cm = 0.0f; }

    // EMA smoothing (same alpha as center-eye)
    m_leia_smooth_lx = alpha * left_h_cm  + (1.0f - alpha) * m_leia_smooth_lx;
    m_leia_smooth_ly = alpha * left_v_cm  + (1.0f - alpha) * m_leia_smooth_ly;
    m_leia_smooth_rx = alpha * right_h_cm + (1.0f - alpha) * m_leia_smooth_rx;
    m_leia_smooth_ry = alpha * right_v_cm + (1.0f - alpha) * m_leia_smooth_ry;

    // Write to MonitorState (I8: values are finite since inputs are finite + linear ops)
    ms.fLeiaHeadX.store(m_leia_smooth_x, std::memory_order_relaxed);
    ms.fLeiaHeadY.store(m_leia_smooth_y, std::memory_order_relaxed);
    ms.fLeiaHeadZ.store(m_leia_smooth_z, std::memory_order_relaxed);
    ms.fLeiaLeftEyeX.store(m_leia_smooth_lx, std::memory_order_relaxed);
    ms.fLeiaLeftEyeY.store(m_leia_smooth_ly, std::memory_order_relaxed);
    ms.fLeiaRightEyeX.store(m_leia_smooth_rx, std::memory_order_relaxed);
    ms.fLeiaRightEyeY.store(m_leia_smooth_ry, std::memory_order_relaxed);
    ms.bLeiaTracking.store(true, std::memory_order_relaxed);
    ms.uLeiaFrameCounter.store(frame, std::memory_order_relaxed);

    // Display dimensions (for auto-calibration)
    float dw = m_data->leia_display_width_cm;
    float dh = m_data->leia_display_height_cm;
    if (std::isfinite(dw) && dw > 0.0f) {
        ms.fLeiaDisplayWidthCm.store(dw, std::memory_order_relaxed);
    }
    if (std::isfinite(dh) && dh > 0.0f) {
        ms.fLeiaDisplayHeightCm.store(dh, std::memory_order_relaxed);
    }
}

} // namespace vrmod
